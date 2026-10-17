// RS(N,K) + Interleave Top Module - Parameterized
// GF(2^8), primitive polynomial 0x11D
//
// Parameters:
//   N         : codeword length (default 255)
//   K         : data length (default 239)
//   PARITY    : N-K (default 16)
//   T         : PARITY/2, max correctable errors (default 8)
//   INTERLEAVE: interleave depth D (default 32)
//
// Supported configurations (update g_rom in rs_encoder for each):
//   RS(255,239) D=32  => バースト256バイト訂正  (FMIQ装置向け)
//   RS(255,223) D=16  => バースト128バイト訂正
//   RS(255,191) D= 8  => バースト 64バイト訂正
//   RS(255,239) D=64  => バースト512バイト訂正
//
// アーキテクチャ（2026-07-20 実装）:
//   単一のrs_encoderコア・単一のrs_decoder_simpleコアをD=32個の
//   コードワードで時分割共有する。理由: 100MHz動作でデコーダ1回=534clk
//   =5.34us、D=32直列でも170.88usしかかからず、8Mbpsで1ブロック
//   (D*N=8160byte)を送るのに要する8.16msの約1/48に収まるため、
//   並列コア化(DSP/リソース増)は不要と判断。
//
// 動作フロー（非パイプライン、ブロック単位）:
//   TX: COLLECT(D*K byte受信) -> ENCODE(D回、コア共有) -> OUTPUT(D*N byte送信、
//       送信順は byte位置iをD個のコードワードにまたがって出す
//       (transmitted[t] = obuf[t%D][t/D]) ため、連続バーストが
//       複数コードワードに分散し、1コードワードあたりの誤り数を抑える。
//   RX: COLLECT(D*N byte受信、送信側と同じ順で再配置) -> DECODE(D回、コア共有、
//       各コードワードのuncorrectableを個別にラッチ) -> OUTPUT(D*K byte送信、
//       各出力バイトに対応するコードワードのuncorrectableフラグを同時出力)。
//
// 注意（未解決・既知の制約）:
//   シングルバッファ構成のため、DECODE/OUTPUTフェーズ中(合計で概ね
//   171us+出力時間)はrx_in_readyがLowになり新規受信を一時停止する。
//   実回線がバックプレッシャーで待てない場合は、上流に171us分
//   (目安171byte程度)を吸収できるエラスティックFIFOが必要。
//   本モジュールはダブルバッファ(ping-pong)ではないため、連続入力を
//   止められない用途では別途バッファ追加を検討すること。

// TX/RXの状態管理は完全に独立（レジスタ・always blockとも共有ゼロ）。
// TXリンク断がRXのslot状態やprocess状態に影響することは構造的にない。
//
// TX側インターフェース: Ethernet/LAN接続。link_upはEthernetリンク状態
//   (物理リンクダウン/ケーブル抜け等)を表す。0の間はCOLLECT開始せず、
//   収集途中なら破棄して先頭から再開。送出(OUT)は一時停止のみ(破棄せず、
//   再接続後に停止位置から再開)。
//
// RX側インターフェース: 別FPGAからの直結ストリーム(Ethernet/LAN経由ではない)。
//   よってlink_upには依存しない。代わりに:
//     - rx_in_valid/rx_in_ready: データイネーブル(標準valid/readyハンドシェイク)
//     - rx_sof: ブロック先頭マーカー。ブロック末尾は固定長(D*N byte)カウントで判定。
//     - RX_TIMEOUT_CYCLES: ブロック収集途中でrx_in_validが連続して
//       この値だけ途切れたら、そのブロックを破棄し次のrx_sofで再同期する。
//     - rx_sofが収集途中(rx_in_cnt!=0)に来た場合も、直前のブロックを破棄して
//       このバイトを新ブロックの先頭として受理する(送信側の異常/再起動に対応)。
//   送信元FPGAとクロックが異なる場合は、rx_in_valid/rx_sof/rx_in_dataを
//   本モジュールのclkドメインへ渡す前に、外部の非同期(CDC)FIFOを
//   挿入すること。本モジュール自体はシングルクロック設計。
module rs_fec_tx_core #(
    parameter N          = 255,
    parameter K          = 239,
    parameter PARITY     = 16,
    parameter INTERLEAVE = 32
)(
    input        clk,
    input        rst_n,

    input        tx_in_valid,
    input  [7:0] tx_in_data,
    output       tx_in_ready,
    output reg   tx_out_valid,
    output reg [7:0] tx_out_data,
    input        tx_out_ready
);

