#include <stdbool.h>
#include <stdint.h>
#include "fpga.h"
#include "hw.h"
#include "sd.h"


#define SD_BUFFER_ADDRESS               (0x05000000UL)

#define CMD6_ARG_CHECK_HS               (0x00FFFFF1UL)
#define CMD6_ARG_SWITCH_HS              (0x80FFFFF1UL)

#define CMD8_ARG_SUPPLY_VOLTAGE_27_36_V (1 << 8)
#define CMD8_ARG_CHECK_PATTERN          (0xAA << 0)

#define ACMD6_ARG_BUS_WIDTH_4BIT        (2 << 0)

#define ACMD41_ARG_OCR                  (0xFF8000 << 0)
#define ACMD41_ARG_HCS                  (1 << 30)

#define R3_OCR                          (0xFF8000 << 0)
#define R3_CCS                          (1 << 30)
#define R3_BUSY                         (1 << 31)

#define R6_RCA_MASK                     (0xFFFF0000UL)

#define R7_SUPPLY_VOLTAGE_27_36_V       (1 << 8)
#define R7_CHECK_PATTERN                (0xAA << 0)

#define SD_BLOCK_SIZE                   (512)
#define DAT_BLOCK_MAX_COUNT             (256)


typedef enum {
    CLOCK_STOP,
    CLOCK_400KHZ,
    CLOCK_25MHZ,
    CLOCK_50MHZ,
} sd_clock_t;

typedef enum {
    RSP_NONE,
    RSP_R1,
    RSP_R1b,
    RSP_R2,
    RSP_R3,
    RSP_R6,
    RSP_R7,
} rsp_type_t;

typedef enum {
    DAT_READ,
    DAT_WRITE,
} dat_mode_t;


struct process {
    bool card_initialized;
    bool card_type_block;
    uint32_t rca;
    volatile bool timeout;
};


static struct process p;


static void sd_trigger_timeout (void) {
    p.timeout = true;
}

static void sd_prepare_timeout (uint16_t value) {
    p.timeout = false;
    hw_tim_setup(TIM_ID_GVR, value, sd_trigger_timeout);
}

static bool sd_did_timeout (void) {
    return p.timeout;
}

static void sd_clear_timeout (void) {
    hw_tim_stop(TIM_ID_GVR);
    p.timeout = false;
}

static void sd_set_clock (sd_clock_t mode) {
    fpga_reg_set(REG_SD_SCR, SD_SCR_CLOCK_MODE_OFF);

    switch (mode) {
        case CLOCK_400KHZ:
            fpga_reg_set(REG_SD_SCR, SD_SCR_CLOCK_MODE_400KHZ);
            break;
        case CLOCK_25MHZ:
            fpga_reg_set(REG_SD_SCR, SD_SCR_CLOCK_MODE_25MHZ);
            break;
        case CLOCK_50MHZ:
            fpga_reg_set(REG_SD_SCR, SD_SCR_CLOCK_MODE_50MHZ);
            break;
        default:
            break;
    }
}

static bool sd_cmd (uint8_t cmd, uint32_t arg, rsp_type_t rsp_type, void *rsp) {
    uint32_t scr;
    uint32_t cmd_data;

    cmd_data = ((cmd << SD_CMD_INDEX_BIT) & SD_CMD_INDEX_MASK);
    switch (rsp_type) {
        case RSP_NONE:
            cmd_data |= SD_CMD_SKIP_RESPONSE;
            break;
        case RSP_R2:
            cmd_data |= (SD_CMD_LONG_RESPONSE | SD_CMD_RESERVED_RESPONSE);
            break;
        case RSP_R3:
            cmd_data |= (SD_CMD_IGNORE_CRC | SD_CMD_RESERVED_RESPONSE);
            break;
        default:
            break;
    }

    fpga_reg_set(REG_SD_ARG, arg);
    fpga_reg_set(REG_SD_CMD, cmd_data);

    do {
        scr = fpga_reg_get(REG_SD_SCR);
    } while (scr & SD_SCR_CMD_BUSY);

    if (rsp != NULL) {
        bool rsp_long = (cmd_data & SD_CMD_LONG_RESPONSE);
        uint8_t *rsp_8 = (uint8_t *) (rsp);
        for (int i = 0; i < (rsp_long ? 4 : 1); i++) {
            uint32_t rsp_data = fpga_reg_get(REG_SD_RSP_0 + i);
            uint8_t *rsp_data_8 = (uint8_t *) (&rsp_data);
            for (int i = 0; i < 4; i++) {
                *rsp_8++ = *rsp_data_8++;
            }
        }
    }

    if (rsp_type == RSP_R1b) {
        do {
            scr = fpga_reg_get(REG_SD_SCR);
        } while (scr & SD_SCR_CARD_BUSY);
    }

    return (scr & SD_SCR_CMD_ERROR);
}

