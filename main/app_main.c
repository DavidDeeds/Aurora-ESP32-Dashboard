#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_partition.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "lwip/inet.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "aurora_favicon_data.h"
#include "chart_umd_min_js.h"
#include "inverter_status_labels.h"
#include "pcf85063_rtc.h"
#include "daylog.h"

#define APP_VERSION "0.3.0"
#define FIRMWARE_BUILD_TIMESTAMP __DATE__ " " __TIME__
#define FACTORY_ADMIN_USER "Admin"
#define FACTORY_ADMIN_PASSWORD "Password"
#define DEFAULT_TZ_ID "Australia/Perth"
#define DEFAULT_TZ_POSIX "AWST-8"
#define DEFAULT_NTP_SERVER "au.pool.ntp.org"
#define PROVISIONING_AP_IP "192.168.4.1"
#define PROVISIONING_AP_NETMASK "255.255.255.0"
#define PROVISIONING_AP_MAX_CONNECTIONS 4
#define PROVISIONING_TIMEOUT_MS (30U * 60U * 1000U)
#define FORM_BODY_MAX_LEN 1536
#define ADMIN_PASSWORD_MIN_LEN 8U
#define ADMIN_PASSWORD_MAX_LEN 64U
#define ADMIN_PBKDF2_ITERATIONS 10000U
#define ADMIN_PBKDF2_ITERATIONS_LEGACY 20000U
/** Survives esp_restart(); triggers partition erase on next boot from app_main (internal stack). */
#define FACTORY_RESET_RTC_MAGIC 0xA0FA5C01U
#define ADMIN_SALT_LEN 16U
#define ADMIN_HASH_LEN 32U
#define SESSION_COOKIE_NAME "aurora_sid"
#define SESSION_TOKEN_BYTES 32U
#define SESSION_HEX_LEN ((SESSION_TOKEN_BYTES)*2U)
#define SESSION_IDLE_TIMEOUT_MS (10U * 60U * 1000U)
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASSWORD_MAX_LEN 64
#define RS485_UART_NUM UART_NUM_1
#define RS485_TX_GPIO 17
#define RS485_RX_GPIO 18
#define RS485_DE_GPIO 21
#define RS485_BAUD_RATE 19200
#define RS485_DEFAULT_INVERTER_ADDRESS 2
#define AURORA_TIME_READ_OPCODE 70
#define AURORA_TIME_SET_OPCODE 71
#define AURORA_TIME_BASE_UNIX 946684800L
#define RS485_POLL_INTERVAL_MS 10000
#define RS485_OFFLINE_FAILURES 5
#define AURORA_STATE_OPCODE 50
#define AURORA_DSP_OPCODE 59
#define AURORA_CE_OPCODE 78
#define AURORA_STATE_OK 0
#define AURORA_FRAME_LEN 10
#define AURORA_RESPONSE_LEN 8
#define AURORA_CRC_POLY 0x8408
#define AUTH_MAX_TRACKED_CLIENTS 4
#define AUTH_FAILURE_LOCK_THRESHOLD 5
#define AUTH_LOCKOUT_MS (5U * 60U * 1000U)
#define SAMPLE_INTERVAL_SECONDS 10U
#define SAMPLE_RING_TARGET_COUNT ((2U * 24U * 60U * 60U) / SAMPLE_INTERVAL_SECONDS)
#define SAMPLE_RING_FALLBACK_COUNT 720U
#define SAMPLES_API_DEFAULT_LIMIT 120U
#define SAMPLES_API_MAX_LIMIT 320U
#define DASHBOARD_REFRESH_SECONDS 10U
#define PVOUTPUT_DEFAULT_BASE_URL "http://pvoutput.org/service/r2/"
#define PVOUTPUT_BASE_URL_MAX_LEN 96
#define PVOUTPUT_SYSTEM_ID_MAX_LEN 16
#define PVOUTPUT_API_KEY_MAX_LEN 48
#define NTP_SERVER_MAX_LEN 64
#define TZ_ID_MAX_LEN 32
#define TZ_POSIX_MAX_LEN 32
#define PVOUTPUT_INTERVAL_OFF 0
#define PVOUTPUT_INTERVAL_1_MIN 1
#define PVOUTPUT_INTERVAL_2_MIN 2
#define PVOUTPUT_INTERVAL_3_MIN 3
#define PVOUTPUT_INTERVAL_4_MIN 4
#define PVOUTPUT_INTERVAL_5_MIN 5
#define PVOUTPUT_INTERVAL_10_MIN 10
#define PVOUTPUT_INTERVAL_15_MIN 15
#define PVOUTPUT_FLAG_INSTANT_POWER 0x01
#define PVOUTPUT_FLAG_GRID_VOLTAGE 0x02
#define PVOUTPUT_FLAG_TEMPERATURE 0x04
#define PVOUTPUT_HTTP_TIMEOUT_MS 30000
#define CPU_FREQ_MHZ_DEFAULT 160U
#define CPU_FREQ_MHZ_MIN 80U
#define CPU_FREQ_MHZ_MAX 240U

typedef struct {
    uint32_t ip;
    uint8_t failures;
    uint32_t locked_until_ms;
} auth_client_state_t;

typedef struct {
    uint32_t uptime_ms;
    uint32_t unix_sec;
    float output_power_w;
    float grid_voltage_v;
    float grid_frequency_hz;
    float booster_temp_c;
    float inverter_temp_c;
    uint32_t energy_today_wh;
} sample_t;

typedef struct {
    char base_url[PVOUTPUT_BASE_URL_MAX_LEN];
    char system_id[PVOUTPUT_SYSTEM_ID_MAX_LEN];
    char api_key[PVOUTPUT_API_KEY_MAX_LEN];
    uint8_t interval_minutes;
    uint8_t flags;
} pvoutput_config_t;

static const char *TAG = "aurora";
static char s_provisioning_ssid[sizeof("aurora-xxxxxx")];
static char s_hostname[sizeof("aurora-xxxxxx")];
static char s_sta_ssid[WIFI_SSID_MAX_LEN + 1];
static char s_sta_ip[16];
static char s_ntp_server[NTP_SERVER_MAX_LEN] = DEFAULT_NTP_SERVER;
static char s_tz_id[TZ_ID_MAX_LEN] = DEFAULT_TZ_ID;
static char s_tz_posix[TZ_POSIX_MAX_LEN] = DEFAULT_TZ_POSIX;
static httpd_handle_t s_http_server = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_provisioning_active = false;
static bool s_sta_has_config = false;
static bool s_sta_connected = false;
static bool s_mdns_started = false;
static bool s_sntp_started = false;
static bool s_sntp_synced = false;
static bool s_softap_disable_pending = false;
static bool s_rs485_ready = false;
static bool s_inverter_offline = true;
static uint32_t s_rs485_poll_count = 0;
static uint32_t s_rs485_ok_count = 0;
static uint32_t s_rs485_fail_count = 0;
static uint32_t s_rs485_consecutive_failures = 0;
static uint32_t s_rs485_last_poll_ms = 0;
static uint32_t s_rs485_last_ok_ms = 0;
static int s_rs485_last_result = 0;
static bool s_live_metrics_valid = false;
static float s_output_power_w = 0.0f;
static float s_grid_voltage_v = 0.0f;
static float s_grid_frequency_hz = 0.0f;
static float s_booster_temp_c = 0.0f;
static float s_inverter_temp_c = 0.0f;
static uint32_t s_energy_today_wh = 0;
static int s_live_metrics_last_result = 0;
/** esp_log_timestamp() ms of last successful live-metrics RS485 read (for PVOutput freshness). */
static uint32_t s_live_metrics_last_ok_ms = 0;
static uint32_t s_auth_failed_total = 0;
static uint32_t s_auth_lockout_total = 0;
static auth_client_state_t s_auth_clients[AUTH_MAX_TRACKED_CLIENTS] = {0};
static uint8_t s_admin_salt[ADMIN_SALT_LEN];
static uint8_t s_admin_hash[ADMIN_HASH_LEN];
static bool s_admin_custom_password = false;
static uint32_t s_admin_pbkdf2_iterations = ADMIN_PBKDF2_ITERATIONS;
static pvoutput_config_t s_pvoutput_config = {0};
static bool s_pvoutput_config_loaded = false;
static int s_pvoutput_last_result = 0;
static uint32_t s_pvoutput_last_attempt_ms = 0;
static uint32_t s_pvoutput_last_success_ms = 0;
static uint32_t s_pvoutput_success_count = 0;
static uint32_t s_pvoutput_fail_count = 0;
static int64_t s_pvoutput_last_live_slot = -1;
/** Wall-clock time of last successful addstatus POST to PVOutput (0 = never). */
static time_t s_pvoutput_last_post_unix = 0;
static uint8_t s_inverter_state[AURORA_RESPONSE_LEN] = {0};
static sample_t *s_samples = NULL;
static size_t s_sample_capacity = 0;
static size_t s_sample_count = 0;
static size_t s_sample_write_index = 0;
static SemaphoreHandle_t s_sample_mutex = NULL;
static char s_root_html[24000];
static char s_dashboard_html[32768];
static char s_status_json[12288];
static char s_samples_json[DAYLOG_TODAY_JSON_BUF];
static char s_aux_html[16384];
static char s_config_json[1536];
static uint32_t s_provisioning_started_ms = 0;
static SemaphoreHandle_t s_session_mutex;
static bool s_session_active;
static uint8_t s_session_token[SESSION_TOKEN_BYTES];
static uint32_t s_session_last_activity_ms;
static uint8_t s_inverter_address = RS485_DEFAULT_INVERTER_ADDRESS;
static SemaphoreHandle_t s_rs485_mutex;
static bool s_use_sta_static_ip;
static char s_sta_static_ip[16];
static char s_sta_static_gw[16];
static char s_sta_static_nm[16];
static char s_sta_static_dns[16];
#define STA_HOSTNAME_MAX_LEN 32
static char s_sta_hostname[STA_HOSTNAME_MAX_LEN + 1];
#define WEB_LOG_CAP 8192U
static char s_web_log[WEB_LOG_CAP];
static size_t s_web_log_len;
static SemaphoreHandle_t s_web_log_mutex;
static vprintf_like_t s_prev_log_vprintf;
static uint8_t s_cpu_freq_mhz = CPU_FREQ_MHZ_DEFAULT;
static bool s_psa_crypto_ready = false;
RTC_NOINIT_ATTR static uint32_t s_factory_reset_rtc_magic;

static void start_mdns_service(void);
static void start_sntp_service(void);
static void aurora_poll_task(void *arg);
static void delayed_restart_task(void *arg);
static void factory_erase_data_partition(const char *label, esp_partition_subtype_t subtype);
static void perform_factory_reset_erase(void);
static void try_handle_pending_factory_reset_at_boot(void);
static bool schedule_factory_reset_after_response(void);
static esp_err_t init_psa_crypto_once(void);
static esp_err_t apply_cpu_frequency(uint8_t mhz);
static esp_err_t load_cpu_freq_from_nvs(void);
static esp_err_t save_cpu_freq_mhz(uint8_t mhz);
static bool cpu_freq_mhz_is_valid(uint8_t mhz);
static void disable_softap_after_sta_task(void *arg);
static esp_err_t start_provisioning_http_server(void);
static esp_err_t dashboard_get_handler(httpd_req_t *req);
static void init_sample_ring(void);
static int aurora_poll_value(uint8_t address, uint8_t opcode, uint8_t param, uint8_t response[AURORA_RESPONSE_LEN]);
static uint32_t aurora_u32_from_response(const uint8_t response[AURORA_RESPONSE_LEN]);
static int aurora_write_time(uint8_t address, uint32_t inv_raw);
static int aurora_partial_reset(uint8_t address);
static void ensure_web_log_mutex(void);
static bool rs485_bus_take(TickType_t ticks);
static void rs485_bus_give(void);
static long inverter_gmt_offset_seconds(void);
static esp_err_t save_rs485_address(uint8_t address);
static void load_net_settings(void);
static esp_err_t save_net_settings(bool use_static, const char *ip, const char *gw, const char *mask, const char *dns,
                                   const char *hostname);

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase before first use: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static void apply_default_timezone(void)
{
    setenv("TZ", s_tz_posix, 1);
    tzset();
    ESP_LOGI(TAG, "timezone applied: %s (%s)", s_tz_id, s_tz_posix);
}

static bool timezone_lookup(const char *id, const char **posix)
{
    if (strcmp(id, "Australia/Perth") == 0) {
        *posix = "AWST-8";
        return true;
    }
    if (strcmp(id, "Australia/Sydney") == 0) {
        *posix = "AEST-10AEDT,M10.1.0,M4.1.0/3";
        return true;
    }
    if (strcmp(id, "Pacific/Auckland") == 0) {
        *posix = "NZST-12NZDT,M9.5.0/2,M4.1.0/3";
        return true;
    }
    if (strcmp(id, "Asia/Tokyo") == 0) {
        *posix = "JST-9";
        return true;
    }
    if (strcmp(id, "Asia/Singapore") == 0) {
        *posix = "SGT-8";
        return true;
    }
    if (strcmp(id, "Asia/Shanghai") == 0) {
        *posix = "CST-8";
        return true;
    }
    if (strcmp(id, "Europe/London") == 0) {
        *posix = "GMT0BST,M3.5.0/1,M10.5.0/3";
        return true;
    }
    if (strcmp(id, "Europe/Paris") == 0) {
        *posix = "CET-1CEST,M3.5.0/2,M10.5.0/3";
        return true;
    }
    if (strcmp(id, "Europe/Berlin") == 0) {
        *posix = "CET-1CEST,M3.5.0/2,M10.5.0/3";
        return true;
    }
    if (strcmp(id, "America/New_York") == 0) {
        *posix = "EST5EDT,M3.2.0,M11.1.0";
        return true;
    }
    if (strcmp(id, "America/Chicago") == 0) {
        *posix = "CST6CDT,M3.2.0,M11.1.0";
        return true;
    }
    if (strcmp(id, "America/Denver") == 0) {
        *posix = "MST7MDT,M3.2.0,M11.1.0";
        return true;
    }
    if (strcmp(id, "America/Los_Angeles") == 0) {
        *posix = "PST8PDT,M3.2.0,M11.1.0";
        return true;
    }
    if (strcmp(id, "America/Phoenix") == 0) {
        *posix = "MST7";
        return true;
    }
    if (strcmp(id, "UTC") == 0) {
        *posix = "UTC0";
        return true;
    }
    return false;
}

static const char *tz_opt_sel(const char *id)
{
    return strcmp(s_tz_id, id) == 0 ? " selected" : "";
}

static esp_err_t load_device_settings(void)
{
    strlcpy(s_ntp_server, DEFAULT_NTP_SERVER, sizeof(s_ntp_server));
    strlcpy(s_tz_id, DEFAULT_TZ_ID, sizeof(s_tz_id));
    strlcpy(s_tz_posix, DEFAULT_TZ_POSIX, sizeof(s_tz_posix));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("device_cfg", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    char tz_id[TZ_ID_MAX_LEN] = {0};
    size_t len = sizeof(s_ntp_server);
    (void)nvs_get_str(nvs, "ntp_server", s_ntp_server, &len);
    len = sizeof(tz_id);
    if (nvs_get_str(nvs, "tz_id", tz_id, &len) == ESP_OK) {
        const char *posix = NULL;
        if (timezone_lookup(tz_id, &posix)) {
            strlcpy(s_tz_id, tz_id, sizeof(s_tz_id));
            strlcpy(s_tz_posix, posix, sizeof(s_tz_posix));
        }
    }

    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t save_device_settings(const char *ntp_server, const char *tz_id)
{
    const char *posix = NULL;
    if (ntp_server[0] == '\0' ||
        strlen(ntp_server) >= NTP_SERVER_MAX_LEN ||
        !timezone_lookup(tz_id, &posix)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("device_cfg", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    if ((err = nvs_set_str(nvs, "ntp_server", ntp_server)) == ESP_OK &&
        (err = nvs_set_str(nvs, "tz_id", tz_id)) == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        strlcpy(s_ntp_server, ntp_server, sizeof(s_ntp_server));
        strlcpy(s_tz_id, tz_id, sizeof(s_tz_id));
        strlcpy(s_tz_posix, posix, sizeof(s_tz_posix));
        apply_default_timezone();
    }
    return err;
}

static bool cpu_freq_mhz_is_valid(uint8_t mhz)
{
    return mhz == 80U || mhz == 160U || mhz == 240U;
}

static esp_err_t load_cpu_freq_from_nvs(void)
{
    s_cpu_freq_mhz = CPU_FREQ_MHZ_DEFAULT;
    nvs_handle_t nvs;
    if (nvs_open("device_cfg", NVS_READONLY, &nvs) != ESP_OK) {
        return ESP_OK;
    }
    uint8_t mhz = 0;
    if (nvs_get_u8(nvs, "cpu_mhz", &mhz) == ESP_OK && cpu_freq_mhz_is_valid(mhz)) {
        s_cpu_freq_mhz = mhz;
    }
    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t save_cpu_freq_mhz(uint8_t mhz)
{
    if (!cpu_freq_mhz_is_valid(mhz)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("device_cfg", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(nvs, "cpu_mhz", mhz);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        s_cpu_freq_mhz = mhz;
    }
    return err;
}

static esp_err_t apply_cpu_frequency(uint8_t mhz)
{
    if (!cpu_freq_mhz_is_valid(mhz)) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = {
        .max_freq_mhz = (int)mhz,
        .min_freq_mhz = (int)mhz,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure(%u MHz) failed: %s", (unsigned)mhz, esp_err_to_name(err));
        return err;
    }
    s_cpu_freq_mhz = mhz;
    ESP_LOGI(TAG, "CPU frequency configured to %u MHz", (unsigned)mhz);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "PM not enabled; CPU frequency remains at build default");
    (void)mhz;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t init_psa_crypto_once(void)
{
    if (s_psa_crypto_ready) {
        return ESP_OK;
    }
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)st);
        return ESP_FAIL;
    }
    s_psa_crypto_ready = true;
    return ESP_OK;
}

static void pvoutput_apply_defaults(pvoutput_config_t *config)
{
    memset(config, 0, sizeof(*config));
    strlcpy(config->base_url, PVOUTPUT_DEFAULT_BASE_URL, sizeof(config->base_url));
    config->interval_minutes = PVOUTPUT_INTERVAL_5_MIN;
    config->flags = PVOUTPUT_FLAG_INSTANT_POWER | PVOUTPUT_FLAG_GRID_VOLTAGE | PVOUTPUT_FLAG_TEMPERATURE;
}

static bool pvoutput_interval_is_valid(uint8_t interval_minutes)
{
    return interval_minutes == PVOUTPUT_INTERVAL_OFF ||
           interval_minutes == PVOUTPUT_INTERVAL_1_MIN ||
           interval_minutes == PVOUTPUT_INTERVAL_2_MIN ||
           interval_minutes == PVOUTPUT_INTERVAL_3_MIN ||
           interval_minutes == PVOUTPUT_INTERVAL_4_MIN ||
           interval_minutes == PVOUTPUT_INTERVAL_5_MIN ||
           interval_minutes == PVOUTPUT_INTERVAL_10_MIN ||
           interval_minutes == PVOUTPUT_INTERVAL_15_MIN;
}

static bool pvoutput_config_is_ready(void)
{
    return s_pvoutput_config.interval_minutes != PVOUTPUT_INTERVAL_OFF &&
           s_pvoutput_config.system_id[0] != '\0' &&
           s_pvoutput_config.api_key[0] != '\0';
}

static esp_err_t load_pvoutput_config(void)
{
    pvoutput_apply_defaults(&s_pvoutput_config);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("pvoutput", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_pvoutput_config_loaded = true;
        ESP_LOGI(TAG, "PVOutput config not found; defaults applied");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(s_pvoutput_config.base_url);
    (void)nvs_get_str(nvs, "base_url", s_pvoutput_config.base_url, &len);
    len = sizeof(s_pvoutput_config.system_id);
    (void)nvs_get_str(nvs, "system_id", s_pvoutput_config.system_id, &len);
    len = sizeof(s_pvoutput_config.api_key);
    (void)nvs_get_str(nvs, "api_key", s_pvoutput_config.api_key, &len);
    uint8_t interval = s_pvoutput_config.interval_minutes;
    if (nvs_get_u8(nvs, "interval", &interval) == ESP_OK && pvoutput_interval_is_valid(interval)) {
        s_pvoutput_config.interval_minutes = interval;
    }
    uint8_t flags = s_pvoutput_config.flags;
    if (nvs_get_u8(nvs, "flags", &flags) == ESP_OK) {
        s_pvoutput_config.flags = flags;
    }

    nvs_close(nvs);
    s_pvoutput_config_loaded = true;
    ESP_LOGI(TAG, "PVOutput config loaded: configured=%s interval=%u",
             pvoutput_config_is_ready() ? "true" : "false", s_pvoutput_config.interval_minutes);
    return ESP_OK;
}

static void log_board_identity(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    uint8_t mac[6] = {0};

    esp_chip_info(&chip_info);
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));

    esp_err_t flash_err = esp_flash_get_size(NULL, &flash_size);
    if (flash_err != ESP_OK) {
        ESP_LOGW(TAG, "flash size read failed: %s", esp_err_to_name(flash_err));
    }

    ESP_LOGI(TAG, "Aurora ESP32 appliance %s", APP_VERSION);
    ESP_LOGI(TAG, "Wi-Fi MAC %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "provisioning hostname/SSID suffix: aurora-%02x%02x%02x",
             mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "chip cores=%d revision=%d features=0x%08" PRIx32,
             chip_info.cores, chip_info.revision, chip_info.features);
    if (flash_err == ESP_OK) {
        ESP_LOGI(TAG, "detected flash size: %" PRIu32 " MB", flash_size / (1024 * 1024));
    }
    ESP_LOGI(TAG, "reset reason: %d", esp_reset_reason());
}

static void set_device_identity_from_mac(void)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_hostname, sizeof(s_hostname), "aurora-%02x%02x%02x", mac[3], mac[4], mac[5]);
    strlcpy(s_provisioning_ssid, s_hostname, sizeof(s_provisioning_ssid));
}

static bool wifi_credentials_present(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    size_t ssid_len = 0;
    err = nvs_get_str(nvs, "ssid", NULL, &ssid_len);
    nvs_close(nvs);

    return err == ESP_OK && ssid_len > 1;
}

