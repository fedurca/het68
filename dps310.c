// dps310.c — Infineon DPS310 over hardware I2C1 on GP2/GP3.
//
// Continuous pressure+temperature (32 Hz, 16× OS) with datasheet compensation.
// Init may sleep (~50 ms); poll/read never block the audio path.
#include "dps310.h"
#include "debug_io.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"
#include <math.h>
#include <string.h>

#define DPS310_I2C          i2c1
#define DPS310_I2C_HZ       400000u

#define REG_PSR_B2          0x00u
#define REG_PRS_CFG         0x06u
#define REG_TMP_CFG         0x07u
#define REG_MEAS_CFG        0x08u
#define REG_CFG_REG         0x09u
#define REG_RESET           0x0Cu
#define REG_PRODUCT_ID      0x0Du
#define REG_COEF            0x10u
#define REG_COEF_SRCE       0x28u

#define PRODUCT_ID_DPS310   0x10u
#define SOFT_RST            0x09u

#define MEAS_COEF_RDY       0x80u
#define MEAS_SENSOR_RDY     0x40u
#define MEAS_TMP_RDY        0x20u
#define MEAS_PRS_RDY        0x10u
#define MEAS_CTRL_CONT_PT   0x07u

#define CFG_T_SHIFT         0x08u
#define CFG_P_SHIFT         0x04u

#define TMP_COEF_SRCE       0x80u

// 32 Hz rate + 16× oversampling (kT/kP = 253952 with result bit-shift)
#define PRS_CFG_32HZ_16X    0x54u
#define TMP_CFG_32HZ_16X    0x54u

#define SCALE_16X           253952.0f
#define SEA_LEVEL_PA        101325.0f

static bool g_ok;
static uint8_t g_addr;
static int32_t g_c0, g_c1, g_c00, g_c10, g_c01, g_c11, g_c20, g_c21, g_c30;
static float g_pressure_pa;
static float g_temperature_c;
static uint32_t g_sample_ms;
static bool g_have_sample;
// Assumed RH (0..1) until a humidity sensor is added — default 50 %.
static float g_rh = 0.50f;

static int32_t twos_complement(uint32_t raw, uint8_t bits) {
    uint32_t sign = 1u << (bits - 1u);
    if (raw & sign)
        return (int32_t)raw - (int32_t)(1u << bits);
    return (int32_t)raw;
}

static bool i2c_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    int n = i2c_write_blocking(DPS310_I2C, g_addr, buf, 2, false);
    return n == 2;
}

static bool i2c_read_regs(uint8_t reg, uint8_t *dst, size_t len) {
    if (i2c_write_blocking(DPS310_I2C, g_addr, &reg, 1, true) != 1)
        return false;
    return i2c_read_blocking(DPS310_I2C, g_addr, dst, len, false) == (int)len;
}

static bool i2c_read_u8(uint8_t reg, uint8_t *val) {
    return i2c_read_regs(reg, val, 1);
}

static bool probe_addr(uint8_t addr) {
    uint8_t reg = REG_PRODUCT_ID;
    uint8_t id = 0;
    if (i2c_write_blocking(DPS310_I2C, addr, &reg, 1, true) != 1)
        return false;
    if (i2c_read_blocking(DPS310_I2C, addr, &id, 1, false) != 1)
        return false;
    return id == PRODUCT_ID_DPS310;
}

static bool pins_look_free(void) {
    // Reject if already claimed by a non-SIO/I2C function (e.g. SPI/UART/PWM).
    uint f2 = gpio_get_function(DPS310_I2C_SDA_PIN);
    uint f3 = gpio_get_function(DPS310_I2C_SCL_PIN);
    if (f2 != GPIO_FUNC_SIO && f2 != GPIO_FUNC_NULL && f2 != GPIO_FUNC_I2C)
        return false;
    if (f3 != GPIO_FUNC_SIO && f3 != GPIO_FUNC_NULL && f3 != GPIO_FUNC_I2C)
        return false;
    return true;
}

static bool load_coefficients(void) {
    uint8_t coef[18];
    if (!i2c_read_regs(REG_COEF, coef, sizeof(coef)))
        return false;

    g_c0  = twos_complement(((uint32_t)coef[0] << 4) | ((coef[1] >> 4) & 0x0Fu), 12);
    g_c1  = twos_complement((((uint32_t)coef[1] & 0x0Fu) << 8) | coef[2], 12);
    g_c00 = twos_complement(((uint32_t)coef[3] << 12) | ((uint32_t)coef[4] << 4) |
                            ((coef[5] >> 4) & 0x0Fu), 20);
    g_c10 = twos_complement((((uint32_t)coef[5] & 0x0Fu) << 16) |
                            ((uint32_t)coef[6] << 8) | coef[7], 20);
    g_c01 = twos_complement(((uint32_t)coef[8] << 8) | coef[9], 16);
    g_c11 = twos_complement(((uint32_t)coef[10] << 8) | coef[11], 16);
    g_c20 = twos_complement(((uint32_t)coef[12] << 8) | coef[13], 16);
    g_c21 = twos_complement(((uint32_t)coef[14] << 8) | coef[15], 16);
    g_c30 = twos_complement(((uint32_t)coef[16] << 8) | coef[17], 16);
    return true;
}

