#include "daylog.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *const TAG = "daylog";
static const char *const DAYLOG_BASE = "/daylog";
static const char *const PART_LABEL = "daylog";

static SemaphoreHandle_t s_mtx;
static bool s_mounted;

static time_t local_midnight(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return mktime(&tm);
}

static time_t floor_5min_bucket(time_t t)
{
    time_t d0 = local_midnight(t);
    return d0 + ((t - d0) / 300L) * 300L;
}

static void path_for_date(int y, int mo, int d, char *out, size_t outlen)
{
    snprintf(out, outlen, "%s/%04d%02d%02d.jsonl", DAYLOG_BASE, y, mo, d);
}

static void unlink_stale_files(int keep_y, int keep_mo, int keep_d)
{
    DIR *dir = opendir(DAYLOG_BASE);
    if (dir == NULL) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        int y = 0, mo = 0, d = 0;
        if (sscanf(ent->d_name, "%4d%2d%2d.jsonl", &y, &mo, &d) != 3) {
            continue;
        }
        if (y == keep_y && mo == keep_mo && d == keep_d) {
            continue;
        }
        char full[300];
        snprintf(full, sizeof(full), "%s/%s", DAYLOG_BASE, ent->d_name);
        if (unlink(full) == 0) {
            ESP_LOGI(TAG, "removed old log %s", full);
        }
    }
    closedir(dir);
}

static int format_sample_json(const daylog_sample_t *s, uint32_t unix_bucket, char *buf, size_t cap)
{
    return snprintf(
        buf, cap,
        "{\"uptime_ms\":%" PRIu32 ",\"unix_sec\":%" PRIu32 ",\"output_power_w\":%.1f,"
        "\"grid_voltage_v\":%.1f,\"grid_frequency_hz\":%.2f,"
        "\"booster_temp_c\":%.1f,\"inverter_temp_c\":%.1f,"
        "\"energy_today_wh\":%" PRIu32 ",\"energy_today_kwh\":%.3f}",
        s->uptime_ms, unix_bucket, (double)s->output_power_w, (double)s->grid_voltage_v, (double)s->grid_frequency_hz,
        (double)s->booster_temp_c, (double)s->inverter_temp_c, s->energy_today_wh, (double)s->energy_today_wh / 1000.0);
}

static time_t s_cur_bucket = (time_t)-1;
static daylog_sample_t s_bucket_last;
static bool s_have_bucket;
static int s_tracked_y = -1;
static int s_tracked_yd = -1;

esp_err_t daylog_init(void)
{
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = DAYLOG_BASE,
        .partition_label = PART_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        s_mounted = false;
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "LittleFS mounted at %s (partition %s)", DAYLOG_BASE, PART_LABEL);
    return ESP_OK;
}

bool daylog_is_mounted(void)
{
    return s_mounted;
}

void daylog_factory_reset_unmount(void)
{
    if (!s_mounted) {
        return;
    }
    esp_err_t err = esp_vfs_littlefs_unregister(PART_LABEL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "littlefs unregister failed: %s", esp_err_to_name(err));
    }
    s_mounted = false;
}

void daylog_on_retained_sample(const daylog_sample_t *s)
{
    if (!s_mounted || s_mtx == NULL || s->unix_sec == 0U) {
        return;
    }

    time_t ts = (time_t)s->unix_sec;
    struct tm tml;
    localtime_r(&ts, &tml);
    const int y = tml.tm_year + 1900;
    const int mo = tml.tm_mon + 1;
    const int d = tml.tm_mday;

    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }

    if (s_tracked_y < 0 || s_tracked_yd < 0 || tml.tm_year != s_tracked_y || tml.tm_yday != s_tracked_yd) {
        unlink_stale_files(y, mo, d);
        s_tracked_y = tml.tm_year;
        s_tracked_yd = tml.tm_yday;
        s_cur_bucket = (time_t)-1;
        s_have_bucket = false;
    }

    time_t bucket = floor_5min_bucket(ts);

    if (s_cur_bucket == (time_t)-1) {
        s_cur_bucket = bucket;
        s_bucket_last = *s;
        s_have_bucket = true;
        xSemaphoreGive(s_mtx);
        return;
    }

    if (bucket != s_cur_bucket) {
        char path[48];
        path_for_date(y, mo, d, path, sizeof(path));
        char line[384];
        int ln = format_sample_json(&s_bucket_last, (uint32_t)s_cur_bucket, line, sizeof(line));
        if (ln > 0 && (size_t)ln < sizeof(line)) {
            FILE *f = fopen(path, "a");
            if (f != NULL) {
                fputs(line, f);
                fputc('\n', f);
                fclose(f);
            } else {
                ESP_LOGW(TAG, "append open failed %s errno=%d", path, errno);
            }
        }
        s_cur_bucket = bucket;
        s_bucket_last = *s;
        s_have_bucket = true;
        xSemaphoreGive(s_mtx);
        return;
    }

    s_bucket_last = *s;
    xSemaphoreGive(s_mtx);
}