static esp_err_t load_wifi_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(nvs, "ssid", ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, "password", password, &password_len);
    }

    nvs_close(nvs);
    return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "setup AP client joined: " MACSTR ", AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "setup AP client left: " MACSTR ", AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        s_sta_ip[0] = '\0';
        if (s_sta_has_config) {
            s_provisioning_started_ms = esp_log_timestamp();
            ESP_LOGW(TAG, "STA disconnected from '%s'; retrying saved Wi-Fi connection", s_sta_ssid);
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_connected = true;
        s_provisioning_started_ms = esp_log_timestamp();
        ESP_LOGI(TAG, "STA connected to '%s'", s_sta_ssid);
        ESP_LOGI(TAG, "STA got LAN IP: %s", s_sta_ip);
        if (s_sta_netif != NULL) {
            esp_netif_set_default_netif(s_sta_netif);
        }
        ESP_LOGI(TAG, "open the device from your normal Wi-Fi at: http://%s/", s_sta_ip);
        start_mdns_service();
        start_sntp_service();
        if (s_provisioning_active && !s_softap_disable_pending) {
            s_softap_disable_pending = true;
            xTaskCreate(disable_softap_after_sta_task, "disableSoftAP", 2048, NULL, 5, NULL);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        ip_event_assigned_ip_to_client_t *event = (ip_event_assigned_ip_to_client_t *)event_data;
        ESP_LOGI(TAG, "setup AP assigned client IP: " IPSTR, IP2STR(&event->ip));
    }
}

static void sntp_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    const esp_netif_sntp_time_sync_t *event = (const esp_netif_sntp_time_sync_t *)event_data;
    time_t now = event != NULL ? event->tv.tv_sec : 0;
    if (now <= 0) {
        time(&now);
    }

    char local_time[64] = {0};
    struct tm timeinfo = {0};
    localtime_r(&now, &timeinfo);
    strftime(local_time, sizeof(local_time), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);

    s_sntp_synced = true;
    ESP_LOGI(TAG, "SNTP time synced: %s", local_time);

    if (pcf85063_rtc_present()) {
        time_t utc = 0;
        time(&utc);
        time_t rtc_epoch = 0;
        esp_err_t rrtc = pcf85063_rtc_read_utc_epoch(&rtc_epoch);
        if (rrtc != ESP_OK || llabs((long long)(utc - rtc_epoch)) > 2) {
            if (pcf85063_rtc_write_utc_epoch(utc) == ESP_OK) {
                ESP_LOGI(TAG, "PCF85063 RTC updated from SNTP");
            }
        }
    }
}

static void init_network_stack(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, sntp_event_handler, NULL));
}

static int hex_digit_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode_component(const char *src, size_t src_len, char *dst, size_t dst_len)
{
    size_t di = 0;

    for (size_t si = 0; si < src_len && di + 1 < dst_len; ++si) {
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (src[si] == '%' && si + 2 < src_len) {
            int high = hex_digit_value(src[si + 1]);
            int low = hex_digit_value(src[si + 2]);
            if (high >= 0 && low >= 0) {
                dst[di++] = (char)((high << 4) | low);
                si += 2;
            } else {
                dst[di++] = src[si];
            }
        } else {
            dst[di++] = src[si];
        }
    }

    dst[di] = '\0';
}

static bool extract_form_value(const char *body, const char *name, char *out, size_t out_len)
{
    const size_t name_len = strlen(name);
    const char *pair = body;

    while (pair != NULL && *pair != '\0') {
        const char *next = strchr(pair, '&');
        const size_t pair_len = next == NULL ? strlen(pair) : (size_t)(next - pair);

        if (pair_len > name_len && strncmp(pair, name, name_len) == 0 && pair[name_len] == '=') {
            url_decode_component(pair + name_len + 1, pair_len - name_len - 1, out, out_len);
            return true;
        }

        pair = next == NULL ? NULL : next + 1;
    }

    return false;
}

static void html_escape_attr(const char *src, char *dst, size_t dst_len)
{
    size_t j = 0;
    if (dst_len == 0) {
        return;
    }
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_len; ++i) {
        const char *rep = NULL;
        switch (src[i]) {
            case '&':
                rep = "&amp;";
                break;
            case '"':
                rep = "&quot;";
                break;
            case '<':
                rep = "&lt;";
                break;
            case '>':
                rep = "&gt;";
                break;
            default:
                dst[j++] = src[i];
                continue;
        }
        size_t rlen = strlen(rep);
        if (j + rlen >= dst_len) {
            break;
        }
        memcpy(dst + j, rep, rlen);
        j += rlen;
    }
    dst[j] = '\0';
}

static void json_escape_string(const char *src, char *dst, size_t dst_len)
{
    size_t j = 0;
    if (dst_len == 0) {
        return;
    }
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_len; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\\' || c == '"') {
            if (j + 2 >= dst_len) {
                break;
            }
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c == '\n') {
            if (j + 2 >= dst_len) {
                break;
            }
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 >= dst_len) {
                break;
            }
            dst[j++] = '\\';
            dst[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 >= dst_len) {
                break;
            }
            dst[j++] = '\\';
            dst[j++] = 't';
        } else if (c < 0x20U) {
            int n = snprintf(dst + j, dst_len - j, "\\u%04x", (unsigned)c);
            if (n <= 0 || (size_t)n >= dst_len - j) {
                break;
            }
            j += (size_t)n;
        } else {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

static uint8_t admin_xor_memcmp(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff;
}

static int admin_pbkdf2_derive(const char *password, const uint8_t *salt, size_t salt_len, uint32_t iterations,
                               uint8_t out[ADMIN_HASH_LEN])
{
    if (init_psa_crypto_once() != ESP_OK) {
        return -1;
    }

    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (st != PSA_SUCCESS) {
        psa_key_derivation_abort(&op);
        return -1;
    }

    st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, (uint64_t)iterations);
    if (st != PSA_SUCCESS) {
        goto fail;
    }
    st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len);
    if (st != PSA_SUCCESS) {
        goto fail;
    }
    st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD, (const uint8_t *)password,
                                        strlen(password));
    if (st != PSA_SUCCESS) {
        goto fail;
    }

    st = psa_key_derivation_output_bytes(&op, out, ADMIN_HASH_LEN);
    if (st != PSA_SUCCESS) {
        goto fail;
    }

    psa_key_derivation_abort(&op);
    return 0;

fail:
    psa_key_derivation_abort(&op);
    return -1;
}

static void load_admin_password_verifier(void)
{
    s_admin_custom_password = false;
    s_admin_pbkdf2_iterations = ADMIN_PBKDF2_ITERATIONS;
    memset(s_admin_salt, 0, sizeof(s_admin_salt));
    memset(s_admin_hash, 0, sizeof(s_admin_hash));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("auth_cfg", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return;
    }

    size_t salt_len = sizeof(s_admin_salt);
    size_t hash_len = sizeof(s_admin_hash);
    if (nvs_get_blob(nvs, "salt", s_admin_salt, &salt_len) == ESP_OK && salt_len == ADMIN_SALT_LEN &&
        nvs_get_blob(nvs, "hash", s_admin_hash, &hash_len) == ESP_OK && hash_len == ADMIN_HASH_LEN) {
        s_admin_custom_password = true;
        uint32_t iters = 0;
        if (nvs_get_u32(nvs, "pbkdf2_iters", &iters) == ESP_OK && iters > 0U) {
            s_admin_pbkdf2_iterations = iters;
        } else {
            s_admin_pbkdf2_iterations = ADMIN_PBKDF2_ITERATIONS_LEGACY;
        }
        ESP_LOGI(TAG, "custom Admin password verifier loaded from NVS (%" PRIu32 " PBKDF2 iterations)",
                 s_admin_pbkdf2_iterations);
    }

    nvs_close(nvs);
}

static bool verify_admin_password_plain(const char *password)
{
    if (password == NULL) {
        return false;
    }
    if (s_admin_custom_password) {
        uint8_t derived[ADMIN_HASH_LEN];
        if (admin_pbkdf2_derive(password, s_admin_salt, ADMIN_SALT_LEN, s_admin_pbkdf2_iterations, derived) != 0) {
            return false;
        }
        return admin_xor_memcmp(derived, s_admin_hash, ADMIN_HASH_LEN) == 0;
    }
    return strcmp(password, FACTORY_ADMIN_PASSWORD) == 0;
}

static esp_err_t save_admin_password_to_nvs(const char *new_password)
{
    uint8_t salt[ADMIN_SALT_LEN];
    uint8_t hash[ADMIN_HASH_LEN];
    esp_fill_random(salt, sizeof(salt));
    if (admin_pbkdf2_derive(new_password, salt, sizeof(salt), ADMIN_PBKDF2_ITERATIONS, hash) != 0) {
        return ESP_FAIL;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("auth_cfg", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    if ((err = nvs_set_blob(nvs, "salt", salt, sizeof(salt))) == ESP_OK &&
        (err = nvs_set_blob(nvs, "hash", hash, sizeof(hash))) == ESP_OK &&
        (err = nvs_set_u32(nvs, "pbkdf2_iters", ADMIN_PBKDF2_ITERATIONS)) == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        memcpy(s_admin_salt, salt, sizeof(salt));
        memcpy(s_admin_hash, hash, sizeof(hash));
        s_admin_custom_password = true;
        s_admin_pbkdf2_iterations = ADMIN_PBKDF2_ITERATIONS;
        ESP_LOGI(TAG, "Admin password verifier updated in NVS");
    }
    return err;
}

static void ensure_session_mutex(void)
{
    if (s_session_mutex == NULL) {
        s_session_mutex = xSemaphoreCreateMutex();
        if (s_session_mutex == NULL) {
            ESP_LOGE(TAG, "session mutex allocation failed");
        }
    }
}

static void bin_to_hex_lower(const uint8_t *bin, size_t bin_len, char *hex)
{
    static const char dig[] = "0123456789abcdef";
    for (size_t i = 0; i < bin_len; ++i) {
        hex[i * 2] = dig[bin[i] >> 4];
        hex[i * 2 + 1] = dig[bin[i] & 15];
    }
    hex[bin_len * 2] = '\0';
}

static bool hex_to_bin_32(const char *hex, uint8_t out[SESSION_TOKEN_BYTES])
{
    if (strlen(hex) != SESSION_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < SESSION_TOKEN_BYTES; ++i) {
        int hi = hex_digit_value(hex[i * 2]);
        int lo = hex_digit_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((unsigned)hi << 4 | (unsigned)lo);
    }
    return true;
}

static bool extract_cookie_value(const char *cookie_hdr, const char *name, char *out, size_t out_len)
{
    const size_t nlen = strlen(name);
    const char *segment = cookie_hdr;

    while (segment != NULL && *segment != '\0') {
        while (*segment == ' ' || *segment == '\t') {
            segment++;
        }
        const char *semi = strchr(segment, ';');
        size_t seglen = semi != NULL ? (size_t)(semi - segment) : strlen(segment);
        if (seglen > nlen + 1 && strncmp(segment, name, nlen) == 0 && segment[nlen] == '=') {
            const char *val = segment + nlen + 1;
            size_t vlen = seglen - (nlen + 1);
            while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) {
                vlen--;
            }
            if (vlen == 0 || vlen >= out_len) {
                return false;
            }
            memcpy(out, val, vlen);
            out[vlen] = '\0';
            return true;
        }
        segment = semi != NULL ? semi + 1 : NULL;
    }
    return false;
}

static void clear_web_session_locked(void)
{
    s_session_active = false;
    memset(s_session_token, 0, sizeof(s_session_token));
    s_session_last_activity_ms = 0;
}

static void open_web_session_locked(void)
{
    esp_fill_random(s_session_token, sizeof(s_session_token));
    s_session_active = true;
    s_session_last_activity_ms = esp_log_timestamp();
}

static bool web_session_accept(httpd_req_t *req, bool touch_idle)
{
    char cookie_hdr[512];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_hdr, sizeof(cookie_hdr)) != ESP_OK) {
        return false;
    }

    char presented[SESSION_HEX_LEN + 1];
    if (!extract_cookie_value(cookie_hdr, SESSION_COOKIE_NAME, presented, sizeof(presented))) {
        return false;
    }

    uint8_t presented_bin[SESSION_TOKEN_BYTES];
    if (!hex_to_bin_32(presented, presented_bin)) {
        return false;
    }

    ensure_session_mutex();
    if (s_session_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }

    bool ok = false;
    if (s_session_active) {
        if (admin_xor_memcmp(presented_bin, s_session_token, SESSION_TOKEN_BYTES) == 0) {
            const uint32_t now_ms = esp_log_timestamp();
            if (now_ms - s_session_last_activity_ms > SESSION_IDLE_TIMEOUT_MS) {
                clear_web_session_locked();
            } else {
                if (touch_idle) {
                    s_session_last_activity_ms = now_ms;
                }
                ok = true;
            }
        }
    }

    xSemaphoreGive(s_session_mutex);
    return ok;
}

static inline bool is_web_session_ok(httpd_req_t *req)
{
    return web_session_accept(req, true);
}

static void send_json_session_required(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"session_required\",\"message\":\"Sign in at the home page\"}\n");
}