static bool configure_continuous(void) {
    uint8_t coef_srce = 0;
    (void)i2c_read_u8(REG_COEF_SRCE, &coef_srce);
    uint8_t tmp_ext = (uint8_t)(coef_srce & TMP_COEF_SRCE);

    if (!i2c_write_reg(REG_PRS_CFG, PRS_CFG_32HZ_16X)) return false;
    if (!i2c_write_reg(REG_TMP_CFG, (uint8_t)(TMP_CFG_32HZ_16X | tmp_ext))) return false;
    if (!i2c_write_reg(REG_CFG_REG, (uint8_t)(CFG_T_SHIFT | CFG_P_SHIFT))) return false;
    // Datasheet/Linux: write 0 before enabling continuous mode.
    if (!i2c_write_reg(REG_MEAS_CFG, 0x00u)) return false;
    if (!i2c_write_reg(REG_MEAS_CFG, MEAS_CTRL_CONT_PT)) return false;
    return true;
}

bool dps310_init(void) {
    g_ok = false;
    g_addr = 0;
    g_have_sample = false;
    g_pressure_pa = 0.0f;
    g_temperature_c = 0.0f;
    g_sample_ms = 0;

    if (!pins_look_free()) {
        dbg_puts("BARO: pin GP2/GP3 busy — DPS310 skipped\n");
        return false;
    }

    i2c_init(DPS310_I2C, DPS310_I2C_HZ);
    gpio_set_function(DPS310_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(DPS310_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(DPS310_I2C_SDA_PIN);
    gpio_pull_up(DPS310_I2C_SCL_PIN);

    uint8_t try_addrs[2] = { 0x77u, 0x76u };
    for (int i = 0; i < 2; i++) {
        if (probe_addr(try_addrs[i])) {
            g_addr = try_addrs[i];
            break;
        }
    }
    if (!g_addr) {
        // Release pins so another peripheral can use them later.
        gpio_set_function(DPS310_I2C_SDA_PIN, GPIO_FUNC_NULL);
        gpio_set_function(DPS310_I2C_SCL_PIN, GPIO_FUNC_NULL);
        i2c_deinit(DPS310_I2C);
        dbg_puts("BARO: no DPS310 on GP2/GP3 (I2C1)\n");
        return false;
    }

    if (!i2c_write_reg(REG_RESET, SOFT_RST)) {
        dbg_puts("BARO: soft reset failed\n");
        return false;
    }
    sleep_ms(50);

    uint8_t meas = 0;
    for (int i = 0; i < 20; i++) {
        if (!i2c_read_u8(REG_MEAS_CFG, &meas)) {
            dbg_puts("BARO: MEAS_CFG read failed\n");
            return false;
        }
        if ((meas & MEAS_COEF_RDY) && (meas & MEAS_SENSOR_RDY))
            break;
        sleep_ms(5);
    }
    if (!((meas & MEAS_COEF_RDY) && (meas & MEAS_SENSOR_RDY))) {
        dbg_puts("BARO: coeffs/sensor not ready\n");
        return false;
    }

    if (!load_coefficients() || !configure_continuous()) {
        dbg_puts("BARO: configure failed\n");
        return false;
    }

    g_ok = true;
    uint32_t lock = dbg_line_lock();
    dbg_puts("BARO: DPS310 OK I2C1 GP2/GP3 addr=0x");
    dbg_puthex8(g_addr);
    dbg_puts(" (continuous 32Hz/16x)\n");
    dbg_line_unlock(lock);
    return true;
}

bool dps310_available(void) { return g_ok; }
uint8_t dps310_i2c_addr(void) { return g_ok ? g_addr : 0; }

void dps310_poll(void) {
    if (!g_ok) return;

    // Keep I2C traffic light — sample at ~10 Hz max from the main loop.
    static uint32_t s_last_poll_ms;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((now - s_last_poll_ms) < 100u) return;
    s_last_poll_ms = now;

    uint8_t meas = 0;
    if (!i2c_read_u8(REG_MEAS_CFG, &meas)) return;
    if (!(meas & MEAS_PRS_RDY)) return;

    uint8_t buf[6];
    if (!i2c_read_regs(REG_PSR_B2, buf, sizeof(buf))) return;

    int32_t praw = twos_complement(((uint32_t)buf[0] << 16) |
                                   ((uint32_t)buf[1] << 8) | buf[2], 24);
    int32_t traw = twos_complement(((uint32_t)buf[3] << 16) |
                                   ((uint32_t)buf[4] << 8) | buf[5], 24);

    const float praw_sc = (float)praw / SCALE_16X;
    const float traw_sc = (float)traw / SCALE_16X;

    const float pcomp =
        (float)g_c00 +
        praw_sc * ((float)g_c10 + praw_sc * ((float)g_c20 + praw_sc * (float)g_c30)) +
        traw_sc * (float)g_c01 +
        traw_sc * praw_sc * ((float)g_c11 + praw_sc * (float)g_c21);
    const float tcomp = (float)g_c0 * 0.5f + (float)g_c1 * traw_sc;

    // Sanity: DPS310 operating window
    if (pcomp < 20000.0f || pcomp > 130000.0f) return;
    if (tcomp < -50.0f || tcomp > 95.0f) return;

    g_pressure_pa = pcomp;
    g_temperature_c = tcomp;
    g_sample_ms = to_ms_since_boot(get_absolute_time());
    g_have_sample = true;
}

bool dps310_read(dps310_sample_t *out) {
    if (!out || !g_have_sample) return false;
    out->pressure_pa = g_pressure_pa;
    out->temperature_c = g_temperature_c;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    out->age_ms = now - g_sample_ms;
    return true;
}

float dps310_pressure_hpa(void) {
    return g_have_sample ? (g_pressure_pa * 0.01f) : 0.0f;
}

float dps310_temperature_c(void) {
    return g_have_sample ? g_temperature_c : 0.0f;
}

float dps310_altitude_m(void) {
    if (!g_have_sample) return 0.0f;
    // International barometric formula, QNH 1013.25 hPa
    float ratio = g_pressure_pa / SEA_LEVEL_PA;
    if (ratio <= 0.0f) return 0.0f;
    return 44330.0f * (1.0f - powf(ratio, 0.19029495f));
}

void dps310_set_rh(float rh_frac) {
    if (rh_frac < 0.0f) rh_frac = 0.0f;
    if (rh_frac > 1.0f) rh_frac = 1.0f;
    g_rh = rh_frac;
}

float dps310_rh(void) { return g_rh; }

float dps310_speed_of_sound_m_s(void) {
    if (!g_have_sample) return 0.0f;

    float T = g_temperature_c;
    float p = g_pressure_pa;
    // DPS310 operating window; reject nonsense before Arden-Buck / x_w.
    if (T < -40.0f) T = -40.0f;
    if (T > 85.0f) T = 85.0f;
    if (p < 30000.0f || p > 120000.0f) return 0.0f;

    // Step 1: Arden-Buck saturation vapour pressure (Pa), T in °C.
    float psat = 611.21f * expf((18.678f - T / 234.5f) * (T / (235.7f + T)));

    // Step 2: water-vapour mole fraction using DPS310 absolute pressure.
    float pv = g_rh * psat;
    float xw = pv / p;
    if (xw < 0.0f) xw = 0.0f;
    if (xw > 0.15f) xw = 0.15f; // physical sanity cap

    // Step 3: Cramer / NPL-style moist-air speed (CO2 ~420 ppm baseline 331.36).
    float tk = T + 273.15f;
    float c_dry = 331.36f * sqrtf(tk / 273.15f);
    float humid = 1.0f + 0.314f * xw + 0.037f * xw * xw;
    return c_dry * humid;
}

static void put_f1(float v) {
    if (v < 0.0f) { dbg_putc('-'); v = -v; }
    uint32_t ip = (uint32_t)v;
    uint32_t fp = (uint32_t)((v - (float)ip) * 10.0f + 0.5f);
    if (fp >= 10u) { ip++; fp = 0u; }
    dbg_putu32(ip);
    dbg_putc('.');
    dbg_putu32(fp);
}

static void put_f2(float v) {
    if (v < 0.0f) { dbg_putc('-'); v = -v; }
    uint32_t ip = (uint32_t)v;
    uint32_t fp = (uint32_t)((v - (float)ip) * 100.0f + 0.5f);
    if (fp >= 100u) { ip++; fp = 0u; }
    dbg_putu32(ip);
    dbg_putc('.');
    if (fp < 10u) dbg_putc('0');
    dbg_putu32(fp);
}

void dps310_dump_uart(void) {
    uint32_t lock = dbg_line_lock();
    dbg_puts("BARO");
    if (!g_ok) {
        dbg_puts(" avail=0\n");
        dbg_line_unlock(lock);
        return;
    }
    dbg_puts(" avail=1 addr=0x");
    dbg_puthex8(g_addr);
    dbg_puts(" pins=GP2/GP3");
    if (!g_have_sample) {
        dbg_puts(" (no sample yet)\n");
        dbg_line_unlock(lock);
        return;
    }
    dps310_sample_t s;
    (void)dps310_read(&s);
    dbg_puts(" P=");
    put_f1(s.pressure_pa * 0.01f);
    dbg_puts("hPa T=");
    put_f1(s.temperature_c);
    dbg_puts("C RH=");
    put_f1(dps310_rh() * 100.0f);
    dbg_puts("% alt=");
    put_f1(dps310_altitude_m());
    dbg_puts("m c=");
    put_f2(dps310_speed_of_sound_m_s());
    dbg_puts("m/s (Cramer/NPL) age_ms=");
    dbg_putu32(s.age_ms);
    dbg_putc('\n');
    dbg_line_unlock(lock);
}
