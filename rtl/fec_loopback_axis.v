// fec_loopback_axis : RS-FEC(tx_core->rx_core) を AXI4-Stream で PS に橋渡し
// -------------------------------------------------------------
//  PS --[AXI Stream FIFO TXD]--> s_axis --> tx_core(符号化)
//        -> (sof外部生成) -> rx_core(復号+訂正) -> m_axis
//        --[AXI Stream FIFO RXD]--> PS で読み戻して照合
//
//  8bitストリーム(tdata[7:0])。単一クロック aclk(=FCLK_CLK0)。
//  Stage3aの1台ループバック検証用。TX/RX機に分ける時はこの中の
//  tx_core側 / rx_core側だけを使う版に展開する。
module fec_loopback_axis #(
    parameter N          = 255,
    parameter K          = 239,
    parameter PARITY     = 16,
    parameter T          = 8,
    parameter INTERLEAVE = 32
)(
    input        aclk,
    input        aresetn,
    // slave stream: PS -> FEC入力
    input  [7:0] s_axis_tdata,
    input        s_axis_tvalid,
    output       s_axis_tready,
    // master stream: FEC出力 -> PS
    output [7:0] m_axis_tdata,
    output       m_axis_tvalid,
    input        m_axis_tready,
    output       m_axis_tlast
);
    localparam integer DN = INTERLEAVE * N;

    wire rst_n = aresetn;

    wire       tx_in_ready;
    wire       enc_valid;
    wire [7:0] enc_data;
    wire       rx_in_ready;

    rs_fec_tx_core #(.N(N), .K(K), .PARITY(PARITY), .INTERLEAVE(INTERLEAVE)) u_tx (
        .clk(aclk), .rst_n(rst_n),
        .tx_in_valid(s_axis_tvalid), .tx_in_data(s_axis_tdata), .tx_in_ready(tx_in_ready),
        .tx_out_valid(enc_valid), .tx_out_data(enc_data), .tx_out_ready(rx_in_ready)
    );
    assign s_axis_tready = tx_in_ready;

    // sof生成: 送出バイトをDN周期でカウント
    reg [15:0] wire_count;
    wire is_xfer = enc_valid & rx_in_ready;
    always @(posedge aclk) begin
        if (!rst_n)       wire_count <= 16'd0;
        else if (is_xfer) wire_count <= (wire_count == DN-1) ? 16'd0 : wire_count + 16'd1;
    end
    wire enc_sof = enc_valid && (wire_count == 16'd0);

    wire       rx_out_valid, rx_out_unc, rx_block_done;
    wire [7:0] rx_out_data;

    rs_fec_rx_core #(.N(N), .K(K), .PARITY(PARITY), .T(T), .INTERLEAVE(INTERLEAVE), .RX_SLOTS(1)) u_rx (
        .clk(aclk), .rst_n(rst_n),
        .rx_in_valid(enc_valid), .rx_sof(enc_sof), .rx_in_data(enc_data), .rx_in_ready(rx_in_ready),
        .rx_out_valid(rx_out_valid), .rx_out_data(rx_out_data), .rx_out_ready(m_axis_tready),
        .rx_out_uncorrectable(rx_out_unc), .rx_block_done(rx_block_done)
    );

    assign m_axis_tdata  = rx_out_data;
    assign m_axis_tvalid = rx_out_valid;
    assign m_axis_tlast  = rx_block_done;   // ブロック末尾で tlast
endmodule