static bool sd_acmd (uint8_t acmd, uint32_t arg, rsp_type_t rsp_type, void *rsp) {
    if (sd_cmd(55, p.rca, RSP_R1, NULL)) {
        return true;
    }
    if (sd_cmd(acmd, arg, rsp_type, rsp)) {
        return true;
    }
    return false;
}

static void sd_dat_prepare (uint32_t address, uint32_t count, dat_mode_t mode) {
    uint32_t length = (count * SD_BLOCK_SIZE);
    uint32_t sd_dat = (((count - 1) << SD_DAT_BLOCKS_BIT) | SD_DAT_FIFO_FLUSH);
    uint32_t sd_dma_scr = DMA_SCR_START;

    if (mode == DAT_READ) {
        sd_dat |= SD_DAT_START_READ;
        sd_dma_scr |= DMA_SCR_DIRECTION;
    } else {
        sd_dat |= SD_DAT_START_WRITE;
    }

    fpga_reg_set(REG_SD_DAT, sd_dat);
    fpga_reg_set(REG_SD_DMA_ADDRESS, address);
    fpga_reg_set(REG_SD_DMA_LENGTH, length);
    fpga_reg_set(REG_SD_DMA_SCR, sd_dma_scr);
}

static void sd_dat_abort (void) {
    fpga_reg_set(REG_SD_DMA_SCR, DMA_SCR_STOP);
    fpga_reg_set(REG_SD_DAT, SD_DAT_STOP | SD_DAT_FIFO_FLUSH);
}

static bool sd_dat_wait (uint16_t timeout) {
    sd_prepare_timeout(timeout);

    do {
        uint32_t sd_dat = fpga_reg_get(REG_SD_DAT);
        uint32_t sd_dma_scr = fpga_reg_get(REG_SD_DMA_SCR);
        if ((!(sd_dat & SD_DAT_BUSY)) && (!(sd_dma_scr & DMA_SCR_BUSY))) {
            sd_clear_timeout();
            return (sd_dat & SD_DAT_ERROR);
        }
    } while (!sd_did_timeout());

    sd_dat_abort();

    return true;
}


bool sd_read_sectors (uint32_t address, uint32_t sector, uint32_t count) {
    if (!p.card_initialized || (count == 0)) {
        return true;
    }

    if (!p.card_type_block) {
        sector *= SD_BLOCK_SIZE;
    }

    while (count > 0) {
        uint32_t blocks = ((count > DAT_BLOCK_MAX_COUNT) ? DAT_BLOCK_MAX_COUNT : count);
        sd_dat_prepare(address, blocks, DAT_READ);
        if (sd_cmd(23, blocks, RSP_R1, NULL)) {
            sd_dat_abort();
            return true;
        }
        if (sd_cmd(18, sector, RSP_R1, NULL)) {
            sd_dat_abort();
            return true;
        }
        if (sd_dat_wait(1000)) {
            if (sd_did_timeout()) {
                sd_cmd(12, 0, RSP_R1b, NULL);
            }
            return true;
        }
        address += (blocks * SD_BLOCK_SIZE);
        sector += (blocks * (p.card_type_block ? 1 : SD_BLOCK_SIZE));
        count -= blocks;
    }

    return false;
}

