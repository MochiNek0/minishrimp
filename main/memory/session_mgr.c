#include "session_mgr.h"
#include "shrimp_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "session";
static SemaphoreHandle_t s_session_lock = NULL;

/* FNV-1a 64-bit hash for compact, collision-resistant filenames */
static uint64_t fnv1a_64(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (; *s; s++) {
        h ^= (uint64_t)(unsigned char)*s;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/*
 * Build a compact session file path that fits within SPIFFS default
 * OBJ_NAME_LEN of 32 bytes.  Internal SPIFFS name: "s_<16hex>.jsonl"
 * = 22 chars + null = 23 bytes, well within the 32 byte limit.
 */
static void session_path(const char *chat_id, char *buf, size_t size)
{
    uint64_t h = fnv1a_64(chat_id);
    snprintf(buf, size, "%s/s_%016llx.jsonl", SHRIMP_SPIFFS_BASE,
             (unsigned long long)h);
}

esp_err_t session_mgr_init(void)
{
    if (s_session_lock == NULL) {
        s_session_lock = xSemaphoreCreateRecursiveMutex();
    }
    ESP_LOGI(TAG, "Session manager initialized at %s", SHRIMP_SPIFFS_BASE);
    return ESP_OK;
}

/*
 * Trim session file to keep only the most recent messages.
 * Reads the file, keeps the last SHRIMP_SESSION_MAX_MSGS messages,
 * and rewrites the file.
 */
static void trim_session_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    int total_lines = 0;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        total_lines++;
    }
    fclose(f);

    if (total_lines <= SHRIMP_SESSION_MAX_MSGS) {
        return;
    }

    int skip_lines = total_lines - SHRIMP_SESSION_MAX_MSGS;
    const char *tmp_path = "/spiffs/trim.tmp";
    
    FILE *src = fopen(path, "r");
    FILE *dst = fopen(tmp_path, "w");
    if (!src || !dst) {
        if (src) fclose(src);
        if (dst) fclose(dst);
        return;
    }

    int current_line = 0;
    while (fgets(line, sizeof(line), src)) {
        if (current_line >= skip_lines) {
            fputs(line, dst);
        }
        current_line++;
    }

    fclose(src);
    fclose(dst);

    unlink(path);
    rename(tmp_path, path);

    ESP_LOGI(TAG, "Streaming trim: %d -> %d lines", total_lines, SHRIMP_SESSION_MAX_MSGS);
}

/*
 * Check file size and trim if necessary.
 */
static void check_and_trim_session(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    /* Check number of lines first for hysteresis trimming */
    int total_lines = 0;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        total_lines++;
    }

    /* Get file size */
    long size = ftell(f);
    fclose(f);

    /* 
     * Hysteresis Trimming: 
     * Only trim if count exceeds MAX_MSGS + MARGIN or File size exceeds LIMIT.
     * This significantly reduces Flash writes for active conversations.
     */
    if (total_lines > (SHRIMP_SESSION_MAX_MSGS + SHRIMP_SESSION_TRIM_MARGIN) || 
        size > SHRIMP_SESSION_MAX_FILE_SIZE) {
        
        ESP_LOGI(TAG, "Trimming triggered: %d lines, %ld bytes", total_lines, size);
        trim_session_file(path);
    }
}

esp_err_t session_append(const char *chat_id, const char *role, const char *content)
{
    if (!s_session_lock) return ESP_FAIL;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);

    char path[128];
    session_path(chat_id, path, sizeof(path));

    /* Check and trim before appending if file is too large */
    check_and_trim_session(path);

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open session file %s", path);
        xSemaphoreGive(s_session_lock);
        return ESP_FAIL;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "role", role);
    cJSON_AddStringToObject(obj, "content", content);
    cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (line) {
        fprintf(f, "%s\n", line);
        free(line);
    }

    fclose(f);
    xSemaphoreGive(s_session_lock);
    return ESP_OK;
}

/*
 * Format timestamp as human-readable relative time.
 * Returns "just now", "X min ago", "X hours ago", "yesterday", or date string.
 */