static void send_login_page(httpd_req_t *req)
{
    bool bad = false;
    char query[48] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char v[8] = {0};
        if (httpd_query_key_value(query, "retry", v, sizeof(v)) == ESP_OK && strcmp(v, "1") == 0) {
            bad = true;
        }
    }

    static const char head[] =
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<link rel=\"icon\" href=\"/favicon.ico\" type=\"image/png\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Aurora — Sign in</title>"
        "<style>"
        ":root{color-scheme:light;--bg:#eef3f8;--card:#fff;--line:#d7e0ea;--text:#17202a;--muted:#607086;--btn:#2459a6}"
        "*{box-sizing:border-box}body{font-family:system-ui,Segoe UI,sans-serif;margin:0;background:var(--bg);color:var(--text);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:1rem}"
        ".box{background:var(--card);border:1px solid var(--line);border-radius:1rem;padding:1.75rem;width:22rem;max-width:100%%;box-shadow:0 1px 2px rgba(20,30,40,.06)}"
        "h1{margin:0 0 .35rem;font-size:1.25rem}p{margin:.5rem 0 1rem;color:var(--muted);font-size:.95rem;line-height:1.45}"
        ".box form{display:grid;grid-template-columns:minmax(0,1fr);width:100%%;margin:0;padding:0}"
        ".box form label{display:block;margin:.85rem 0 .25rem;font-weight:650;font-size:.9rem}"
        ".box form label:first-child{margin-top:0}"
        ".box form input{display:block;width:100%%;max-width:100%%;min-width:0;box-sizing:border-box;margin:0;font-size:1rem;padding:.55rem;border:1px solid #b8c4d2;border-radius:.45rem}"
        ".box form button{margin-top:1.1rem;justify-self:start;width:auto;max-width:100%%;font-size:1rem;padding:.65rem 1rem;border:0;border-radius:.55rem;background:var(--btn);color:#fff;font-weight:700;cursor:pointer}"
        ".err{background:#fee4e2;color:#b42318;padding:.55rem .75rem;border-radius:.45rem;font-size:.9rem;margin-bottom:.75rem}"
        "</style></head><body><div class=\"box\">"
        "<h1>Sign in</h1>"
        "<p>Session expires after 10 minutes without a page load or dashboard refresh.</p>";

    static const char form[] =
        "<form method=\"post\" action=\"/api/auth/login\" autocomplete=\"on\" "
        "onsubmit=\"var b=this.querySelector('button[type=submit]');if(b){b.disabled=true;b.textContent='Signing in…';}\">"
        "<label for=\"user\">Username</label>"
        "<input id=\"user\" name=\"username\" type=\"text\" value=\"\" autocomplete=\"username\" required>"
        "<label for=\"pw\">Password</label>"
        "<input id=\"pw\" name=\"password\" type=\"password\" autocomplete=\"current-password\" required>"
        "<button type=\"submit\">Continue</button></form>"
        "</div></body></html>";

    char buf[3072];
    int len = snprintf(buf, sizeof(buf), "%s%s%s", head, bad ? "<div class=\"err\">Incorrect username or password.</div>" : "", form);
    if (len < 0 || len >= (int)sizeof(buf)) {
        httpd_resp_send_500(req);
        return;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, len);
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    if (ssid[0] == '\0' || strlen(ssid) > WIFI_SSID_MAX_LEN) {
        ESP_LOGW(TAG, "rejecting Wi-Fi save: SSID length is invalid");
        return ESP_ERR_INVALID_ARG;
    }

    if (password[0] == '\0' || strlen(password) >= WIFI_PASSWORD_MAX_LEN) {
        ESP_LOGW(TAG, "rejecting Wi-Fi save: password is empty or too long");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    if ((err = nvs_set_str(nvs, "ssid", ssid)) == ESP_OK) {
        err = nvs_set_str(nvs, "password", password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

static esp_err_t save_pvoutput_config(const pvoutput_config_t *config)
{
    if (config->base_url[0] == '\0' ||
        strlen(config->base_url) >= PVOUTPUT_BASE_URL_MAX_LEN ||
        strlen(config->system_id) >= PVOUTPUT_SYSTEM_ID_MAX_LEN ||
        strlen(config->api_key) >= PVOUTPUT_API_KEY_MAX_LEN ||
        !pvoutput_interval_is_valid(config->interval_minutes)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("pvoutput", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    if ((err = nvs_set_str(nvs, "base_url", config->base_url)) == ESP_OK &&
        (err = nvs_set_str(nvs, "system_id", config->system_id)) == ESP_OK &&
        (err = nvs_set_str(nvs, "api_key", config->api_key)) == ESP_OK &&
        (err = nvs_set_u8(nvs, "interval", config->interval_minutes)) == ESP_OK &&
        (err = nvs_set_u8(nvs, "flags", config->flags)) == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    if (err == ESP_OK) {
        s_pvoutput_config = *config;
        s_pvoutput_config_loaded = true;
    }
    return err;
}

static void start_mdns_service(void)
{
    if (s_mdns_started) {
        return;
    }

    ESP_ERROR_CHECK(mdns_init());
    const char *host = (s_sta_hostname[0] != '\0') ? s_sta_hostname : s_hostname;
    ESP_ERROR_CHECK(mdns_hostname_set(host));
    ESP_ERROR_CHECK(mdns_instance_name_set("Aurora ESP32 Appliance"));

    mdns_txt_item_t service_txt[] = {
        {"path", "/"},
        {"version", APP_VERSION},
    };
    ESP_ERROR_CHECK(mdns_service_add("Aurora ESP32", "_http", "_tcp", 80, service_txt,
                                     sizeof(service_txt) / sizeof(service_txt[0])));

    s_mdns_started = true;
    ESP_LOGI(TAG, "mDNS started: http://%s.local/", s_hostname);
}

static void start_sntp_service(void)
{
    if (s_sntp_started) {
        return;
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_server);
    esp_netif_sntp_init(&config);
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started with server: %s", s_ntp_server);
}

static void disable_softap_after_sta_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (!s_sta_connected || !s_provisioning_active) {
        s_softap_disable_pending = false;
        vTaskDelete(NULL);
        return;
    }

    /* Stopping HTTP before APSTA -> STA avoids esp_http_server racing lwIP/WiFi teardown:
     * active handlers could see httpd_req_aux.sd cleared (LoadProhibited ~NULL+0x18). */
    const bool had_httpd = (s_http_server != NULL);
    if (had_httpd) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_stop(s_http_server));
        s_http_server = NULL;
    }

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        s_provisioning_active = false;
        s_provisioning_started_ms = 0;
        ESP_LOGI(TAG, "saved Wi-Fi is connected; setup AP disabled for normal STA-only operation");
    } else {
        ESP_LOGW(TAG, "failed to disable setup AP after STA join: %s", esp_err_to_name(err));
    }

    if (had_httpd) {
        esp_err_t hs = start_provisioning_http_server();
        if (hs != ESP_OK) {
            ESP_LOGE(TAG, "HTTP server restart after WiFi mode change failed: %s", esp_err_to_name(hs));
        }
    }

    s_softap_disable_pending = false;
    vTaskDelete(NULL);
}

static void format_uptime_human(char *out, size_t out_len)
{
    uint64_t seconds = (uint64_t)(esp_timer_get_time() / 1000000LL);
    uint64_t days = seconds / 86400U;
    uint64_t hours = (seconds / 3600U) % 24U;
    uint64_t minutes = (seconds / 60U) % 60U;

    snprintf(out, out_len, "%02" PRIu64 "-%02" PRIu64 "-%02" PRIu64, days, hours, minutes);
}

/** Device-local wall clock as HH:MM:SS (uses `setenv` TZ / NVS timezone). */
static void format_local_time_hms(char *out, size_t out_len)
{
    if (out_len == 0U) {
        return;
    }
    out[0] = '\0';
    time_t now = 0;
    time(&now);
    struct tm lt;
    localtime_r(&now, &lt);
    (void)strftime(out, out_len, "%H:%M:%S", &lt);
}

static void pvoutput_format_last_result(int code, char *buf, size_t buflen)
{
    if (buflen == 0U) {
        return;
    }
    buf[0] = '\0';
    if (code >= 100 && code <= 599) {
        if (code == 200) {
            (void)snprintf(buf, buflen, "OK 200: Added Output");
            return;
        }
        if (code == 400) {
            (void)snprintf(buf, buflen, "Bad request 400");
            return;
        }
        if (code >= 200 && code < 300) {
            (void)snprintf(buf, buflen, "OK %d: Success", code);
            return;
        }
        if (code >= 400 && code < 500) {
            (void)snprintf(buf, buflen, "Client error %d", code);
            return;
        }
        (void)snprintf(buf, buflen, "Server error %d", code);
        return;
    }
    (void)snprintf(buf, buflen, "%s (%d)", esp_err_to_name((esp_err_t)code), code);
}

static void pvoutput_format_last_post_hhmmss(time_t t, char *buf, size_t buflen)
{
    if (buflen == 0U) {
        return;
    }
    if (t <= (time_t)0) {
        (void)snprintf(buf, buflen, "--:--:--");
        return;
    }
    struct tm lt;
    if (localtime_r(&t, &lt) == NULL) {
        (void)snprintf(buf, buflen, "--:--:--");
        return;
    }
    (void)strftime(buf, buflen, "%H:%M:%S", &lt);
}

static uint32_t get_request_client_ip(httpd_req_t *req)
{
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0 || getpeername(sockfd, (struct sockaddr *)&peer_addr, &peer_addr_len) != 0 ||
        peer_addr.ss_family != AF_INET) {
        return 0;
    }

    struct sockaddr_in *addr = (struct sockaddr_in *)&peer_addr;
    return addr->sin_addr.s_addr;
}

static const char *format_ipv4(uint32_t ip, char *out, size_t out_len)
{
    if (ip == 0) {
        strlcpy(out, "unknown", out_len);
        return out;
    }

    const uint8_t *bytes = (const uint8_t *)&ip;
    snprintf(out, out_len, "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
    return out;
}

static auth_client_state_t *get_auth_client_state(uint32_t ip)
{
    auth_client_state_t *empty_slot = NULL;

    for (size_t i = 0; i < AUTH_MAX_TRACKED_CLIENTS; ++i) {
        if (s_auth_clients[i].ip == ip) {
            return &s_auth_clients[i];
        }
        if (empty_slot == NULL && s_auth_clients[i].ip == 0) {
            empty_slot = &s_auth_clients[i];
        }
    }

    auth_client_state_t *slot = empty_slot != NULL ? empty_slot : &s_auth_clients[0];
    memset(slot, 0, sizeof(*slot));
    slot->ip = ip;
    return slot;
}

static bool auth_client_is_locked(auth_client_state_t *client, uint32_t now_ms)
{
    return client->locked_until_ms != 0 && now_ms < client->locked_until_ms;
}

static esp_err_t login_post_handler(httpd_req_t *req)
{
    char client_ip_text[16] = {0};
    const uint32_t client_ip = get_request_client_ip(req);
    auth_client_state_t *client = get_auth_client_state(client_ip);
    const uint32_t now_ms = esp_log_timestamp();

    if (auth_client_is_locked(client, now_ms)) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_sendstr(req,
                           "<!doctype html><html><body><h1>Too many attempts</h1><p>Wait about five minutes.</p></body></html>");
        return ESP_OK;
    }

    if (req->content_len > FORM_BODY_MAX_LEN) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, "Request too large");
        return ESP_OK;
    }

    char body[FORM_BODY_MAX_LEN + 1];
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    char username[48] = {0};
    char password[ADMIN_PASSWORD_MAX_LEN + 1] = {0};
    if (!extract_form_value(body, "username", username, sizeof(username)) ||
        !extract_form_value(body, "password", password, sizeof(password))) {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/?retry=1");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    if (strcmp(username, FACTORY_ADMIN_USER) != 0 || !verify_admin_password_plain(password)) {
        s_auth_failed_total++;
        if (client->failures < UINT8_MAX) {
            client->failures++;
        }
        if (client->failures >= AUTH_FAILURE_LOCK_THRESHOLD) {
            client->locked_until_ms = now_ms + AUTH_LOCKOUT_MS;
            s_auth_lockout_total++;
            ESP_LOGW(TAG, "login lockout for %s after %u failures",
                     format_ipv4(client_ip, client_ip_text, sizeof(client_ip_text)), client->failures);
        } else {
            ESP_LOGW(TAG, "login failed for %s (%u/%u)",
                     format_ipv4(client_ip, client_ip_text, sizeof(client_ip_text)), client->failures,
                     AUTH_FAILURE_LOCK_THRESHOLD);
        }

        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/?retry=1");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    client->failures = 0;
    client->locked_until_ms = 0;

    char hex[SESSION_HEX_LEN + 1];
    char set_cookie[160];

    ensure_session_mutex();
    if (s_session_mutex == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    open_web_session_locked();
    bin_to_hex_lower(s_session_token, SESSION_TOKEN_BYTES, hex);
    xSemaphoreGive(s_session_mutex);

    int clen = snprintf(set_cookie, sizeof(set_cookie),
                        SESSION_COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=86400", hex);
    if (clen < 0 || clen >= (int)sizeof(set_cookie)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Set-Cookie", set_cookie);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t logout_post_handler(httpd_req_t *req)
{
    ensure_session_mutex();
    if (s_session_mutex != NULL && xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        clear_web_session_locked();
        xSemaphoreGive(s_session_mutex);
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Set-Cookie", SESSION_COOKIE_NAME "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void init_sample_ring(void)
{
    s_sample_mutex = xSemaphoreCreateMutex();
    if (s_sample_mutex == NULL) {
        ESP_LOGE(TAG, "sample ring mutex allocation failed");
        return;
    }

    s_samples = heap_caps_calloc(SAMPLE_RING_TARGET_COUNT, sizeof(sample_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_sample_capacity = SAMPLE_RING_TARGET_COUNT;
    if (s_samples == NULL) {
        s_samples = heap_caps_calloc(SAMPLE_RING_FALLBACK_COUNT, sizeof(sample_t), MALLOC_CAP_8BIT);
        s_sample_capacity = SAMPLE_RING_FALLBACK_COUNT;
    }

    if (s_samples == NULL) {
        s_sample_capacity = 0;
        ESP_LOGE(TAG, "sample ring allocation failed");
        return;
    }

    ESP_LOGI(TAG, "sample ring ready: capacity=%u samples (~%u minutes at %us cadence)",
             (unsigned)s_sample_capacity,
             (unsigned)((s_sample_capacity * SAMPLE_INTERVAL_SECONDS) / 60U),
             SAMPLE_INTERVAL_SECONDS);
}

static void retain_live_sample(void)
{
    if (s_samples == NULL || s_sample_capacity == 0 || s_sample_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_sample_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    sample_t *sample = &s_samples[s_sample_write_index];
    sample->uptime_ms = esp_log_timestamp();
    sample->unix_sec = (uint32_t)time(NULL);
    sample->output_power_w = s_output_power_w;
    sample->grid_voltage_v = s_grid_voltage_v;
    sample->grid_frequency_hz = s_grid_frequency_hz;
    sample->booster_temp_c = s_booster_temp_c;
    sample->inverter_temp_c = s_inverter_temp_c;
    sample->energy_today_wh = s_energy_today_wh;

    sample_t written_snapshot = *sample;

    s_sample_write_index = (s_sample_write_index + 1U) % s_sample_capacity;
    if (s_sample_count < s_sample_capacity) {
        s_sample_count++;
    }

    xSemaphoreGive(s_sample_mutex);

    daylog_on_retained_sample((const daylog_sample_t *)&written_snapshot);
}

static esp_err_t root_send_dashboard_page(httpd_req_t *req)
{
    char lan_url[64] = {0};
    if (s_sta_connected && s_sta_ip[0] != '\0') {
        snprintf(lan_url, sizeof(lan_url), "http://%s/", s_sta_ip);
    } else {
        strlcpy(lan_url, "not connected yet", sizeof(lan_url));
    }

    char esc_pvo_api[320];
    char esc_pvo_base[PVOUTPUT_BASE_URL_MAX_LEN * 6];
    char esc_pvo_sid[PVOUTPUT_SYSTEM_ID_MAX_LEN * 6];
    html_escape_attr(s_pvoutput_config.api_key, esc_pvo_api, sizeof(esc_pvo_api));
    html_escape_attr(s_pvoutput_config.base_url, esc_pvo_base, sizeof(esc_pvo_base));
    html_escape_attr(s_pvoutput_config.system_id, esc_pvo_sid, sizeof(esc_pvo_sid));

    char pvo_result_initial[192];
    char pvo_lastpost_initial[16];
    pvoutput_format_last_result(s_pvoutput_last_result, pvo_result_initial, sizeof(pvo_result_initial));
    pvoutput_format_last_post_hhmmss(s_pvoutput_last_post_unix, pvo_lastpost_initial, sizeof(pvo_lastpost_initial));
    char esc_pvo_result_init[384];
    char esc_pvo_lastpost_init[64];
    html_escape_attr(pvo_result_initial, esc_pvo_result_init, sizeof(esc_pvo_result_init));
    html_escape_attr(pvo_lastpost_initial, esc_pvo_lastpost_init, sizeof(esc_pvo_lastpost_init));

    char iv_g[192], iv_inv[192], iv_c1[192], iv_c2[192], iv_al[192];
    char esc_iv_g[512], esc_iv_inv[512], esc_iv_c1[512], esc_iv_c2[512], esc_iv_al[512];
    if (s_inverter_offline) {
        const char *u = "Unavailable";
        strlcpy(iv_g, u, sizeof(iv_g));
        strlcpy(iv_inv, u, sizeof(iv_inv));
        strlcpy(iv_c1, u, sizeof(iv_c1));
        strlcpy(iv_c2, u, sizeof(iv_c2));
        strlcpy(iv_al, u, sizeof(iv_al));
    } else {
        aurora_decode_inverter_status(s_inverter_state, iv_g, sizeof(iv_g), iv_inv, sizeof(iv_inv), iv_c1, sizeof(iv_c1), iv_c2, sizeof(iv_c2), iv_al,
                                      sizeof(iv_al));
    }
    html_escape_attr(iv_g, esc_iv_g, sizeof(esc_iv_g));
    html_escape_attr(iv_inv, esc_iv_inv, sizeof(esc_iv_inv));
    html_escape_attr(iv_c1, esc_iv_c1, sizeof(esc_iv_c1));
    html_escape_attr(iv_c2, esc_iv_c2, sizeof(esc_iv_c2));
    html_escape_attr(iv_al, esc_iv_al, sizeof(esc_iv_al));

    const char *setup_ap_state = s_provisioning_active ? s_provisioning_ssid : "disabled";
    const char *setup_ap_url = s_provisioning_active ? "http://" PROVISIONING_AP_IP "/" : "disabled";
    const char *mdns_host = (s_sta_hostname[0] != '\0') ? s_sta_hostname : s_hostname;
    char uptime[16] = {0};
    format_uptime_human(uptime, sizeof(uptime));
    char local_hms_page[16];
    format_local_time_hms(local_hms_page, sizeof(local_hms_page));

    int len = snprintf(
        s_root_html,
        sizeof(s_root_html),
        "<!doctype html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"utf-8\">"
        "<link rel=\"icon\" href=\"/favicon.ico\" type=\"image/png\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Aurora ESP32 Dashboard</title>"
        "<style>"
        ":root{color-scheme:light;--bg:#eef3f8;--card:#fff;--line:#d7e0ea;--text:#17202a;--muted:#607086;--ok:#067647;--bad:#b42318;--warn:#936200}"
        "*{box-sizing:border-box}body{font-family:system-ui,Segoe UI,sans-serif;margin:0;background:var(--bg);color:var(--text)}"
        "main{max-width:72rem;margin:0 auto;padding:1.25rem}"
        ".top{display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:.75rem 1rem;margin-bottom:1rem}"
        ".top h1{margin:0;flex:1;min-width:12rem;font-size:1.35rem;display:flex;align-items:center;gap:.5rem}"
        ".hdr-logo{display:block;width:40px;height:40px;min-width:40px;object-fit:contain;flex-shrink:0}"
        ".top-actions{display:flex;flex-wrap:wrap;gap:.5rem;align-items:stretch}"
        ".top form{margin:0;display:flex;align-items:stretch}"
        ".top-actions>a.btn-dash,.top-actions>a.btn-maint,.top-actions button{display:inline-flex;align-items:center;justify-content:center;min-height:2.5rem;box-sizing:border-box;font-family:inherit;line-height:1.2}"
        ".btn-restart{margin:0;padding:.55rem 1rem;font-size:1rem;border:0;border-radius:.55rem;background:#ea580c;color:#fff;font-weight:700}"
        ".btn-logout{margin:0;padding:.55rem 1rem;font-size:1rem;border:1px solid #4b5563;border-radius:.55rem;background:#6b7280;color:#fff;font-weight:650}"
        ".btn-dash{margin:0;padding:.55rem 1rem;font-size:1rem;border:1px solid #15803d;border-radius:.55rem;background:#22c55e;color:#fff;font-weight:700;text-decoration:none}"
        ".btn-maint{margin:0;padding:.55rem 1rem;font-size:1rem;border:1px solid #ca8a04;border-radius:.55rem;background:#facc15;color:#171717;font-weight:700;text-decoration:none}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(16rem,1fr));gap:1rem}"
        ".h2row{display:flex;align-items:center;justify-content:space-between;gap:.5rem;flex-wrap:nowrap;min-height:2rem}"
        ".h2row h2{font-size:1rem;margin:0;color:#263447;flex-shrink:0}"
        ".h2trail{display:inline-flex;align-items:center;gap:.45rem;flex-wrap:nowrap;flex-shrink:0}"
        ".pill-sm,.pill-status{display:inline-flex;align-items:center;justify-content:center;font-size:.72rem;line-height:1.2;padding:.28rem .65rem;border-radius:999px;font-weight:700;white-space:nowrap;min-width:5.2rem;box-sizing:border-box}"
        ".pill-sm{border:1px solid #2459a6;background:#eef4ff;color:#2459a6;text-decoration:none}"
        ".pill-status{border:1px solid #c5d0df;background:#e8eef5;color:#304254}"
        ".pill-status.ok{border-color:#86efac;background:#dcfae6;color:var(--ok)}"
        ".pill-status.bad{border-color:#fca5a5;background:#fee4e2;color:var(--bad)}"
        ".pill-status.warn{border-color:#fcd34d;background:#fff3c7;color:var(--warn)}"
        ".pill-status.pill-click{cursor:pointer}"
        ".modal-backdrop{position:fixed;inset:0;background:rgba(15,23,42,.45);display:none;align-items:center;justify-content:center;z-index:50;padding:1rem}"
        ".modal-backdrop.open{display:flex}"
        ".iv-modal{background:#f1f4f8;border:1px solid #b8c4d2;border-radius:.65rem;min-width:min(22rem,100%%);max-width:28rem;padding:1rem .9rem;box-shadow:0 12px 28px rgba(15,23,42,.22)}"
        ".iv-modal.iv-modal-wide{max-width:min(40rem,96%%)}"
        ".iv-modal h3{margin:0 0 .65rem;font-size:1.05rem;border-bottom:1px solid #d7e0ea;padding-bottom:.45rem}"
        ".iv-modal .row{border-color:#e0e6ed}"
        ".iv-modal button{margin-top:.8rem;width:100%%;cursor:pointer}"
        ".help-i{cursor:pointer;background:transparent;border:0;color:#2459a6;padding:0 .15rem;margin-top:0;font-size:1.05rem;line-height:1;font-weight:600;display:inline-flex;align-items:center;justify-content:center}"
        ".help-i:focus{outline:2px solid #2459a6;outline-offset:2px;border-radius:2px}"
        ".info-table{width:100%%;border-collapse:collapse;font-size:.86rem;margin:.5rem 0 0}"
        ".info-table th,.info-table td{border:1px solid #d7e0ea;padding:.45rem .5rem;text-align:left;vertical-align:top}"
        ".info-table th{background:#e8eef5;font-weight:650}"
        ".pvo-help-blurb{font-size:.86rem;line-height:1.45;color:var(--muted);margin:0 0 .5rem}"
        ".sect{display:flex;align-items:center;gap:.35rem;flex-wrap:nowrap;margin:0}"
        ".sect-label{font-size:1rem;margin:0;color:#263447;font-weight:650}"
        ".sect-label.rs485-addr{font-weight:400}"
        ".sect h2{font-size:1.05rem;margin:0;color:#263447;font-weight:650}"
        ".sect h2.sect-lwnorm{font-weight:400}"
        ".card{background:var(--card);border:1px solid var(--line);border-radius:1rem;padding:1rem;box-shadow:0 1px 2px rgba(20,30,40,.04)}"
        "h1{display:flex;align-items:center;gap:.7rem;flex-wrap:wrap}"
        ".pill{display:inline-flex;align-items:center;border-radius:999px;padding:.25rem .65rem;font-weight:700;background:#e8eef5;color:#304254}.ok{background:#dcfae6;color:var(--ok)}.bad{background:#fee4e2;color:var(--bad)}.warn{background:#fff3c7;color:var(--warn)}"
        ".row{display:flex;justify-content:space-between;gap:1rem;border-top:1px solid #eef2f6;padding:.55rem 0}.row:first-of-type{border-top:0}.muted{color:var(--muted)}"
        "label{display:block;margin:.8rem 0 .25rem;font-weight:650}input,select{font-size:1rem;padding:.55rem;width:100%%;border:1px solid #b8c4d2;border-radius:.45rem}"
        "#admin-pw-form input{display:block;width:100%%;max-width:22rem}"
        "div.pvo-actions{display:flex;flex-wrap:wrap;gap:.5rem;align-items:center;margin-top:.5rem}"
        "div.pvo-actions button{margin-top:0}"
        "button{font-size:1rem;margin-top:1rem;padding:.65rem 1rem;border:0;border-radius:.55rem;background:#2459a6;color:#fff;font-weight:700}code{background:#eef2f6;padding:.1rem .28rem;border-radius:.3rem}details{margin-top:1rem}pre{white-space:pre-wrap;overflow:auto}"
        "</style>"
        "</head>"
        "<body>"
        "<main>"
        "<div class=\"top\">"
        "<h1><img class=\"hdr-logo\" src=\"/favicon.ico\" width=\"40\" height=\"40\" alt=\"\">Aurora ESP32 Dashboard</h1>"
        "<div class=\"top-actions\">"
        "<a class=\"btn-dash\" href=\"/dashboard\">Charts</a>"
        "<a class=\"btn-maint\" href=\"/maintenance\">Maintenance</a>"
        "<form id=\"restart-form\" method=\"post\" action=\"/api/restart\">"
        "<button type=\"submit\" class=\"btn-restart\">Restart Device</button></form>"
        "<form method=\"post\" action=\"/api/auth/logout\">"
        "<button type=\"submit\" class=\"btn-logout\">Log out</button></form>"
        "</div></div>"
        "<noscript><p class=\"warn\">JavaScript is required for live updates. Static status is shown below.</p></noscript>"
        "<div class=\"grid\">"
        "<section class=\"card\"><div class=\"h2row\"><h2>Network</h2><div class=\"h2trail\"><span class=\"pill-status %s\" id=\"sta-state\">%s</span><a class=\"pill-sm\" href=\"/network-settings\">Settings</a></div></div>"
        "<div class=\"row\"><span>LAN</span><code id=\"lan-url\">%s</code></div><div class=\"row\"><span>mDNS</span><code>http://%s.local/</code></div><div class=\"row\"><span>Setup AP</span><code id=\"setup-ap\">%s</code></div><div class=\"row\"><span>Setup URL</span><code id=\"setup-url\">%s</code></div></section>"
        "<section class=\"card\"><div class=\"h2row\"><h2>Time</h2><div class=\"h2trail\"><span class=\"pill-status %s\" id=\"time-state\">%s</span><a class=\"pill-sm\" href=\"/time-settings\">Settings</a></div></div>"
        "<div class=\"row\"><span>NTP</span><code>%s</code></div><div class=\"row\"><span>Clock</span><code id=\"clock-hms\">%s</code></div><div class=\"row\"><span>Timezone</span><code>%s</code></div><div class=\"row\"><span>Uptime</span><code id=\"uptime\">%s</code></div></section>"
        "<section class=\"card\"><div class=\"h2row\"><h2>Inverter</h2><div class=\"h2trail\"><span class=\"pill-status %s pill-click\" id=\"inv-state\" role=\"button\" tabindex=\"0\" title=\"Inverter status\">%s</span><a class=\"pill-sm\" href=\"/inverter-settings\">Settings</a></div></div>"
        "<div class=\"row\"><div class=\"sect\"><span class=\"sect-label rs485-addr\">RS485 address</span><button type=\"button\" class=\"help-i\" id=\"rs485-help-btn\" aria-label=\"RS485 polling help\" title=\"RS485 polling help\" tabindex=\"0\">\xe2\x93\x98</button></div><code>%d</code></div><div class=\"row\"><span>Output power</span><code id=\"output-power\">%.1f W</code></div><div class=\"row\"><span>Energy today</span><code id=\"energy-today\">%.3f kWh</code></div><div class=\"row\"><span>Grid</span><code id=\"grid\">%.1f V / %.2f Hz</code></div><div class=\"row\"><div class=\"sect\"><h2 class=\"sect-lwnorm\">Temperature</h2><button type=\"button\" class=\"help-i\" id=\"temp-help-btn\" aria-label=\"Temperature help\" title=\"Temperature help\" tabindex=\"0\">\xe2\x93\x98</button></div><code id=\"temperature\">%.1f / %.1f C</code></div></section>"
        "</div>"
        "<section class=\"card\"><h2>Device APIs</h2>"
        "<p>Status JSON: <a href=\"/api/status\">/api/status</a></p>"
        "<p>Samples Today PVOutput JSON: <a href=\"/api/samples?today=1\">/api/samples?today=1</a></p>"
        "<p>Samples Today raw modbus: <a href=\"/api/samples/raw.csv\">/api/samples/raw.csv</a></p></section>"
        "<div id=\"inv-status-modal\" class=\"modal-backdrop\" aria-hidden=\"true\">"
        "<div class=\"iv-modal\" onclick=\"event.stopPropagation()\">"
        "<h3 id=\"iv-status-title\">Inverter Status</h3>"
        "<div class=\"row\"><span>Global state</span><code id=\"iv-st-global\">%s</code></div>"
        "<div class=\"row\"><span>Inverter state</span><code id=\"iv-st-inv\">%s</code></div>"
        "<div class=\"row\"><span>Chnl 1 DC/DC</span><code id=\"iv-st-ch1\">%s</code></div>"
        "<div class=\"row\"><span>Chnl 2 DC/DC</span><code id=\"iv-st-ch2\">%s</code></div>"
        "<div class=\"row\"><span>Alarm state</span><code id=\"iv-st-alarm\">%s</code></div>"
        "<button type=\"button\" id=\"inv-status-ok\">Ok</button>"
        "</div></div>"
        "<div id=\"temp-help-modal\" class=\"modal-backdrop\" aria-hidden=\"true\">"
        "<div class=\"iv-modal\" onclick=\"event.stopPropagation()\">"
        "<h3>Temperature</h3>"
        "<p class=\"pvo-help-blurb\">Booster / Inverter: the first value is booster temperature (degrees C); the second is inverter temperature (degrees C).</p>"
        "<button type=\"button\" id=\"temp-help-ok\">Ok</button>"
        "</div></div>"
        "<div id=\"rs485-help-modal\" class=\"modal-backdrop\" aria-hidden=\"true\">"
        "<div class=\"iv-modal\" onclick=\"event.stopPropagation()\">"
        "<h3>RS485 Polling note</h3>"
        "<p class=\"pvo-help-blurb\">The inverter polls for data every 10 seconds, regardless of its power state. This behaviour is intentional and fixed within the firmware. The free pvoutput.org API limits submissions to 60 requests per hour, so polling the RS485 Modbus interface more frequently than every 10 seconds provides no practical benefit.</p>"
        "<button type=\"button\" id=\"rs485-help-ok\">Ok</button>"
        "</div></div>"
        "<section class=\"card\"><h2>PVOutput</h2>"
        "<div class=\"row\"><span>Status</span><code id=\"pvoutput-state\">%s</code></div>"
        "<div class=\"row\"><span>Interval</span><code id=\"pvoutput-interval\">%u min</code></div>"
        "<div class=\"row\"><span>Last post</span><code id=\"pvoutput-lastpost\">%s</code></div>"
        "<div class=\"row\"><div class=\"sect\"><h2>Last result</h2><button type=\"button\" class=\"help-i\" id=\"pvo-result-help-btn\" aria-label=\"Last result codes\" title=\"Last result codes\" tabindex=\"0\">\xe2\x93\x98</button></div><code id=\"pvoutput-result\">%s</code></div>"
        "<div id=\"pvo-result-modal\" class=\"modal-backdrop\" aria-hidden=\"true\">"
        "<div class=\"iv-modal iv-modal-wide\" onclick=\"event.stopPropagation()\">"
        "<h3>PVOutput last result</h3>"
        "<p class=\"pvo-help-blurb\">HTTP status from PVOutput is used only when the stored value is in the 100–599 range; otherwise it is treated as an ESP-IDF <code>esp_err_t</code> and shown as a name and numeric code.</p>"
        "<table class=\"info-table\"><thead><tr><th>Condition</th><th>Text shown</th></tr></thead><tbody>"
        "<tr><td><code>ESP_OK (0)</code></td><td>Some code paths never update it; 0 is not a HTTP status here.</td></tr>"
        "<tr><td>HTTP <code>200</code></td><td>OK 200: Added Output</td></tr>"
        "<tr><td>HTTP <code>400</code></td><td>Bad request 400</td></tr>"
        "<tr><td>Other HTTP 2xx</td><td>OK &lt;code&gt;: Success</td></tr>"
        "<tr><td>Other HTTP 4xx</td><td>Client error &lt;code&gt;</td></tr>"
        "<tr><td>HTTP 5xx</td><td>Server error &lt;code&gt;</td></tr>"
        "<tr><td>Other values (e.g. negative)</td><td><code>esp_err_to_name</code> (code)</td></tr>"
        "</tbody></table>"
        "<button type=\"button\" id=\"pvo-result-help-ok\">Ok</button>"
        "</div></div>"
        "<details><summary>PVOutput settings</summary>"
        "<form id=\"pvoutput-form\" method=\"post\" action=\"/api/config/pvoutput\">"
        "<label for=\"pvo-base-url\">Base URL</label>"
        "<input id=\"pvo-base-url\" name=\"base_url\" maxlength=\"95\" value=\"%s\" required>"
        "<label for=\"pvo-system-id\">System ID</label>"
        "<input id=\"pvo-system-id\" name=\"system_id\" maxlength=\"15\" value=\"%s\" autocomplete=\"off\">"
        "<label for=\"pvo-api-key\">API key</label>"
        "<input id=\"pvo-api-key\" name=\"api_key\" type=\"text\" maxlength=\"47\" value=\"%s\" autocomplete=\"off\">"
        "<label for=\"pvo-interval\">Live update interval</label>"
        "<select id=\"pvo-interval\" name=\"interval\">"
        "<option value=\"0\"%s>Off</option><option value=\"1\"%s>1 minute</option><option value=\"2\"%s>2 minutes</option><option value=\"3\"%s>3 minutes</option><option value=\"4\"%s>4 minutes</option><option value=\"5\"%s>5 minutes</option><option value=\"10\"%s>10 minutes</option><option value=\"15\"%s>15 minutes</option>"
        "</select>"
        "<label for=\"pvo-flags\">Upload fields</label>"
        "<select id=\"pvo-flags\" name=\"flags\">"
        "<option value=\"7\"%s>Power, grid voltage, and temperature</option>"
        "<option value=\"1\"%s>Instantaneous power only</option>"
        "<option value=\"0\"%s>Energy only</option>"
        "</select>"
        "<div class=\"pvo-actions\">"
        "<button type=\"submit\" formaction=\"/api/pvoutput/test\" formmethod=\"post\">Test PVOutput Connection</button>"
        "<button type=\"submit\">Save PVOutput Settings</button>"
        "</div>"
        "</form>"
        "</details></section>"
        "<section class=\"card\"><h2>Admin Password</h2>"
        "<details><summary>Admin password</summary>"
        "<form id=\"admin-pw-form\" method=\"post\" action=\"/api/auth/password\">"
        "<label for=\"adm-cur\">Current password</label>"
        "<input id=\"adm-cur\" name=\"current_password\" type=\"password\" maxlength=\"64\" required autocomplete=\"current-password\">"
        "<label for=\"adm-new\">New password</label>"
        "<input id=\"adm-new\" name=\"new_password\" type=\"password\" maxlength=\"64\" required autocomplete=\"new-password\">"
        "<label for=\"adm-new2\">Confirm new password</label>"
        "<input id=\"adm-new2\" name=\"new_password_confirm\" type=\"password\" maxlength=\"64\" required autocomplete=\"new-password\">"
        "<button type=\"submit\">Change Admin Password</button>"
        "</form>"
        "</details></section>"
        "<script>"
        "var invModal=false;var pvoHelpModal=false;var tempHelpModal=false;var rs485HelpModal=false;var lastStatus=null;"
        "function fillInvModal(s){var d=function(id,x){var el=document.getElementById(id);if(el)el.textContent=x;};"
        "if(!s||!s.inverter||!s.inverter.state_text){d('iv-st-global','Unavailable');d('iv-st-inv','Unavailable');d('iv-st-ch1','Unavailable');d('iv-st-ch2','Unavailable');d('iv-st-alarm','Unavailable');return;}"
        "var t=s.inverter.state_text;d('iv-st-global',t.global);d('iv-st-inv',t.inverter);d('iv-st-ch1',t.chn1);d('iv-st-ch2',t.chn2);d('iv-st-alarm',t.alarm);}"
        "function openInvModal(){invModal=true;var m=document.getElementById('inv-status-modal');if(m){m.classList.add('open');m.setAttribute('aria-hidden','false');fillInvModal(lastStatus);}}"
        "function closeInvModal(){invModal=false;var m=document.getElementById('inv-status-modal');if(m){m.classList.remove('open');m.setAttribute('aria-hidden','true');}}"
        "function openPvoHelpModal(){pvoHelpModal=true;var m=document.getElementById('pvo-result-modal');if(m){m.classList.add('open');m.setAttribute('aria-hidden','false');}}"
        "function closePvoHelpModal(){pvoHelpModal=false;var m=document.getElementById('pvo-result-modal');if(m){m.classList.remove('open');m.setAttribute('aria-hidden','true');}}"
        "function openTempHelpModal(){tempHelpModal=true;var m=document.getElementById('temp-help-modal');if(m){m.classList.add('open');m.setAttribute('aria-hidden','false');}}"
        "function closeTempHelpModal(){tempHelpModal=false;var m=document.getElementById('temp-help-modal');if(m){m.classList.remove('open');m.setAttribute('aria-hidden','true');}}"
        "function openRs485HelpModal(){rs485HelpModal=true;var m=document.getElementById('rs485-help-modal');if(m){m.classList.add('open');m.setAttribute('aria-hidden','false');}}"
        "function closeRs485HelpModal(){rs485HelpModal=false;var m=document.getElementById('rs485-help-modal');if(m){m.classList.remove('open');m.setAttribute('aria-hidden','true');}}"
        "async function refresh(){try{const r=await fetch('/api/status',{cache:'no-store',credentials:'same-origin'});if(r.status===401){location.assign('/');return;}const s=await r.json();lastStatus=s;"
        "document.getElementById('uptime').textContent=s.uptime;"
        "document.getElementById('pvoutput-state').textContent=s.pvoutput.configured?(s.pvoutput.enabled?'Configured':'Configured, off'):'Not configured';"
        "document.getElementById('pvoutput-interval').textContent=s.pvoutput.interval_minutes+' min';"
        "document.getElementById('pvoutput-lastpost').textContent=s.pvoutput.last_post_hhmmss||'--:--:--';"
        "document.getElementById('pvoutput-result').textContent=(s.pvoutput.last_result_text!==undefined&&s.pvoutput.last_result_text!==null)?s.pvoutput.last_result_text:String(s.pvoutput.last_result);"
        "document.getElementById('sta-state').textContent=s.sta.connected?'Connected':'Not connected';"
        "document.getElementById('sta-state').className='pill-status '+(s.sta.connected?'ok':'bad');"
        "document.getElementById('lan-url').textContent=s.sta.connected?('http://'+s.sta.ip+'/'):'not connected yet';"
        "document.getElementById('setup-ap').textContent=s.provisioning_ap.active?s.provisioning_ap.ssid:'disabled';"
        "document.getElementById('setup-url').textContent=s.provisioning_ap.active?('http://'+s.provisioning_ap.ip+'/'):'disabled';"
        "document.getElementById('time-state').textContent=s.time.sntp_synced?'Synced':'Waiting';"
        "document.getElementById('time-state').className='pill-status '+(s.time.sntp_synced?'ok':'warn');"
        "var clk=document.getElementById('clock-hms');if(clk){clk.textContent=(s.time&&s.time.local_time_hms)?s.time.local_time_hms:'--:--:--';}"
        "document.getElementById('inv-state').textContent=s.inverter.offline?'Offline':'Online';"
        "document.getElementById('inv-state').className='pill-status pill-click '+(s.inverter.offline?'bad':'ok');"
        "document.getElementById('output-power').textContent=s.inverter.live.valid?(s.inverter.live.output_power_w.toFixed(1)+' W'):'waiting';"
        "document.getElementById('energy-today').textContent=s.inverter.live.valid?(s.inverter.live.energy_today_kwh.toFixed(3)+' kWh'):'waiting';"
        "document.getElementById('grid').textContent=s.inverter.live.valid?(s.inverter.live.grid_voltage_v.toFixed(1)+' V / '+s.inverter.live.grid_frequency_hz.toFixed(2)+' Hz'):'waiting';"
        "document.getElementById('temperature').textContent=s.inverter.live.valid?(s.inverter.live.booster_temp_c.toFixed(1)+' / '+s.inverter.live.inverter_temp_c.toFixed(1)+' C'):'waiting';"
        "if(invModal){fillInvModal(s);}"
        "}catch(e){console.log('Status fetch failed',e);}}"
        "document.getElementById('restart-form').addEventListener('submit',function(e){if(!confirm('Restart the device now?')){e.preventDefault();}});"
        "document.getElementById('admin-pw-form').addEventListener('submit',function(e){var n=document.getElementById('adm-new').value,c=document.getElementById('adm-new2').value;if(n!==c){e.preventDefault();alert('New passwords do not match');return;}if(!confirm('Change the admin sign-in password? You will use it on the next login.')){e.preventDefault();}});"
        "(function(){var pill=document.getElementById('inv-state');var m=document.getElementById('inv-status-modal');var ok=document.getElementById('inv-status-ok');"
        "var pvoBtn=document.getElementById('pvo-result-help-btn');var pvoM=document.getElementById('pvo-result-modal');var pvoOk=document.getElementById('pvo-result-help-ok');"
        "var tempBtn=document.getElementById('temp-help-btn');var tempM=document.getElementById('temp-help-modal');var tempOk=document.getElementById('temp-help-ok');"
        "var rs485Btn=document.getElementById('rs485-help-btn');var rs485M=document.getElementById('rs485-help-modal');var rs485Ok=document.getElementById('rs485-help-ok');"
        "if(pill){pill.addEventListener('click',openInvModal);pill.addEventListener('keydown',function(e){if(e.key==='Enter'||e.key===' '){e.preventDefault();openInvModal();}});}"
        "if(m){m.addEventListener('click',function(e){if(e.target===m)closeInvModal();});}"
        "if(ok){ok.addEventListener('click',closeInvModal);}"
        "if(pvoBtn){pvoBtn.addEventListener('click',openPvoHelpModal);pvoBtn.addEventListener('keydown',function(e){if(e.key==='Enter'||e.key===' '){e.preventDefault();openPvoHelpModal();}});}"
        "if(pvoM){pvoM.addEventListener('click',function(e){if(e.target===pvoM)closePvoHelpModal();});}"
        "if(pvoOk){pvoOk.addEventListener('click',closePvoHelpModal);}"
        "if(tempBtn){tempBtn.addEventListener('click',openTempHelpModal);tempBtn.addEventListener('keydown',function(e){if(e.key==='Enter'||e.key===' '){e.preventDefault();openTempHelpModal();}});}"
        "if(tempM){tempM.addEventListener('click',function(e){if(e.target===tempM)closeTempHelpModal();});}"
        "if(tempOk){tempOk.addEventListener('click',closeTempHelpModal);}"
        "if(rs485Btn){rs485Btn.addEventListener('click',openRs485HelpModal);rs485Btn.addEventListener('keydown',function(e){if(e.key==='Enter'||e.key===' '){e.preventDefault();openRs485HelpModal();}});}"
        "if(rs485M){rs485M.addEventListener('click',function(e){if(e.target===rs485M)closeRs485HelpModal();});}"
        "if(rs485Ok){rs485Ok.addEventListener('click',closeRs485HelpModal);}"
        "document.addEventListener('keydown',function(e){if(e.key==='Escape'){if(invModal)closeInvModal();else if(pvoHelpModal)closePvoHelpModal();else if(tempHelpModal)closeTempHelpModal();else if(rs485HelpModal)closeRs485HelpModal();}});"
        "})();"
        "refresh();setInterval(refresh,%u);"
        "</script>"
        "</main></body></html>",
        s_sta_connected ? "ok" : "bad",
        s_sta_connected ? "Connected" : "Not connected",
        lan_url,
        mdns_host,
        setup_ap_state,
        setup_ap_url,
        s_sntp_synced ? "ok" : "warn",
        s_sntp_synced ? "Synced" : "Waiting",
        s_ntp_server,
        local_hms_page,
        s_tz_id,
        uptime,
        s_inverter_offline ? "bad" : "ok",
        s_inverter_offline ? "Offline" : "Online",
        s_inverter_address,
        (double)s_output_power_w,
        (double)s_energy_today_wh / 1000.0,
        (double)s_grid_voltage_v,
        (double)s_grid_frequency_hz,
        (double)s_booster_temp_c,
        (double)s_inverter_temp_c,
        esc_iv_g,
        esc_iv_inv,
        esc_iv_c1,
        esc_iv_c2,
        esc_iv_al,
        pvoutput_config_is_ready() ? (s_pvoutput_config.interval_minutes == PVOUTPUT_INTERVAL_OFF ? "Configured, off" : "Configured") : "Not configured",
        s_pvoutput_config.interval_minutes,
        esc_pvo_lastpost_init,
        esc_pvo_result_init,
        esc_pvo_base,
        esc_pvo_sid,
        esc_pvo_api,
        s_pvoutput_config.interval_minutes == PVOUTPUT_INTERVAL_OFF ? " selected" : "",
        s_pvoutput_config.interval_minutes == PVOUTPUT_INTERVAL_1_MIN ? " selected" : "",
        s_pvoutput_config.interval_minutes == PVOUTPUT_INTERVAL_2_MIN ? " selected" : "",
        s_pvoutput_config.interval_minutes == PVOUTPUT_INTERVAL_3_MIN ? " selected" : "",
        s_pvoutput_config.interval_minutes == PVOUTPUT_INTERVAL_4_MIN ? " selected" : "",
        s_pvoutput_config.interval_minutes == PVOUTPUT_INTERVAL_5_MIN ? " selected" : "",
        s_pvoutput_config.interval_minutes == PVOUTPUT_INTERVAL_10_MIN ? " selected" : "",
        s_pvoutput_config.interval_minutes == PVOUTPUT_INTERVAL_15_MIN ? " selected" : "",
        s_pvoutput_config.flags == (PVOUTPUT_FLAG_INSTANT_POWER | PVOUTPUT_FLAG_GRID_VOLTAGE | PVOUTPUT_FLAG_TEMPERATURE) ? " selected" : "",
        s_pvoutput_config.flags == PVOUTPUT_FLAG_INSTANT_POWER ? " selected" : "",
        s_pvoutput_config.flags == 0 ? " selected" : "",
        (unsigned)(DASHBOARD_REFRESH_SECONDS * 1000U));

    if (len < 0 || len >= (int)sizeof(s_root_html)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, s_root_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_login_page(req);
        return ESP_OK;
    }
    return root_send_dashboard_page(req);
}

static const char s_dashboard_doc_1[] =
    "<!doctype html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\">"
    "<link rel=\"icon\" href=\"/favicon.ico\" type=\"image/png\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Aurora — Charts</title>"
    "<style>"
    ":root{color-scheme:light;--bg:#eef3f8;--card:#fff;--line:#d7e0ea;--text:#17202a;--muted:#607086}"
    "body{font-family:system-ui,Segoe UI,sans-serif;margin:0;background:var(--bg);color:var(--text)}"
    "main{max-width:72rem;margin:0 auto;padding:1.25rem}"
    ".nav{margin-bottom:1rem}a.navlink{color:#2459a6;font-weight:650;text-decoration:none}"
    ".gridc{display:grid;grid-template-columns:repeat(auto-fit,minmax(22rem,1fr));gap:1rem;margin-top:1rem}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:1rem;padding:1rem;box-shadow:0 1px 2px rgba(20,30,40,.04)}"
    "h1{font-size:1.35rem;margin:0 0 .5rem}h2{font-size:1rem;margin:0 0 .75rem;color:#263447}"
    ".hint{color:var(--muted);font-size:.9rem;margin:.25rem 0 1rem}"
    "</style>"
    "<script src=\"/static/chart.umd.min.js\"></script>"
    "</head><body><main>"
    "<nav><a class=\"navlink\" href=\"/\">← Home</a></nav>"
    "<h1>History charts</h1>"
    "<p class=\"hint\">Today's chart data: 5-minute JSON samples (LittleFS, 1 MiB); x-axis half-hour marks from first to last sample.</p>"
    "<div class=\"gridc\">"
    "<section class=\"card\"><h2>Output power (W)</h2><canvas id=\"cP\"></canvas></section>"
    "<section class=\"card\"><h2>Grid voltage (V)</h2><canvas id=\"cV\"></canvas></section>"
    "</div>"
    "<section class=\"card\" style=\"margin-top:1rem\"><h2>Energy today (kWh)</h2><canvas id=\"cE\"></canvas></section>"
    "<script>";

static const char s_dashboard_doc_2[] =
    "function pad2(n){return n<10?'0'+n:''+n;}"
    "function fmtHm(sec){var d=new Date(sec*1000);return pad2(d.getHours())+':'+pad2(d.getMinutes());}"
    "function tipTitle(items){if(!items||!items.length)return'';var p=items[0].parsed;if(p&&typeof p.x==='number')return fmtHm(p.x);"
    "var r=items[0].raw;return r&&typeof r.x==='number'?fmtHm(r.x):'';}"
    "function floorLocalHalfHourSec(sec){var d=new Date(sec*1000);var m=d.getHours()*60+d.getMinutes();"
    "var f=Math.floor(m/30)*30;d.setHours(Math.floor(f/60),f%60,0,0);return Math.floor(d.getTime()/1000);}"
    "function xScaleHalfHour(t0,t1){var lo=floorLocalHalfHourSec(t0),hi=t1;if(hi<=lo){hi=lo+1800;}"
    "return{type:'linear',min:lo,max:hi,ticks:{stepSize:1800,autoSkip:false,maxRotation:90,minRotation:90,color:'#607086',font:{size:10},"
    "callback:function(v){return fmtHm(v);}},grid:{display:false}};}"
    "async function go(){"
    "const r=await fetch('/api/samples?today=1&limit=320',{credentials:'same-origin',cache:'no-store'});"
    "if(r.status===401){location.href='/';return;}"
    "const j=await r.json();"
    "if(!j.ok||!j.samples||j.samples.length===0){"
    "var m=document.createElement('p');m.className='hint';m.textContent='No samples for today yet.';"
    "document.querySelector('main').appendChild(m);return;}"
    "var s=j.samples,t0=s[0].unix_sec,t1=s[s.length-1].unix_sec,cP='#067647',cV='#dc2626',cE='#22c55e',"
    "tip={callbacks:{title:tipTitle}};"
    "new Chart(document.getElementById('cP'),{type:'line',data:{datasets:[{label:'W',data:s.map(function(x){return{x:x.unix_sec,y:x.output_power_w};}),borderColor:cP,tension:0.15}]},options:{parsing:false,responsive:true,plugins:{legend:{display:true,labels:{color:cP}},tooltip:tip},scales:{x:xScaleHalfHour(t0,t1),y:{beginAtZero:true}}}});"
    "new Chart(document.getElementById('cV'),{type:'line',data:{datasets:[{label:'V',data:s.map(function(x){return{x:x.unix_sec,y:x.grid_voltage_v};}),borderColor:cV,tension:0.15}]},options:{parsing:false,responsive:true,plugins:{legend:{display:true,labels:{color:cV}},tooltip:tip},scales:{x:xScaleHalfHour(t0,t1),y:{beginAtZero:false}}}});"
    "new Chart(document.getElementById('cE'),{type:'line',data:{datasets:[{label:'kWh',data:s.map(function(x){return{x:x.unix_sec,y:x.energy_today_kwh};}),borderColor:cE,tension:0.15}]},options:{parsing:false,responsive:true,plugins:{legend:{display:true,labels:{color:cE}},tooltip:tip},scales:{x:xScaleHalfHour(t0,t1),y:{beginAtZero:false}}}});"
    "}"
    "go();"
    "</script></main></body></html>";

static esp_err_t dashboard_get_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_login_page(req);
        return ESP_OK;
    }

    int len = snprintf(s_dashboard_html, sizeof(s_dashboard_html), "%s%s", s_dashboard_doc_1, s_dashboard_doc_2);
    if (len < 0 || len >= (int)sizeof(s_dashboard_html)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, s_dashboard_html, len);
    return ESP_OK;
}

static esp_err_t status_send_json_payload(httpd_req_t *req)
{
    char state_hex[3 * AURORA_RESPONSE_LEN + 1] = {0};
    char uptime[16] = {0};
    for (int i = 0; i < AURORA_RESPONSE_LEN; ++i) {
        snprintf(state_hex + (i * 3), sizeof(state_hex) - (i * 3), "%02x%s",
                 s_inverter_state[i], i + 1 == AURORA_RESPONSE_LEN ? "" : " ");
    }
    format_uptime_human(uptime, sizeof(uptime));

    char pv_result_txt[192];
    char pv_post_str[16];
    pvoutput_format_last_result(s_pvoutput_last_result, pv_result_txt, sizeof(pv_result_txt));
    pvoutput_format_last_post_hhmmss(s_pvoutput_last_post_unix, pv_post_str, sizeof(pv_post_str));
    char esc_pv_result[384];
    char esc_pv_post[64];
    json_escape_string(pv_result_txt, esc_pv_result, sizeof(esc_pv_result));
    json_escape_string(pv_post_str, esc_pv_post, sizeof(esc_pv_post));

    char ig[192], iinv[192], ic1[192], ic2[192], ial[192];
    char esc_ig[512], esc_iinv[512], esc_ic1[512], esc_ic2[512], esc_ial[512];
    if (s_inverter_offline) {
        const char *u = "Unavailable";
        strlcpy(ig, u, sizeof(ig));
        strlcpy(iinv, u, sizeof(iinv));
        strlcpy(ic1, u, sizeof(ic1));
        strlcpy(ic2, u, sizeof(ic2));
        strlcpy(ial, u, sizeof(ial));
    } else {
        aurora_decode_inverter_status(s_inverter_state, ig, sizeof(ig), iinv, sizeof(iinv), ic1, sizeof(ic1), ic2, sizeof(ic2), ial,
                                      sizeof(ial));
    }
    json_escape_string(ig, esc_ig, sizeof(esc_ig));
    json_escape_string(iinv, esc_iinv, sizeof(esc_iinv));
    json_escape_string(ic1, esc_ic1, sizeof(esc_ic1));
    json_escape_string(ic2, esc_ic2, sizeof(esc_ic2));
    json_escape_string(ial, esc_ial, sizeof(esc_ial));

    char local_hms[16];
    format_local_time_hms(local_hms, sizeof(local_hms));

    int len = snprintf(
        s_status_json,
        sizeof(s_status_json),
        "{"
        "\"ok\":true,"
        "\"version\":\"%s\","
        "\"hostname\":\"%s\","
        "\"uptime\":\"%s\","
        "\"system\":{\"reset_reason\":%d,\"heap\":{\"free_bytes\":%" PRIu32 ",\"minimum_free_bytes\":%" PRIu32 "}},"
        "\"provisioning_ap\":{\"active\":%s,\"ssid\":\"%s\",\"ip\":\"%s\"},"
        "\"sta\":{\"configured\":%s,\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\"},"
        "\"time\":{\"sntp_started\":%s,\"sntp_synced\":%s,\"ntp_server\":\"%s\",\"tz\":\"%s\",\"local_time_hms\":\"%s\"},"
        "\"retention\":{\"sample_count\":%u,\"sample_capacity\":%u,\"sample_interval_seconds\":%u},"
        "\"pvoutput\":{\"configured\":%s,\"enabled\":%s,\"base_url\":\"%s\","
        "\"system_id_set\":%s,\"api_key_set\":%s,\"interval_minutes\":%u,\"flags\":%u,"
        "\"last_result\":%d,\"last_result_text\":\"%s\",\"last_post_hhmmss\":\"%s\","
        "\"last_attempt_ms\":%" PRIu32 ",\"last_success_ms\":%" PRIu32 ","
        "\"success_count\":%" PRIu32 ",\"fail_count\":%" PRIu32 ",\"queue_depth\":0},"
        "\"inverter\":{\"address\":%d,\"rs485_ready\":%s,\"offline\":%s,"
        "\"poll_count\":%" PRIu32 ",\"ok_count\":%" PRIu32 ",\"fail_count\":%" PRIu32 ","
        "\"consecutive_failures\":%" PRIu32 ",\"last_result\":%d,"
        "\"last_poll_ms\":%" PRIu32 ",\"last_ok_ms\":%" PRIu32 ",\"state_hex\":\"%s\","
        "\"state_text\":{\"global\":\"%s\",\"inverter\":\"%s\",\"chn1\":\"%s\",\"chn2\":\"%s\",\"alarm\":\"%s\"},"
        "\"live\":{\"valid\":%s,\"last_result\":%d,\"output_power_w\":%.1f,"
        "\"grid_voltage_v\":%.1f,\"grid_frequency_hz\":%.2f,"
        "\"booster_temp_c\":%.1f,\"inverter_temp_c\":%.1f,"
        "\"energy_today_wh\":%" PRIu32 ",\"energy_today_kwh\":%.3f}},"
        "\"auth\":{\"failed_total\":%" PRIu32 ",\"lockout_total\":%" PRIu32 "},"
        "\"ui\":{\"dashboard_refresh_seconds\":%u,\"firmware_build_timestamp\":\"%s\"},"
        "\"health\":{\"wifi_ok\":%s,\"time_ok\":%s,\"rs485_ready\":%s,\"inverter_online\":%s}"
        "}\n",
        APP_VERSION,
        s_hostname,
        uptime,
        esp_reset_reason(),
        (uint32_t)esp_get_free_heap_size(),
        (uint32_t)esp_get_minimum_free_heap_size(),
        s_provisioning_active ? "true" : "false",
        s_provisioning_ssid,
        PROVISIONING_AP_IP,
        s_sta_has_config ? "true" : "false",
        s_sta_connected ? "true" : "false",
        s_sta_ssid,
        s_sta_ip,
        s_sntp_started ? "true" : "false",
        s_sntp_synced ? "true" : "false",
        s_ntp_server,
        s_tz_id,
        local_hms,
        (unsigned)s_sample_count,
        (unsigned)s_sample_capacity,
        SAMPLE_INTERVAL_SECONDS,
        pvoutput_config_is_ready() ? "true" : "false",
        s_pvoutput_config.interval_minutes != PVOUTPUT_INTERVAL_OFF ? "true" : "false",
        s_pvoutput_config.base_url,
        s_pvoutput_config.system_id[0] != '\0' ? "true" : "false",
        s_pvoutput_config.api_key[0] != '\0' ? "true" : "false",
        s_pvoutput_config.interval_minutes,
        s_pvoutput_config.flags,
        s_pvoutput_last_result,
        esc_pv_result,
        esc_pv_post,
        s_pvoutput_last_attempt_ms,
        s_pvoutput_last_success_ms,
        s_pvoutput_success_count,
        s_pvoutput_fail_count,
        s_inverter_address,
        s_rs485_ready ? "true" : "false",
        s_inverter_offline ? "true" : "false",
        s_rs485_poll_count,
        s_rs485_ok_count,
        s_rs485_fail_count,
        s_rs485_consecutive_failures,
        s_rs485_last_result,
        s_rs485_last_poll_ms,
        s_rs485_last_ok_ms,
        state_hex,
        esc_ig,
        esc_iinv,
        esc_ic1,
        esc_ic2,
        esc_ial,
        s_live_metrics_valid ? "true" : "false",
        s_live_metrics_last_result,
        (double)s_output_power_w,
        (double)s_grid_voltage_v,
        (double)s_grid_frequency_hz,
        (double)s_booster_temp_c,
        (double)s_inverter_temp_c,
        s_energy_today_wh,
        (double)s_energy_today_wh / 1000.0,
        s_auth_failed_total,
        s_auth_lockout_total,
        (unsigned)DASHBOARD_REFRESH_SECONDS,
        FIRMWARE_BUILD_TIMESTAMP,
        s_sta_connected ? "true" : "false",
        s_sntp_synced ? "true" : "false",
        s_rs485_ready ? "true" : "false",
        s_inverter_offline ? "false" : "true");

    if (len < 0 || len >= (int)sizeof(s_status_json)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, s_status_json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    if (!web_session_accept(req, false)) {
        send_json_session_required(req);
        return ESP_OK;
    }
    return status_send_json_payload(req);
}

static unsigned samples_limit_from_query(httpd_req_t *req)
{
    char query[128] = {0};
    char limit_text[12] = {0};
    unsigned limit = SAMPLES_API_DEFAULT_LIMIT;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "limit", limit_text, sizeof(limit_text)) == ESP_OK) {
        int parsed = atoi(limit_text);
        if (parsed > 0) {
            limit = (unsigned)parsed;
        }
    }

    if (limit > SAMPLES_API_MAX_LIMIT) {
        limit = SAMPLES_API_MAX_LIMIT;
    }
    return limit;
}

static bool samples_today_query(httpd_req_t *req)
{
    char query[96] = {0};
    char val[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(query, "today", val, sizeof(val)) != ESP_OK) {
        return false;
    }
    return val[0] == '1' && val[1] == '\0';
}

static esp_err_t samples_get_handler(httpd_req_t *req)
{
    if (!web_session_accept(req, false)) {
        send_json_session_required(req);
        return ESP_OK;
    }

    const bool today_only = samples_today_query(req);
    const unsigned requested_limit = samples_limit_from_query(req);
    unsigned emit_cap = requested_limit;
    if (emit_cap > SAMPLES_API_MAX_LIMIT) {
        emit_cap = SAMPLES_API_MAX_LIMIT;
    }

    size_t count = 0;
    size_t capacity = s_sample_capacity;
    size_t write_index = 0;
    int offset = 0;

    if (s_samples == NULL || s_sample_mutex == NULL || capacity == 0) {
        int len = snprintf(s_samples_json, sizeof(s_samples_json),
                           "{\"ok\":true,\"sample_count\":0,\"sample_capacity\":0,\"samples\":[]}\n");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, s_samples_json, len);
        return ESP_OK;
    }

    if (xSemaphoreTake(s_sample_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"samples_busy\",\"message\":\"Sample ring is busy\"}\n");
        return ESP_OK;
    }

    count = s_sample_count;
    write_index = s_sample_write_index;

    if (today_only && daylog_is_mounted()) {
        uint32_t cc = (uint32_t)count;
        uint32_t capu = (uint32_t)capacity;
        xSemaphoreGive(s_sample_mutex);
        int outl = 0;
        esp_err_t dr = daylog_format_today_json(cc, capu, SAMPLE_INTERVAL_SECONDS, s_samples_json, sizeof(s_samples_json), &outl);
        if (dr != ESP_OK || outl <= 0 || outl >= (int)sizeof(s_samples_json)) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, s_samples_json, (size_t)outl);
        return ESP_OK;
    }

    time_t tw = time(NULL);
    struct tm tnow;
    localtime_r(&tw, &tnow);

    if (!today_only) {
        unsigned limit = emit_cap;
        if (limit > (unsigned)count) {
            limit = (unsigned)count;
        }

        offset = snprintf(s_samples_json, sizeof(s_samples_json),
                            "{\"ok\":true,\"sample_count\":%u,\"sample_capacity\":%u,"
                            "\"sample_interval_seconds\":%u,\"returned\":%u,\"today_only\":false,\"samples\":[",
                            (unsigned)count, (unsigned)capacity, SAMPLE_INTERVAL_SECONDS, limit);

        for (unsigned i = 0; i < limit && offset > 0 && offset < (int)sizeof(s_samples_json); ++i) {
            size_t ring_index = (write_index + capacity - limit + i) % capacity;
            const sample_t *sample = &s_samples[ring_index];
            offset += snprintf(
                s_samples_json + offset,
                sizeof(s_samples_json) - offset,
                "%s{\"uptime_ms\":%" PRIu32 ",\"unix_sec\":%" PRIu32 ",\"output_power_w\":%.1f,"
                "\"grid_voltage_v\":%.1f,\"grid_frequency_hz\":%.2f,"
                "\"booster_temp_c\":%.1f,\"inverter_temp_c\":%.1f,"
                "\"energy_today_wh\":%" PRIu32 ",\"energy_today_kwh\":%.3f}",
                i == 0 ? "" : ",",
                sample->uptime_ms,
                sample->unix_sec,
                (double)sample->output_power_w,
                (double)sample->grid_voltage_v,
                (double)sample->grid_frequency_hz,
                (double)sample->booster_temp_c,
                (double)sample->inverter_temp_c,
                sample->energy_today_wh,
                (double)sample->energy_today_wh / 1000.0);
        }
    } else {
        size_t *pick = NULL;
        size_t n_pick = 0;

        if (count > 0) {
            pick = (size_t *)malloc(count * sizeof(size_t));
            if (pick == NULL) {
                xSemaphoreGive(s_sample_mutex);
                httpd_resp_set_status(req, "503 Service Unavailable");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"samples_oom\",\"message\":\"Out of memory\"}\n");
                return ESP_OK;
            }
            for (size_t i = 0; i < count; ++i) {
                size_t ring_index = (write_index + capacity - count + i) % capacity;
                const sample_t *sp = &s_samples[ring_index];
                if (sp->unix_sec == 0U) {
                    continue;
                }
                time_t ts = (time_t)sp->unix_sec;
                struct tm ts_tm;
                localtime_r(&ts, &ts_tm);
                if (ts_tm.tm_year == tnow.tm_year && ts_tm.tm_yday == tnow.tm_yday) {
                    pick[n_pick++] = ring_index;
                }
            }
        }

        size_t stride = 1;
        if (n_pick > (size_t)emit_cap && emit_cap > 0) {
            stride = (n_pick + (size_t)emit_cap - 1U) / (size_t)emit_cap;
        }
        unsigned returned = 0;
        if (n_pick > 0 && stride > 0) {
            returned = (unsigned)((n_pick + stride - 1U) / stride);
        }

        offset = snprintf(s_samples_json, sizeof(s_samples_json),
                          "{\"ok\":true,\"sample_count\":%u,\"sample_capacity\":%u,"
                          "\"sample_interval_seconds\":%u,\"returned\":%u,\"today_only\":true,\"samples\":[",
                          (unsigned)count, (unsigned)capacity, SAMPLE_INTERVAL_SECONDS, returned);

        unsigned out_i = 0;
        for (size_t k = 0; k < n_pick && offset > 0 && offset < (int)sizeof(s_samples_json); k += stride) {
            const sample_t *sample = &s_samples[pick[k]];
            offset += snprintf(
                s_samples_json + offset,
                sizeof(s_samples_json) - offset,
                "%s{\"uptime_ms\":%" PRIu32 ",\"unix_sec\":%" PRIu32 ",\"output_power_w\":%.1f,"
                "\"grid_voltage_v\":%.1f,\"grid_frequency_hz\":%.2f,"
                "\"booster_temp_c\":%.1f,\"inverter_temp_c\":%.1f,"
                "\"energy_today_wh\":%" PRIu32 ",\"energy_today_kwh\":%.3f}",
                out_i == 0 ? "" : ",",
                sample->uptime_ms,
                sample->unix_sec,
                (double)sample->output_power_w,
                (double)sample->grid_voltage_v,
                (double)sample->grid_frequency_hz,
                (double)sample->booster_temp_c,
                (double)sample->inverter_temp_c,
                sample->energy_today_wh,
                (double)sample->energy_today_wh / 1000.0);
            out_i++;
        }

        if (pick != NULL) {
            free(pick);
        }
    }

    xSemaphoreGive(s_sample_mutex);

    if (offset < 0 || offset >= (int)sizeof(s_samples_json)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    offset += snprintf(s_samples_json + offset, sizeof(s_samples_json) - offset, "]}\n");
    if (offset < 0 || offset >= (int)sizeof(s_samples_json)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, s_samples_json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t samples_raw_csv_get_handler(httpd_req_t *req)
{
    if (!web_session_accept(req, false)) {
        send_json_session_required(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"samples-today-raw.csv\"");

    static const char header[] =
        "uptime_ms,unix_sec,output_power_w,grid_voltage_v,grid_frequency_hz,booster_temp_c,"
        "inverter_temp_c,energy_today_wh,energy_today_kwh\r\n";
    esp_err_t err = httpd_resp_send_chunk(req, header, sizeof(header) - 1U);
    if (err != ESP_OK) {
        return err;
    }

    if (s_samples != NULL && s_sample_mutex != NULL && s_sample_capacity > 0U) {
        if (xSemaphoreTake(s_sample_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            time_t tw = time(NULL);
            struct tm tnow;
            localtime_r(&tw, &tnow);

            const size_t count = s_sample_count;
            const size_t capacity = s_sample_capacity;
            const size_t write_index = s_sample_write_index;
            char line[256];

            for (size_t i = 0; i < count; ++i) {
                const size_t ring_index = (write_index + capacity - count + i) % capacity;
                const sample_t *sample = &s_samples[ring_index];
                if (sample->unix_sec == 0U) {
                    continue;
                }
                time_t ts = (time_t)sample->unix_sec;
                struct tm ts_tm;
                localtime_r(&ts, &ts_tm);
                if (ts_tm.tm_year != tnow.tm_year || ts_tm.tm_yday != tnow.tm_yday) {
                    continue;
                }
                const int ln = snprintf(
                    line,
                    sizeof(line),
                    "%" PRIu32 ",%" PRIu32 ",%.1f,%.1f,%.2f,%.1f,%.1f,%" PRIu32 ",%.3f\r\n",
                    sample->uptime_ms,
                    sample->unix_sec,
                    (double)sample->output_power_w,
                    (double)sample->grid_voltage_v,
                    (double)sample->grid_frequency_hz,
                    (double)sample->booster_temp_c,
                    (double)sample->inverter_temp_c,
                    sample->energy_today_wh,
                    (double)sample->energy_today_wh / 1000.0);
                if (ln <= 0 || ln >= (int)sizeof(line)) {
                    continue;
                }
                err = httpd_resp_send_chunk(req, line, (size_t)ln);
                if (err != ESP_OK) {
                    break;
                }
            }
            xSemaphoreGive(s_sample_mutex);
        } else {
            err = ESP_ERR_TIMEOUT;
        }
    }

    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t pvoutput_config_post_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_json_session_required(req);
        return ESP_OK;
    }

    if (req->content_len > FORM_BODY_MAX_LEN) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"body_too_large\",\"message\":\"Form body too large\"}\n");
        return ESP_OK;
    }

    char body[FORM_BODY_MAX_LEN + 1];
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    pvoutput_config_t next = s_pvoutput_config;
    char interval_text[8] = {0};
    char flags_text[8] = {0};
    char api_key[PVOUTPUT_API_KEY_MAX_LEN] = {0};

    if (!extract_form_value(body, "base_url", next.base_url, sizeof(next.base_url)) ||
        !extract_form_value(body, "system_id", next.system_id, sizeof(next.system_id)) ||
        !extract_form_value(body, "api_key", api_key, sizeof(api_key)) ||
        !extract_form_value(body, "interval", interval_text, sizeof(interval_text)) ||
        !extract_form_value(body, "flags", flags_text, sizeof(flags_text))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"missing_field\",\"message\":\"Missing PVOutput field\"}\n");
        return ESP_OK;
    }

    if (api_key[0] != '\0') {
        strlcpy(next.api_key, api_key, sizeof(next.api_key));
    }
    next.interval_minutes = (uint8_t)atoi(interval_text);
    next.flags = (uint8_t)atoi(flags_text);

    esp_err_t err = save_pvoutput_config(&next);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"invalid_pvoutput_config\",\"message\":\"PVOutput settings are invalid\"}\n");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"PVOutput settings saved\"}\n");
    return ESP_OK;
}

static esp_err_t pvoutput_test_request(void)
{
    if (!pvoutput_config_is_ready()) {
        s_pvoutput_last_result = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_sta_connected) {
        s_pvoutput_last_result = ESP_ERR_WIFI_NOT_CONNECT;
        return ESP_ERR_WIFI_NOT_CONNECT;
    }

    char url[PVOUTPUT_BASE_URL_MAX_LEN + sizeof("getstatus.jsp")];
    int url_len = snprintf(url, sizeof(url), "%sgetstatus.jsp", s_pvoutput_config.base_url);
    if (url_len < 0 || url_len >= (int)sizeof(url)) {
        s_pvoutput_last_result = ESP_ERR_INVALID_ARG;
        return ESP_ERR_INVALID_ARG;
    }

    s_pvoutput_last_attempt_ms = esp_log_timestamp();
    esp_http_client_config_t client_config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = PVOUTPUT_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&client_config);
    if (client == NULL) {
        s_pvoutput_last_result = ESP_ERR_NO_MEM;
        s_pvoutput_fail_count++;
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "X-Pvoutput-Apikey", s_pvoutput_config.api_key);
    esp_http_client_set_header(client, "X-Pvoutput-SystemId", s_pvoutput_config.system_id);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status >= 200 && status < 300) {
        s_pvoutput_last_result = status;
        s_pvoutput_last_success_ms = esp_log_timestamp();
        s_pvoutput_success_count++;
        ESP_LOGI(TAG, "PVOutput test request succeeded: HTTP %d", status);
        return ESP_OK;
    }

    s_pvoutput_last_result = err == ESP_OK ? status : err;
    s_pvoutput_fail_count++;
    ESP_LOGW(TAG, "PVOutput test request failed: err=%s status=%d", esp_err_to_name(err), status);
    return err == ESP_OK ? ESP_FAIL : err;
}

