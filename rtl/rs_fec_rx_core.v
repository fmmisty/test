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
module rs_fec_rx_core #(
    parameter N          = 255,
    parameter K          = 239,
    parameter PARITY     = 16,
    parameter T          = 8,
    parameter INTERLEAVE = 32,
    parameter RX_TIMEOUT_CYCLES = 2000, // ブロック途中での入力途絶タイムアウト(clkサイクル)
    parameter RX_SLOTS = 3 // RX側スロット数。COLLECT/DECODE/OUTPUTを
                            // 3並列FSM化したため最低3必要
                            // (2026-07-20: DECODE+OUTPUTが同一FSMで直列
                            // だった旧2スロットping-pong版は、真の連続
                            // 実時間試験でOUTPUT(8Mbps律速)がDECODEの
                            // 次スロット着手を止め続け、10ブロックで
                            // 473byteドロップが発生。3スロット化必須)
)(
    input        clk,
    input        rst_n,

    input        rx_in_valid,
    input        rx_sof,     // ブロック先頭マーカー(rx_in_valid同時アサート)
    input  [7:0] rx_in_data,
    output       rx_in_ready,
    output reg   rx_out_valid,
    output reg [7:0] rx_out_data,
    input        rx_out_ready,
    output reg   rx_out_uncorrectable, // rx_out_valid時、そのバイトが属する
                                        // コードワードが訂正不能だったかを示す
    output reg   rx_block_done         // Dコードワード分の出力が完了した1clkパルス
);

localparam integer DK = INTERLEAVE * K;
localparam integer DN = INTERLEAVE * N;

// ============================================================
// Shared RS encoder core (time-multiplexed across D codewords)
// ============================================================
`ifdef RS_FEC_INCLUDE_UNUSED_TX
reg        enc_in_valid;
reg  [7:0] enc_in_data;
wire       enc_out_valid;
wire [7:0] enc_out_data;

rs_encoder #(.N(N), .K(K), .PARITY(PARITY)) u_encoder (
    .clk(clk), .rst_n(rst_n),
    .in_valid(enc_in_valid), .in_data(enc_in_data),
    .out_valid(enc_out_valid), .out_data(enc_out_data)
);
`endif

// ============================================================
// Shared RS decoder core (time-multiplexed across D codewords)
// ============================================================
reg              dec_load_cw;
reg  [8*N-1:0]   dec_cw_in;    // 平坦化: dec_cw_in[8*i +: 8] = byte i (配列ポート合成回避)
wire             dec_done;
wire [8*N-1:0]   dec_cw_out;   // 平坦化: dec_cw_out[8*i +: 8] = byte i
wire             dec_uncorrectable;

rs_decoder_simple #(.N(N), .K(K), .PARITY(PARITY), .T(T)) u_decoder (
    .clk(clk), .rst_n(rst_n),
    .load_cw(dec_load_cw), .cw_in(dec_cw_in),
    .dec_done(dec_done), .cw_out(dec_cw_out),
    .uncorrectable(dec_uncorrectable)
);

// ============================================================
// TX side
// ============================================================
`ifdef RS_FEC_INCLUDE_UNUSED_TX
reg [7:0] tx_ibuf [0:INTERLEAVE-1][0:K-1];
reg [7:0] tx_obuf [0:INTERLEAVE-1][0:N-1];

localparam TX_COLLECT      = 3'd0,
           TX_ENC_FEED     = 3'd1,
           TX_ENC_CAPTURE  = 3'd2,
           TX_OUT          = 3'd3;

reg [2:0]  tx_state;
reg [15:0] tx_in_cnt;    // 0 .. DK-1  during COLLECT
reg [7:0]  tx_d;         // current codeword index 0..D-1
reg [7:0]  tx_k;         // byte index within codeword (feed/capture)
reg [15:0] tx_out_cnt;   // 0 .. DN-1  during OUT

assign tx_in_ready = link_up && (tx_state == TX_COLLECT);

integer ti;

always @(posedge clk or negedge rst_n) begin
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
            if (!link_up) begin
                // リンク断: 収集途中データを破棄し、再接続時は
                // 必ずブロック先頭(tx_in_cnt=0)から再開する。
                tx_in_cnt <= 0;
            end else if (tx_in_valid) begin
                tx_ibuf[tx_in_cnt % INTERLEAVE][tx_in_cnt / INTERLEAVE] <= tx_in_data;
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
            enc_in_data  <= tx_ibuf[tx_d][tx_k];
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
                tx_obuf[tx_d][tx_k] <= enc_out_data;
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
            if (link_up && tx_out_ready) begin
                tx_out_valid <= 1;
                tx_out_data  <= tx_obuf[tx_out_cnt % INTERLEAVE][tx_out_cnt / INTERLEAVE];
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
`endif
// BRAM化(2026-09-02): 元は3次元unpacked配列で全部FF推論→容量オーバー。
// rx_ibuf/rx_obuf は「書込FSMと読出FSMが別always」のためVivadoが自動
// BRAM推論できない(Synth 8-3391)。明示的な単純デュアルポートRAM
// (rs_sdp_ram, 同期読み1clkレイテンシ)に置き換えてBlock RAMへ確実に載せる。
// 線形番地: rx_ibuf = (slot*INTERLEAVE + d)*N + n
//           rx_obuf = (slot*INTERLEAVE + d)*K + k
localparam integer IB_DEPTH = RX_SLOTS*INTERLEAVE*N;   // 24480
localparam integer OB_DEPTH = RX_SLOTS*INTERLEAVE*K;   // 22944
localparam integer IB_AW    = 15;
localparam integer OB_AW    = 15;