bool sd_card_init (void) {
    uint32_t arg;
    uint32_t rsp;
    uint16_t sd_function;

    if (p.card_initialized) {
        return false;
    }

    p.card_initialized = true;
    p.rca = 0;

    sd_set_clock(CLOCK_400KHZ);

    sd_cmd(0, 0, RSP_NONE, NULL);

    arg = (CMD8_ARG_SUPPLY_VOLTAGE_27_36_V | CMD8_ARG_CHECK_PATTERN);
    if (sd_cmd(8, arg, RSP_R7, &rsp)) {
        arg = ACMD41_ARG_OCR;
    } else {
        if (rsp != (R7_SUPPLY_VOLTAGE_27_36_V | R7_CHECK_PATTERN)) {
            sd_card_deinit();
            return true;
        }
        arg = (ACMD41_ARG_HCS | ACMD41_ARG_OCR);
    }

    sd_prepare_timeout(1000);
    do {
        if (sd_did_timeout()) {
            sd_card_deinit();
            return true;            
        }
        if (sd_acmd(41, arg, RSP_R3, &rsp)) {
            sd_card_deinit();
            return true;
        }
        if (rsp & R3_BUSY) {
            if ((rsp & R3_OCR) == 0) {
                sd_card_deinit();
                return true;
            }
            p.card_type_block = (rsp & R3_CCS);
            break;
        }
    } while (1);
    sd_clear_timeout();

    if (sd_cmd(2, 0, RSP_R2, NULL)) {
        sd_card_deinit();
        return true;
    }

    if (sd_cmd(3, 0, RSP_R6, &rsp)) {
        sd_card_deinit();
        return true;
    }
    p.rca = (rsp & R6_RCA_MASK);

    if (sd_cmd(7, p.rca, RSP_R1b, NULL)) {
        sd_card_deinit();
        return true;
    }

    sd_set_clock(CLOCK_25MHZ);

    if (sd_acmd(6, ACMD6_ARG_BUS_WIDTH_4BIT, RSP_R1, NULL)) {
        sd_card_deinit();
        return true;
    }

    sd_dat_prepare(SD_BUFFER_ADDRESS, 1, DAT_READ);
    if (sd_cmd(6, CMD6_ARG_CHECK_HS, RSP_R1, NULL)) {
        sd_dat_abort();
        sd_card_deinit();
        return true;
    }
    sd_dat_wait(1000);
    if (sd_did_timeout()) {
        sd_card_deinit();
        return true;
    }
    fpga_mem_read(SD_BUFFER_ADDRESS + 12, 2, (uint8_t *) (&sd_function));
    if (SWAP16(sd_function) & (1 << 1)) {
        sd_dat_prepare(SD_BUFFER_ADDRESS, 1, DAT_READ);
        if (sd_cmd(6, CMD6_ARG_SWITCH_HS, RSP_R1, NULL)) {
            sd_dat_abort();
            sd_card_deinit();
            return true;
        }
        sd_dat_wait(1000);
        if (sd_did_timeout()) {
            sd_card_deinit();
            return true;
        }
        fpga_mem_read(SD_BUFFER_ADDRESS + 12, 2, (uint8_t *) (&sd_function));
        if (SWAP16(sd_function) & (1 << 1)) {
            sd_set_clock(CLOCK_50MHZ);
        }
    }

    return false;
}

void sd_card_deinit (void) {
    if (p.card_initialized) {
        p.card_initialized = false;
        sd_set_clock(CLOCK_400KHZ);
        sd_cmd(0, 0, RSP_NONE, NULL);
        sd_set_clock(CLOCK_STOP);
    }
}

void sd_init (void) {
    p.card_initialized = false;
    sd_set_clock(CLOCK_STOP);
}

void sd_process (void) {
    if (!(fpga_reg_get(REG_SD_SCR) & SD_SCR_CARD_INSERTED)) {
        sd_card_deinit();
    }
}