static esp_err_t pvoutput_upload_live_status(void)
{
    if (!pvoutput_config_is_ready() || !s_sta_connected || !s_sntp_synced || !s_live_metrics_valid) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t mono_ms = esp_log_timestamp();
    if (s_live_metrics_last_ok_ms == 0 || (mono_ms - s_live_metrics_last_ok_ms) > 60000U) {
        return ESP_ERR_INVALID_STATE;
    }

    time_t now = time(NULL);
    if (now < 100000) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t period_sec = (int64_t)s_pvoutput_config.interval_minutes * 60LL;
    const int64_t slot = (int64_t)now / period_sec;
    if (slot == s_pvoutput_last_live_slot) {
        return ESP_OK;
    }

    struct tm local_now;
    localtime_r(&now, &local_now);

    char url[PVOUTPUT_BASE_URL_MAX_LEN + sizeof("addstatus.jsp")];
    int url_len = snprintf(url, sizeof(url), "%saddstatus.jsp", s_pvoutput_config.base_url);
    if (url_len < 0 || url_len >= (int)sizeof(url)) {
        s_pvoutput_last_result = ESP_ERR_INVALID_ARG;
        return ESP_ERR_INVALID_ARG;
    }

    char body[256];
    int body_len = snprintf(
        body,
        sizeof(body),
        "d=%04d%02d%02d&t=%02d:%02d&v1=%" PRIu32,
        local_now.tm_year + 1900,
        local_now.tm_mon + 1,
        local_now.tm_mday,
        local_now.tm_hour,
        local_now.tm_min,
        s_energy_today_wh);
    if (body_len < 0 || body_len >= (int)sizeof(body)) {
        s_pvoutput_last_result = ESP_ERR_INVALID_SIZE;
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_pvoutput_config.flags & PVOUTPUT_FLAG_INSTANT_POWER) {
        body_len += snprintf(body + body_len, sizeof(body) - body_len, "&v2=%.0f", (double)s_output_power_w);
    }
    if (s_pvoutput_config.flags & PVOUTPUT_FLAG_TEMPERATURE) {
        body_len += snprintf(body + body_len, sizeof(body) - body_len, "&v5=%.1f", (double)s_inverter_temp_c);
    }
    if (s_pvoutput_config.flags & PVOUTPUT_FLAG_GRID_VOLTAGE) {
        body_len += snprintf(body + body_len, sizeof(body) - body_len, "&v6=%.1f", (double)s_grid_voltage_v);
    }
    if (body_len < 0 || body_len >= (int)sizeof(body)) {
        s_pvoutput_last_result = ESP_ERR_INVALID_SIZE;
        return ESP_ERR_INVALID_SIZE;
    }

    s_pvoutput_last_attempt_ms = esp_log_timestamp();
    esp_http_client_config_t client_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = PVOUTPUT_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&client_config);
    if (client == NULL) {
        s_pvoutput_last_result = ESP_ERR_NO_MEM;
        s_pvoutput_fail_count++;
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "X-Pvoutput-Apikey", s_pvoutput_config.api_key);
    esp_http_client_set_header(client, "X-Pvoutput-SystemId", s_pvoutput_config.system_id);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    s_pvoutput_last_live_slot = slot;
    if (err == ESP_OK && status >= 200 && status < 300) {
        s_pvoutput_last_result = status;
        s_pvoutput_last_success_ms = esp_log_timestamp();
        s_pvoutput_last_post_unix = now;
        s_pvoutput_success_count++;
        ESP_LOGI(TAG, "PVOutput live upload succeeded: HTTP %d", status);
        return ESP_OK;
    }

    s_pvoutput_last_result = err == ESP_OK ? status : err;
    s_pvoutput_fail_count++;
    ESP_LOGW(TAG, "PVOutput live upload failed: err=%s status=%d", esp_err_to_name(err), status);
    return err == ESP_OK ? ESP_FAIL : err;
}

