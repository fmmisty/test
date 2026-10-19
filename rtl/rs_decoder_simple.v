// RS(N,K) Decoder - Parameterized
// GF(2^8), primitive polynomial 0x11D
// Default: RS(255,239), PARITY=16, T=8
//
// Parameters:
//   N      : codeword length (default 255)
//   K      : data length (default 239)
//   PARITY : N-K (default 16)
//   T      : PARITY/2, max correctable errors (default 8)

module rs_decoder_simple #(
    parameter N      = 255,
    parameter K      = 239,
    parameter PARITY = 16,
    parameter T      = 8
)(
    input        clk,
    input        rst_n,
    input        load_cw,
    input  [8*N-1:0] cw_in,          // 平坦化: cw_in[8*i +: 8] = byte i (配列ポート合成回避)
    output reg        dec_done,
    output reg [8*N-1:0] cw_out,      // 平坦化: cw_out[8*i +: 8] = byte i
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

// 最適化(2026-09-02): ROM方式(log/exp)から直接XOR方式(シフトXOR)へ。
// 小さいXOR木で実装され、定数倍(α^r倍等)は合成時に固定XOR網へ最適化される。
// GF(2^8), 0x11D(還元0x1D)。挙動は元と完全一致(同じGF積)。
function [7:0] gf_mul_fn;
    input [7:0] x, y;
    reg [7:0] a, b, p;
    integer n;
    begin
        a = x; b = y; p = 8'h00;
        for (n = 0; n < 8; n = n + 1) begin
            if (b[0]) p = p ^ a;
            b = b >> 1;
            if (a[7]) a = (a << 1) ^ 8'h1D;
            else      a = (a << 1);
        end
        gf_mul_fn = p;
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

reg [7:0] recv [0:N-1];
reg [7:0] synd [0:PARITY-1];
reg [7:0] sigma [0:T];
reg [3:0] L_val;
reg [7:0] errloc [0:T-1];
reg [7:0] errval [0:T-1];
reg [3:0] nerr;

// BM module
reg        bm_load;
wire       bm_done;
wire [7:0] bm_sigma [0:T];
wire [3:0] bm_L;
wire       bm_unc;

bm_module #(.PARITY(PARITY), .T(T)) u_bm (
    .clk         (clk),
    .rst_n       (rst_n),
    .load_synd   (bm_load),
    .synd_in     (synd),
    .bm_done     (bm_done),
    .sigma_out   (bm_sigma),
    .L_out       (bm_L),
    .uncorrectable(bm_unc)
);

localparam ST_IDLE   = 3'd0;
localparam ST_SYND   = 3'd1;
localparam ST_BM     = 3'd2;
localparam ST_CHIEN  = 3'd3;
localparam ST_FORNEY = 3'd4;
localparam ST_CORRECT= 3'd5;
localparam ST_DONE   = 3'd6;

reg [2:0] state;
reg [7:0] iter;
reg bm_done_latch;

integer i, r, k;

// ===== 逐次Forney(2026-09-02 最適化) =====
// 旧ST_FORNEYは omega計算+最大8誤り分の num/den/除算を1クロックで全展開し、
// GF乗算器を約440個並列に生成(デコーダLUT=39k・最悪パス165段)。
// gf_mul/gf_divを時分割共有する逐次FSMに変更しLUT/タイミングを大幅削減する。
// 予算は潤沢(Chienだけで255clk)なので数百clk増でも8Mbpsに間に合う。
localparam F_OMEGA = 3'd0,
           F_ESET  = 3'd1,
           F_NUM   = 3'd2,
           F_DEN   = 3'd3,
           F_DIV   = 3'd4,
           F_DONE  = 3'd5;
reg [2:0] f_state;
reg [4:0] f_jj;         // omega外側 0..PARITY
reg [3:0] f_kk;         // 内側カウンタ
reg [3:0] f_ei;         // 誤り索引 0..T
reg [7:0] f_acc;        // omega累算器
reg [7:0] f_xpow;
reg [7:0] f_xk, f_xkinv, f_xkinv2;
reg [7:0] f_num, f_den;
reg [7:0] omega [0:PARITY-1];

