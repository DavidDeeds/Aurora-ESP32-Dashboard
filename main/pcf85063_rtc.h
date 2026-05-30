#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

/** Waveshare ESP32-S3-RS485-CAN: PCF85063A on I2C (GPIO39 SDA, GPIO38 SCL), addr 0x51. */

esp_err_t pcf85063_rtc_init(void);
bool pcf85063_rtc_present(void);

/** Read UTC calendar from RTC. Returns ESP_OK if time looks valid (not VL/OS garbage). */
esp_err_t pcf85063_rtc_read_utc(struct tm *out_tm);

/** Write UTC calendar to RTC (single burst per datasheet). */
esp_err_t pcf85063_rtc_write_utc(const struct tm *tm);

/** Convenience: read and convert to time_t (UTC). */
esp_err_t pcf85063_rtc_read_utc_epoch(time_t *out);

/** Set RTC from UTC epoch seconds. */
esp_err_t pcf85063_rtc_write_utc_epoch(time_t t);
