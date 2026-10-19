// 単純デュアルポートRAM (1書込ポート + 1読出ポート、同期読み出し1clkレイテンシ)
// XC7Z007SのBlock RAMに確実にマップさせるための明示モジュール。
// rx_ibuf/rx_obuf のように「書込FSMと読出FSMが別always」の場合、
// 直書き2次元配列ではVivadoがBRAM推論しない(Synth 8-3391)。本モジュールで
// 書込/読出を単一alwaysの標準テンプレートにまとめBRAM化する。
//
// レイテンシ: raddr を与えた次クロックで rdata に mem[raddr] が出る。
// 読み書き同一アドレス同時アクセスの振る舞いは "read-first"(旧データ)だが、
// 本設計では書込ポート(COLLECT/DECODE)と読出ポート(DECODE/OUTPUT)は
// 常に別スロットを触るため衝突しない。
module rs_sdp_ram #(
    parameter DW    = 8,
    parameter DEPTH = 24480,
    parameter AW    = 15      // >= clog2(DEPTH)
)(
    input               clk,
    input               we,
    input  [AW-1:0]     waddr,
    input  [DW-1:0]     wdata,
    input  [AW-1:0]     raddr,
    output reg [DW-1:0] rdata
);
    (* ram_style = "block" *) reg [DW-1:0] mem [0:DEPTH-1];
    always @(posedge clk) begin
        if (we) mem[waddr] <= wdata;
        rdata <= mem[raddr];
    end
endmodule
