#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Must match `sample_t` layout in app_main.c. */
typedef struct {
    uint32_t uptime_ms;
    uint32_t unix_sec;
    float output_power_w;
    float grid_voltage_v;
    float grid_frequency_hz;
    float booster_temp_c;
    float inverter_temp_c;
    uint32_t energy_today_wh;
} daylog_sample_t;

esp_err_t daylog_init(void);
bool daylog_is_mounted(void);

/** Unmount LittleFS before erasing the daylog partition (factory reset). Safe if not mounted. */
void daylog_factory_reset_unmount(void);

/** Call after a sample is appended to the RAM ring (outside sample mutex). */
void daylog_on_retained_sample(const daylog_sample_t *s);

/**
 * Build full JSON body for GET /api/samples?today=1 when daylog is active.
 * `buf` must hold at least `buf_size` bytes (use DAYLOG_TODAY_JSON_BUF).
 */
esp_err_t daylog_format_today_json(uint32_t ring_sample_count, uint32_t ring_capacity, unsigned sample_interval_seconds,
                                   char *buf, size_t buf_size, int *out_len);

/** Human-readable row for the maintenance HTML table (plain text, escaped later if needed). */
void daylog_fill_maintenance_row(char *buf, size_t buf_size);

#define DAYLOG_TODAY_JSON_BUF (112 * 1024)