static void format_relative_time(time_t msg_ts, time_t now_ts, char *buf, size_t size)
{
    if (msg_ts <= 0 || now_ts <= 0) {
        buf[0] = '\0';
        return;
    }

    double diff = difftime(now_ts, msg_ts);

    if (diff < 60) {
        snprintf(buf, size, "just now");
    } else if (diff < 3600) {
        int mins = (int)(diff / 60);
        snprintf(buf, size, "%d min ago", mins);
    } else if (diff < 86400) {
        int hours = (int)(diff / 3600);
        snprintf(buf, size, "%d hours ago", hours);
    } else if (diff < 172800) {
        snprintf(buf, size, "yesterday");
    } else {
        struct tm tm_info;
        localtime_r(&msg_ts, &tm_info);
        strftime(buf, size, "%m/%d", &tm_info);
    }
}

esp_err_t session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs)
{
    if (!s_session_lock) return ESP_FAIL;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);

    char path[128];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        /* No history yet */
        snprintf(buf, size, "[]");
        xSemaphoreGive(s_session_lock);
        return ESP_OK;
    }

    /* Get current time for relative timestamps */
    time_t now_ts = time(NULL);

    /* Read all lines into a ring buffer of cJSON objects */
    cJSON *messages[SHRIMP_SESSION_MAX_MSGS];
    int count = 0;
    int write_idx = 0;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;

        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;

        /* Ring buffer: overwrite oldest if full */
        if (count >= max_msgs) {
            cJSON_Delete(messages[write_idx]);
        }
        messages[write_idx] = obj;
        write_idx = (write_idx + 1) % max_msgs;
        if (count < max_msgs) count++;
    }
    fclose(f);

    /* Build JSON array with role, content, and time hint */
    cJSON *arr = cJSON_CreateArray();
    int start = (count < max_msgs) ? 0 : write_idx;

    for (int i = 0; i < count; i++) {
        int idx = (start + i) % max_msgs;
        cJSON *src = messages[idx];

        cJSON *role = cJSON_GetObjectItem(src, "role");
        cJSON *content = cJSON_GetObjectItem(src, "content");
        cJSON *ts = cJSON_GetObjectItem(src, "ts");

        /* Skip entries with missing role or content */
        if (!role || !cJSON_IsString(role) || !content || !cJSON_IsString(content)) {
            continue;
        }

        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "role", role->valuestring);

        /* Add time hint to content if available */
        if (ts && cJSON_IsNumber(ts)) {
            char time_hint[64];
            format_relative_time((time_t)ts->valuedouble, now_ts, time_hint, sizeof(time_hint));

            char content_with_time[2100];
            snprintf(content_with_time, sizeof(content_with_time), "[%s] %s",
                time_hint, content->valuestring);
            cJSON_AddStringToObject(entry, "content", content_with_time);
        } else {
            cJSON_AddStringToObject(entry, "content", content->valuestring);
        }

        cJSON_AddItemToArray(arr, entry);
    }

    /* Cleanup ring buffer */
    int cleanup_start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (cleanup_start + i) % max_msgs;
        cJSON_Delete(messages[idx]);
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (json_str) {
        strncpy(buf, json_str, size - 1);
        buf[size - 1] = '\0';
        free(json_str);
    } else {
        snprintf(buf, size, "[]");
    }

    xSemaphoreGive(s_session_lock);
    return ESP_OK;
}

esp_err_t session_clear(const char *chat_id)
{
    if (!s_session_lock) return ESP_FAIL;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);

    char path[128];
    session_path(chat_id, path, sizeof(path));

    if (remove(path) == 0) {
        ESP_LOGI(TAG, "Session %s cleared", chat_id);
        xSemaphoreGive(s_session_lock);
        return ESP_OK;
    }
    xSemaphoreGive(s_session_lock);
    return ESP_ERR_NOT_FOUND;
}

void session_list(void)
{
    DIR *dir = opendir(SHRIMP_SPIFFS_SESSION_DIR);
    if (!dir) {
        /* SPIFFS is flat, so list all files matching pattern */
        dir = opendir(SHRIMP_SPIFFS_BASE);
        if (!dir) {
            ESP_LOGW(TAG, "Cannot open SPIFFS directory");
            return;
        }
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, "s_") && strstr(entry->d_name, ".jsonl")) {
            ESP_LOGI(TAG, "  Session: %s", entry->d_name);
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        ESP_LOGI(TAG, "  No sessions found");
    }
}
