// cora_selftest_top : Cora Z7-07S 実機トップ
// -------------------------------------------------------------
// 基板搭載の 125MHz(H16)を Clocking Wizard で 40MHz に落とし、
// rs_fec_selftest_top(RS+インターリーブ自己テスト)を駆動する。
// コアの Fmax≒68MHz なので 40MHz は十分マージンあり。
//
// LED(RGB) 割当:
//   led0_g (緑) = led_pass : 誤りを注入しても訂正して復元できた → 点灯
//   led0_r (赤) = led_err  : 訂正不能を検出したとき点灯(正常時は消灯)
//   led1_b (青) = led_hb   : 生存ハートビート点滅
//
// btn[0] = リセット(押している間リセット)。btn は Cora ではアクティブHigh。
module cora_selftest_top (
    input  wire clk_125,     // H16 125MHz
    input  wire [1:0] btn,   // D20,D19
    // LD0
    output wire led0_r,
    output wire led0_g,
    output wire led0_b,
    // LD1
    output wire led1_r,
    output wire led1_g,
    output wire led1_b
);
    // ---- クロック生成 125MHz -> 40MHz ----
    wire clk_core;
    wire locked;
    clk_wiz_0 u_clk (
        .clk_in1 (clk_125),
        .clk_out1(clk_core),
        .locked  (locked)
    );

    // ---- リセット生成 ----
    // 実機では clk_wiz がロックするまで clk_core が出ない。単純な
    // rst_n=locked&~btn だと「クロックが動き出した時にはもう rst_n=1」で
    // 同期リセットが一度もかからず、FFが不定状態で起動する(赤LED誤点灯)。
    // そこで locked を非同期リセットにして、ロック後 clk_core で数百サイクル
    // リセットを保持してから解除する(async-assert / sync-deassert)。
    reg [7:0] rst_cnt;
    always @(posedge clk_core or negedge locked) begin
        if (!locked)               rst_cnt <= 8'd0;
        else if (rst_cnt != 8'hFF) rst_cnt <= rst_cnt + 8'd1;
    end
    wire rst_n = (rst_cnt == 8'hFF) & ~btn[0];

    // ---- 自己テスト本体(誤り注入ON: 200byteバーストを訂正する様子を緑LEDで示す) ----
    wire led_pass, led_err, led_hb;
    rs_fec_selftest_top #(
        .HEARTBEAT_BITS (24),     // 40MHzで約0.4s→~1.2Hz点滅
        .ERR_INJECT     (1),
        .ERR_START      (1000),
        .ERR_BURST_LEN  (200)     // <=256 なので訂正可能。緑点灯=訂正成功
    ) u_selftest (
        .clk     (clk_core),
        .rst_n   (rst_n),
        .led_pass(led_pass),
        .led_err (led_err),
        .led_hb  (led_hb)
    );

    // ---- LED 出力(未使用色は消灯) ----
    assign led0_g = led_pass;   // 緑 = 訂正成功
    assign led0_r = led_err;    // 赤 = 訂正不能検出
    assign led0_b = 1'b0;
    assign led1_b = led_hb;     // 青 = 生存点滅
    assign led1_r = 1'b0;
    assign led1_g = 1'b0;
endmodule
