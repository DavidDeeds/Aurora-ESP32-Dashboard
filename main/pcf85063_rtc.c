#include "pcf85063_rtc.h"

#include <string.h>
#include <time.h>

#include "driver/i2c.h"
#include "esp_log.h"

#define TAG "pcf85063"

#define PCF85063_ADDR 0x51
#define PCF_I2C_PORT I2C_NUM_0
#define PCF_GPIO_SDA 39
#define PCF_GPIO_SCL 38

#define REG_CTRL1 0x00U
#define REG_SECONDS 0x04U

#define CTRL1_STOP (1U << 5)
#define CTRL1_12H (1U << 1) /* 1 = 12-hour mode; 0 = 24-hour (NXP Table 5) */

static bool s_present;
static bool s_i2c_installed;

static uint8_t bcd_encode(uint8_t v)
{
    return (uint8_t)(((v / 10U) << 4) | (v % 10U));
}

static uint8_t bcd_decode(uint8_t v)
{
    return (uint8_t)(((v >> 4) & 0x0FU) * 10U + (v & 0x0FU));
}

static esp_err_t i2c_write_reg(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[16];
    if (len + 1U > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_write_to_device(PCF_I2C_PORT, PCF85063_ADDR, buf, len + 1, pdMS_TO_TICKS(80));
}

static esp_err_t i2c_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(PCF_I2C_PORT, PCF85063_ADDR, &reg, 1, data, len, pdMS_TO_TICKS(80));
}

esp_err_t pcf85063_rtc_init(void)
{
    s_present = false;
    if (s_i2c_installed) {
        return ESP_OK;
    }

    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PCF_GPIO_SDA,
        .scl_io_num = PCF_GPIO_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };

    esp_err_t err = i2c_param_config(PCF_I2C_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(PCF_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    s_i2c_installed = true;

    uint8_t probe = 0;
    err = i2c_read_regs(REG_CTRL1, &probe, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 not responding: %s", esp_err_to_name(err));
        return err;
    }

    /* 24-hour mode (clear 12_24), clear STOP, do not trigger soft reset. */
    uint8_t c1 = (uint8_t)(probe & (uint8_t)~CTRL1_12H);
    c1 = (uint8_t)(c1 & (uint8_t)~CTRL1_STOP);
    err = i2c_write_reg(REG_CTRL1, &c1, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 Control_1 write failed: %s", esp_err_to_name(err));
        return err;
    }

    s_present = true;
    ESP_LOGI(TAG, "PCF85063 RTC detected (I2C 0x%02x)", PCF85063_ADDR);
    return ESP_OK;
}

bool pcf85063_rtc_present(void)
{
    return s_present;
}

esp_err_t pcf85063_rtc_read_utc(struct tm *out_tm)
{
    if (!s_present || out_tm == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[7];
    esp_err_t err = i2c_read_regs(REG_SECONDS, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t sec_bcd = (uint8_t)(raw[0] & 0x7FU);
    const bool os = (raw[0] & 0x80U) != 0; /* oscillator stop / integrity */
    if (os) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out_tm, 0, sizeof(*out_tm));
    out_tm->tm_sec = bcd_decode(sec_bcd);
    out_tm->tm_min = bcd_decode((uint8_t)(raw[1] & 0x7FU));
    out_tm->tm_hour = bcd_decode((uint8_t)(raw[2] & 0x3FU));
    out_tm->tm_mday = bcd_decode((uint8_t)(raw[3] & 0x3FU));
    out_tm->tm_wday = (int)(raw[4] & 0x07U);
    out_tm->tm_mon = (int)bcd_decode((uint8_t)(raw[5] & 0x1FU)) - 1;
    out_tm->tm_year = (int)bcd_decode(raw[6]) + 100; /* 2000–2099 */

    if (out_tm->tm_year < 124 || out_tm->tm_year > 199 || out_tm->tm_mon < 0 || out_tm->tm_mon > 11 ||
        out_tm->tm_mday < 1 || out_tm->tm_mday > 31 || out_tm->tm_hour > 23 || out_tm->tm_min > 59 ||
        out_tm->tm_sec > 59) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    out_tm->tm_isdst = 0;
    return ESP_OK;
}

esp_err_t pcf85063_rtc_read_utc_epoch(time_t *out)
{
    struct tm tm;
    esp_err_t err = pcf85063_rtc_read_utc(&tm);
    if (err != ESP_OK) {
        return err;
    }
    time_t t = timegm(&tm);
    if (t == (time_t)-1) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out = t;
    return ESP_OK;
}

esp_err_t pcf85063_rtc_write_utc(const struct tm *tm)
{
    if (!s_present || tm == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t c1 = 0;
    esp_err_t err = i2c_read_regs(REG_CTRL1, &c1, 1);
    if (err != ESP_OK) {
        return err;
    }

    c1 = (uint8_t)((c1 & (uint8_t)~CTRL1_12H) | CTRL1_STOP);
    err = i2c_write_reg(REG_CTRL1, &c1, 1);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t blk[7];
    blk[0] = (uint8_t)(bcd_encode((uint8_t)tm->tm_sec) & 0x7FU);
    blk[1] = (uint8_t)(bcd_encode((uint8_t)tm->tm_min) & 0x7FU);
    blk[2] = (uint8_t)(bcd_encode((uint8_t)tm->tm_hour) & 0x3FU);
    blk[3] = (uint8_t)(bcd_encode((uint8_t)tm->tm_mday) & 0x3FU);
    blk[4] = (uint8_t)(tm->tm_wday & 0x07);
    blk[5] = (uint8_t)(bcd_encode((uint8_t)(tm->tm_mon + 1)) & 0x1FU);
    blk[6] = bcd_encode((uint8_t)((tm->tm_year + 1900) % 100));

    err = i2c_write_reg(REG_SECONDS, blk, sizeof(blk));
    if (err != ESP_OK) {
        (void)i2c_read_regs(REG_CTRL1, &c1, 1);
        c1 = (uint8_t)(c1 & (uint8_t)~CTRL1_STOP);
        (void)i2c_write_reg(REG_CTRL1, &c1, 1);
        return err;
    }

    err = i2c_read_regs(REG_CTRL1, &c1, 1);
    if (err == ESP_OK) {
        c1 = (uint8_t)(c1 & (uint8_t)~CTRL1_STOP);
        err = i2c_write_reg(REG_CTRL1, &c1, 1);
    }
    return err;
}

esp_err_t pcf85063_rtc_write_utc_epoch(time_t t)
{
    struct tm tm;
    if (gmtime_r(&t, &tm) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return pcf85063_rtc_write_utc(&tm);
}
