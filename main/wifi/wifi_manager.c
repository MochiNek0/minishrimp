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

static const char *TAG = "wifi";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;
static char s_ip_str[16] = "0.0.0.0";
static bool s_connected = false;
static char s_current_ssid[33] = {0};
static char s_current_pass[65] = {0};

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
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (disc) {
            ESP_LOGW(TAG, "Disconnected (reason=%d:%s)", disc->reason, wifi_reason_to_str(disc->reason));
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
                /* Consider triggering fallback to AP here if needed */
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

    /* Scan surrounding APs to match with NVS list */
    ESP_LOGI(TAG, "Starting boot-time WiFi scan...");
    wifi_scan_config_t scan_cfg = {0};
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    
    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_list = NULL;
    
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 0) {
            ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
            if (ap_list) {
                esp_wifi_scan_get_ap_records(&ap_count, ap_list);
            }
        }
    } else {
        ESP_LOGE(TAG, "Boot-time WiFi scan failed: %s", esp_err_to_name(err));
    }

    /* 1. Collect all nearby networks from the NVS WiFi list */
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
                            bool is_nearby = false;
                            if (!ap_list || ap_count == 0) {
                                is_nearby = true;
                            } else {
                                for (uint16_t j = 0; j < ap_count; j++) {
                                    if (strcmp((const char *)ap_list[j].ssid, ssid->valuestring) == 0) {
                                        is_nearby = true;
                                        break;
                                    }
                                }
                            }
                            
                            if (is_nearby) {
                                strncpy(s_candidates[s_candidate_count].ssid, ssid->valuestring, 32);
                                strncpy(s_candidates[s_candidate_count].pass, pass->valuestring, 64);
                                s_candidate_count++;
                            }
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

    /* 2. Collect build-time secrets if not already in list or as fallback */
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
        if (ap_list) free(ap_list);
        ESP_LOGW(TAG, "No WiFi credentials available. Use AP to configure.");
        return ESP_ERR_NOT_FOUND;
    }

    if (ap_list) free(ap_list);

    /* 3. Start connecting to the first candidate */
    strncpy(s_current_ssid, s_candidates[0].ssid, sizeof(s_current_ssid)-1);
    strncpy(s_current_pass, s_candidates[0].pass, sizeof(s_current_pass)-1);
    s_current_candidate_idx = 0;

    strncpy((char *)wifi_cfg.sta.ssid, s_current_ssid, sizeof(wifi_cfg.sta.ssid)-1);
    strncpy((char *)wifi_cfg.sta.password, s_current_pass, sizeof(wifi_cfg.sta.password)-1);

    ESP_LOGI(TAG, "Starting connection sequence with %d candidates. Primary: %s", 
             s_candidate_count, s_current_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

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
    wifi_config_t ap_cfg = {0};
    const char *ssid = SHRIMP_CONFIG_AP_SSID;
    const char *pass = SHRIMP_CONFIG_AP_PASS;

    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char *)ap_cfg.ap.password, pass, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len = strlen(ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = (pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Config AP started: SSID=%s (IP: 192.168.4.1)", ssid);
    return ESP_OK;
}
EventGroupHandle_t wifi_manager_get_event_group(void)
{
    return s_wifi_event_group;
}

void wifi_manager_scan_and_print(void)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    ESP_LOGI(TAG, "Scanning nearby APs...");

    /* Pause auto-connect to allow scan */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (err == ESP_ERR_WIFI_STATE) {
        /* Try a quick stop/start cycle and scan again */
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_wifi_start();
        vTaskDelay(pdMS_TO_TICKS(200));
        err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        esp_wifi_connect();
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGW(TAG, "No APs found");
        esp_wifi_connect();
        return;
    }

    wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_list) {
        ESP_LOGE(TAG, "Out of memory for AP list");
        return;
    }

    uint16_t ap_max = ap_count;
    if (esp_wifi_scan_get_ap_records(&ap_max, ap_list) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP records");
        free(ap_list);
        esp_wifi_connect();
        return;
    }

    ESP_LOGI(TAG, "Found %u APs:", ap_max);
    for (uint16_t i = 0; i < ap_max; i++) {
        const wifi_ap_record_t *ap = &ap_list[i];
        ESP_LOGI(TAG, "  [%u] SSID=%s RSSI=%d CH=%d Auth=%d",
                 i + 1, (const char *)ap->ssid, ap->rssi, ap->primary, ap->authmode);
    }

    free(ap_list);
    esp_wifi_connect();
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

    wifi_mode_t old_mode;
    esp_wifi_get_mode(&old_mode);

    bool switched_to_apsta = false;
    if (old_mode == WIFI_MODE_AP) {
        /* Switch to APSTA to enable STA scanning while keeping AP alive */
        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (mode_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to switch to APSTA mode: %s", esp_err_to_name(mode_err));
            return mode_err;
        }
        switched_to_apsta = true;
        /* Wait for STA interface to be ready after mode switch */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* We do not need to disconnect STA to scan. ESP32 supports scanning while connected. */

    /* Try scan with retries */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = esp_wifi_scan_start(&scan_cfg, true);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "Scan attempt %d failed (%s), retrying...", attempt + 1, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed after retries: %s", esp_err_to_name(err));
        if (switched_to_apsta) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        return err;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        if (switched_to_apsta) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
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

    /* Restore original WiFi mode */
    if (switched_to_apsta) {
        esp_wifi_set_mode(WIFI_MODE_AP);
        ESP_LOGD(TAG, "Restored WiFi mode to AP");
    }

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
