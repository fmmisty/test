`timescale 1ns/1ps
module tb_txready;
  reg clk=0, rst_n=0; always #5 clk=~clk;
  reg in_v=0; reg [7:0] in_d=0; wire in_r;
  wire ov; wire [7:0] od; reg ordy=1;
  rs_fec_tx_core dut(.clk(clk),.rst_n(rst_n),.tx_in_valid(in_v),.tx_in_data(in_d),.tx_in_ready(in_r),
    .tx_out_valid(ov),.tx_out_data(od),.tx_out_ready(ordy));
  integer GAP, sent, rcvd, i, mism; reg [7:0] refa [0:8159];
  initial begin
    if(!$value$plusargs("GAP=%d",GAP)) GAP=0;
    for(i=0;i<8160;i=i+1) refa[i]=0;
    sent=0; rcvd=0; mism=0;
    repeat(5) @(posedge clk); rst_n=1;
    fork
      begin while(sent<7648) begin in_v<=1; in_d<=sent[7:0]^8'h5a; @(posedge clk); if(in_v&&in_r) sent=sent+1; end in_v<=0; end
      begin while(rcvd<8160) begin ordy <= GAP ? ($random%3!=0) : 1; @(posedge clk);
          if(ov&&ordy) begin
            if (GAP==0) refa[rcvd]=od;
            else if (od!==ref_get(rcvd)) mism=mism+1;
            rcvd=rcvd+1; end end end
    join
    $display("GAP=%0d rcvd=%0d mism=%0d", GAP, rcvd, mism);
    if(GAP==0) begin $writememh("ref.hex", refa); end
    $finish;
  end
  initial begin #20000000 $display("TIMEOUT rcvd=%0d",rcvd); $finish; end
  reg [7:0] refmem [0:8159]; initial if(GAP!=0) $readmemh("ref.hex", refmem);
  function [7:0] ref_get(input integer k); ref_get=refmem[k]; endfunction
endmodule