static bool append_live_tail(char *buf, size_t cap, int *pos, unsigned *returned, bool need_comma)
{
    if (!s_have_bucket || s_cur_bucket == (time_t)-1) {
        return need_comma;
    }
    char line[384];
    int ln = format_sample_json(&s_bucket_last, (uint32_t)s_cur_bucket, line, sizeof(line));
    if (ln <= 0 || (size_t)ln >= sizeof(line)) {
        return need_comma;
    }
    int p = *pos;
    int add = snprintf(buf + p, cap - (size_t)p, "%s%s", need_comma ? "," : "", line);
    if (add < 0 || p + add >= (int)cap) {
        return need_comma;
    }
    *pos = p + add;
    (*returned)++;
    return true;
}

esp_err_t daylog_format_today_json(uint32_t ring_sample_count, uint32_t ring_capacity, unsigned sample_interval_seconds,
                                   char *buf, size_t buf_size, int *out_len)
{
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (buf == NULL || buf_size < 512U) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now = time(NULL);
    struct tm tnow;
    localtime_r(&now, &tnow);
    const int y = tnow.tm_year + 1900;
    const int mo = tnow.tm_mon + 1;
    const int d = tnow.tm_mday;
    char path[48];
    path_for_date(y, mo, d, path, sizeof(path));

    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    unsigned returned = 0;
    int offset = snprintf(
        buf, buf_size,
        "{\"ok\":true,\"sample_count\":%" PRIu32 ",\"sample_capacity\":%" PRIu32 ","
        "\"sample_interval_seconds\":%u,\"chart_bucket_seconds\":300,"
        "\"today_only\":true,\"daylog\":true,\"samples\":[",
        ring_sample_count, ring_capacity, sample_interval_seconds);

    if (offset < 0 || offset >= (int)buf_size) {
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "r");
    bool need_comma = false;
    if (f != NULL) {
        char line[384];
        while (fgets(line, sizeof(line), f) != NULL) {
            size_t n = strlen(line);
            while (n > 0U && (line[n - 1U] == '\n' || line[n - 1U] == '\r')) {
                line[--n] = '\0';
            }
            if (n == 0U) {
                continue;
            }
            int add = snprintf(buf + offset, buf_size - (size_t)offset, "%s%s", need_comma ? "," : "", line);
            if (add < 0 || offset + add >= (int)buf_size) {
                fclose(f);
                xSemaphoreGive(s_mtx);
                return ESP_ERR_NO_MEM;
            }
            offset += add;
            returned++;
            need_comma = true;
        }
        fclose(f);
    }

    if (append_live_tail(buf, buf_size, &offset, &returned, need_comma)) {
        need_comma = true;
    }

    int tail = snprintf(buf + offset, buf_size - (size_t)offset,
                        "],\"returned\":%u,\"source\":\"littlefs\"}\n", returned);
    if (tail < 0 || offset + tail >= (int)buf_size) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NO_MEM;
    }
    offset += tail;

    xSemaphoreGive(s_mtx);

    if (out_len != NULL) {
        *out_len = offset;
    }
    return ESP_OK;
}

void daylog_fill_maintenance_row(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0U) {
        return;
    }
    if (!s_mounted) {
        snprintf(buf, buf_size, "LittleFS partition \"%s\" not mounted", PART_LABEL);
        return;
    }

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PART_LABEL);
    size_t total = 0;
    size_t used = 0;
    esp_err_t inf = esp_littlefs_info(PART_LABEL, &total, &used);
    if (part == NULL) {
        snprintf(buf, buf_size, "Mounted at %s; partition lookup failed", DAYLOG_BASE);
        return;
    }
    if (inf != ESP_OK) {
        snprintf(buf, buf_size,
                 "Mounted at %s; partition ~%" PRIu32 " KiB (info call failed: %s)", DAYLOG_BASE,
                 (uint32_t)(part->size / 1024U), esp_err_to_name(inf));
        return;
    }

    uint32_t tot_kb = (uint32_t)(total / 1024U);
    uint32_t used_kb = (uint32_t)(used / 1024U);
    uint32_t free_kb = (total > used) ? (uint32_t)((total - used) / 1024U) : 0U;
    snprintf(buf, buf_size,
             "Mounted at %s; partition ~%" PRIu32 " KiB; LittleFS used ~%" PRIu32 " KiB, free ~%" PRIu32 " KiB",
             DAYLOG_BASE, tot_kb, used_kb, free_kb);
}
