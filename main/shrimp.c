#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_sntp.h"

#include "shrimp_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"
#include "channels/telegram/telegram_bot.h"
#include "channels/feishu/feishu_bot.h"
#include "llm/llm_proxy.h"
#include "agent/agent_loop.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "gateway/ws_server.h"
#include "proxy/http_proxy.h"
#include "tools/tool_registry.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "skills/skill_loader.h"

static const char *TAG = "shrimp";

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP time synchronization event triggered (底层同步成功)");
}

static void init_sntp(void)
{
    /* Set timezone from config */
    setenv("TZ", SHRIMP_TIMEZONE, 1);
    tzset();

    /* Stop SNTP if already running to allow reconfiguration */
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
        ESP_LOGI(TAG, "SNTP stopped for reconfiguration");
    }

    /* Configure SNTP with immediate sync mode */
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    /* 注册时间同步回调 */
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);

    esp_sntp_setservername(0, "203.107.6.88");      /* 阿里云 NTP IP */
    esp_sntp_setservername(1, "139.199.214.202");   /* 腾讯云 NTP IP */
    esp_sntp_setservername(2, "cn.pool.ntp.org");   /* 备用域名池 */

    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP initialized, syncing time via %s...", esp_sntp_getservername(0));
}

static void wait_for_time_sync(int timeout_sec)
{
    int retry = 0;
    time_t now = 0;
    struct tm timeinfo = { 0 };

    // 循环检查系统时间，如果年份 > 2020，说明同步成功
    while (retry < timeout_sec) {
        time(&now);
        localtime_r(&now, &timeinfo);
        
        // tm_year 是从 1900 年开始计算的。2020 年的 tm_year 是 120
        if (timeinfo.tm_year >= (2020 - 1900)) {
            break; 
        }
        
        ESP_LOGI(TAG, "Waiting for time sync... (%d/%d)", retry + 1, timeout_sec);
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }

    // 再次判断是否成功
    if (timeinfo.tm_year >= (2020 - 1900)) {
        char time_buf[64];
        // 打印带时区的当前时间
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
        ESP_LOGI(TAG, "Time synchronized: %s", time_buf);
    } else {
        ESP_LOGW(TAG, "Time sync timeout, using unsynced time");
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SHRIMP_SPIFFS_BASE,
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", (int)total, (int)used);

    return ESP_OK;
}

/* Outbound dispatch task: reads from outbound queue and routes to channels */
static void outbound_dispatch_task(void *arg)
{
    ESP_LOGI(TAG, "Outbound dispatch started");

    while (1) {
        shrimp_msg_t msg;
        if (message_bus_pop_outbound(&msg, UINT32_MAX) != ESP_OK) continue;

        ESP_LOGI(TAG, "Dispatching response to %s:%s", msg.channel, msg.chat_id);

        if (strcmp(msg.channel, SHRIMP_CHAN_TELEGRAM) == 0) {
            if (!telegram_bot_is_configured()) {
                ESP_LOGW(TAG, "Telegram not configured, dropping outbound message for %s", msg.chat_id);
            } else {
                esp_err_t send_err = telegram_send_message(msg.chat_id, msg.content);
                if (send_err != ESP_OK) {
                    ESP_LOGE(TAG, "Telegram send failed for %s: %s", msg.chat_id, esp_err_to_name(send_err));
                } else {
                    ESP_LOGI(TAG, "Telegram send success for %s (%d bytes)", msg.chat_id, (int)strlen(msg.content));
                }
            }
        } else if (strcmp(msg.channel, SHRIMP_CHAN_FEISHU) == 0) {
            if (!feishu_bot_is_configured()) {
                ESP_LOGW(TAG, "Feishu not configured, dropping outbound message for %s", msg.chat_id);
            } else {
                esp_err_t send_err = feishu_send_message(msg.chat_id, msg.content);
                if (send_err != ESP_OK) {
                    ESP_LOGE(TAG, "Feishu send failed for %s: %s", msg.chat_id, esp_err_to_name(send_err));
                } else {
                    ESP_LOGI(TAG, "Feishu send success for %s (%d bytes)", msg.chat_id, (int)strlen(msg.content));
                }
            }
        } else if (strcmp(msg.channel, SHRIMP_CHAN_WEBSOCKET) == 0) {
            esp_err_t ws_err = ws_server_send(msg.chat_id, msg.content);
            if (ws_err != ESP_OK) {
                ESP_LOGW(TAG, "WS send failed for %s: %s", msg.chat_id, esp_err_to_name(ws_err));
            }
        } else if (strcmp(msg.channel, SHRIMP_CHAN_SYSTEM) == 0) {
            ESP_LOGI(TAG, "System message [%s]: %.128s", msg.chat_id, msg.content);
        } else {
            ESP_LOGW(TAG, "Unknown channel: %s", msg.channel);
        }

        free(msg.content);
    }
}

void app_main(void)
{
    /* Silence noisy components */
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  MiniShrimp - ESP32-S3 AI Agent");
    ESP_LOGI(TAG, "========================================");

    /* Print memory info */
    ESP_LOGI(TAG, "Internal free: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "PSRAM free:    %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Phase 1: Core infrastructure */
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(init_spiffs());

    /* Initialize subsystems */
    ESP_ERROR_CHECK(message_bus_init());
    ESP_ERROR_CHECK(memory_store_init());
    ESP_ERROR_CHECK(skill_loader_init());
    ESP_ERROR_CHECK(session_mgr_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_proxy_init());
    ESP_ERROR_CHECK(telegram_bot_init());
    ESP_ERROR_CHECK(feishu_bot_init());
    ESP_ERROR_CHECK(llm_proxy_init());
    ESP_ERROR_CHECK(tool_registry_init());
    ESP_ERROR_CHECK(cron_service_init());
    ESP_ERROR_CHECK(heartbeat_init());
    ESP_ERROR_CHECK(agent_loop_init());

    /* Start WiFi */
    esp_err_t wifi_err = wifi_manager_start();
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Scanning nearby APs on boot...");
        wifi_manager_scan_and_print();
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        if (wifi_manager_wait_connected(30000) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());

            /* Wait for network routes to stabilize */
            ESP_LOGI(TAG, "Waiting 2 seconds for network routes to stabilize...");
            vTaskDelay(pdMS_TO_TICKS(2000));

            /* Sync time via SNTP */
            init_sntp();
            wait_for_time_sync(30);

            /* Outbound dispatch task should start first to avoid dropping early replies. */
            ESP_ERROR_CHECK((xTaskCreatePinnedToCore(
                outbound_dispatch_task, "outbound",
                SHRIMP_OUTBOUND_STACK, NULL,
                SHRIMP_OUTBOUND_PRIO, NULL, SHRIMP_OUTBOUND_CORE) == pdPASS)
                ? ESP_OK : ESP_FAIL);

            /* Start network-dependent services */
            ESP_ERROR_CHECK(agent_loop_start());
            if (telegram_bot_is_configured()) {
                ESP_ERROR_CHECK(telegram_bot_start());
            } else {
                ESP_LOGI(TAG, "Telegram not configured, skipping");
            }
            if (feishu_bot_is_configured()) {
                ESP_ERROR_CHECK(feishu_bot_start());
            } else {
                ESP_LOGI(TAG, "Feishu not configured, skipping");
            }
            cron_service_start();
            heartbeat_start();
            ESP_ERROR_CHECK(ws_server_start());

            ESP_LOGI(TAG, "All services started! Config UI: http://%s:%d/config", wifi_manager_get_ip(), SHRIMP_WS_PORT);
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout. Check credentials in the web config UI.");
        }
    } else {
        ESP_LOGW(TAG, "No WiFi credentials. Starting config access point...");
        ESP_ERROR_CHECK(wifi_manager_start_config_ap());
        ESP_ERROR_CHECK(ws_server_start());
        ESP_LOGI(TAG, "Connect to AP '%s' and open http://192.168.4.1:%d/config", SHRIMP_CONFIG_AP_SSID, SHRIMP_WS_PORT);
    }

    ESP_LOGI(TAG, "MiniShrimp ready.");
}