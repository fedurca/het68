// psram.c — APS6404 8 MB on QMI CS1 (GPIO 47). See psram.h.
//
// Sequence matches Arduino-pico / MicroPython rp2 PSRAM bring-up (MIT).
// Direct-mode QMI pauses XIP, so the whole bring-up runs from SRAM.
#include "psram.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "pico/platform.h"

#ifndef PIMORONI_PICO_PLUS2_W_PSRAM_CS_PIN
#define PIMORONI_PICO_PLUS2_W_PSRAM_CS_PIN 47
#endif

#define PSRAM_CS_PIN       PIMORONI_PICO_PLUS2_W_PSRAM_CS_PIN
#define PSRAM_MAX_SCK_HZ   109000000u
#define PSRAM_KGD          0x5Du
#define PSRAM_CMD_READ_ID  0x9Fu
#define PSRAM_CMD_QPI_EN   0x35u
#define PSRAM_CMD_QREAD    0xEBu
#define PSRAM_CMD_QWRITE   0x38u

static size_t s_size;
static bool s_ok;

static void __no_inline_not_in_flash_func(qmi_wait_busy)(void) {
    uint32_t spins = 1000000u;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
        if (--spins == 0u)
            break;
        tight_loop_contents();
    }
}

static void __no_inline_not_in_flash_func(psram_apply_window)(uint32_t clock_hz) {
    uint32_t divisor = (clock_hz + PSRAM_MAX_SCK_HZ - 1u) / PSRAM_MAX_SCK_HZ;
    if (divisor == 1u && clock_hz > 100000000u) divisor = 2u;
    uint32_t rxdelay = divisor;
    if (clock_hz / divisor > 100000000u) rxdelay += 1u;

    const uint32_t clock_period_fs = 1000000000000000ull / clock_hz;
    uint32_t max_select = (125u * 1000000u) / clock_period_fs;
    if (max_select > 62u) max_select = 62u;
    uint32_t min_deselect =
        (18u * 1000000u + (clock_period_fs - 1u)) / clock_period_fs;
    const uint32_t hold = (divisor + 1u) / 2u;
    if (min_deselect > hold)
        min_deselect -= hold;
    else
        min_deselect = 0;

    qmi_hw->m[1].timing =
        (1u << QMI_M1_TIMING_COOLDOWN_LSB) |
        (QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB) |
        (max_select << QMI_M1_TIMING_MAX_SELECT_LSB) |
        (min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB) |
        (rxdelay << QMI_M1_TIMING_RXDELAY_LSB) |
        (divisor << QMI_M1_TIMING_CLKDIV_LSB);

    qmi_hw->m[1].rfmt =
        (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB) |
        (QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB) |
        (QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB) |
        (QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB) |
        (6u << QMI_M0_RFMT_DUMMY_LEN_LSB);
    qmi_hw->m[1].rcmd = PSRAM_CMD_QREAD;

    qmi_hw->m[1].wfmt =
        (QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB) |
        (QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB) |
        (QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB) |
        (QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = PSRAM_CMD_QWRITE;

    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
}

// Entire QMI direct-mode sequence lives in SRAM. Enabling DIRECT_CSR_EN from
// flash would immediately IBUSERR (XIP is paused for the duration).
static size_t __no_inline_not_in_flash_func(psram_bringup)(uint32_t clock_hz) {
    qmi_hw->direct_csr = 30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    qmi_wait_busy();

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                        (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) |
                        0xf5u;
    qmi_wait_busy();
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0;
    uint8_t eid = 0;
    for (int i = 0; i < 7; i++) {
        qmi_hw->direct_tx = (i == 0) ? PSRAM_CMD_READ_ID : 0xffu;
        uint32_t spins = 1000000u;
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {
            if (--spins == 0u)
                break;
            tight_loop_contents();
        }
        qmi_wait_busy();
        uint8_t b = (uint8_t)qmi_hw->direct_rx;
        if (i == 5) kgd = b;
        else if (i == 6) eid = b;
    }
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    if (kgd != PSRAM_KGD) {
        qmi_hw->direct_csr = 0;
        return 0;
    }
    size_t sz = 1024u * 1024u;
    uint8_t size_id = (uint8_t)(eid >> 5);
    if (size_id == 4) sz *= 16u;
    else if (eid == 0x26u || size_id == 2 || size_id == 3) sz *= 8u;
    else if (size_id == 1) sz *= 4u;
    else if (size_id == 0) sz *= 2u;
    else {
        qmi_hw->direct_csr = 0;
        return 0;
    }
    if (sz != HET68_PSRAM_BYTES) {
        qmi_hw->direct_csr = 0;
        return 0;
    }

    qmi_hw->direct_csr = 10u << QMI_DIRECT_CSR_CLKDIV_LSB |
                         QMI_DIRECT_CSR_EN_BITS |
                         QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    qmi_wait_busy();
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | PSRAM_CMD_QPI_EN;
    qmi_wait_busy();

    psram_apply_window(clock_hz);
    qmi_hw->direct_csr = 0;
    return sz;
}

void __no_inline_not_in_flash_func(het68_psram_restore)(void) {
    if (!s_ok) return;
    uint32_t clock_hz = clock_get_hz(clk_sys);
    uint32_t irq = save_and_disable_interrupts();
    psram_apply_window(clock_hz);
    restore_interrupts(irq);
}

bool __no_inline_not_in_flash_func(het68_psram_init)(void) {
    s_ok = false;
    s_size = 0;

    gpio_set_function(PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);
    uint32_t clock_hz = clock_get_hz(clk_sys);

    uint32_t irq = save_and_disable_interrupts();
    size_t sz = psram_bringup(clock_hz);
    restore_interrupts(irq);

    if (sz == 0)
        return false;
    s_size = sz;
    s_ok = true;
    return true;
}

size_t het68_psram_size(void) { return s_size; }