static void discard_httpd_request_body(httpd_req_t *req)
{
    char buf[128];
    size_t total = 0;
    const size_t cl = req->content_len;
    while (total < cl) {
        size_t remain = cl - total;
        size_t chunk = remain > sizeof(buf) ? sizeof(buf) : remain;
        int ret = httpd_req_recv(req, buf, (int)chunk);
        if (ret <= 0) {
            break;
        }
        total += (size_t)ret;
    }
}

static esp_err_t pvoutput_test_post_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_json_session_required(req);
        return ESP_OK;
    }

    discard_httpd_request_body(req);

    esp_err_t err = pvoutput_test_request();
    int len = snprintf(
        s_config_json,
        sizeof(s_config_json),
        "{\"ok\":%s,\"last_result\":%d,\"success_count\":%" PRIu32 ",\"fail_count\":%" PRIu32 "}\n",
        err == ESP_OK ? "true" : "false",
        s_pvoutput_last_result,
        s_pvoutput_success_count,
        s_pvoutput_fail_count);

    if (len < 0 || len >= (int)sizeof(s_config_json)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, s_config_json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t device_config_post_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_json_session_required(req);
        return ESP_OK;
    }

    if (req->content_len > FORM_BODY_MAX_LEN) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"body_too_large\",\"message\":\"Form body too large\"}\n");
        return ESP_OK;
    }

    char body[FORM_BODY_MAX_LEN + 1];
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    char tz_id[TZ_ID_MAX_LEN] = {0};
    if (!extract_form_value(body, "tz_id", tz_id, sizeof(tz_id))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"missing_field\",\"message\":\"Missing tz_id field\"}\n");
        return ESP_OK;
    }

    esp_err_t err = save_device_settings(s_ntp_server, tz_id);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"invalid_device_settings\",\"message\":\"Device settings are invalid\"}\n");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Device settings saved\"}\n");
    return ESP_OK;
}

