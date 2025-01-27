module n64_top (
    input clk,
    input reset,

    n64_scb n64_scb,
    dd_scb.dd dd_scb,

    mem_bus.controller mem_bus,

    input n64_reset,
    input n64_nmi,
    output n64_irq,

    input n64_pi_alel,
    input n64_pi_aleh,
    input n64_pi_read,
    input n64_pi_write,
    inout [15:0] n64_pi_ad,

    input n64_si_clk,
    inout n64_si_dq
);

    logic n64_dd_irq;
    logic n64_cfg_irq;
    logic n64_irq_oe;

    always @(posedge clk) begin
        n64_irq_oe <= (n64_dd_irq || n64_cfg_irq);
    end

    assign n64_irq = n64_irq_oe ? 1'b0 : 1'bZ;

    n64_reg_bus reg_bus ();

    n64_pi n64_pi_inst (
        .clk(clk),
        .reset(reset),

        .mem_bus(mem_bus),
        .reg_bus(reg_bus),

        .n64_scb(n64_scb),

        .n64_reset(n64_reset),
        .n64_nmi(n64_nmi),
        .n64_pi_alel(n64_pi_alel),
        .n64_pi_aleh(n64_pi_aleh),
        .n64_pi_read(n64_pi_read),
        .n64_pi_write(n64_pi_write),
        .n64_pi_ad(n64_pi_ad)
    );

    n64_dd n64_dd_inst (
        .clk(clk),
        .reset(reset),

        .reg_bus(reg_bus),

        .n64_scb(n64_scb),
        .dd_scb(dd_scb),

        .irq(n64_dd_irq)
    );

    n64_flashram n64_flashram_inst (
        .clk(clk),
        .reset(reset),

        .reg_bus(reg_bus),

        .n64_scb(n64_scb)
    );

    n64_cfg n64_cfg_inst (
        .clk(clk),
        .reset(reset),

        .reg_bus(reg_bus),

        .n64_scb(n64_scb),

        .irq(n64_cfg_irq)
    );

    n64_si n64_si_inst (
        .clk(clk),
        .reset(reset),

        .n64_scb(n64_scb),

        .n64_reset(n64_reset),
        .n64_si_clk(n64_si_clk),
        .n64_si_dq(n64_si_dq)
    );

    n64_save_counter n64_save_counter_inst (
        .clk(clk),
        .reset(reset),

        .n64_scb(n64_scb)
    );

endmodule
