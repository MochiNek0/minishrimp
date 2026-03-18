#include "wifi_manager.h"
#include "shrimp_config.h"

#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/semphr.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;
static char s_ip_str[16] = "0.0.0.0";
static bool s_connected = false;
static char s_current_ssid[33] = {0};
static char s_current_pass[65] = {0};
static bool s_is_config_ap_mode = false;
static SemaphoreHandle_t s_scan_done_sem = NULL;

#define WIFI_CANDIDATE_MAX  10

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_candidate_t;

static wifi_candidate_t s_candidates[WIFI_CANDIDATE_MAX];
static int s_candidate_count = 0;
static int s_current_candidate_idx = 0;

/* Hex utilities for Chinese SSIDs */
static void bytes_to_hex(const uint8_t *in, size_t in_len, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < in_len; i++) {
        out[i * 2] = hex[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[in_len * 2] = '\0';
}

static const char *wifi_reason_to_str(wifi_err_reason_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
    case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_MIC_FAILURE: return "MIC_FAILURE";
    case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
    default: return "UNKNOWN";
    }
}

/* ── WiFi List JSON helpers ───────────────────────────────────── */

/* Load WiFi list from NVS, returns cJSON array (caller must free) */
static cJSON *wifi_list_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(SHRIMP_NVS_WIFI, NVS_READONLY, &nvs) != ESP_OK) {
        return cJSON_CreateArray();
    }

    size_t len = 0;
    cJSON *list = NULL;
    if (nvs_get_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, NULL, &len) == ESP_OK && len > 0) {
        char *json_str = malloc(len);
        if (json_str && nvs_get_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, json_str, &len) == ESP_OK) {
            list = cJSON_Parse(json_str);
        }
        free(json_str);
    }
    nvs_close(nvs);

    if (!list || !cJSON_IsArray(list)) {
        if (list) cJSON_Delete(list);
        return cJSON_CreateArray();
    }
    return list;
}

/* Save WiFi list to NVS */
static esp_err_t wifi_list_save(cJSON *list)
{
    if (!list) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SHRIMP_NVS_WIFI, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    char *json_str = cJSON_PrintUnformatted(list);
    if (json_str) {
        err = nvs_set_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, json_str);
        if (err == ESP_OK) {
            nvs_commit(nvs);
        }
        free(json_str);
    }
    nvs_close(nvs);
    return err;
}