static esp_err_t admin_password_post_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_json_session_required(req);
        return ESP_OK;
    }

    if (req->content_len > FORM_BODY_MAX_LEN) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"body_too_large\",\"message\":\"Form body too large\"}\n");
        return ESP_OK;
    }

    char body[FORM_BODY_MAX_LEN + 1];
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    char current_password[ADMIN_PASSWORD_MAX_LEN + 1] = {0};
    char new_password[ADMIN_PASSWORD_MAX_LEN + 1] = {0};
    char new_password_confirm[ADMIN_PASSWORD_MAX_LEN + 1] = {0};
    if (!extract_form_value(body, "current_password", current_password, sizeof(current_password)) ||
        !extract_form_value(body, "new_password", new_password, sizeof(new_password)) ||
        !extract_form_value(body, "new_password_confirm", new_password_confirm, sizeof(new_password_confirm))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"missing_field\",\"message\":\"Missing password field\"}\n");
        return ESP_OK;
    }

    if (strcmp(new_password, new_password_confirm) != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"password_mismatch\",\"message\":\"New password and confirmation do not match\"}\n");
        return ESP_OK;
    }

    size_t new_len = strlen(new_password);
    if (new_len < ADMIN_PASSWORD_MIN_LEN) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"password_too_short\",\"message\":\"New password is too short\"}\n");
        return ESP_OK;
    }
    if (new_len > ADMIN_PASSWORD_MAX_LEN) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"password_too_long\",\"message\":\"New password is too long\"}\n");
        return ESP_OK;
    }

    if (!verify_admin_password_plain(current_password)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"current_password_invalid\",\"message\":\"Current password is incorrect\"}\n");
        return ESP_OK;
    }

    esp_err_t err = save_admin_password_to_nvs(new_password);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ensure_session_mutex();
    if (s_session_mutex != NULL && xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        clear_web_session_locked();
        xSemaphoreGive(s_session_mutex);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Set-Cookie", SESSION_COOKIE_NAME "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Admin password updated; use the new password on the next login\"}\n");
    return ESP_OK;
}

static esp_err_t device_restart_post_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_json_session_required(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Restart scheduled\"}\n");
    xTaskCreate(delayed_restart_task, "deviceRestart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    (void)req;
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    httpd_resp_send(req, (const char *)aurora_favicon_png, (ssize_t)aurora_favicon_png_len);
    return ESP_OK;
}

static esp_err_t chart_js_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=604800");
    httpd_resp_send(req, (const char *)chart_umd_min_js, (ssize_t)chart_umd_min_js_len);
    return ESP_OK;
}

static esp_err_t maintenance_log_clear_post_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_json_session_required(req);
        return ESP_OK;
    }
    ensure_web_log_mutex();
    if (s_web_log_mutex != NULL && xSemaphoreTake(s_web_log_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_web_log_len = 0;
        s_web_log[0] = '\0';
        xSemaphoreGive(s_web_log_mutex);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true}\n");
    return ESP_OK;
}

static void factory_reset_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static bool schedule_factory_reset_after_response(void)
{
    s_factory_reset_rtc_magic = FACTORY_RESET_RTC_MAGIC;
    if (xTaskCreate(factory_reset_reboot_task, "factoryRst", 2048, NULL, 5, NULL) == pdPASS) {
        return true;
    }
    s_factory_reset_rtc_magic = 0;
    ESP_LOGE(TAG, "factory reset reboot task create failed (internal free=%" PRIu32 ")",
             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return false;
}

static void factory_erase_data_partition(const char *label, esp_partition_subtype_t subtype)
{
    const esp_partition_t *part = NULL;
    if (label != NULL) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    } else {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, NULL);
    }
    if (part == NULL) {
        ESP_LOGW(TAG, "factory reset: partition not found (%s)", label != NULL ? label : "phy/coredump");
        return;
    }
    esp_err_t e = esp_partition_erase_range(part, 0, part->size);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "factory reset: erase %s failed: %s", part->label, esp_err_to_name(e));
    } else {
        ESP_LOGW(TAG, "factory reset: erased %s (%" PRIu32 " kB)", part->label, (uint32_t)(part->size / 1024U));
    }
}

static esp_err_t maintenance_factory_reset_post_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_json_session_required(req);
        return ESP_OK;
    }
    if (!schedule_factory_reset_after_response()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"task\",\"message\":\"Could not start factory reset\"}\n");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Factory reset scheduled; device will reboot\"}\n");
    return ESP_OK;
}

static esp_err_t maintenance_log_txt_get_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_sendstr(req, "Session required. Sign in at the home page, then open this link again.\n");
        return ESP_OK;
    }

    char *buf = (char *)malloc(WEB_LOG_CAP);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t n = 0;
    ensure_web_log_mutex();
    if (s_web_log_mutex != NULL && xSemaphoreTake(s_web_log_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        n = s_web_log_len;
        if (n > WEB_LOG_CAP) {
            n = WEB_LOG_CAP;
        }
        memcpy(buf, s_web_log, n);
        xSemaphoreGive(s_web_log_mutex);
    }

    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t sr = httpd_resp_send(req, buf, n);
    free(buf);
    return (sr == ESP_OK) ? ESP_OK : ESP_FAIL;
}

/** JSON object text for inverter clock compare (embedded in inverter settings HTML). */
static void inverter_clock_fill_json(char *buf, size_t cap)
{
    if (cap < 64U) {
        if (cap > 0U) {
            buf[0] = '\0';
        }
        return;
    }
    if (!rs485_bus_take(pdMS_TO_TICKS(8000))) {
        snprintf(buf, cap, "{\"ok\":false,\"code\":\"busy\"}");
        return;
    }
    uint8_t response[AURORA_RESPONSE_LEN] = {0};
    int r = aurora_poll_value(s_inverter_address, AURORA_TIME_READ_OPCODE, 0, response);
    rs485_bus_give();
    if (r != 0) {
        snprintf(buf, cap, "{\"ok\":false,\"code\":\"read_failed\"}");
        return;
    }
    uint32_t raw = aurora_u32_from_response(response);
    long go = inverter_gmt_offset_seconds();
    time_t inv_epoch = (time_t)raw + AURORA_TIME_BASE_UNIX - (time_t)go;
    time_t dev_epoch = 0;
    time(&dev_epoch);
    snprintf(buf, cap,
             "{\"ok\":true,\"inverter_unix\":%" PRId64 ",\"device_unix\":%" PRId64 ",\"delta_seconds\":%" PRId64
             ",\"raw\":%" PRIu32 ",\"gmt_offset_s\":%ld}",
             (int64_t)inv_epoch, (int64_t)dev_epoch, (int64_t)(inv_epoch - dev_epoch), raw, go);
}

static esp_err_t maintenance_page_get_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_login_page(req);
        return ESP_OK;
    }

    esp_chip_info_t ci;
    esp_chip_info(&ci);

    uint32_t flash_size = 0;
    esp_err_t flerr = esp_flash_get_size(NULL, &flash_size);
    uint32_t flash_kb = (flerr == ESP_OK && flash_size > 0U) ? (flash_size / 1024U) : 0U;

    uint32_t heap_free = (uint32_t)esp_get_free_heap_size();
    uint32_t heap_min = (uint32_t)esp_get_minimum_free_heap_size();
    uint32_t int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t int_used = (int_total > int_free) ? (int_total - int_free) : 0U;

    uint32_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t psram_used = (psram_total > psram_free) ? (psram_total - psram_free) : 0U;

    const esp_partition_t *run = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (run == NULL) {
        run = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    }
    const char *part_label = "—";
    uint32_t part_kb = 0;
    uint32_t part_off_kb = 0;
    if (run != NULL) {
        part_label = run->label;
        part_kb = (uint32_t)(run->size / 1024U);
        part_off_kb = (uint32_t)(run->address / 1024U);
    }

    uint32_t cpu_mhz = (uint32_t)s_cpu_freq_mhz;
    const char *cpu_sel_80 = (s_cpu_freq_mhz == 80U) ? " selected" : "";
    const char *cpu_sel_160 = (s_cpu_freq_mhz == 160U) ? " selected" : "";
    const char *cpu_sel_240 = (s_cpu_freq_mhz == 240U) ? " selected" : "";

    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char flash_row[96];
    if (flerr == ESP_OK && flash_kb > 0U) {
        (void)snprintf(flash_row, sizeof(flash_row), "%" PRIu32 " kB total", flash_kb);
    } else {
        (void)snprintf(flash_row, sizeof(flash_row), "Unknown (%s)", esp_err_to_name(flerr));
    }

    char psram_row[160];
    if (psram_total > 0U) {
        (void)snprintf(psram_row, sizeof(psram_row),
                       "~%" PRIu32 " kB total; ~%" PRIu32 " kB in use; ~%" PRIu32 " kB free",
                       psram_total / 1024U, psram_used / 1024U, psram_free / 1024U);
    } else {
        (void)snprintf(psram_row, sizeof(psram_row), "Not detected");
    }

    char part_row[192];
    if (run != NULL) {
        (void)snprintf(part_row, sizeof(part_row), "%s, offset %" PRIu32 " kB, size %" PRIu32 " kB", part_label, part_off_kb, part_kb);
    } else {
        (void)snprintf(part_row, sizeof(part_row), "Unknown");
    }

    char ram_row[224];
    if (int_total > 0U) {
        (void)snprintf(ram_row, sizeof(ram_row),
                       "%" PRIu32 " kB total; ~%" PRIu32 " kB in use; ~%" PRIu32 " kB free now; min free %" PRIu32 " kB since boot",
                       int_total / 1024U, int_used / 1024U, int_free / 1024U, heap_min / 1024U);
    } else {
        (void)snprintf(ram_row, sizeof(ram_row),
                       "~%" PRIu32 " kB heap free now; min free %" PRIu32 " kB since boot (internal RAM total not reported)",
                       heap_free / 1024U, heap_min / 1024U);
    }

    char daylog_row[384];
    daylog_fill_maintenance_row(daylog_row, sizeof(daylog_row));

    int len = snprintf(
        s_aux_html,
        sizeof(s_aux_html),
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<link rel=\"icon\" href=\"/favicon.ico\" type=\"image/png\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Maintenance</title>"
        "<style>"
        ":root{--bg:#eef3f8;--line:#d7e0ea;--text:#17202a;--muted:#607086}"
        "body{font-family:system-ui,Segoe UI,sans-serif;margin:0;background:var(--bg);color:var(--text)}"
        "main{max-width:48rem;margin:0 auto;padding:1.25rem}"
        ".nav{margin-bottom:1rem}a.navlink{color:#2459a6;font-weight:650;text-decoration:none}"
        "h1{font-size:1.35rem;margin:.25rem 0 1rem}"
        ".h2sect{font-size:1.1rem;margin:1.5rem 0 .65rem;color:#263447;border-bottom:1px solid #d7e0ea;padding-bottom:.35rem}"
        ".stat-note{font-size:.88rem;color:var(--muted);margin:0 0 .5rem;line-height:1.45}"
        ".stat-table{width:100%%;border-collapse:collapse;font-size:.92rem;margin:.25rem 0 1.25rem}"
        ".stat-table td{border:1px solid #d7e0ea;padding:.45rem .55rem;vertical-align:top}"
        ".stat-table td:first-child{font-weight:650;width:42%%;background:#f8fafc;color:#304254}"
        "label{display:block;margin:.6rem 0 .25rem;font-weight:650}"
        "select{font-size:1rem;padding:.55rem;box-sizing:border-box;border:1px solid #b8c4d2;border-radius:.45rem}"
        ".cpu-freq-row{display:flex;flex-wrap:wrap;align-items:center;gap:.75rem;margin:.5rem 0 0}"
        ".cpu-freq-row label{display:inline;margin:0;font-weight:650;white-space:nowrap}"
        ".cpu-freq-row select{width:auto;min-width:9rem;margin:0}"
        ".cpu-freq-row .btn-primary{margin-top:0}"
        ".btn-primary{font-size:1rem;margin-top:.75rem;padding:.65rem 1rem;border:0;border-radius:.55rem;background:#2459a6;color:#fff;font-weight:700;cursor:pointer}"
        ".btn-danger{font-size:1rem;margin-top:.75rem;padding:.65rem 1rem;border:0;border-radius:.55rem;background:#b42318;color:#fff;font-weight:700;cursor:pointer}"
        "</style></head><body><main>"
        "<nav class=\"nav\"><a class=\"navlink\" href=\"/\">← Home</a></nav>"
        "<h1>Maintenance</h1>"
        "<h2 class=\"h2sect\">ESP_32 Statistics</h2>"
        "<p class=\"stat-note\">Snapshot when this page was opened (not live-updating).</p>"
        "<table class=\"stat-table\">"
        "<tr><td>SoC target</td><td>%s</td></tr>"
        "<tr><td>CPU cores</td><td>%u</td></tr>"
        "<tr><td>Chip revision</td><td>%u</td></tr>"
        "<tr><td>Chip features</td><td>0x%08" PRIx32 "</td></tr>"
        "<tr><td>CPU frequency</td><td>%" PRIu32 " MHz</td></tr>"
        "<tr><td>Wi-Fi STA MAC</td><td>%02x:%02x:%02x:%02x:%02x:%02x</td></tr>"
        "<tr><td>ESP-IDF version</td><td>%s</td></tr>"
        "<tr><td>Firmware version</td><td>%s</td></tr>"
        "<tr><td>Firmware build</td><td>%s</td></tr>"
        "<tr><td>SPI flash chip</td><td>%s</td></tr>"
        "<tr><td>Running partition</td><td>%s</td></tr>"
        "<tr><td>Internal RAM (approx.)</td><td>%s</td></tr>"
        "<tr><td>PSRAM</td><td>%s</td></tr>"
        "<tr><td>Day chart log (LittleFS, 1 MiB)</td><td>%s</td></tr>"
        "<tr><td>Reset reason (code)</td><td>%d</td></tr>"
        "</table>"
        "<h2 class=\"h2sect\">Diagnostic log (RAM buffer).</h2>"
        "<p class=\"stat-note\">Compact events stored in RAM. Open the log in a new tab (snapshot when loaded; refresh the tab to update).</p>"
        "<p style=\"margin:.25rem 0 1rem\"><a class=\"navlink\" href=\"/api/maintenance/log.txt\" target=\"_blank\" rel=\"noopener\">Open diagnostic log (.txt)</a></p>"
        "<form method=\"post\" action=\"/api/maintenance/log/clear\" onsubmit=\"return confirm('Clear log buffer?');\">"
        "<button type=\"submit\" class=\"btn-primary\">Clear log</button></form>"
        "<h2 class=\"h2sect\">CPU frequency</h2>"
        "<p class=\"stat-note\">Lowering CPU frequency can reduce power use. After reducing speed, monitor RS485 polling, the web UI, and PVOutput uploads to ensure everything still runs in time.</p>"
        "<form method=\"post\" action=\"/api/maintenance/cpu-freq\" onsubmit=\"return confirm('Save CPU frequency and restart the device?');\">"
        "<div class=\"cpu-freq-row\">"
        "<label for=\"cpu-mhz\">CPU clock</label>"
        "<select id=\"cpu-mhz\" name=\"cpu_mhz\">"
        "<option value=\"240\"%s>240 MHz</option><option value=\"160\"%s>160 MHz</option><option value=\"80\"%s>80 MHz</option>"
        "</select>"
        "<button type=\"submit\" class=\"btn-primary\">Save CPU frequency</button>"
        "</div></form>"
        "<h2 class=\"h2sect\">Factory reset</h2>"
        "<p class=\"stat-note\"><strong>Warning:</strong> This clears Wi-Fi credentials, admin password hash, PVOutput config, RS-485 address in NVS, network/static IP preferences, timezone in NVS, and other stored device settings. After reboot, behaviour matches a fresh unit: the provisioning setup access point appears if no Wi-Fi credentials are saved, and you sign in with <strong>Admin</strong> / <strong>Password</strong> until you set a new password. It also clears sample history (LittleFS daylog), PHY calibration data, and coredump storage. The factory application image in flash is left as-is; only configuration and data partitions are wiped, matching a clean device as closely as the firmware allows.</p>"
        "<form method=\"post\" action=\"/api/maintenance/factory-reset\" onsubmit=\"return confirm('Are you sure?');\">"
        "<button type=\"submit\" class=\"btn-danger\">Reset</button></form>"
        "</main></body></html>",
        CONFIG_IDF_TARGET,
        (unsigned)ci.cores,
        (unsigned)ci.revision,
        (uint32_t)ci.features,
        cpu_mhz,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        esp_get_idf_version(),
        APP_VERSION,
        FIRMWARE_BUILD_TIMESTAMP,
        flash_row,
        part_row,
        ram_row,
        psram_row,
        daylog_row,
        (int)esp_reset_reason(),
        cpu_sel_240,
        cpu_sel_160,
        cpu_sel_80);

    if (len < 0 || len >= (int)sizeof(s_aux_html)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, s_aux_html, len);
    return ESP_OK;
}

static esp_err_t maintenance_cpu_freq_post_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_json_session_required(req);
        return ESP_OK;
    }
    if (req->content_len > FORM_BODY_MAX_LEN) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char body[FORM_BODY_MAX_LEN + 1];
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    char mhz_txt[8] = {0};
    if (!extract_form_value(body, "cpu_mhz", mhz_txt, sizeof(mhz_txt))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"missing_cpu_mhz\",\"message\":\"Missing cpu_mhz\"}\n");
        return ESP_OK;
    }
    const int parsed = atoi(mhz_txt);
    if (!cpu_freq_mhz_is_valid((uint8_t)parsed)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"invalid_cpu_freq\",\"message\":\"Invalid CPU frequency\"}\n");
        return ESP_OK;
    }

    esp_err_t err = save_cpu_freq_mhz((uint8_t)parsed);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "CPU frequency saved (%d MHz); restarting", parsed);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"CPU frequency saved\"}\n");
    xTaskCreate(delayed_restart_task, "cpuFreqRestart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t inverter_time_sync_post_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_json_session_required(req);
        return ESP_OK;
    }
    if (!rs485_bus_take(pdMS_TO_TICKS(8000))) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"busy\"}\n");
        return ESP_OK;
    }
    time_t computer_time = 0;
    time(&computer_time);
    long go = inverter_gmt_offset_seconds();
    uint64_t inv64 = (uint64_t)((int64_t)computer_time - AURORA_TIME_BASE_UNIX + (int64_t)go + 1);
    uint32_t inv = (uint32_t)inv64;
    int w = aurora_write_time(s_inverter_address, inv);
    rs485_bus_give();
    httpd_resp_set_type(req, "application/json");
    if (w != 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"write_failed\"}\n");
    } else {
        httpd_resp_sendstr(req, "{\"ok\":true}\n");
    }
    return ESP_OK;
}

static esp_err_t inverter_rs485_post_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_json_session_required(req);
        return ESP_OK;
    }
    if (req->content_len > FORM_BODY_MAX_LEN) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char body[FORM_BODY_MAX_LEN + 1];
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';
    char addr_txt[8] = {0};
    if (!extract_form_value(body, "address", addr_txt, sizeof(addr_txt))) {
        httpd_resp_sendstr(req, "{\"ok\":false}\n");
        return ESP_OK;
    }
    int a = atoi(addr_txt);
    if (save_rs485_address((uint8_t)a) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false}\n");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}\n");
    return ESP_OK;
}

static esp_err_t inverter_partial_reset_post_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_json_session_required(req);
        return ESP_OK;
    }
    if (!rs485_bus_take(pdMS_TO_TICKS(8000))) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"busy\"}\n");
        return ESP_OK;
    }
    int r = aurora_partial_reset(s_inverter_address);
    rs485_bus_give();
    httpd_resp_set_type(req, "application/json");
    if (r != 0) {
        httpd_resp_sendstr(req, "{\"ok\":false}\n");
    } else {
        httpd_resp_sendstr(req, "{\"ok\":true}\n");
    }
    return ESP_OK;
}