localparam integer DK = INTERLEAVE * K;
localparam integer DN = INTERLEAVE * N;

// ============================================================
// Shared RS encoder core (time-multiplexed across D codewords)
// ============================================================
reg        enc_in_valid;
reg  [7:0] enc_in_data;
wire       enc_out_valid;
wire [7:0] enc_out_data;

rs_encoder #(.N(N), .K(K), .PARITY(PARITY)) u_encoder (
    .clk(clk), .rst_n(rst_n),
    .in_valid(enc_in_valid), .in_data(enc_in_data),
    .out_valid(enc_out_valid), .out_data(enc_out_data)
);

// ============================================================
// Shared RS decoder core (time-multiplexed across D codewords)
// ============================================================
`ifdef RS_FEC_INCLUDE_UNUSED_RX
reg              dec_load_cw;
reg  [7:0]       dec_cw_in [0:N-1];
wire             dec_done;
wire [7:0]       dec_cw_out [0:N-1];
wire             dec_uncorrectable;

rs_decoder_simple #(.N(N), .K(K), .PARITY(PARITY), .T(T)) u_decoder (
    .clk(clk), .rst_n(rst_n),
    .load_cw(dec_load_cw), .cw_in(dec_cw_in),
    .dec_done(dec_done), .cw_out(dec_cw_out),
    .uncorrectable(dec_uncorrectable)
);
`endif

// ============================================================
// TX side
// ============================================================
// BRAM化(2026-09-02): 2次元配列はFF推論(容量オーバー)。1次元+ram_style="block"
// + TX FSMを同期リセット化(非同期リセット下だとBRAM推論不可 Synth 8-4767)で
// Block RAMに載せる。アクセスは元から1byte/clk・<=でレジスタ捕捉のため挙動不変。
// 線形番地: tx_ibuf = d*K + k , tx_obuf = d*N + n
(* ram_style = "block" *) reg [7:0] tx_ibuf [0:DK-1];
(* ram_style = "block" *) reg [7:0] tx_obuf [0:DN-1];

localparam TX_COLLECT      = 3'd0,
           TX_ENC_FEED     = 3'd1,
           TX_ENC_CAPTURE  = 3'd2,
           TX_OUT          = 3'd3;

reg [2:0]  tx_state;
reg [15:0] tx_in_cnt;    // 0 .. DK-1  during COLLECT
reg [7:0]  tx_d;         // current codeword index 0..D-1
reg [7:0]  tx_k;         // byte index within codeword (feed/capture)
reg [15:0] tx_out_cnt;   // 0 .. DN-1  during OUT

assign tx_in_ready = (tx_state == TX_COLLECT);

integer ti;