// Chien内のexp_t参照用 (combinational)
// exp_rom と同一内容、Chienブロック内で使用
wire [7:0] exp_t [0:511];
genvar gi;
generate
    for (gi = 0; gi < 512; gi = gi + 1)
        assign exp_t[gi] = exp_rom[gi];
endgenerate

// 同期リセット化(2026-09-12): 非同期リセット下では recv 等が Synth 8-7137
// 「set/reset 同優先度=sim/実機不一致」となり、実機で recv が0に張り付いて
// 復号出力が全0化していた。tx_core と同じ同期リセットに統一して解消する。
always @(posedge clk) begin
    if (!rst_n) begin
        state <= ST_IDLE; dec_done <= 0; uncorrectable <= 0;
        bm_load <= 0; bm_done_latch <= 0;
        iter <= 0; nerr <= 0;
        f_state <= F_OMEGA; f_jj <= 0; f_kk <= 0; f_ei <= 0; f_acc <= 0;
        f_xpow <= 0; f_xk <= 0; f_xkinv <= 0; f_xkinv2 <= 0; f_num <= 0; f_den <= 0;
        cw_out <= {(8*N){1'b0}};
    end else begin
        dec_done <= 0; bm_load <= 0;
        if (bm_done) bm_done_latch <= 1;

        case (state)
        ST_IDLE: begin
            if (load_cw) begin
                for (i = 0; i < N; i = i + 1) recv[i] <= cw_in[8*i +: 8];
                for (i = 0; i < PARITY; i = i + 1) synd[i] <= 8'h00;
                nerr <= 0; bm_done_latch <= 0; iter <= 0;
                state <= ST_SYND;
            end
        end
        ST_SYND: begin
            begin : synd_compute
                reg [7:0] s_new [0:PARITY-1];
                for (r = 0; r < PARITY; r = r + 1)
                    s_new[r] = gf_mul_fn(synd[r], exp_rom[r]) ^ recv[iter];
                for (r = 0; r < PARITY; r = r + 1) synd[r] <= s_new[r];
            end
            iter <= iter + 1;
            if (iter == N-1) begin
                iter <= 0; state <= ST_BM; bm_load <= 1;
            end
        end
        ST_BM: begin
            if (bm_done_latch) begin
                bm_done_latch <= 0;
                for (i = 0; i <= T; i = i + 1) sigma[i] <= bm_sigma[i];
                L_val <= bm_L;
                uncorrectable <= bm_unc;
                if (bm_unc) begin
                    for (i = 0; i < N; i = i + 1) cw_out[8*i +: 8] <= recv[i];
                    state <= ST_DONE;
                end else begin
                    iter <= 1; nerr <= 0; state <= ST_CHIEN;
                end
            end
        end
        ST_CHIEN: begin
            begin : chien_eval
                reg [7:0] val, xinvj, xpow;
                integer kk;
                xinvj = exp_t[(iter - 1) % 255];  // alpha^{j-1}
                val = sigma[0]; xpow = xinvj;
                for (kk = 1; kk <= T; kk = kk + 1) begin
                    val  = val ^ gf_mul_fn(sigma[kk], xpow);
                    xpow = gf_mul_fn(xpow, xinvj);
                end
                if (val == 0 && nerr < L_val) begin
                    // position p = 254 - (256-j)%255
                    errloc[nerr] <= (254 - ((256 - iter) % 255));
                    nerr <= nerr + 1;
                end
            end
            iter <= iter + 1;
            if (iter == 255) begin
                iter <= 0; state <= ST_FORNEY;
                f_state <= F_OMEGA; f_jj <= 0; f_kk <= 0; f_acc <= 8'h00;
            end
        end
        // 逐次Forney: gf_mul/gf_div を時分割共有(旧版は1clk全展開でLUT39k)
        ST_FORNEY: begin
            case (f_state)
            // omega[jj] = Σ_{kk=0..min(T,jj)} sigma[kk]*synd[jj-kk] を1乗算/clkで累算
            F_OMEGA: begin
                if (f_kk <= f_jj && f_kk <= T) begin
                    f_acc <= f_acc ^ gf_mul_fn(sigma[f_kk], synd[f_jj - f_kk]);
                    f_kk  <= f_kk + 1;
                end else begin
                    omega[f_jj] <= f_acc;
                    f_acc <= 8'h00;
                    f_kk  <= 0;
                    if (f_jj == PARITY-1) begin
                        f_jj <= 0; f_ei <= 0; f_state <= F_ESET;
                    end else f_jj <= f_jj + 1;
                end
            end
            // 誤りei毎の初期化(xk, xkinv, xkinv^2)。nerrに達したら終了。
            F_ESET: begin
                if (f_ei < nerr) begin
                    f_xk     <= exp_rom[(254 - errloc[f_ei]) % 255];
                    f_xkinv  <= exp_rom[(errloc[f_ei] + 1) % 255];
                    f_xkinv2 <= gf_mul_fn(exp_rom[(errloc[f_ei] + 1) % 255],
                                          exp_rom[(errloc[f_ei] + 1) % 255]);
                    f_num  <= 8'h00; f_den <= 8'h00; f_xpow <= 8'h01; f_kk <= 0;
                    f_state <= F_NUM;
                end else begin
                    f_state <= F_DONE;
                end
            end
            // num = Σ_{kk=0..PARITY-1} omega[kk]*xkinv^kk (Horner)
            F_NUM: begin
                f_num <= f_num ^ gf_mul_fn(omega[f_kk], f_xpow);
                if (f_kk == PARITY-1) begin
                    f_kk <= 1; f_xpow <= 8'h01; f_state <= F_DEN;
                end else begin
                    f_xpow <= gf_mul_fn(f_xpow, f_xkinv);
                    f_kk   <= f_kk + 1;
                end
            end
            // den = Σ_{kk=1,3,5,7} sigma[kk]*(xkinv^2)^((kk-1)/2)  (σ'の奇数項)
            F_DEN: begin
                f_den <= f_den ^ gf_mul_fn(sigma[f_kk], f_xpow);
                if (f_kk >= T-1) begin
                    f_state <= F_DIV;
                end else begin
                    f_xpow <= gf_mul_fn(f_xpow, f_xkinv2);
                    f_kk   <= f_kk + 2;
                end
            end
            // errval[ei] = xk * (num/den)
            F_DIV: begin
                if (f_den != 8'h00)
                    errval[f_ei] <= gf_mul_fn(f_xk, gf_div_fn(f_num, f_den));
                else
                    errval[f_ei] <= 8'h00;
                f_ei    <= f_ei + 1;
                f_state <= F_ESET;
            end
            F_DONE: begin
                if (nerr != L_val) uncorrectable <= 1;
                iter  <= 0;
                state <= ST_CORRECT;
            end
            endcase
        end
        // 最適化: 8誤り並列訂正(255:1読みMUX×8 + 255レジスタ書込MUX網)を
        // 1誤り/クロックに逐次化。iter=0で全コピー、以降errloc[iter-1]を1つずつ訂正。
        ST_CORRECT: begin
            if (iter == 0) begin
                for (i = 0; i < N; i = i + 1) cw_out[8*i +: 8] <= recv[i];
                if (uncorrectable || nerr == 0) begin
                    state <= ST_DONE;
                end else begin
                    iter <= 1;
                end
            end else begin
                cw_out[8*errloc[iter-1] +: 8] <= recv[errloc[iter-1]] ^ errval[iter-1];
                if (iter >= nerr) begin
                    iter  <= 0;
                    state <= ST_DONE;
                end else begin
                    iter <= iter + 1;
                end
            end
        end
        ST_DONE: begin
            dec_done <= 1; state <= ST_IDLE;
        end
        endcase
    end
end

endmodule
