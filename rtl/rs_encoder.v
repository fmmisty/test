// RS(N,K) Encoder - Parameterized
// GF(2^8), primitive polynomial 0x11D
// Default: RS(255,239), PARITY=16
//
// Parameters:
//   N      : codeword length (default 255)
//   K      : data length (default 239)
//   PARITY : N-K = number of parity bytes (default 16)
//
// Note: Generator polynomial G(x) coefficients (g_rom) are valid for
//       PARITY=16 (roots alpha^0..alpha^15).
//       For other PARITY values, g_rom must be updated accordingly.

module rs_encoder #(
    parameter N      = 255,
    parameter K      = 239,
    parameter PARITY = 16   // must equal N-K
)(
    input        clk,
    input        rst_n,
    input        in_valid,
    input  [7:0] in_data,
    output reg        out_valid,
    output reg [7:0]  out_data
);

// Generator polynomial G coefficients for PARITY=16
// G(x) = prod_{i=0}^{15}(x - alpha^i), degree 16
reg [7:0] g_rom [0:PARITY];
initial begin
    g_rom[ 0] = 8'h01;
    g_rom[ 1] = 8'h3B;
    g_rom[ 2] = 8'h0D;
    g_rom[ 3] = 8'h68;
    g_rom[ 4] = 8'hBD;
    g_rom[ 5] = 8'h44;
    g_rom[ 6] = 8'hD1;
    g_rom[ 7] = 8'h1E;
    g_rom[ 8] = 8'h08;
    g_rom[ 9] = 8'hA3;
    g_rom[10] = 8'h41;
    g_rom[11] = 8'h29;
    g_rom[12] = 8'hE5;
    g_rom[13] = 8'h62;
    g_rom[14] = 8'h32;
    g_rom[15] = 8'h24;
    g_rom[16] = 8'h3B;
end

// EXP/LOG tables
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

// Shift buffer
reg [7:0] sbuf [0:N-1];

// State machine
localparam ST_IDLE = 2'd0;
localparam ST_DATA = 2'd1;
localparam ST_OUT  = 2'd2;

reg [1:0]  state;
reg [7:0]  in_cnt;
reg [7:0]  out_cnt;

function [7:0] gf_mul_fn;
    input [7:0] x, y;
    reg [8:0] lx, ly, ls;
    begin
        if (x == 0 || y == 0)
            gf_mul_fn = 8'h00;
        else begin
            lx = {1'b0, log_rom[x]};
            ly = {1'b0, log_rom[y]};
            ls = lx + ly;
            if (ls >= 255) ls = ls - 255;
            gf_mul_fn = exp_rom[ls[7:0]];
        end
    end
endfunction

integer i;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        state     <= ST_IDLE;
        in_cnt    <= 0;
        out_cnt   <= 0;
        out_valid <= 0;
        out_data  <= 0;
        for (i = 0; i < N; i = i + 1) sbuf[i] <= 8'h00;
    end else begin
        out_valid <= 0;
        case (state)
        ST_IDLE: begin
            if (in_valid) begin
                for (i = 0; i < N; i = i + 1) sbuf[i] <= 8'h00;
                sbuf[0] <= in_data;
                in_cnt  <= 1;
                state   <= ST_DATA;
            end
        end
        ST_DATA: begin
            if (in_valid) begin
                sbuf[in_cnt] <= in_data;
                in_cnt <= in_cnt + 1;
                if (in_cnt == K-1) state <= ST_OUT;
            end
        end
        ST_OUT: begin
            if (out_cnt == 0) begin : parity_compute
                reg [7:0] tmp [0:N-1];
                reg [7:0] fb;
                integer j, k;
                for (j = 0; j < N; j = j + 1) tmp[j] = 8'h00;
                for (j = 0; j < K; j = j + 1) tmp[j] = sbuf[j];
                for (j = 0; j < K; j = j + 1) begin
                    fb = tmp[j];
                    if (fb != 0)
                        for (k = 1; k <= PARITY; k = k + 1)
                            tmp[j+k] = tmp[j+k] ^ gf_mul_fn(g_rom[k], fb);
                end
                for (j = K; j < N; j = j + 1) sbuf[j] = tmp[j];
            end
            out_valid <= 1;
            out_data  <= sbuf[out_cnt];
            out_cnt   <= out_cnt + 1;
            if (out_cnt == N-1) begin
                out_cnt <= 0;
                state   <= ST_IDLE;
            end
        end
        endcase
    end
end

endmodule
