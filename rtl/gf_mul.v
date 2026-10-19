// GF(2^8) Multiplier
// Primitive polynomial: x^8 + x^4 + x^3 + x^2 + 1 (0x11D)
//
// Reduction coefficients (confirmed by Python reference, 2026-06-25):
//   x^8  = 0x1D = x^4+x^3+x^2+x^0
//   x^9  = 0x3A = x^5+x^4+x^3+x^1
//   x^10 = 0x74 = x^6+x^5+x^4+x^2
//   x^11 = 0xE8 = x^7+x^6+x^5+x^3
//   x^12 = 0xCD = x^7+x^6+x^3+x^2+x^0
//   x^13 = 0x87 = x^7+x^2+x^1+x^0
//   x^14 = 0x13 = x^4+x^1+x^0

module gf_mul (
    input  [7:0] a,
    input  [7:0] b,
    output [7:0] result
);

wire [15:0] p;

assign p[ 0] = a[0]&b[0];
assign p[ 1] = a[1]&b[0] ^ a[0]&b[1];
assign p[ 2] = a[2]&b[0] ^ a[1]&b[1] ^ a[0]&b[2];
assign p[ 3] = a[3]&b[0] ^ a[2]&b[1] ^ a[1]&b[2] ^ a[0]&b[3];
assign p[ 4] = a[4]&b[0] ^ a[3]&b[1] ^ a[2]&b[2] ^ a[1]&b[3] ^ a[0]&b[4];
assign p[ 5] = a[5]&b[0] ^ a[4]&b[1] ^ a[3]&b[2] ^ a[2]&b[3] ^ a[1]&b[4] ^ a[0]&b[5];
assign p[ 6] = a[6]&b[0] ^ a[5]&b[1] ^ a[4]&b[2] ^ a[3]&b[3] ^ a[2]&b[4] ^ a[1]&b[5] ^ a[0]&b[6];
assign p[ 7] = a[7]&b[0] ^ a[6]&b[1] ^ a[5]&b[2] ^ a[4]&b[3] ^ a[3]&b[4] ^ a[2]&b[5] ^ a[1]&b[6] ^ a[0]&b[7];
assign p[ 8] = a[7]&b[1] ^ a[6]&b[2] ^ a[5]&b[3] ^ a[4]&b[4] ^ a[3]&b[5] ^ a[2]&b[6] ^ a[1]&b[7];
assign p[ 9] = a[7]&b[2] ^ a[6]&b[3] ^ a[5]&b[4] ^ a[4]&b[5] ^ a[3]&b[6] ^ a[2]&b[7];
assign p[10] = a[7]&b[3] ^ a[6]&b[4] ^ a[5]&b[5] ^ a[4]&b[6] ^ a[3]&b[7];
assign p[11] = a[7]&b[4] ^ a[6]&b[5] ^ a[5]&b[6] ^ a[4]&b[7];
assign p[12] = a[7]&b[5] ^ a[6]&b[6] ^ a[5]&b[7];
assign p[13] = a[7]&b[6] ^ a[6]&b[7];
assign p[14] = a[7]&b[7];
assign p[15] = 1'b0;

// Reduction: x^n mod 0x11D (confirmed 2026-06-25)
wire [7:0] r8, r9, r10, r11, r12, r13, r14;

// x^8 = 0x1D: bit7=0,6=0,5=0,4=1,3=1,2=1,1=0,0=1
assign r8  = {1'b0,   1'b0,   1'b0,   p[8],  p[8],  p[8],  1'b0,  p[8] };
// x^9 = 0x3A: bit7=0,6=0,5=1,4=1,3=1,2=0,1=1,0=0
assign r9  = {1'b0,   1'b0,   p[9],  p[9],  p[9],  1'b0,  p[9],  1'b0 };
// x^10= 0x74: bit7=0,6=1,5=1,4=1,3=0,2=1,1=0,0=0
assign r10 = {1'b0,   p[10], p[10], p[10], 1'b0,  p[10], 1'b0,  1'b0 };
// x^11= 0xE8: bit7=1,6=1,5=1,4=0,3=1,2=0,1=0,0=0
assign r11 = {p[11], p[11], p[11], 1'b0,  p[11], 1'b0,  1'b0,  1'b0 };
// x^12= 0xCD: bit7=1,6=1,5=0,4=0,3=1,2=1,1=0,0=1
assign r12 = {p[12], p[12], 1'b0,  1'b0,  p[12], p[12], 1'b0,  p[12]};
// x^13= 0x87: bit7=1,6=0,5=0,4=0,3=0,2=1,1=1,0=1
assign r13 = {p[13], 1'b0,  1'b0,  1'b0,  1'b0,  p[13], p[13], p[13]};
// x^14= 0x13: bit7=0,6=0,5=0,4=1,3=0,2=0,1=1,0=1
assign r14 = {1'b0,  1'b0,  1'b0,  p[14], 1'b0,  1'b0,  p[14], p[14]};

assign result = p[7:0] ^ r8 ^ r9 ^ r10 ^ r11 ^ r12 ^ r13 ^ r14;

endmodule
