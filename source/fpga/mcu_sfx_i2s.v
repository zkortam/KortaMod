// ESP32 I2S master → FPGA. Pins are FPGA inputs (WS/DIN/DOUT).
// BCLK = I2S_DIN (MCU GPIO26), WS = I2S_WS (GPIO25), SD = I2S_DOUT (GPIO27).
// Do not use I2S_BCLK; stock RTL drives that as menuDisabled.
module mcu_sfx_i2s (
    input               clk,
    input               reset_n,
    input               bclk,
    input               ws,
    input               sd,
    output reg [15:0]   pcm_l,
    output reg [15:0]   pcm_r
);
    reg [2:0] bclk_s, ws_s, sd_s;
    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            bclk_s <= 3'b000;
            ws_s <= 3'b000;
            sd_s <= 3'b000;
        end else begin
            bclk_s <= {bclk_s[1:0], bclk};
            ws_s <= {ws_s[1:0], ws};
            sd_s <= {sd_s[1:0], sd};
        end
    end

    wire rise = (bclk_s[2:1] == 2'b01);
    wire ws_now = ws_s[1];
    wire sd_now = sd_s[1];

    reg ws_d;
    reg [4:0] bitpos;
    reg [15:0] acc;

    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            ws_d <= 1'b0;
            bitpos <= 5'd0;
            acc <= 16'd0;
            pcm_l <= 16'd0;
            pcm_r <= 16'd0;
        end else if (rise) begin
            if (ws_now != ws_d) begin
                ws_d <= ws_now;
                bitpos <= 5'd0;
                acc <= 16'd0;
            end else begin
                if (bitpos < 5'd16)
                    acc <= {acc[14:0], sd_now};
                if (bitpos == 5'd15) begin
                    if (ws_d == 1'b0)
                        pcm_l <= {acc[14:0], sd_now};
                    else
                        pcm_r <= {acc[14:0], sd_now};
                end
                if (bitpos != 5'd31)
                    bitpos <= bitpos + 5'd1;
            end
        end
    end
endmodule