always @(posedge clk) begin  // BRAM化のため同期リセット(rst_nは起動時複数clk保持で挙動同一)
    if (!rst_n) begin
        tx_state     <= TX_COLLECT;
        tx_in_cnt    <= 0;
        tx_d         <= 0;
        tx_k         <= 0;
        tx_out_cnt   <= 0;
        tx_out_valid <= 0;
        tx_out_data  <= 0;
        enc_in_valid <= 0;
        enc_in_data  <= 0;
    end else begin
        case (tx_state)
        TX_COLLECT: begin
            tx_out_valid <= 0;
            if (tx_in_valid) begin
                tx_ibuf[(tx_in_cnt % INTERLEAVE)*K + (tx_in_cnt / INTERLEAVE)] <= tx_in_data;
                if (tx_in_cnt == DK - 1) begin
                    tx_in_cnt <= 0;
                    tx_d      <= 0;
                    tx_k      <= 0;
                    tx_state  <= TX_ENC_FEED;
                end else begin
                    tx_in_cnt <= tx_in_cnt + 1;
                end
            end
        end

        TX_ENC_FEED: begin
            enc_in_valid <= 1;
            enc_in_data  <= tx_ibuf[tx_d*K + tx_k];
            if (tx_k == K - 1) begin
                tx_k     <= 0;
                tx_state <= TX_ENC_CAPTURE;
            end else begin
                tx_k <= tx_k + 1;
            end
        end

        TX_ENC_CAPTURE: begin
            enc_in_valid <= 0;
            if (enc_out_valid) begin
                tx_obuf[tx_d*N + tx_k] <= enc_out_data;
                if (tx_k == N - 1) begin
                    tx_k <= 0;
                    if (tx_d == INTERLEAVE - 1) begin
                        tx_d       <= 0;
                        tx_out_cnt <= 0;
                        tx_state   <= TX_OUT;
                    end else begin
                        tx_d     <= tx_d + 1;
                        tx_state <= TX_ENC_FEED;
                    end
                end else begin
                    tx_k <= tx_k + 1;
                end
            end
        end

        TX_OUT: begin
            // リンクダウン中は送出を一時停止するのみ(破棄しない)。
            // 送信データはまだLANに出ていないため、再接続後は
            // 停止した位置からそのまま再開してよい。
            if (tx_out_ready) begin
                tx_out_valid <= 1;
                tx_out_data  <= tx_obuf[(tx_out_cnt % INTERLEAVE)*N + (tx_out_cnt / INTERLEAVE)];
                if (tx_out_cnt == DN - 1) begin
                    tx_out_cnt <= 0;
                    tx_state   <= TX_COLLECT;
                end else begin
                    tx_out_cnt <= tx_out_cnt + 1;
                end
            end else begin
                tx_out_valid <= 0;
            end
        end
        endcase
    end
end