// rx_ibuf: 書込=COLLECT / 読出=DECODE
wire              ib_we;
wire [IB_AW-1:0]  ib_waddr;
wire [7:0]        ib_wdata;
wire [IB_AW-1:0]  ib_raddr;
wire [7:0]        ib_rdata;
rs_sdp_ram #(.DW(8), .DEPTH(IB_DEPTH), .AW(IB_AW)) u_rx_ibuf (
    .clk(clk), .we(ib_we), .waddr(ib_waddr), .wdata(ib_wdata),
    .raddr(ib_raddr), .rdata(ib_rdata));

// rx_obuf: 書込=DECODE(STORE) / 読出=OUTPUT
wire              ob_we;
wire [OB_AW-1:0]  ob_waddr;
wire [7:0]        ob_wdata;
wire [OB_AW-1:0]  ob_raddr;
wire [7:0]        ob_rdata;
rs_sdp_ram #(.DW(8), .DEPTH(OB_DEPTH), .AW(OB_AW)) u_rx_obuf (
    .clk(clk), .we(ob_we), .waddr(ob_waddr), .wdata(ob_wdata),
    .raddr(ob_raddr), .rdata(ob_rdata));

reg       rx_unc  [0:RX_SLOTS-1][0:INTERLEAVE-1];  // 3*32=96bit、FFのまま

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

// rx_ibuf 書込ポート(COLLECTから駆動、SDP-RAMが同期ラッチ)。
// 元の rx_ibuf[...] <= rx_in_data と同一クロックで書けるよう組合せ駆動。
assign ib_we    = rx_accept_sof || rx_accept_cont;
assign ib_waddr = rx_accept_sof
                ? (collect_slot*INTERLEAVE)*N
                : (collect_slot*INTERLEAVE + (rx_in_cnt % INTERLEAVE))*N + (rx_in_cnt / INTERLEAVE);
assign ib_wdata = rx_in_data;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        collect_slot   <= 0;
        rx_in_cnt      <= 0;
        rx_timeout_cnt <= 0;
    end else if (rx_accept_sof) begin
        rx_in_cnt      <= 1;
        rx_timeout_cnt <= 0;
    end else if (rx_accept_cont) begin
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
localparam DEC_IDLE  = 3'd0,
           DEC_COPY  = 3'd1,
           DEC_PULSE = 3'd2,
           DEC_WAIT  = 3'd3,
           DEC_STORE = 3'd4;

reg [2:0]  dec_state;
reg [7:0]  decode_slot;
reg [7:0]  dec_d;
reg [8:0]  dec_c;        // 直列コピー用バイト索引 0..N-1 (BRAM 1byte/clk)
reg        dec_unc_lat;  // STORE中に保持する uncorrectable ラッチ

wire decode_claim_pulse = (dec_state == DEC_IDLE) && (slot_state[decode_slot] == SLOT_FILLED);
// スロット完了(DECODED)は最終コードワードの直列STORE完了時に立てる。
wire decode_done_pulse  = (dec_state == DEC_STORE) && (dec_c == K - 1)
                          && (dec_d == INTERLEAVE - 1);

