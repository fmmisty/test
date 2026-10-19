// rs_fec_selftest_top : Cora Z7-07S 1台完結の自己テスト
// (合成対応 optimized 版コア用: tx_core に tx_out_sof が無いので、
//  送出バイトを DN 周期でカウントして sof を外部生成する)
// -------------------------------------------------------------
//   pattern gen -> tx_core(符号化+インターリーブ)
//        -> 誤り注入(バースト) -> rx_core(復号) -> 照合 -> LED
//
//   led_pass : 1ブロック以上を誤りゼロで復元できた(=訂正成功) → 点灯
//   led_err  : 照合ミス or 訂正不能を検出 → 点灯(訂正能力超え時のみ)
//   led_hb   : 生存ハートビート点滅
//
// clk は実機では PS の FCLK_CLK0 を供給する。単一クロックで tx_out を
// rx_in へ直結するため CDC 不要・FIFO 不要。
module rs_fec_selftest_top #(
    parameter N          = 255,
    parameter K          = 239,
    parameter PARITY     = 16,
    parameter T          = 8,
    parameter INTERLEAVE = 32,
    parameter HEARTBEAT_BITS = 24,
    parameter ERR_INJECT    = 1,
    parameter ERR_START     = 1000,
    parameter ERR_BURST_LEN = 200
)(
    input  clk,
    input  rst_n,
    output led_pass,
    output led_err,
    output led_hb
);
    localparam integer DN = INTERLEAVE * N;   // 8160

    // ------------------------------------------------------------
    // TX: テストパターン(0,1,2,...)を符号化+インターリーブ
    // ------------------------------------------------------------
    wire       tx_in_ready;
    // リセット直後の数サイクルは投入を止める(境界での先頭バイト誤ラッチ回避)
    reg  [2:0] start_dly;
    always @(posedge clk) begin
        if (!rst_n)                 start_dly <= 3'd0;
        else if (start_dly != 3'd7) start_dly <= start_dly + 3'd1;
    end
    wire       tx_in_valid = (start_dly == 3'd7);
    reg  [7:0] pat;
    always @(posedge clk) begin
        if (!rst_n)                         pat <= 8'd0;
        else if (tx_in_valid & tx_in_ready) pat <= pat + 8'd1;
    end

    wire       enc_valid;
    wire [7:0] enc_data;
    wire       rx_in_ready;      // rx のバックプレッシャ = tx_out_ready

    rs_fec_tx_core #(
        .N(N), .K(K), .PARITY(PARITY), .INTERLEAVE(INTERLEAVE)
    ) u_tx (
        .clk(clk), .rst_n(rst_n),
        .tx_in_valid(tx_in_valid), .tx_in_data(pat), .tx_in_ready(tx_in_ready),
        .tx_out_valid(enc_valid), .tx_out_data(enc_data),
        .tx_out_ready(rx_in_ready)
    );

    // ------------------------------------------------------------
    // sof 生成: 送出バイトを DN 周期でカウントし、先頭で sof
    // ------------------------------------------------------------
    reg  [15:0] wire_count;
    wire        is_xfer = enc_valid & rx_in_ready;
    always @(posedge clk) begin
        if (!rst_n)              wire_count <= 16'd0;
        else if (is_xfer)        wire_count <= (wire_count == DN-1) ? 16'd0 : wire_count + 16'd1;
    end
    wire enc_sof = enc_valid && (wire_count == 16'd0);

    // ------------------------------------------------------------
    // 誤り注入: ブロック内の連続バイトに XOR 0xFF を重畳
    // ------------------------------------------------------------
    wire in_win = (wire_count >= ERR_START) && (wire_count < (ERR_START + ERR_BURST_LEN));
    wire [7:0] chan_data = (ERR_INJECT && in_win) ? (enc_data ^ 8'hFF) : enc_data;

    // ------------------------------------------------------------
    // RX: デインターリーブ+復号
    // ------------------------------------------------------------
    wire       rx_out_valid, rx_out_uncorrectable, rx_block_done;
    wire [7:0] rx_out_data;

    rs_fec_rx_core #(
        .N(N), .K(K), .PARITY(PARITY), .T(T), .INTERLEAVE(INTERLEAVE)
    ) u_rx (
        .clk(clk), .rst_n(rst_n),
        .rx_in_valid(enc_valid), .rx_sof(enc_sof), .rx_in_data(chan_data),
        .rx_in_ready(rx_in_ready),
        .rx_out_valid(rx_out_valid), .rx_out_data(rx_out_data),
        .rx_out_ready(1'b1),
        .rx_out_uncorrectable(rx_out_uncorrectable),
        .rx_block_done(rx_block_done)
    );

    // ------------------------------------------------------------
    // 照合: 復号出力が元パターン(0,1,2,...)と一致するか
    // ------------------------------------------------------------
    reg [7:0] exp;
    reg       err_latch;
    reg       good_block;
    always @(posedge clk) begin
        if (!rst_n) begin
            exp <= 8'd0; err_latch <= 1'b0; good_block <= 1'b0;
        end else begin
            if (rx_out_valid) begin
                if (rx_out_data != exp)   err_latch  <= 1'b1;
                if (rx_out_uncorrectable) err_latch  <= 1'b1;
                exp <= exp + 8'd1;
            end
            if (rx_block_done && !err_latch) good_block <= 1'b1;
        end
    end

    assign led_pass = good_block & ~err_latch;
    assign led_err  = err_latch;

    // ------------------------------------------------------------
    // ハートビート
    // ------------------------------------------------------------
    reg [HEARTBEAT_BITS-1:0] hb;
    always @(posedge clk) begin
        if (!rst_n) hb <= 0;
        else        hb <= hb + 1'b1;
    end
    assign led_hb = hb[HEARTBEAT_BITS-1];
endmodule