static esp_err_t time_settings_page_get_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_login_page(req);
        return ESP_OK;
    }
    int len = snprintf(
        s_aux_html,
        sizeof(s_aux_html),
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<link rel=\"icon\" href=\"/favicon.ico\" type=\"image/png\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Time &amp; timezone</title>"
        "<style>"
        "body{font-family:system-ui,Segoe UI,sans-serif;margin:0;background:#eef3f8;color:#17202a}"
        "main{max-width:44rem;margin:0 auto;padding:1.25rem}"
        ".nav{margin-bottom:1rem}a.navlink{color:#2459a6;font-weight:650;text-decoration:none}"
        "label{display:block;margin:.6rem 0 .25rem;font-weight:650}"
        "select{font-size:1rem;padding:.55rem;width:100%%;box-sizing:border-box;border:1px solid #b8c4d2;border-radius:.45rem}"
        ".muted{color:#607086;font-size:.95rem;margin:.5rem 0 0}"
        ".btn-primary{font-size:1rem;margin-top:1rem;padding:.65rem 1rem;border:0;border-radius:.55rem;background:#2459a6;color:#fff;font-weight:700;cursor:pointer}"
        "</style></head><body><main>"
        "<nav class=\"nav\"><a class=\"navlink\" href=\"/\">← Home</a></nav>"
        "<h1>Time &amp; timezone</h1>"
        "<form method=\"post\" action=\"/api/config/device\">"
        "<label for=\"tz-id\">Timezone</label>"
        "<select id=\"tz-id\" name=\"tz_id\">"
        "<option value=\"Australia/Perth\"%s>Australia/Perth</option>"
        "<option value=\"Australia/Sydney\"%s>Australia/Sydney</option>"
        "<option value=\"Pacific/Auckland\"%s>Pacific/Auckland</option>"
        "<option value=\"Asia/Tokyo\"%s>Asia/Tokyo</option>"
        "<option value=\"Asia/Singapore\"%s>Asia/Singapore</option>"
        "<option value=\"Asia/Shanghai\"%s>Asia/Shanghai</option>"
        "<option value=\"Europe/London\"%s>Europe/London</option>"
        "<option value=\"Europe/Paris\"%s>Europe/Paris</option>"
        "<option value=\"Europe/Berlin\"%s>Europe/Berlin</option>"
        "<option value=\"UTC\"%s>UTC</option>"
        "<option value=\"America/New_York\"%s>America/New_York</option>"
        "<option value=\"America/Chicago\"%s>America/Chicago</option>"
        "<option value=\"America/Denver\"%s>America/Denver</option>"
        "<option value=\"America/Los_Angeles\"%s>America/Los_Angeles</option>"
        "<option value=\"America/Phoenix\"%s>America/Phoenix</option>"
        "</select>"
        "<p class=\"muted\">NTP server is configured under Network Settings.</p>"
        "<button type=\"submit\" class=\"btn-primary\">Save timezone</button>"
        "</form></main></body></html>",
        tz_opt_sel("Australia/Perth"),
        tz_opt_sel("Australia/Sydney"),
        tz_opt_sel("Pacific/Auckland"),
        tz_opt_sel("Asia/Tokyo"),
        tz_opt_sel("Asia/Singapore"),
        tz_opt_sel("Asia/Shanghai"),
        tz_opt_sel("Europe/London"),
        tz_opt_sel("Europe/Paris"),
        tz_opt_sel("Europe/Berlin"),
        tz_opt_sel("UTC"),
        tz_opt_sel("America/New_York"),
        tz_opt_sel("America/Chicago"),
        tz_opt_sel("America/Denver"),
        tz_opt_sel("America/Los_Angeles"),
        tz_opt_sel("America/Phoenix"));
    if (len < 0 || len >= (int)sizeof(s_aux_html)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, s_aux_html, len);
    return ESP_OK;
}

static esp_err_t inverter_settings_page_get_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_login_page(req);
        return ESP_OK;
    }
    char inv_clock_json[256];
    inverter_clock_fill_json(inv_clock_json, sizeof(inv_clock_json));
    int len = snprintf(
        s_aux_html,
        sizeof(s_aux_html),
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<link rel=\"icon\" href=\"/favicon.ico\" type=\"image/png\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Inverter settings</title>"
        "<style>"
        "body{font-family:system-ui,Segoe UI,sans-serif;margin:0;background:#eef3f8;color:#17202a}"
        "main{max-width:44rem;margin:0 auto;padding:1.25rem}"
        ".nav{margin-bottom:1rem}a.navlink{color:#2459a6;font-weight:650;text-decoration:none}"
        "h2.h2sect{font-size:1.1rem;margin:1.25rem 0 .65rem;color:#263447;border-bottom:1px solid #d7e0ea;padding-bottom:.35rem}"
        "form>h2.h2sect:first-of-type{margin-top:0}"
        ".h2row{display:flex;align-items:center;justify-content:flex-start;gap:.35rem;flex-wrap:wrap;margin:1.25rem 0 0;padding-bottom:.35rem;border-bottom:1px solid #d7e0ea}"
        ".h2row h2{font-size:1.1rem;margin:0;color:#263447;font-weight:650}"
        ".help-i{cursor:pointer;background:transparent;border:0;color:#2459a6;padding:0 .15rem;font-size:1.05rem;line-height:1;font-weight:600}"
        ".help-i:focus{outline:2px solid #2459a6;outline-offset:2px;border-radius:2px}"
        ".help-tip{display:none;margin:0 0 .85rem;padding:.6rem .85rem;background:#f8fafc;border:1px solid #d7e0ea;border-radius:.45rem;font-size:.88rem;line-height:1.45}"
        "label{display:block;margin:.6rem 0 .2rem;font-weight:650}"
        "input[type=number]{font-size:1rem;padding:.5rem;width:100%%;max-width:12rem;box-sizing:border-box;border:1px solid #b8c4d2;border-radius:.45rem}"
        ".btn-primary{font-size:1rem;margin-top:.5rem;padding:.65rem 1rem;border:0;border-radius:.55rem;background:#2459a6;color:#fff;font-weight:700;cursor:pointer;margin-right:.5rem}"
        "</style></head><body><main>"
        "<nav class=\"nav\"><a class=\"navlink\" href=\"/\">← Home</a></nav>"
        "<h1>Inverter settings</h1>"
        "<form method=\"post\" action=\"/api/inverter/rs485\">"
        "<h2 class=\"h2sect\">Inverter address (1–127)</h2>"
        "<input id=\"addr\" name=\"address\" type=\"number\" min=\"1\" max=\"127\" value=\"%u\" required aria-label=\"Inverter RS-485 address (1 to 127)\">"
        "<p><button type=\"submit\" class=\"btn-primary\">Save address</button></p></form>"
        "<div class=\"h2row\"><h2>Clock</h2><button type=\"button\" class=\"help-i\" aria-label=\"Help about clock\" title=\"Help about clock\" onclick=\"var t=document.getElementById('tip-clock');t.style.display=t.style.display==='block'?'none':'block'\">\xe2\x93\x98</button></div>"
        "<div id=\"tip-clock\" class=\"help-tip\">Compares inverter time with this device. The inverter clock affects internal energy totals and timestamps; drift can skew daily yields. Sync after NTP has set the device clock.</div>"
        "<p id=\"clk\">Loading…</p>"
        "<p><button type=\"button\" class=\"btn-primary\" id=\"ref\">Refresh comparison</button></p>"
        "<form method=\"post\" action=\"/api/inverter/time/sync\" onsubmit=\"return confirm('Set inverter clock from this device?');\">"
        "<button type=\"submit\" class=\"btn-primary\">Sync inverter clock</button></form>"
        "<div class=\"h2row\"><h2>Maintenance</h2><button type=\"button\" class=\"help-i\" aria-label=\"Help about maintenance\" title=\"Help about maintenance\" onclick=\"var t=document.getElementById('tip-maint');t.style.display=t.style.display==='block'?'none':'block'\">\xe2\x93\x98</button></div>"
        "<div id=\"tip-maint\" class=\"help-tip\">Partial energy reset sends a maintenance command to clear the inverter partial energy counter. Use only when your inverter manual says it is appropriate; misuse can affect reported production.</div>"
        "<form method=\"post\" action=\"/api/inverter/partial-reset\" onsubmit=\"return confirm('Reset partial energy counter?');\">"
        "<button type=\"submit\" class=\"btn-primary\">Reset partial energy</button></form>"
        "<script type=\"application/json\" id=\"inv-time-data\">%s</script>"
        "<script>"
        "function tHMMS(u){const d=new Date(Number(u)*1e3);return String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0')+':'+String(d.getSeconds()).padStart(2,'0');}"
        "function apply(){const el=document.getElementById('inv-time-data');if(!el){document.getElementById('clk').textContent='Missing data';return;}let j;try{j=JSON.parse(el.textContent);}catch(e){document.getElementById('clk').textContent='Invalid data';return;}"
        "document.getElementById('clk').textContent=j.ok?('Inverter '+tHMMS(j.inverter_unix)+' vs device '+tHMMS(j.device_unix)+' (delta '+j.delta_seconds+' s)'):JSON.stringify(j);}"
        "document.getElementById('ref').addEventListener('click',function(){location.reload();});apply();"
        "</script></main></body></html>",
        (unsigned)s_inverter_address,
        inv_clock_json);
    if (len < 0 || len >= (int)sizeof(s_aux_html)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, s_aux_html, len);
    return ESP_OK;
}

static esp_err_t network_settings_page_get_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_login_page(req);
        return ESP_OK;
    }
    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char pw[WIFI_PASSWORD_MAX_LEN] = {0};
    (void)load_wifi_credentials(ssid, sizeof(ssid), pw, sizeof(pw));
    char e_ssid[80];
    char e_pw[200];
    char e_ntp[NTP_SERVER_MAX_LEN * 6];
    char e_host[STA_HOSTNAME_MAX_LEN * 4];
    html_escape_attr(ssid, e_ssid, sizeof(e_ssid));
    html_escape_attr(pw, e_pw, sizeof(e_pw));
    html_escape_attr(s_ntp_server, e_ntp, sizeof(e_ntp));
    html_escape_attr(s_sta_hostname, e_host, sizeof(e_host));
    int len = snprintf(
        s_aux_html,
        sizeof(s_aux_html),
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<link rel=\"icon\" href=\"/favicon.ico\" type=\"image/png\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Network settings</title>"
        "<style>body{font-family:system-ui,Segoe UI,sans-serif;margin:0;background:#eef3f8;color:#17202a}"
        "main{max-width:44rem;margin:0 auto;padding:1.25rem}"
        ".nav{margin-bottom:1rem}a.navlink{color:#2459a6;font-weight:650;text-decoration:none}"
        "h2.h2sect{font-size:1.1rem;margin:1.25rem 0 .65rem;color:#263447;border-bottom:1px solid #d7e0ea;padding-bottom:.35rem}"
        "form>h2.h2sect:first-of-type{margin-top:0}"
        "label{display:block;margin:.6rem 0 .2rem;font-weight:650}"
        "input:not([type=checkbox]),select{font-size:1rem;padding:.5rem;width:100%%;box-sizing:border-box;border:1px solid #b8c4d2;border-radius:.45rem}"
        ".chk-row{display:flex;align-items:flex-end;gap:.55rem;margin:.35rem 0 .6rem}"
        ".chk-row input[type=checkbox]{width:1.25rem;height:1.25rem;min-width:1.25rem;margin:0;flex-shrink:0;accent-color:#2459a6}"
        ".chk-row label{display:inline;margin:0 0 .08rem 0;font-weight:650}"
        ".btn-primary{font-size:1rem;margin-top:.75rem;padding:.65rem 1rem;border:0;border-radius:.55rem;background:#2459a6;color:#fff;font-weight:700;cursor:pointer}"
        "</style></head><body><main>"
        "<nav class=\"nav\"><a class=\"navlink\" href=\"/\">← Home</a></nav><h1>Network settings</h1>"
        "<form method=\"post\" action=\"/api/config/network\" onsubmit=\"return confirm('Saving network settings restarts the device. Continue?');\">"
        "<h2 class=\"h2sect\">Wi-Fi</h2>"
        "<label for=\"ssid\">SSID</label><input id=\"ssid\" name=\"ssid\" value=\"%s\" maxlength=\"32\" required>"
        "<label for=\"pw\">Password</label><input id=\"pw\" name=\"password\" value=\"%s\" maxlength=\"63\" autocomplete=\"off\">"
        "<h2 class=\"h2sect\">IPv4</h2>"
        "<div class=\"chk-row\"><input type=\"checkbox\" name=\"use_static\" value=\"1\" id=\"use_static\"%s>"
        "<label for=\"use_static\">Check to use static IPv4 address</label></div>"
        "<label for=\"ip\">IP address</label><input id=\"ip\" name=\"sta_ip\" value=\"%s\" maxlength=\"15\">"
        "<label for=\"nm\">Subnet mask</label><input id=\"nm\" name=\"sta_nm\" value=\"%s\" maxlength=\"15\">"
        "<label for=\"gw\">Gateway</label><input id=\"gw\" name=\"sta_gw\" value=\"%s\" maxlength=\"15\">"
        "<label for=\"dns\">DNS (optional)</label><input id=\"dns\" name=\"sta_dns\" value=\"%s\" maxlength=\"15\">"
        "<h2 class=\"h2sect\">Hostname</h2>"
        "<label for=\"host\">mDNS hostname (empty = default)</label>"
        "<input id=\"host\" name=\"hostname\" value=\"%s\" maxlength=\"%d\">"
        "<h2 class=\"h2sect\">NTP</h2>"
        "<label for=\"ntp\">NTP server</label><input id=\"ntp\" name=\"ntp_server\" value=\"%s\" maxlength=\"63\" required>"
        "<button type=\"submit\" class=\"btn-primary\">Save and restart</button></form></main></body></html>",
        e_ssid,
        e_pw,
        s_use_sta_static_ip ? " checked" : "",
        s_sta_static_ip,
        s_sta_static_nm,
        s_sta_static_gw,
        s_sta_static_dns,
        e_host,
        STA_HOSTNAME_MAX_LEN,
        e_ntp);
    if (len < 0 || len >= (int)sizeof(s_aux_html)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, s_aux_html, len);
    return ESP_OK;
}

static esp_err_t network_config_post_handler(httpd_req_t *req)
{
    if (!is_web_session_ok(req)) {
        send_json_session_required(req);
        return ESP_OK;
    }
    if (req->content_len > FORM_BODY_MAX_LEN) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char body[FORM_BODY_MAX_LEN + 1];
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char password[WIFI_PASSWORD_MAX_LEN] = {0};
    char sta_ip[16] = {0};
    char sta_nm[16] = {0};
    char sta_gw[16] = {0};
    char sta_dns[16] = {0};
    char hostname[STA_HOSTNAME_MAX_LEN + 1] = {0};
    char ntp_server[NTP_SERVER_MAX_LEN] = {0};
    char use_static_txt[8] = {0};

    if (!extract_form_value(body, "ssid", ssid, sizeof(ssid)) ||
        !extract_form_value(body, "password", password, sizeof(password)) ||
        !extract_form_value(body, "ntp_server", ntp_server, sizeof(ntp_server))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Missing required field");
        return ESP_OK;
    }
    (void)extract_form_value(body, "sta_ip", sta_ip, sizeof(sta_ip));
    (void)extract_form_value(body, "sta_nm", sta_nm, sizeof(sta_nm));
    (void)extract_form_value(body, "sta_gw", sta_gw, sizeof(sta_gw));
    (void)extract_form_value(body, "sta_dns", sta_dns, sizeof(sta_dns));
    (void)extract_form_value(body, "hostname", hostname, sizeof(hostname));
    (void)extract_form_value(body, "use_static", use_static_txt, sizeof(use_static_txt));
    const bool use_static = (strcmp(use_static_txt, "1") == 0);

    esp_err_t err = save_wifi_credentials(ssid, password);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    err = save_device_settings(ntp_server, s_tz_id);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Invalid NTP server");
        return ESP_OK;
    }
    err = save_net_settings(use_static, sta_ip, sta_gw, sta_nm, sta_dns, hostname);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    s_provisioning_started_ms = esp_log_timestamp();
    ESP_LOGI(TAG, "network settings saved; restarting");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(
        req,
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>Saved</title></head>"
        "<body><h1>Saved</h1><p>Restarting…</p></body></html>");
    xTaskCreate(delayed_restart_task, "netRestart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void delayed_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void perform_factory_reset_erase(void)
{
    ESP_LOGW(TAG, "factory reset: erasing data partitions");
    daylog_factory_reset_unmount();
    factory_erase_data_partition("daylog", 0);
    factory_erase_data_partition(NULL, ESP_PARTITION_SUBTYPE_DATA_PHY);
    factory_erase_data_partition(NULL, ESP_PARTITION_SUBTYPE_DATA_COREDUMP);
    factory_erase_data_partition(NULL, ESP_PARTITION_SUBTYPE_DATA_NVS);
}

static void try_handle_pending_factory_reset_at_boot(void)
{
    if (s_factory_reset_rtc_magic != FACTORY_RESET_RTC_MAGIC) {
        return;
    }
    s_factory_reset_rtc_magic = 0;
    perform_factory_reset_erase();
    ESP_LOGW(TAG, "factory reset erase complete; rebooting into clean state");
    esp_restart();
}

static esp_err_t redirect_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Redirecting");
    return ESP_OK;
}

static esp_err_t start_provisioning_http_server(void)
{
    ensure_session_mutex();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 24576;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 32;

    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t auth_login_post_uri = {
        .uri = "/api/auth/login",
        .method = HTTP_POST,
        .handler = login_post_handler,
    };
    const httpd_uri_t auth_logout_post_uri = {
        .uri = "/api/auth/logout",
        .method = HTTP_POST,
        .handler = logout_post_handler,
    };
    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t dashboard_uri = {
        .uri = "/dashboard",
        .method = HTTP_GET,
        .handler = dashboard_get_handler,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    const httpd_uri_t samples_uri = {
        .uri = "/api/samples",
        .method = HTTP_GET,
        .handler = samples_get_handler,
    };
    const httpd_uri_t samples_raw_csv_uri = {
        .uri = "/api/samples/raw.csv",
        .method = HTTP_GET,
        .handler = samples_raw_csv_get_handler,
    };
    const httpd_uri_t pvoutput_config_post_uri = {
        .uri = "/api/config/pvoutput",
        .method = HTTP_POST,
        .handler = pvoutput_config_post_handler,
    };
    const httpd_uri_t pvoutput_test_post_uri = {
        .uri = "/api/pvoutput/test",
        .method = HTTP_POST,
        .handler = pvoutput_test_post_handler,
    };
    const httpd_uri_t device_config_post_uri = {
        .uri = "/api/config/device",
        .method = HTTP_POST,
        .handler = device_config_post_handler,
    };
    const httpd_uri_t device_restart_post_uri = {
        .uri = "/api/restart",
        .method = HTTP_POST,
        .handler = device_restart_post_handler,
    };
    const httpd_uri_t admin_password_post_uri = {
        .uri = "/api/auth/password",
        .method = HTTP_POST,
        .handler = admin_password_post_handler,
    };
    const httpd_uri_t chart_js_uri = {
        .uri = "/static/chart.umd.min.js",
        .method = HTTP_GET,
        .handler = chart_js_get_handler,
    };
    const httpd_uri_t maintenance_page_uri = {
        .uri = "/maintenance",
        .method = HTTP_GET,
        .handler = maintenance_page_get_handler,
    };
    const httpd_uri_t maintenance_log_clear_uri = {
        .uri = "/api/maintenance/log/clear",
        .method = HTTP_POST,
        .handler = maintenance_log_clear_post_handler,
    };
    const httpd_uri_t maintenance_log_txt_uri = {
        .uri = "/api/maintenance/log.txt",
        .method = HTTP_GET,
        .handler = maintenance_log_txt_get_handler,
    };
    const httpd_uri_t maintenance_factory_reset_uri = {
        .uri = "/api/maintenance/factory-reset",
        .method = HTTP_POST,
        .handler = maintenance_factory_reset_post_handler,
    };
    const httpd_uri_t maintenance_cpu_freq_uri = {
        .uri = "/api/maintenance/cpu-freq",
        .method = HTTP_POST,
        .handler = maintenance_cpu_freq_post_handler,
    };
    const httpd_uri_t network_page_uri = {
        .uri = "/network-settings",
        .method = HTTP_GET,
        .handler = network_settings_page_get_handler,
    };
    const httpd_uri_t time_settings_page_uri = {
        .uri = "/time-settings",
        .method = HTTP_GET,
        .handler = time_settings_page_get_handler,
    };
    const httpd_uri_t inverter_page_uri = {
        .uri = "/inverter-settings",
        .method = HTTP_GET,
        .handler = inverter_settings_page_get_handler,
    };
    const httpd_uri_t network_config_post_uri = {
        .uri = "/api/config/network",
        .method = HTTP_POST,
        .handler = network_config_post_handler,
    };
    const httpd_uri_t inverter_time_sync_uri = {
        .uri = "/api/inverter/time/sync",
        .method = HTTP_POST,
        .handler = inverter_time_sync_post_handler,
    };
    const httpd_uri_t inverter_rs485_uri = {
        .uri = "/api/inverter/rs485",
        .method = HTTP_POST,
        .handler = inverter_rs485_post_handler,
    };
    const httpd_uri_t inverter_partial_reset_uri = {
        .uri = "/api/inverter/partial-reset",
        .method = HTTP_POST,
        .handler = inverter_partial_reset_post_handler,
    };
    const httpd_uri_t favicon_uri = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_get_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &auth_login_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &auth_logout_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &dashboard_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &samples_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &samples_raw_csv_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &pvoutput_config_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &pvoutput_test_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &device_config_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &device_restart_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &admin_password_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &chart_js_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &maintenance_page_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &maintenance_log_clear_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &maintenance_log_txt_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &maintenance_factory_reset_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &maintenance_cpu_freq_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &network_page_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &time_settings_page_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &inverter_page_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &network_config_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &inverter_time_sync_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &inverter_rs485_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &inverter_partial_reset_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &favicon_uri));
    ESP_ERROR_CHECK(httpd_register_err_handler(s_http_server, HTTPD_404_NOT_FOUND, redirect_404_handler));
    return ESP_OK;
}

static void stop_provisioning(void)
{
    if (s_http_server != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_stop(s_http_server));
        s_http_server = NULL;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
    s_provisioning_active = false;
    ESP_LOGW(TAG, "provisioning timed out after 30 minutes; AP disabled");
    ESP_LOGW(TAG, "reboot or reflash over USB serial to start the setup window again");
}

static esp_netif_t *create_provisioning_netif(void)
{
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));

    esp_netif_ip_info_t ip_info = {0};
    ip_info.ip.addr = ipaddr_addr(PROVISIONING_AP_IP);
    ip_info.gw.addr = ipaddr_addr(PROVISIONING_AP_IP);
    ip_info.netmask.addr = ipaddr_addr(PROVISIONING_AP_NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    return ap_netif;
}