// rx_ibuf 読出ポート(DECODE): dec_c のアドレスを提示、ib_rdata は次clkに出る。
// dec_c==N の余分読みは範囲外にならないよう N-1 にクランプ(データは未使用)。
assign ib_raddr = (decode_slot*INTERLEAVE + dec_d)*N + ((dec_c >= N) ? (N-1) : dec_c);
// rx_obuf 書込ポート(DECODE STORE): dec_cw_out を1byte/clkで書込む。
assign ob_we    = (dec_state == DEC_STORE);
assign ob_waddr = (decode_slot*INTERLEAVE + dec_d)*K + dec_c;
`ifdef RX_BYPASS_DECODE
assign ob_wdata = dec_cw_in[8*dec_c +: 8];   // 診断: 収集した生符号語データ部を素通し
`else
assign ob_wdata = dec_cw_out[8*dec_c +: 8];
`endif

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        dec_state   <= DEC_IDLE;
        decode_slot <= 0;
        dec_d       <= 0;
        dec_c       <= 0;
        dec_unc_lat <= 0;
        dec_load_cw <= 0;
        dec_cw_in <= {(8*N){1'b0}};
        // rx_unc のリセットは DECODE(唯一の機能書込元)に一本化(multi-driver解消)
        for (si = 0; si < RX_SLOTS; si = si + 1)
            for (ri = 0; ri < INTERLEAVE; ri = ri + 1) rx_unc[si][ri] <= 0;
    end else begin
        case (dec_state)
        DEC_IDLE: begin
            dec_load_cw <= 0;
            if (decode_claim_pulse) begin
                dec_d     <= 0;
                dec_c     <= 0;
                dec_state <= DEC_COPY;
            end
        end
        // rx_ibuf(BRAM)から1コードワードを1byte/clkでdec_cw_inへ直列コピー。
        // dec_cw_in[c]は<=でレジスタ捕捉。dec_c=N-1でPULSEへ抜ける頃には
        // 全バイト有効(元の1clk全並列コピーと同じデータが揃う)。
        // rx_ibuf(SDP-RAM)から1コードワードを1byte/clkでdec_cw_inへ直列コピー。
        // ib_raddr=idx(dec_c)を提示し、ib_rdata(=mem[idx(dec_c-1)], 1clk遅延)を
        // dec_cw_in[dec_c-1]へ捕捉。dec_c=0..N の N+1clk で全N byte収集。
        DEC_COPY: begin
            dec_load_cw <= 0;
            if (dec_c != 0) dec_cw_in[8*(dec_c-1) +: 8] <= ib_rdata;
            if (dec_c == N) begin
                dec_c     <= 0;
                dec_state <= DEC_PULSE;
            end else begin
                dec_c <= dec_c + 1;
            end
        end
        DEC_PULSE: begin
            dec_load_cw <= 1;
            dec_state   <= DEC_WAIT;
        end
        DEC_WAIT: begin
            dec_load_cw <= 0;
            if (dec_done) begin
                dec_unc_lat <= dec_uncorrectable;  // STORE中保持
                dec_c       <= 0;
                dec_state   <= DEC_STORE;
            end
        end
        // 復号結果dec_cw_out(復号完了後は安定保持)をrx_obuf(SDP-RAM)へ
        // 1byte/clkで直列書込み(ob_we/ob_waddr/ob_wdata経由)。
        // K byte書き終えたら次コードワード/次スロットへ。
        DEC_STORE: begin
            if (dec_c == K - 1) begin
                rx_unc[decode_slot][dec_d] <= dec_unc_lat;
                dec_c <= 0;
                if (dec_d == INTERLEAVE - 1) begin
                    dec_d       <= 0;
                    decode_slot <= (decode_slot == RX_SLOTS-1) ? 8'd0 : decode_slot + 8'd1;
                    dec_state   <= DEC_IDLE;
                end else begin
                    dec_d     <= dec_d + 1;
                    dec_state <= DEC_COPY;
                end
            end else begin
                dec_c <= dec_c + 1;
            end
        end
        endcase
    end
end

// ---- OUTPUT: 先読みFIFO付き rx_obuf 読み出し → valid/ready ストリーム ----
// rx_obuf は SDP-RAM(同期読み1clkレイテンシ)。元設計の連続ハンドシェイク
// 意味論(readyの各エッジで data をロードし、TBはエッジ後(#1)に valid&&ready
// で捕捉する)を維持するため、深さ4の先読みFIFOでBRAMの1clk遅延を吸収する。
// スロットを掴んだら o_fetch を進めて連続読みし、結果をFIFOへ push。
// 消費側は元と同一: rx_out_ready かつ FIFO非空のとき head をロードし pop。
reg [7:0]  output_slot;
reg        o_active;
reg [15:0] o_fetch;     // BRAM読み発行索引 (0..DK)
reg [15:0] o_pop;       // pf→出力レジスタへ移したバイト数 (0..DK)
reg [7:0]  pf_d [0:3];  // 先読みFIFO データ
reg        pf_u [0:3];  // 先読みFIFO uncorrectable
reg [1:0]  pf_wr, pf_rd;
reg [2:0]  pf_cnt;      // 占有 0..4
reg        pf_infl;     // 前clkで読み発行済み(今clk ob_rdata 到着=push)
reg        pf_infl_u;

assign ob_raddr = (output_slot*INTERLEAVE + (o_fetch % INTERLEAVE))*K + (o_fetch / INTERLEAVE);

wire       pf_empty = (pf_cnt == 0);
wire       can_issue = o_active && (o_fetch < DK) && (pf_cnt < 3'd3);
// AXIS準拠hold: 出力レジスタが空く(!valid) か 今拍で受理される(ready) 時だけ
// pf から次バイトを取り出す。ready が落ちている間は data/valid/tlast を保持する。
wire       reg_free = (!rx_out_valid) || rx_out_ready;
wire       do_pop   = o_active && reg_free && !pf_empty;
wire       out_fire = rx_out_valid && rx_out_ready;   // 出力ビート受理

wire output_claim_pulse = !o_active && (slot_state[output_slot] == SLOT_DECODED);
// ブロック完了 = 最終バイト(tlast付き)が受理された瞬間
wire output_done_pulse  = out_fire && rx_block_done;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        output_slot          <= 0;
        o_active             <= 0;
        o_fetch              <= 0;
        o_pop                <= 0;
        pf_wr <= 0; pf_rd <= 0; pf_cnt <= 0; pf_infl <= 0; pf_infl_u <= 0;
        rx_out_valid         <= 0;
        rx_out_data          <= 0;
        rx_out_uncorrectable <= 0;
        rx_block_done        <= 0;
    end else begin
        if (!o_active) begin
            rx_out_valid  <= 0;
            rx_block_done <= 0;
            if (output_claim_pulse) begin
                o_active <= 1; o_fetch <= 0; o_pop <= 0;
                pf_wr <= 0; pf_rd <= 0; pf_cnt <= 0; pf_infl <= 0;
            end
        end else begin
            // ---- FIFO push: 前clk発行した読みの結果 ob_rdata が到着 ----
            if (pf_infl) begin
                pf_d[pf_wr] <= ob_rdata;
                pf_u[pf_wr] <= pf_infl_u;
                pf_wr       <= pf_wr + 1;
            end
            // ---- 次の読みを発行 ----
            if (can_issue) begin
                pf_infl   <= 1;
                pf_infl_u <= rx_unc[output_slot][o_fetch % INTERLEAVE];
                o_fetch   <= o_fetch + 1;
            end else begin
                pf_infl   <= 0;
            end
            // ---- 出力レジスタ: 空きor受理時のみロード、readyが低い間は保持 ----
            if (reg_free) begin
                if (!pf_empty) begin
                    rx_out_data          <= pf_d[pf_rd];
                    rx_out_uncorrectable <= pf_u[pf_rd];
                    rx_out_valid         <= 1'b1;
                    rx_block_done        <= (o_pop == DK - 1); // 最終バイトにtlast
                    pf_rd                <= pf_rd + 1;
                    o_pop                <= o_pop + 1;
                end else begin
                    rx_out_valid  <= 1'b0;
                    rx_block_done <= 1'b0;
                end
            end
            // ---- 最終バイト(tlast)受理でブロック完了・次スロットへ ----
            if (out_fire && rx_block_done) begin
                o_active    <= 1'b0;
                output_slot <= (output_slot == RX_SLOTS-1) ? 8'd0 : output_slot + 8'd1;
            end
            // ---- FIFO 占有更新(push +1, pop -1) ----
            pf_cnt <= pf_cnt + (pf_infl ? 3'd1 : 3'd0) - (do_pop ? 3'd1 : 3'd0);
        end
    end
end

// ---- Slot state manager: sole writer of slot_state[] ----
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        for (si = 0; si < RX_SLOTS; si = si + 1) slot_state[si] <= SLOT_FREE;
        // rx_unc のリセットは DECODE 側に移動(multi-driver 解消: Synth 8-6859)
    end else begin
        if (collect_fill_done)   slot_state[collect_slot] <= SLOT_FILLED;
        if (decode_claim_pulse)  slot_state[decode_slot]  <= SLOT_DECODING;
        if (decode_done_pulse)   slot_state[decode_slot]  <= SLOT_DECODED;
        if (output_claim_pulse)  slot_state[output_slot]  <= SLOT_OUTPUTTING;
        if (output_done_pulse)   slot_state[output_slot]  <= SLOT_FREE;
    end
end

endmodule