/* Add or update WiFi entry in list, maintaining max 5 entries */
static void wifi_list_upsert(cJSON *list, const char *ssid, const char *password)
{
    /* Remove existing entry with same SSID */
    int sz = cJSON_GetArraySize(list);
    for (int i = sz - 1; i >= 0; i--) {
        cJSON *item = cJSON_GetArrayItem(list, i);
        cJSON *item_ssid = cJSON_GetObjectItem(item, "ssid");
        if (item_ssid && cJSON_IsString(item_ssid) && strcmp(item_ssid->valuestring, ssid) == 0) {
            cJSON_DeleteItemFromArray(list, i);
            break;
        }
    }

    /* Add new entry at front */
    cJSON *new_item = cJSON_CreateObject();
    cJSON_AddStringToObject(new_item, "ssid", ssid);
    cJSON_AddStringToObject(new_item, "password", password);
    cJSON_InsertItemInArray(list, 0, new_item);

    /* Limit to 5 entries */
    while (cJSON_GetArraySize(list) > 5) {
        cJSON_DeleteItemFromArray(list, cJSON_GetArraySize(list) - 1);
    }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* Only auto-connect if not in config AP mode */
        if (!s_is_config_ap_mode) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        /* Signal scan completion */
        if (s_scan_done_sem) {
            xSemaphoreGive(s_scan_done_sem);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (disc) {
            ESP_LOGW(TAG, "Disconnected (reason=%d:%s)", disc->reason, wifi_reason_to_str(disc->reason));
        }
        /* Don't auto-reconnect in config AP mode */
        if (s_is_config_ap_mode) {
            return;
        }
        if (s_retry_count < SHRIMP_WIFI_MAX_RETRY) {
            /* Exponential backoff: 1s, 2s, 4s, 8s, ... capped at 30s */
            uint32_t delay_ms = SHRIMP_WIFI_RETRY_BASE_MS << s_retry_count;
            if (delay_ms > SHRIMP_WIFI_RETRY_MAX_MS) {
                delay_ms = SHRIMP_WIFI_RETRY_MAX_MS;
            }
            ESP_LOGW(TAG, "Disconnected, retry %d/%d in %" PRIu32 "ms",
                     s_retry_count + 1, SHRIMP_WIFI_MAX_RETRY, delay_ms);
            
            /* Do not block the event loop! Use a freeRTOS timer or simply non-blocking approach. */
            /* We can use a quick one-shot task to do the delay and reconnect */
            void reconnect_task(void *arg) {
                uint32_t ms = (uint32_t)(uintptr_t)arg;
                vTaskDelay(pdMS_TO_TICKS(ms));
                esp_wifi_connect();
                vTaskDelete(NULL);
            }
            xTaskCreate(reconnect_task, "wifi_recon", 2048, (void *)(uintptr_t)delay_ms, 5, NULL);
            
            s_retry_count++;
        } else {
            ESP_LOGE(TAG, "Failed to connect to %s after %d retries", s_current_ssid, SHRIMP_WIFI_MAX_RETRY);
            
            if (s_current_candidate_idx + 1 < s_candidate_count) {
                s_current_candidate_idx++;
                s_retry_count = 0;
                
                strncpy(s_current_ssid, s_candidates[s_current_candidate_idx].ssid, sizeof(s_current_ssid)-1);
                strncpy(s_current_pass, s_candidates[s_current_candidate_idx].pass, sizeof(s_current_pass)-1);
                
                ESP_LOGI(TAG, "Trying next candidate (%d/%d): %s", 
                         s_current_candidate_idx + 1, s_candidate_count, s_current_ssid);
                
                wifi_config_t next_cfg = {0};
                strncpy((char *)next_cfg.sta.ssid, s_current_ssid, sizeof(next_cfg.sta.ssid)-1);
                strncpy((char *)next_cfg.sta.password, s_current_pass, sizeof(next_cfg.sta.password)-1);
                
                esp_wifi_set_config(WIFI_IF_STA, &next_cfg);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "All candidates failed.");
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Connected! IP: %s", s_ip_str);
        s_retry_count = 0;
        s_connected = true;

        /* Save successfully connected WiFi to NVS list */
        if (s_current_ssid[0] != '\0') {
            cJSON *list = wifi_list_load();
            wifi_list_upsert(list, s_current_ssid, s_current_pass);
            if (wifi_list_save(list) == ESP_OK) {
                ESP_LOGI(TAG, "Saved %s to WiFi list", s_current_ssid);
            }
            cJSON_Delete(list);
        }

        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    wifi_config_t wifi_cfg = {0};

    /* 1. Collect ALL candidates from NVS WiFi list (no scan filtering yet) */
    nvs_handle_t nvs;
    s_candidate_count = 0;
    s_current_candidate_idx = 0;

    if (nvs_open(SHRIMP_NVS_WIFI, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = 0;
        char *json_str = NULL;
        if (nvs_get_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, NULL, &len) == ESP_OK && len > 0) {
            json_str = malloc(len);
            if (json_str && nvs_get_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, json_str, &len) == ESP_OK) {
                cJSON *list = cJSON_Parse(json_str);
                if (list && cJSON_IsArray(list)) {
                    int array_sz = cJSON_GetArraySize(list);
                    for (int i = 0; i < array_sz && s_candidate_count < WIFI_CANDIDATE_MAX; i++) {
                        cJSON *item = cJSON_GetArrayItem(list, i);
                        cJSON *ssid = cJSON_GetObjectItem(item, "ssid");
                        cJSON *pass = cJSON_GetObjectItem(item, "password");
                        
                        if (ssid && cJSON_IsString(ssid) && pass && cJSON_IsString(pass)) {
                            strncpy(s_candidates[s_candidate_count].ssid, ssid->valuestring, 32);
                            strncpy(s_candidates[s_candidate_count].pass, pass->valuestring, 64);
                            s_candidate_count++;
                        }
                    }
                }
                if (list) cJSON_Delete(list);
            }
            if (json_str) free(json_str);
        }
        
        /* Backward compatibility with single legacy connection */
        if (s_candidate_count == 0) {
            char legacy_ssid[33] = {0};
            char legacy_pass[65] = {0};
            len = sizeof(legacy_ssid);
            if (nvs_get_str(nvs, SHRIMP_NVS_KEY_SSID, legacy_ssid, &len) == ESP_OK && legacy_ssid[0] != '\0') {
                len = sizeof(legacy_pass);
                nvs_get_str(nvs, SHRIMP_NVS_KEY_PASS, legacy_pass, &len);
                strncpy(s_candidates[s_candidate_count].ssid, legacy_ssid, 32);
                strncpy(s_candidates[s_candidate_count].pass, legacy_pass, 64);
                s_candidate_count++;
            }
        }
        
        nvs_close(nvs);
    }

    /* 2. Add build-time secrets if not already in list */
    if (SHRIMP_SECRET_WIFI_SSID[0] != '\0' && s_candidate_count < WIFI_CANDIDATE_MAX) {
        bool already_added = false;
        for (int i = 0; i < s_candidate_count; i++) {
            if (strcmp(s_candidates[i].ssid, SHRIMP_SECRET_WIFI_SSID) == 0) {
                already_added = true;
                break;
            }
        }
        if (!already_added) {
            strncpy(s_candidates[s_candidate_count].ssid, SHRIMP_SECRET_WIFI_SSID, 32);
            strncpy(s_candidates[s_candidate_count].pass, SHRIMP_SECRET_WIFI_PASS, 64);
            s_candidate_count++;
        }
    }

    if (s_candidate_count == 0) {
        ESP_LOGW(TAG, "No WiFi credentials available. Use AP to configure.");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Collected %d WiFi candidates", s_candidate_count);
    for (int i = 0; i < s_candidate_count; i++) {
        ESP_LOGI(TAG, "  Candidate %d: %s", i + 1, s_candidates[i].ssid);
    }

    /* 3. Start WiFi first (we need STA running before we can scan)
     *    Use a temporary dummy config; the real one will be set after scan.
     *    Set defer flag so STA_START event doesn't auto-connect. */
    s_is_config_ap_mode = true;  /* Temporarily prevent auto-connect on STA_START */
    strncpy((char *)wifi_cfg.sta.ssid, s_candidates[0].ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, s_candidates[0].pass, sizeof(wifi_cfg.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 4. Now scan for nearby APs */
    ESP_LOGI(TAG, "Starting boot-time WiFi scan...");
    
    if (!s_scan_done_sem) {
        s_scan_done_sem = xSemaphoreCreateBinary();
    }
    xSemaphoreTake(s_scan_done_sem, 0); /* clear stale signal */

    wifi_scan_config_t scan_cfg = {0};
    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_list = NULL;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err == ESP_OK) {
        if (xSemaphoreTake(s_scan_done_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
            esp_wifi_scan_get_ap_num(&ap_count);
            if (ap_count > 0) {
                ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
                if (ap_list) {
                    esp_wifi_scan_get_ap_records(&ap_count, ap_list);
                    ESP_LOGI(TAG, "Boot scan found %u APs:", ap_count);
                    for (uint16_t i = 0; i < ap_count; i++) {
                        ESP_LOGI(TAG, "  [%u] %s (RSSI=%d)", i + 1,
                                 (const char *)ap_list[i].ssid, ap_list[i].rssi);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "Boot scan timed out");
            esp_wifi_scan_stop();
        }
    } else {
        ESP_LOGW(TAG, "Boot scan failed: %s (proceeding with all candidates)", esp_err_to_name(err));
    }

    /* 5. Reorder candidates: nearby APs first, then the rest as fallback */
    if (ap_list && ap_count > 0) {
        wifi_candidate_t reordered[WIFI_CANDIDATE_MAX];
        int nearby_count = 0;

        /* First pass: add candidates that match a scanned AP (by signal strength order) */
        for (int i = 0; i < s_candidate_count; i++) {
            for (uint16_t j = 0; j < ap_count; j++) {
                if (strcmp(s_candidates[i].ssid, (const char *)ap_list[j].ssid) == 0) {
                    reordered[nearby_count++] = s_candidates[i];
                    ESP_LOGI(TAG, "  -> Nearby match: %s (RSSI=%d)", s_candidates[i].ssid, ap_list[j].rssi);
                    break;
                }
            }
        }

        /* Second pass: add remaining candidates not seen nearby */
        for (int i = 0; i < s_candidate_count; i++) {
            bool found_nearby = false;
            for (int k = 0; k < nearby_count; k++) {
                if (strcmp(s_candidates[i].ssid, reordered[k].ssid) == 0) {
                    found_nearby = true;
                    break;
                }
            }
            if (!found_nearby && nearby_count < WIFI_CANDIDATE_MAX) {
                reordered[nearby_count++] = s_candidates[i];
            }
        }

        memcpy(s_candidates, reordered, sizeof(reordered));
        s_candidate_count = nearby_count;
    }

    if (ap_list) free(ap_list);

    /* 6. Connect to the first (best) candidate */
    s_is_config_ap_mode = false;  /* Re-enable auto-reconnect on disconnect */

    strncpy(s_current_ssid, s_candidates[0].ssid, sizeof(s_current_ssid) - 1);
    strncpy(s_current_pass, s_candidates[0].pass, sizeof(s_current_pass) - 1);
    s_current_candidate_idx = 0;

    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    strncpy((char *)wifi_cfg.sta.ssid, s_current_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, s_current_pass, sizeof(wifi_cfg.sta.password) - 1);

    ESP_LOGI(TAG, "Starting connection sequence with %d candidates. Primary: %s", 
             s_candidate_count, s_current_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    esp_wifi_connect();

    return ESP_OK;
}

esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, ticks);

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

const char *wifi_manager_get_ip(void)
{
    return s_ip_str;
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(SHRIMP_NVS_WIFI, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, SHRIMP_NVS_KEY_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs, SHRIMP_NVS_KEY_PASS, password));
    nvs_commit(nvs);
    nvs_close(nvs);

    /* Add to WiFi list as first priority */
    cJSON *list = wifi_list_load();
    wifi_list_upsert(list, ssid, password);
    wifi_list_save(list);
    cJSON_Delete(list);

    ESP_LOGI(TAG, "WiFi credentials saved as priority: %s", ssid);

    strncpy(s_current_ssid, ssid, sizeof(s_current_ssid)-1);
    s_current_ssid[sizeof(s_current_ssid)-1] = '\0';
    strncpy(s_current_pass, password, sizeof(s_current_pass)-1);
    s_current_pass[sizeof(s_current_pass)-1] = '\0';

    /* Reset candidates and make this the only one for now */
    strncpy(s_candidates[0].ssid, ssid, 32);
    strncpy(s_candidates[0].pass, password, 64);
    s_candidate_count = 1;
    s_current_candidate_idx = 0;

    return ESP_OK;
}

esp_err_t wifi_manager_set_wifi_list(const char *json_str) {
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(SHRIMP_NVS_WIFI, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, json_str));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    ESP_LOGI(TAG, "WiFi list overridden");
    return ESP_OK;
}



esp_err_t wifi_manager_start_config_ap(void)
{
    s_is_config_ap_mode = true;

    wifi_config_t ap_cfg = {0};
    const char *ssid = SHRIMP_CONFIG_AP_SSID;
    const char *pass = SHRIMP_CONFIG_AP_PASS;

    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char *)ap_cfg.ap.password, pass, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len = strlen(ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = (pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;

    /* Use APSTA mode so STA interface is available for WiFi scanning */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Config AP started in APSTA mode: SSID=%s (IP: 192.168.4.1)", ssid);
    return ESP_OK;
}

EventGroupHandle_t wifi_manager_get_event_group(void)
{
    return s_wifi_event_group;
}

void wifi_manager_scan_and_print(void)
{
    /* This function is now a no-op during boot.
     * Boot-time scanning is already done inside wifi_manager_start().
     * Calling disconnect/reconnect here would break the candidate
     * connection flow, so we just log and return.
     */
    ESP_LOGI(TAG, "scan_and_print: boot scan already completed in wifi_manager_start()");
}

esp_err_t wifi_manager_get_scan_results(char **out_json_str) {
    if (!out_json_str) return ESP_ERR_INVALID_ARG;
    *out_json_str = NULL;

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    ESP_LOGI(TAG, "Scanning nearby APs for API...");

    /* Create semaphore for non-blocking scan if not yet created */
    if (!s_scan_done_sem) {
        s_scan_done_sem = xSemaphoreCreateBinary();
    }

    /* Config AP already runs in APSTA mode, so no mode switching needed.
     * STA mode also supports scanning while connected.
     * Use non-blocking scan to avoid starving other tasks. */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        /* Clear any stale semaphore signal */
        xSemaphoreTake(s_scan_done_sem, 0);

        err = esp_wifi_scan_start(&scan_cfg, false /* non-blocking */);
        if (err == ESP_OK) {
            /* Wait for WIFI_EVENT_SCAN_DONE with timeout */
            if (xSemaphoreTake(s_scan_done_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
                break;
            } else {
                ESP_LOGW(TAG, "Scan timeout on attempt %d", attempt + 1);
                esp_wifi_scan_stop();
                err = ESP_ERR_TIMEOUT;
            }
        } else {
            ESP_LOGW(TAG, "Scan attempt %d failed (%s), retrying...", attempt + 1, esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed after retries: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    if (ap_count > 0) {
        wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (ap_list) {
            if (esp_wifi_scan_get_ap_records(&ap_count, ap_list) == ESP_OK) {
                for (uint16_t i = 0; i < ap_count; i++) {
                    cJSON *ap_obj = cJSON_CreateObject();
                    char hex_buf[65] = {0};
                    bytes_to_hex(ap_list[i].ssid, strnlen((const char *)ap_list[i].ssid, 32), hex_buf);

                    cJSON_AddStringToObject(ap_obj, "ssid_hex", hex_buf);
                    cJSON_AddNumberToObject(ap_obj, "rssi", ap_list[i].rssi);
                    cJSON_AddNumberToObject(ap_obj, "auth", ap_list[i].authmode);
                    cJSON_AddItemToArray(root, ap_obj);
                }
            }
            free(ap_list);
        }
    }

    char *json_txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_txt) {
        *out_json_str = json_txt;
        return ESP_OK;
    }

    return ESP_ERR_NO_MEM;
}

esp_err_t wifi_manager_get_saved_list(char **out_json_str)
{
    if (!out_json_str) return ESP_ERR_INVALID_ARG;
    *out_json_str = NULL;

    nvs_handle_t nvs;
    if (nvs_open(SHRIMP_NVS_WIFI, NVS_READONLY, &nvs) != ESP_OK) {
        /* Return empty array if no NVS data */
        *out_json_str = strdup("[]");
        return *out_json_str ? ESP_OK : ESP_ERR_NO_MEM;
    }

    size_t len = 0;
    char *json_str = NULL;
    esp_err_t err = nvs_get_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, NULL, &len);

    if (err == ESP_OK && len > 0) {
        json_str = malloc(len);
        if (json_str && nvs_get_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, json_str, &len) == ESP_OK) {
            /* Validate it's a valid JSON array */
            cJSON *list = cJSON_Parse(json_str);
            if (list && cJSON_IsArray(list)) {
                /* Build output with only SSID (no password) for security */
                cJSON *out_arr = cJSON_CreateArray();
                int sz = cJSON_GetArraySize(list);
                for (int i = 0; i < sz; i++) {
                    cJSON *item = cJSON_GetArrayItem(list, i);
                    cJSON *ssid = cJSON_GetObjectItem(item, "ssid");
                    if (ssid && cJSON_IsString(ssid)) {
                        cJSON *out_item = cJSON_CreateObject();
                        cJSON_AddStringToObject(out_item, "ssid", ssid->valuestring);
                        cJSON_AddItemToArray(out_arr, out_item);
                    }
                }
                cJSON_Delete(list);
                char *out = cJSON_PrintUnformatted(out_arr);
                cJSON_Delete(out_arr);
                free(json_str);
                nvs_close(nvs);
                if (out) {
                    *out_json_str = out;
                    return ESP_OK;
                }
                return ESP_ERR_NO_MEM;
            }
            if (list) cJSON_Delete(list);
        }
        if (json_str) free(json_str);
    }

    nvs_close(nvs);
    *out_json_str = strdup("[]");
    return *out_json_str ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t wifi_manager_delete_saved(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    cJSON *list = wifi_list_load();
    if (cJSON_GetArraySize(list) == 0) {
        cJSON_Delete(list);
        return ESP_ERR_NOT_FOUND;
    }

    /* Find and delete the item */
    bool found = false;
    int sz = cJSON_GetArraySize(list);
    for (int i = sz - 1; i >= 0; i--) {
        cJSON *item = cJSON_GetArrayItem(list, i);
        cJSON *item_ssid = cJSON_GetObjectItem(item, "ssid");
        if (item_ssid && cJSON_IsString(item_ssid) &&
            strcmp(item_ssid->valuestring, ssid) == 0) {
            cJSON_DeleteItemFromArray(list, i);
            found = true;
            break;
        }
    }

    if (!found) {
        cJSON_Delete(list);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = wifi_list_save(list);
    cJSON_Delete(list);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Deleted WiFi from saved list: %s", ssid);
    }
    return err;
}

esp_err_t wifi_manager_connect_to(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!password) password = "";

    ESP_LOGI(TAG, "Switching WiFi to: %s", ssid);

    /* Save to NVS as primary */
    wifi_manager_set_credentials(ssid, password);

    /* Reset connection state */
    s_retry_count = 0;
    s_connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    /* Configure STA */
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);

    /* Ensure we're in a mode that supports STA */
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    /* If coming from config AP mode, we're now transitioning to STA connection */
    s_is_config_ap_mode = false;

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_err_t err = esp_wifi_connect();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start connection: %s", esp_err_to_name(err));
    }

    return err;
}