static void start_provisioning_ap(bool saved_credentials)
{
    set_device_identity_from_mac();
    (void)create_provisioning_netif();

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.ap.ssid, s_provisioning_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(s_provisioning_ssid);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = PROVISIONING_AP_MAX_CONNECTIONS;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_config.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(start_provisioning_http_server());

    s_provisioning_active = true;
    s_provisioning_started_ms = esp_log_timestamp();

    ESP_LOGW(TAG, "provisioning mode active%s", saved_credentials ? " (saved credentials present; STA connect pending)" : "");
    ESP_LOGW(TAG, "connect to open Wi-Fi SSID: %s", s_provisioning_ssid);
    ESP_LOGW(TAG, "open setup page: http://%s/", PROVISIONING_AP_IP);
    ESP_LOGW(TAG, "setup AP will time out after 30 minutes");
}

static esp_err_t configure_sta_from_nvs(void)
{
    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char password[WIFI_PASSWORD_MAX_LEN] = {0};
    esp_err_t err = load_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password));
    if (err != ESP_OK || ssid[0] == '\0') {
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();

    if (s_sta_hostname[0] != '\0') {
        esp_err_t hn = esp_netif_set_hostname(s_sta_netif, s_sta_hostname);
        if (hn != ESP_OK) {
            ESP_LOGW(TAG, "esp_netif_set_hostname failed: %s", esp_err_to_name(hn));
        }
    }

    if (s_use_sta_static_ip && s_sta_static_ip[0] != '\0' && s_sta_static_nm[0] != '\0') {
        esp_netif_ip_info_t ipi = {0};
        ipi.ip.addr = ipaddr_addr(s_sta_static_ip);
        ipi.netmask.addr = ipaddr_addr(s_sta_static_nm);
        ipi.gw.addr = ipaddr_addr(s_sta_static_gw[0] != '\0' ? s_sta_static_gw : s_sta_static_ip);
        esp_err_t se = esp_netif_dhcpc_stop(s_sta_netif);
        if (se != ESP_OK && se != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            ESP_LOGW(TAG, "esp_netif_dhcpc_stop: %s", esp_err_to_name(se));
        }
        ESP_ERROR_CHECK(esp_netif_set_ip_info(s_sta_netif, &ipi));
        if (s_sta_static_dns[0] != '\0') {
            esp_netif_dns_info_t d = {.ip = {.type = ESP_IPADDR_TYPE_V4}};
            d.ip.u_addr.ip4.addr = ipaddr_addr(s_sta_static_dns);
            (void)esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &d);
        }
        ESP_LOGI(TAG, "STA using static IPv4 %s mask %s gw %s", s_sta_static_ip, s_sta_static_nm, s_sta_static_gw);
    }

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
    s_sta_ip[0] = '\0';
    s_sta_has_config = true;
    s_sta_connected = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "saved Wi-Fi credentials found");
    ESP_LOGI(TAG, "connecting STA to SSID '%s'", s_sta_ssid);
    return ESP_OK;
}

static void start_network(void)
{
    init_network_stack();

    load_net_settings();

    const bool saved_credentials = wifi_credentials_present();
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(saved_credentials ? WIFI_MODE_APSTA : WIFI_MODE_AP));

    if (!saved_credentials) {
        ESP_LOGW(TAG, "no saved Wi-Fi credentials found");
    }

    start_provisioning_ap(saved_credentials);

    if (saved_credentials) {
        esp_err_t err = configure_sta_from_nvs();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "saved Wi-Fi credentials could not be loaded: %s", esp_err_to_name(err));
        }
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    if (s_sta_has_config) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    }
}

static void load_rs485_settings(void)
{
    s_inverter_address = RS485_DEFAULT_INVERTER_ADDRESS;
    nvs_handle_t nvs;
    if (nvs_open("rs485_cfg", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint8_t a = s_inverter_address;
    if (nvs_get_u8(nvs, "address", &a) == ESP_OK && a >= 1U && a <= 127U) {
        s_inverter_address = a;
    }
    nvs_close(nvs);
}

static esp_err_t save_rs485_address(uint8_t address)
{
    if (address < 1U || address > 127U) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("rs485_cfg", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(nvs, "address", address);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        s_inverter_address = address;
    }
    return err;
}

static void load_net_settings(void)
{
    s_use_sta_static_ip = false;
    s_sta_static_ip[0] = '\0';
    s_sta_static_gw[0] = '\0';
    s_sta_static_nm[0] = '\0';
    s_sta_static_dns[0] = '\0';
    s_sta_hostname[0] = '\0';

    nvs_handle_t nvs;
    if (nvs_open("net_cfg", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint8_t u8 = 0;
    if (nvs_get_u8(nvs, "use_static", &u8) == ESP_OK && u8 != 0) {
        s_use_sta_static_ip = true;
    }
    size_t len = sizeof(s_sta_static_ip);
    (void)nvs_get_str(nvs, "sta_ip", s_sta_static_ip, &len);
    len = sizeof(s_sta_static_gw);
    (void)nvs_get_str(nvs, "sta_gw", s_sta_static_gw, &len);
    len = sizeof(s_sta_static_nm);
    (void)nvs_get_str(nvs, "sta_nm", s_sta_static_nm, &len);
    len = sizeof(s_sta_static_dns);
    (void)nvs_get_str(nvs, "sta_dns", s_sta_static_dns, &len);
    len = sizeof(s_sta_hostname);
    (void)nvs_get_str(nvs, "hostname", s_sta_hostname, &len);
    nvs_close(nvs);
}

static esp_err_t save_net_settings(bool use_static, const char *ip, const char *gw, const char *mask, const char *dns,
                                   const char *hostname)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("net_cfg", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(nvs, "use_static", use_static ? 1U : 0U);
    if (err == ESP_OK && use_static) {
        err = nvs_set_str(nvs, "sta_ip", ip != NULL ? ip : "");
    }
    if (err == ESP_OK && use_static) {
        err = nvs_set_str(nvs, "sta_gw", gw != NULL ? gw : "");
    }
    if (err == ESP_OK && use_static) {
        err = nvs_set_str(nvs, "sta_nm", mask != NULL ? mask : "");
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "sta_dns", dns != NULL ? dns : "");
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "hostname", hostname != NULL ? hostname : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        load_net_settings();
    }
    return err;
}

static void ensure_rs485_mutex(void)
{
    if (s_rs485_mutex == NULL) {
        s_rs485_mutex = xSemaphoreCreateMutex();
    }
}

static bool rs485_bus_take(TickType_t ticks)
{
    ensure_rs485_mutex();
    return s_rs485_mutex != NULL && xSemaphoreTake(s_rs485_mutex, ticks) == pdTRUE;
}

static void rs485_bus_give(void)
{
    if (s_rs485_mutex != NULL) {
        xSemaphoreGive(s_rs485_mutex);
    }
}

static void ensure_web_log_mutex(void)
{
    if (s_web_log_mutex == NULL) {
        s_web_log_mutex = xSemaphoreCreateMutex();
    }
}

static void web_log_append_line(const char *line)
{
    if (line == NULL) {
        return;
    }
    ensure_web_log_mutex();
    if (s_web_log_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_web_log_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    size_t l = strlen(line);
    if (l > WEB_LOG_CAP / 2U) {
        l = WEB_LOG_CAP / 2U;
    }
    while (s_web_log_len + l + 2U > WEB_LOG_CAP) {
        const char *nl = memchr(s_web_log, '\n', s_web_log_len);
        if (nl == NULL) {
            s_web_log_len = 0;
            break;
        }
        size_t drop = (size_t)(nl - s_web_log) + 1U;
        memmove(s_web_log, s_web_log + drop, s_web_log_len - drop);
        s_web_log_len -= drop;
    }
    memcpy(s_web_log + s_web_log_len, line, l);
    s_web_log_len += l;
    if (l == 0 || s_web_log[s_web_log_len - 1] != '\n') {
        s_web_log[s_web_log_len++] = '\n';
    }
    xSemaphoreGive(s_web_log_mutex);
}

static int web_log_vprintf(const char *fmt, va_list ap)
{
    char buf[320];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        web_log_append_line(buf);
    }
    if (s_prev_log_vprintf != NULL) {
        va_copy(ap2, ap);
        int r = s_prev_log_vprintf(fmt, ap2);
        va_end(ap2);
        return r;
    }
    return vprintf(fmt, ap);
}

static void web_log_init(void)
{
    ensure_web_log_mutex();
    s_prev_log_vprintf = esp_log_set_vprintf(web_log_vprintf);
}

static long inverter_gmt_offset_seconds(void)
{
    time_t now = time(NULL);
    struct tm lt;
    if (localtime_r(&now, &lt) == NULL) {
        return 0L;
    }
    char zbuf[16] = {0};
    if (strftime(zbuf, sizeof(zbuf), "%z", &lt) == 0U) {
        return 0L;
    }
    if (zbuf[0] != '+' && zbuf[0] != '-') {
        return 0L;
    }
    int sign = (zbuf[0] == '-') ? -1 : 1;
    int hh = (zbuf[1] - '0') * 10 + (zbuf[2] - '0');
    int mm = (zbuf[3] - '0') * 10 + (zbuf[4] - '0');
    if (hh < 0 || hh > 14 || mm < 0 || mm > 59) {
        return 0L;
    }
    return (long)sign * ((long)hh * 3600L + (long)mm * 60L);
}

static uint16_t aurora_crc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xffff;

    if (length == 0) {
        return (uint16_t)(~crc);
    }

    do {
        uint16_t value = *data++;
        for (uint8_t i = 0; i < 8; ++i, value >>= 1) {
            if ((crc & 0x0001) ^ (value & 0x0001)) {
                crc = (crc >> 1) ^ AURORA_CRC_POLY;
            } else {
                crc >>= 1;
            }
        }
    } while (--length);

    return (uint16_t)(~crc);
}

static esp_err_t rs485_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = RS485_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(RS485_UART_NUM, 256, 256, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    ESP_RETURN_ON_ERROR(uart_param_config(RS485_UART_NUM, &uart_config), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(RS485_UART_NUM, RS485_TX_GPIO, RS485_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                     RS485_DE_GPIO, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin failed");
    ESP_RETURN_ON_ERROR(uart_set_mode(RS485_UART_NUM, UART_MODE_RS485_HALF_DUPLEX), TAG, "uart_set_mode failed");
    ESP_RETURN_ON_ERROR(uart_set_rx_timeout(RS485_UART_NUM, 3), TAG, "uart_set_rx_timeout failed");

    uart_flush_input(RS485_UART_NUM);
    s_rs485_ready = true;
    ESP_LOGI(TAG, "RS485 UART ready: uart=%d tx=%d rx=%d de=%d baud=%d 8N1 address=%d",
             RS485_UART_NUM, RS485_TX_GPIO, RS485_RX_GPIO, RS485_DE_GPIO,
             RS485_BAUD_RATE, (int)s_inverter_address);
    return ESP_OK;
}

static int aurora_poll_state(uint8_t address, uint8_t response[AURORA_RESPONSE_LEN])
{
    uint8_t request[AURORA_FRAME_LEN];
    memset(request, ' ', sizeof(request));
    request[0] = address;
    request[1] = AURORA_STATE_OPCODE;
    request[2] = 0;

    uint16_t crc = aurora_crc16(request, 8);
    request[8] = crc & 0xff;
    request[9] = (crc >> 8) & 0xff;

    uart_flush_input(RS485_UART_NUM);
    int written = uart_write_bytes(RS485_UART_NUM, request, sizeof(request));
    if (written != sizeof(request)) {
        return -10;
    }

    if (uart_wait_tx_done(RS485_UART_NUM, pdMS_TO_TICKS(200)) != ESP_OK) {
        return -11;
    }

    int received = uart_read_bytes(RS485_UART_NUM, response, AURORA_RESPONSE_LEN, pdMS_TO_TICKS(700));
    if (received != AURORA_RESPONSE_LEN) {
        return -20 - received;
    }

    uint16_t expected_crc = aurora_crc16(response, 6);
    uint16_t actual_crc = response[6] | ((uint16_t)response[7] << 8);
    if (actual_crc != expected_crc) {
        return -30;
    }

    return 0;
}

static float aurora_float_from_response(const uint8_t response[AURORA_RESPONSE_LEN])
{
    uint8_t value_bytes[4] = {response[5], response[4], response[3], response[2]};
    float value = 0.0f;
    memcpy(&value, value_bytes, sizeof(value));
    return value;
}

static uint32_t aurora_u32_from_response(const uint8_t response[AURORA_RESPONSE_LEN])
{
    return ((uint32_t)response[2] << 24) |
           ((uint32_t)response[3] << 16) |
           ((uint32_t)response[4] << 8) |
           (uint32_t)response[5];
}

static int aurora_poll_value(uint8_t address, uint8_t opcode, uint8_t param,
                             uint8_t response[AURORA_RESPONSE_LEN])
{
    uint8_t request[AURORA_FRAME_LEN];
    memset(request, ' ', sizeof(request));
    request[0] = address;
    request[1] = opcode;
    request[2] = param;
    request[3] = 0;

    uint16_t crc = aurora_crc16(request, 8);
    request[8] = crc & 0xff;
    request[9] = (crc >> 8) & 0xff;

    uart_flush_input(RS485_UART_NUM);
    int written = uart_write_bytes(RS485_UART_NUM, request, sizeof(request));
    if (written != sizeof(request)) {
        return -10;
    }

    if (uart_wait_tx_done(RS485_UART_NUM, pdMS_TO_TICKS(200)) != ESP_OK) {
        return -11;
    }

    int received = uart_read_bytes(RS485_UART_NUM, response, AURORA_RESPONSE_LEN, pdMS_TO_TICKS(700));
    if (received != AURORA_RESPONSE_LEN) {
        return -20 - received;
    }

    uint16_t expected_crc = aurora_crc16(response, 6);
    uint16_t actual_crc = response[6] | ((uint16_t)response[7] << 8);
    if (actual_crc != expected_crc) {
        return -30;
    }

    return 0;
}

static int aurora_write_time(uint8_t address, uint32_t inv_raw)
{
    uint8_t request[AURORA_FRAME_LEN];
    memset(request, ' ', sizeof(request));
    request[0] = address;
    request[1] = AURORA_TIME_SET_OPCODE;
    request[2] = (uint8_t)((inv_raw >> 24) & 0xffU);
    request[3] = (uint8_t)((inv_raw >> 16) & 0xffU);
    request[4] = (uint8_t)((inv_raw >> 8) & 0xffU);
    request[5] = (uint8_t)(inv_raw & 0xffU);
    request[6] = 0;

    uint16_t crc = aurora_crc16(request, 8);
    request[8] = crc & 0xff;
    request[9] = (crc >> 8) & 0xff;

    uart_flush_input(RS485_UART_NUM);
    int written = uart_write_bytes(RS485_UART_NUM, request, sizeof(request));
    if (written != sizeof(request)) {
        return -10;
    }
    if (uart_wait_tx_done(RS485_UART_NUM, pdMS_TO_TICKS(200)) != ESP_OK) {
        return -11;
    }
    uint8_t response[AURORA_RESPONSE_LEN] = {0};
    int received = uart_read_bytes(RS485_UART_NUM, response, AURORA_RESPONSE_LEN, pdMS_TO_TICKS(700));
    if (received != AURORA_RESPONSE_LEN) {
        return -20 - received;
    }
    uint16_t expected_crc = aurora_crc16(response, 6);
    uint16_t actual_crc = response[6] | ((uint16_t)response[7] << 8);
    if (actual_crc != expected_crc) {
        return -30;
    }
    return 0;
}

static int aurora_partial_reset(uint8_t address)
{
    uint8_t request[AURORA_FRAME_LEN];
    memset(request, ' ', sizeof(request));
    request[0] = address;
    request[1] = 80;
    request[2] = 3;

    uint16_t crc = aurora_crc16(request, 8);
    request[8] = crc & 0xff;
    request[9] = (crc >> 8) & 0xff;

    uart_flush_input(RS485_UART_NUM);
    int written = uart_write_bytes(RS485_UART_NUM, request, sizeof(request));
    if (written != sizeof(request)) {
        return -10;
    }
    if (uart_wait_tx_done(RS485_UART_NUM, pdMS_TO_TICKS(200)) != ESP_OK) {
        return -11;
    }
    uint8_t response[AURORA_RESPONSE_LEN] = {0};
    int received = uart_read_bytes(RS485_UART_NUM, response, AURORA_RESPONSE_LEN, pdMS_TO_TICKS(700));
    if (received != AURORA_RESPONSE_LEN) {
        return -20 - received;
    }
    uint16_t expected_crc = aurora_crc16(response, 6);
    uint16_t actual_crc = response[6] | ((uint16_t)response[7] << 8);
    if (actual_crc != expected_crc) {
        return -30;
    }
    return 0;
}

static int aurora_poll_dsp(uint8_t address, uint8_t param, float *value)
{
    uint8_t response[AURORA_RESPONSE_LEN] = {0};
    int result = aurora_poll_value(address, AURORA_DSP_OPCODE, param, response);
    if (result == 0) {
        *value = aurora_float_from_response(response);
    }
    return result;
}

static int aurora_poll_ce(uint8_t address, uint8_t param, uint32_t *value)
{
    uint8_t response[AURORA_RESPONSE_LEN] = {0};
    int result = aurora_poll_value(address, AURORA_CE_OPCODE, param, response);
    if (result == 0) {
        *value = aurora_u32_from_response(response);
    }
    return result;
}

static int aurora_poll_live_metrics(uint8_t address)
{
    float output_power_w = 0.0f;
    float grid_voltage_v = 0.0f;
    float grid_frequency_hz = 0.0f;
    float booster_temp_c = 0.0f;
    float inverter_temp_c = 0.0f;
    uint32_t energy_today_wh = 0;
    int result = 0;

    if ((result = aurora_poll_dsp(address, 3, &output_power_w)) != 0 ||
        (result = aurora_poll_dsp(address, 1, &grid_voltage_v)) != 0 ||
        (result = aurora_poll_dsp(address, 4, &grid_frequency_hz)) != 0 ||
        (result = aurora_poll_dsp(address, 21, &booster_temp_c)) != 0 ||
        (result = aurora_poll_dsp(address, 22, &inverter_temp_c)) != 0 ||
        (result = aurora_poll_ce(address, 0, &energy_today_wh)) != 0) {
        return result;
    }

    s_output_power_w = output_power_w;
    s_grid_voltage_v = grid_voltage_v;
    s_grid_frequency_hz = grid_frequency_hz;
    s_booster_temp_c = booster_temp_c;
    s_inverter_temp_c = inverter_temp_c;
    s_energy_today_wh = energy_today_wh;
    return 0;
}

static void aurora_poll_task(void *arg)
{
    (void)arg;

    esp_err_t err = rs485_init();
    if (err != ESP_OK) {
        s_rs485_last_result = err;
        ESP_LOGE(TAG, "RS485 init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        if (!rs485_bus_take(pdMS_TO_TICKS(5000))) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        uint8_t response[AURORA_RESPONSE_LEN] = {0};
        s_rs485_poll_count++;
        s_rs485_last_poll_ms = esp_log_timestamp();

        int result = aurora_poll_state(s_inverter_address, response);
        s_rs485_last_result = result;

        if (result == 0) {
            memcpy(s_inverter_state, response, sizeof(s_inverter_state));
            s_rs485_ok_count++;
            s_rs485_consecutive_failures = 0;
            s_rs485_last_ok_ms = esp_log_timestamp();
            s_inverter_offline = false;
            ESP_LOGI(TAG, "inverter state ok: state=%u global=%u inverter=%u channel1=%u channel2=%u",
                     response[1], response[2], response[3], response[4], response[5]);

            s_live_metrics_last_result = aurora_poll_live_metrics(s_inverter_address);
            s_live_metrics_valid = s_live_metrics_last_result == 0;
            if (s_live_metrics_valid) {
                s_live_metrics_last_ok_ms = esp_log_timestamp();
                retain_live_sample();
                ESP_LOGI(TAG, "live metrics: power=%.1fW grid=%.1fV %.2fHz temp=%.1f/%.1fC energy_today=%" PRIu32 "Wh",
                         (double)s_output_power_w, (double)s_grid_voltage_v, (double)s_grid_frequency_hz,
                         (double)s_booster_temp_c, (double)s_inverter_temp_c, s_energy_today_wh);
                (void)pvoutput_upload_live_status();
            } else {
                ESP_LOGW(TAG, "live metrics poll failed result=%d", s_live_metrics_last_result);
            }
        } else {
            s_rs485_fail_count++;
            s_rs485_consecutive_failures++;
            s_live_metrics_valid = false;
            if (s_rs485_consecutive_failures >= RS485_OFFLINE_FAILURES) {
                s_inverter_offline = true;
            }
            if (s_rs485_consecutive_failures == 1 ||
                s_rs485_consecutive_failures == RS485_OFFLINE_FAILURES ||
                (s_rs485_consecutive_failures % 12) == 0) {
                ESP_LOGW(TAG, "inverter poll failed result=%d consecutive=%" PRIu32 " offline=%s",
                         result, s_rs485_consecutive_failures, s_inverter_offline ? "true" : "false");
            }
        }

        rs485_bus_give();

        vTaskDelay(pdMS_TO_TICKS(RS485_POLL_INTERVAL_MS));
    }
}

void app_main(void)
{
    try_handle_pending_factory_reset_at_boot();

    ESP_ERROR_CHECK(init_nvs());
    load_admin_password_verifier();
    (void)init_psa_crypto_once();
    ESP_ERROR_CHECK(load_device_settings());
    (void)load_cpu_freq_from_nvs();
    (void)apply_cpu_frequency(s_cpu_freq_mhz);
    apply_default_timezone();
    ESP_ERROR_CHECK(load_pvoutput_config());
    load_rs485_settings();
    (void)pcf85063_rtc_init();
    time_t rtc_boot = 0;
    if (pcf85063_rtc_read_utc_epoch(&rtc_boot) == ESP_OK && rtc_boot > (time_t)1700000000) {
        struct timeval tv = {.tv_sec = rtc_boot, .tv_usec = 0};
        if (settimeofday(&tv, NULL) == 0) {
            ESP_LOGI(TAG, "system time seeded from PCF85063 RTC");
        }
    }
    web_log_init();
    init_sample_ring();
    if (daylog_init() != ESP_OK) {
        ESP_LOGW(TAG, "daylog LittleFS init failed; /api/samples?today=1 falls back to RAM ring");
    }
    log_board_identity();
    start_network();
    xTaskCreate(aurora_poll_task, "auroraPollTask", 4096, NULL, 8, NULL);

    ESP_LOGI(TAG, "dashboard firmware is running; detailed web UI, retention, and PVOutput are pending");

    while (true) {
        if (s_provisioning_active &&
            (esp_log_timestamp() - s_provisioning_started_ms) >= PROVISIONING_TIMEOUT_MS) {
            stop_provisioning();
        }

        ESP_LOGI(TAG, "heartbeat uptime_ms=%" PRIu32 " free_heap=%" PRIu32,
                 (uint32_t)(esp_log_timestamp()), (uint32_t)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