// ============================================================
// RX side - THREE independent, fully concurrent FSMs (COLLECT /
// DECODE / OUTPUT), each free-running over RX_SLOTS>=3 slots.
//
// History (2026-07-20): the original design combined DECODE+OUTPUT
// into a single FSM sharing just 2 ping-pong slots. That looked
// correct in every test except a TRUE continuous test (input and
// output driven by two independent concurrent processes, zero gap
// between blocks, output paced at the real 8Mbps rate): OUTPUT
// (~7.65ms/block at 8Mbps) occupied the combined FSM long enough
// that it could not start DECODEing the next collected block in
// time, so COLLECT eventually had nowhere to put new data and
// started dropping bytes (473 dropped within the first 10 blocks).
// Splitting DECODE out into its own FSM (it only needs ~137us/block,
// negligible next to OUTPUT's ~7.65ms) removes that coupling: COLLECT,
// DECODE and OUTPUT can now each be actively using a DIFFERENT slot
// at the same instant, e.g. slot A collecting while slot B decodes
// while slot C outputs - exactly matching the pipeline's 3 stages,
// hence RX_SLOTS>=3.
//
// Per-slot lifecycle (5 states; single writer = the "slot state
// manager" below, driven by one-shot pulses from each FSM so no
// flag is ever left stale - the same discipline that fixed the
// earlier 2-slot ping-pong bug):
//   FREE       -> (collect_fill_done)   -> FILLED
//   FILLED     -> (decode_claim_pulse)  -> DECODING
//   DECODING   -> (decode_done_pulse)   -> DECODED
//   DECODED    -> (output_claim_pulse)  -> OUTPUTTING
//   OUTPUTTING -> (output_done_pulse)   -> FREE
// Each of COLLECT/DECODE/OUTPUT privately tracks which slot index it
// is currently working (round-robin 0..RX_SLOTS-1), so no two FSMs
// ever touch the same slot's data concurrently.
// ============================================================
`ifdef RS_FEC_INCLUDE_UNUSED_RX
reg [7:0] rx_ibuf [0:RX_SLOTS-1][0:INTERLEAVE-1][0:N-1];
reg [7:0] rx_obuf [0:RX_SLOTS-1][0:INTERLEAVE-1][0:K-1];
reg       rx_unc  [0:RX_SLOTS-1][0:INTERLEAVE-1];

localparam SLOT_FREE       = 3'd0,
           SLOT_FILLED     = 3'd1,
           SLOT_DECODING   = 3'd2,
           SLOT_DECODED    = 3'd3,
           SLOT_OUTPUTTING = 3'd4;

reg [2:0] slot_state [0:RX_SLOTS-1];

integer ri, si;

// ---- COLLECT FSM (fills rx_ibuf[collect_slot]) ----
// link_upには依存しない(RXはEthernetではなく別FPGAからの直結入力)。
// 代わりにrx_sof(ブロック先頭マーカー)とタイムアウトで同期管理する:
//   - rx_sofが来たら(rx_in_cnt=0でも!=0でも)必ずそこを新ブロックの
//     先頭として受理する。途中(!=0)だった場合は前ブロックは
//     暗黙的に破棄(slot_stateがFILLEDに進んでいないため無害)。
//   - rx_in_cnt=0(sof待ち)の間にsof無しでデータが来ても無視する。
//   - 収集途中(rx_in_cnt!=0)にrx_in_validが連続RX_TIMEOUT_CYCLES回
//     途切れたら破棄し、rx_in_cnt=0(sof待ち)に戻る。
reg [7:0]  collect_slot;
reg [15:0] rx_in_cnt;
reg [15:0] rx_timeout_cnt;

assign rx_in_ready = (slot_state[collect_slot] == SLOT_FREE);

wire rx_accept_sof  = rx_in_valid && (slot_state[collect_slot] == SLOT_FREE) && rx_sof;
wire rx_accept_cont = rx_in_valid && (slot_state[collect_slot] == SLOT_FREE) && !rx_sof && (rx_in_cnt != 0);

wire collect_fill_done = rx_accept_cont && (rx_in_cnt == DN - 1);

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        collect_slot   <= 0;
        rx_in_cnt      <= 0;
        rx_timeout_cnt <= 0;
    end else if (rx_accept_sof) begin
        rx_ibuf[collect_slot][0][0] <= rx_in_data;
        rx_in_cnt      <= 1;
        rx_timeout_cnt <= 0;
    end else if (rx_accept_cont) begin
        rx_ibuf[collect_slot][rx_in_cnt % INTERLEAVE][rx_in_cnt / INTERLEAVE] <= rx_in_data;
        rx_timeout_cnt <= 0;
        if (rx_in_cnt == DN - 1) begin
            rx_in_cnt    <= 0;
            collect_slot <= (collect_slot == RX_SLOTS-1) ? 8'd0 : collect_slot + 8'd1;
        end else begin
            rx_in_cnt <= rx_in_cnt + 1;
        end
    end else if (rx_in_cnt != 0) begin
        // ブロック途中で入力途絶: タイムアウト計測
        if (rx_timeout_cnt >= RX_TIMEOUT_CYCLES - 1) begin
            rx_in_cnt      <= 0; // 破棄。以後は次のrx_sofまで無視して待つ
            rx_timeout_cnt <= 0;
        end else begin
            rx_timeout_cnt <= rx_timeout_cnt + 1;
        end
    end
end

// ---- DECODE FSM (drains rx_ibuf[decode_slot] -> rx_obuf[decode_slot]) ----
localparam DEC_IDLE  = 2'd0,
           DEC_COPY  = 2'd1,
           DEC_PULSE = 2'd2,
           DEC_WAIT  = 2'd3;

reg [1:0]  dec_state;
reg [7:0]  decode_slot;
reg [7:0]  dec_d;

wire decode_claim_pulse = (dec_state == DEC_IDLE) && (slot_state[decode_slot] == SLOT_FILLED);
wire decode_done_pulse  = (dec_state == DEC_WAIT) && dec_done && (dec_d == INTERLEAVE - 1);

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        dec_state   <= DEC_IDLE;
        decode_slot <= 0;
        dec_d       <= 0;
        dec_load_cw <= 0;
        for (ri = 0; ri < N; ri = ri + 1) dec_cw_in[ri] <= 8'h00;
    end else begin
        case (dec_state)
        DEC_IDLE: begin
            dec_load_cw <= 0;
            if (decode_claim_pulse) begin
                dec_d     <= 0;
                dec_state <= DEC_COPY;
            end
        end
        DEC_COPY: begin
            for (ri = 0; ri < N; ri = ri + 1)
                dec_cw_in[ri] <= rx_ibuf[decode_slot][dec_d][ri];
            dec_load_cw <= 0;
            dec_state   <= DEC_PULSE;
        end
        DEC_PULSE: begin
            dec_load_cw <= 1;
            dec_state   <= DEC_WAIT;
        end
        DEC_WAIT: begin
            dec_load_cw <= 0;
            if (dec_done) begin
                for (ri = 0; ri < K; ri = ri + 1)
                    rx_obuf[decode_slot][dec_d][ri] <= dec_cw_out[ri];
                rx_unc[decode_slot][dec_d] <= dec_uncorrectable;
                if (dec_d == INTERLEAVE - 1) begin
                    dec_d       <= 0;
                    decode_slot <= (decode_slot == RX_SLOTS-1) ? 8'd0 : decode_slot + 8'd1;
                    dec_state   <= DEC_IDLE;
                end else begin
                    dec_d     <= dec_d + 1;
                    dec_state <= DEC_COPY;
                end
            end
        end
        endcase
    end
end

// ---- OUTPUT FSM (drains rx_obuf[output_slot] -> rx_out) ----
reg        out_busy;
reg [7:0]  output_slot;
reg [15:0] rx_out_cnt;

wire output_claim_pulse = !out_busy && (slot_state[output_slot] == SLOT_DECODED);
wire output_done_pulse  = out_busy && rx_out_ready && (rx_out_cnt == DK - 1);

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        out_busy             <= 0;
        output_slot          <= 0;
        rx_out_cnt            <= 0;
        rx_out_valid          <= 0;
        rx_out_data           <= 0;
        rx_out_uncorrectable  <= 0;
        rx_block_done         <= 0;
    end else begin
        rx_block_done <= 0;
        if (!out_busy) begin
            rx_out_valid <= 0;
            if (output_claim_pulse) begin
                rx_out_cnt <= 0;
                out_busy   <= 1;
            end
        end else begin
            if (rx_out_ready) begin
                rx_out_valid         <= 1;
                rx_out_data          <= rx_obuf[output_slot][rx_out_cnt % INTERLEAVE][rx_out_cnt / INTERLEAVE];
                rx_out_uncorrectable <= rx_unc [output_slot][rx_out_cnt % INTERLEAVE];
                if (rx_out_cnt == DK - 1) begin
                    rx_out_cnt    <= 0;
                    out_busy      <= 0;
                    output_slot   <= (output_slot == RX_SLOTS-1) ? 8'd0 : output_slot + 8'd1;
                    rx_block_done <= 1;
                end else begin
                    rx_out_cnt <= rx_out_cnt + 1;
                end
            end else begin
                rx_out_valid <= 0;
            end
        end
    end
end

// ---- Slot state manager: sole writer of slot_state[] ----
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        for (si = 0; si < RX_SLOTS; si = si + 1) slot_state[si] <= SLOT_FREE;
        for (si = 0; si < RX_SLOTS; si = si + 1)
            for (ri = 0; ri < INTERLEAVE; ri = ri + 1) rx_unc[si][ri] <= 0;
    end else begin
        if (collect_fill_done)   slot_state[collect_slot] <= SLOT_FILLED;
        if (decode_claim_pulse)  slot_state[decode_slot]  <= SLOT_DECODING;
        if (decode_done_pulse)   slot_state[decode_slot]  <= SLOT_DECODED;
        if (output_claim_pulse)  slot_state[output_slot]  <= SLOT_OUTPUTTING;
        if (output_done_pulse)   slot_state[output_slot]  <= SLOT_FREE;
    end
end

`endif

endmodule
