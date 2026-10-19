// Berlekamp-Massey Algorithm Module - Parameterized
// GF(2^8), default: PARITY=16 => up to 8 errors correctable
//
// Parameters:
//   PARITY : number of parity bytes = 2t (default 16, t=8)
//   T      : error correction capability = PARITY/2 (default 8)

module bm_module #(
    parameter PARITY = 16,
    parameter T      = 8    // PARITY/2
)(
    input        clk,
    input        rst_n,
    input        load_synd,
    input  [7:0] synd_in [0:PARITY-1],
    output reg        bm_done,
    output reg [7:0]  sigma_out [0:T],
    output reg [3:0]  L_out,
    output reg        uncorrectable
);

reg [7:0] exp_rom [0:511];
reg [7:0] log_rom [0:255];
integer idx;
initial begin
    exp_rom[0] = 8'h01;
    for (idx = 1; idx < 255; idx = idx + 1) begin
        if (exp_rom[idx-1][7])
            exp_rom[idx] = {exp_rom[idx-1][6:0], 1'b0} ^ 8'h1D;
        else
            exp_rom[idx] = {exp_rom[idx-1][6:0], 1'b0};
    end
    for (idx = 255; idx < 512; idx = idx + 1)
        exp_rom[idx] = exp_rom[idx-255];
    log_rom[0] = 8'hFF;
    for (idx = 0; idx < 255; idx = idx + 1)
        log_rom[exp_rom[idx]] = idx;
end

function [7:0] gf_mul_fn;
    input [7:0] x, y;
    reg [8:0] lx, ly, ls;
    begin
        if (x == 0 || y == 0) gf_mul_fn = 8'h00;
        else begin
            lx = {1'b0, log_rom[x]}; ly = {1'b0, log_rom[y]};
            ls = lx + ly;
            if (ls >= 255) ls = ls - 255;
            gf_mul_fn = exp_rom[ls[7:0]];
        end
    end
endfunction

function [7:0] gf_div_fn;
    input [7:0] x, y;
    reg [8:0] lx, ly, ls;
    begin
        if (x == 0) gf_div_fn = 8'h00;
        else begin
            lx = {1'b0, log_rom[x]}; ly = {1'b0, log_rom[y]};
            ls = (lx >= ly) ? lx - ly : lx + 9'd255 - ly;
            gf_div_fn = exp_rom[ls[7:0]];
        end
    end
endfunction

reg [7:0] S [0:PARITY-1];
reg [7:0] C [0:PARITY];
reg [7:0] B [0:PARITY];
reg [7:0] b;
reg [4:0] L, m, n;

localparam ST_IDLE = 2'd0;
localparam ST_RUN  = 2'd1;
localparam ST_DONE = 2'd2;
reg [1:0] state;

integer i;

// 同期リセット化(2026-09-12): 非同期リセット下で S/C/B 等が Synth 8-7137
// (set/reset同優先度=sim/実機不一致)になるため、同期リセットに統一。
always @(posedge clk) begin
    if (!rst_n) begin
        state <= ST_IDLE; bm_done <= 0; uncorrectable <= 0;
        L <= 0;
        for (i = 0; i <= T; i = i + 1) sigma_out[i] <= 0;
        L_out <= 0;
    end else begin
        bm_done <= 0;
        case (state)
        ST_IDLE: begin
            if (load_synd) begin
                for (i = 0; i < PARITY; i = i + 1) S[i] <= synd_in[i];
                for (i = 0; i <= PARITY; i = i + 1) C[i] <= 8'h00;
                for (i = 0; i <= PARITY; i = i + 1) B[i] <= 8'h00;
                C[0] <= 8'h01; B[0] <= 8'h01;
                b <= 8'h01; L <= 0; m <= 1; n <= 0;
                state <= ST_RUN;
            end
        end
        ST_RUN: begin
            begin : bm_iter
                reg [7:0] d, db;
                reg [7:0] newC [0:PARITY];
                reg [7:0] Ttmp [0:PARITY];
                integer k;
                // 合成対応: 全テンポラリを毎サイクル既定値で駆動し、
                // 未駆動ネット(Synth 8-3848 p_0_out)/推論ラッチを防ぐ(挙動不変)。
                db = 8'h00;
                for (k = 0; k <= PARITY; k = k + 1) begin
                    newC[k] = 8'h00;
                    Ttmp[k] = 8'h00;
                end
                d = S[n];
                // 合成対応: 可変境界Lを静的境界PARITY+ガードに(挙動不変)
                for (k = 1; k <= PARITY; k = k + 1)
                    if (k <= L)
                        d = d ^ gf_mul_fn(C[k], S[n-k]);
                if (d == 0) begin
                    m <= m + 1;
                end else if (2*L <= n) begin
                    db = gf_div_fn(d, b);
                    for (k = 0; k <= PARITY; k = k + 1) Ttmp[k] = C[k];
                    for (k = 0; k <= PARITY; k = k + 1) newC[k] = C[k];
                    // 合成対応(2026-09-02): 可変開始ループはVivadoが静的展開
                    // 不可(loop does not converge)。静的境界+if(k>=m)ガードに
                    // 書換え(機能は元と完全一致)。
                    for (k = 0; k <= PARITY; k = k + 1)
                        if (k >= m)
                            newC[k] = newC[k] ^ gf_mul_fn(db, B[k-m]);
                    for (k = 0; k <= PARITY; k = k + 1) C[k] <= newC[k];
                    for (k = 0; k <= PARITY; k = k + 1) B[k] <= Ttmp[k];
                    L <= n + 1 - L; b <= d; m <= 1;
                end else begin
                    db = gf_div_fn(d, b);
                    for (k = 0; k <= PARITY; k = k + 1) newC[k] = C[k];
                    // 合成対応(2026-09-02): 可変開始ループはVivadoが静的展開
                    // 不可(loop does not converge)。静的境界+if(k>=m)ガードに
                    // 書換え(機能は元と完全一致)。
                    for (k = 0; k <= PARITY; k = k + 1)
                        if (k >= m)
                            newC[k] = newC[k] ^ gf_mul_fn(db, B[k-m]);
                    for (k = 0; k <= PARITY; k = k + 1) C[k] <= newC[k];
                    m <= m + 1;
                end
                n <= n + 1;
            end
            if (n == PARITY-1) state <= ST_DONE;
        end
        ST_DONE: begin
            for (i = 0; i <= T; i = i + 1) sigma_out[i] <= C[i];
            L_out         <= L[3:0];
            uncorrectable <= (L > T) ? 1'b1 : 1'b0;
            bm_done       <= 1;
            state         <= ST_IDLE;
        end
        endcase
    end
end

endmodule
