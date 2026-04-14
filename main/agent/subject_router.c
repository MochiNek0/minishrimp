#include "subject_router.h"
#include "shrimp_config.h"
#include "llm/llm_proxy.h"
#include "cJSON.h"
#include "utils/json_utils.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "router";

#define INDEX_PATH SHRIMP_SPIFFS_SESSION_DIR "/sessions_idx.bin"
#define MAX_SESSIONS 100

static session_meta_t *s_index = NULL;
static int s_session_count = 0;
static SemaphoreHandle_t s_lock = NULL;
static bool s_dirty = false;
static bool s_task_running = false;

/* The 64 Subject Dimensions */
static const char *SUBJECT_NAMES[SUBJECT_VEC_DIM] = {
    "Daily Routine", "Household & Family", "Physical Health", "Food & Dining", "Personal Finance", "Shopping & Goods", "Travel & Geography", "Weather & Nature",
    "Career & Business", "Productivity Tools", "Education", "Science & Math", "Consumer Tech", "Software Dev", "Hardware & Electronics", "Engineering",
    "News & Events", "Politics & Society", "History & Culture", "Philosophy & Ethics", "Literature & Writing", "Movies & TV", "Music & Audio", "Arts & Photography",
    "Emotions & Mental Health", "Humor & Entertainment", "Socializing & Chat", "Pets & Animals", "Fashion & Style", "Sports & Fitness", "Video Games", "Hobbies & DIY",
    "Web Search & Facts", "File Management", "Home Automation", "Scheduling & Alarms", "Navigation & Maps", "Communication", "Languages", "Measurements",
    "Space & Astronomy", "AI & Robotics", "Ecology & Environment", "Economics & Markets", "Psychology", "Sociology", "Legal & Rights", "Security & Privacy",
    "Information Request", "Task Commands", "Personal Opinions", "Problem Reports", "Logic & Reasoning", "Creative Narrative", "Debate & Argument", "Appreciation",
    "User Identity", "Assistant Persona", "Abstract Dreams", "Future Planning", "Logistics & Shipping", "Marketing & Business", "Emergency & Urgent", "General Knowledge"
};

static void subject_router_sync_task(void *arg)
{
    s_task_running = true;
    while (s_task_running) {
        vTaskDelay(pdMS_TO_TICKS(10000)); // Check every 10 seconds
        if (s_dirty) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            subject_router_sync_index();
            s_dirty = false;
            xSemaphoreGive(s_lock);
            ESP_LOGD(TAG, "Background sync completed");
        }
    }
    vTaskDelete(NULL);
}

esp_err_t subject_router_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_index) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    s_index = heap_caps_calloc(MAX_SESSIONS, sizeof(session_meta_t), MALLOC_CAP_SPIRAM);
    if (!s_index) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(INDEX_PATH, "rb");
    if (f) {
        s_session_count = fread(s_index, sizeof(session_meta_t), MAX_SESSIONS, f);
        fclose(f);
        ESP_LOGI(TAG, "Loaded %d session vectors from index", s_session_count);
    } else {
        ESP_LOGI(TAG, "No existing session index found");
    }

    xTaskCreate(subject_router_sync_task, "router_sync", 3072, NULL, 3, NULL);
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

esp_err_t subject_router_classify(const char *content, float *out_vec, char *out_summary, size_t summary_size)
{
    if (!content || !out_vec) return ESP_ERR_INVALID_ARG;

    /* Build the router prompt */
    char *prompt = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!prompt) return ESP_ERR_NO_MEM;

    /* Build the list of subjects for the prompt */
    char subject_list[1024] = "";
    for (int i = 0; i < SUBJECT_VEC_DIM; i++) {
        char item[64];
        snprintf(item, sizeof(item), "%d. %s%s", i + 1, SUBJECT_NAMES[i], (i == SUBJECT_VEC_DIM - 1) ? "" : ", ");
        strncat(subject_list, item, sizeof(subject_list) - strlen(subject_list) - 1);
    }

    /* Define classification tool */
    const char *tools_json = "[{"
        "\"name\": \"set_topic_attributes\","
        "\"description\": \"Identify the top 5 most relevant subject categories and a summary\","
        "\"input_schema\": {"
            "\"type\": \"object\","
            "\"properties\": {"
                "\"indices\": {"
                    "\"type\": \"array\", "
                    "\"items\": {\"type\": \"integer\", \"minimum\": 1, \"maximum\": 64}, "
                    "\"minItems\": 5, \"maxItems\": 5, "
                    "\"description\": \"Indices of the 5 most relevant subjects (1-64)\""
                "},"
                "\"summary\": {\"type\": \"string\", \"description\": \"10-word summary of the topic\"}"
            "},"
            "\"required\": [\"indices\", \"summary\"]"
        "}"
    "}]";

    snprintf(prompt, 4096,
        "Classify the user message by picking the EXACTLY 5 most relevant subject indices (1-64) in descending order of relevance. "
        "Also provide a 10-word summary. "
        "IMPORTANT: You MUST return your response as a JSON object: {\"indices\": [rank1, rank2, rank3, rank4, rank5], \"summary\": \"...\"}\n"
        "Subjects:\n%s",
        subject_list);

    cJSON *messages = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", content);
    cJSON_AddItemToArray(messages, msg);

    llm_response_t resp;
    esp_err_t err = llm_chat_tools(prompt, messages, tools_json, &resp);
    free(prompt);
    cJSON_Delete(messages);

    if (err != ESP_OK) return err;

    /* Parse from tool input or response text */
    const char *json_ptr = (resp.tool_use && resp.call_count > 0) ? resp.calls[0].input : resp.text;
    if (json_ptr) {
        const char *start = strchr(json_ptr, '{');
        if (start) json_ptr = start;
    }

    /* 1. Try JSON repair and standard parse */
    char *repaired = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    cJSON *root = NULL;
    if (repaired && shrimp_json_repair(json_ptr, repaired, 4096)) {
        root = cJSON_Parse(repaired);
    } else {
        root = cJSON_Parse(json_ptr);
    }
    free(repaired);

    bool success = false;
    float sum_sq = 0;
    memset(out_vec, 0, sizeof(float) * SUBJECT_VEC_DIM);

    if (root) {
        cJSON *idx_arr = cJSON_GetObjectItem(root, "indices");
        if (idx_arr && cJSON_IsArray(idx_arr) && cJSON_GetArraySize(idx_arr) == 5) {
            const float weights[] = {1.0f, 0.8f, 0.6f, 0.4f, 0.2f};
            for (int i = 0; i < 5; i++) {
                cJSON *item = cJSON_GetArrayItem(idx_arr, i);
                if (item && cJSON_IsNumber(item)) {
                    int idx = item->valueint - 1; // 1-64 to 0-63
                    if (idx >= 0 && idx < SUBJECT_VEC_DIM) {
                        out_vec[idx] = weights[i];
                        sum_sq += out_vec[idx] * out_vec[idx];
                    }
                }
            }
            success = true;
        }

        /* Extract summary */
        if (out_summary && summary_size > 0) {
            cJSON *sum_item = cJSON_GetObjectItem(root, "summary");
            if (sum_item && cJSON_IsString(sum_item)) {
                strncpy(out_summary, sum_item->valuestring, summary_size - 1);
                out_summary[summary_size - 1] = '\0';
            } else {
                strncpy(out_summary, "No summary", summary_size - 1);
                out_summary[summary_size - 1] = '\0';
            }
        }

        cJSON_Delete(root);
    }

    /* Fallback: Heuristic Scan for indices and Summary if JSON parsing failed */
    if (!success) {
        ESP_LOGW(TAG, "JSON parse failed, attempting heuristic scan for top indices...");
        
        /* Attempt to extract summary from raw text if JSON failed */
        if (out_summary && summary_size > 0) {
            const char *src = (json_ptr ? json_ptr : resp.text);
            const char *sum_tag = strstr(src, "\"summary\":");
            if (!sum_tag) sum_tag = strstr(src, "summary:");
            
            if (sum_tag) {
                const char *val_start = strchr(sum_tag, ':') + 1;
                while (*val_start == ' ' || *val_start == '"') val_start++;
                const char *val_end = val_start;
                while (*val_end && *val_end != '"' && *val_end != ',' && *val_end != '\n') val_end++;
                
                size_t len = val_end - val_start;
                if (len > summary_size - 1) len = summary_size - 1;
                strncpy(out_summary, val_start, len);
                out_summary[len] = '\0';
            } else {
                strncpy(out_summary, "Topic Summary (R)", summary_size - 1);
            }
        }

        int count = 0;
        const char *p = (json_ptr ? json_ptr : resp.text);
        const float weights[] = {1.0f, 0.8f, 0.6f, 0.4f, 0.2f};
        
        while (p && *p && count < 5) {
            while (*p && !(*p >= '0' && *p <= '9')) p++;
            if (!*p) break;
            char *endptr;
            int idx = (int)strtol(p, &endptr, 10) - 1;
            if (p != endptr) {
                if (idx >= 0 && idx < SUBJECT_VEC_DIM) {
                    out_vec[idx] = weights[count++];
                    sum_sq += out_vec[idx] * out_vec[idx];
                }
                p = endptr;
            } else p++;
        }
        if (count > 0) success = true;
    }

    llm_response_free(&resp);

    if (success) {
        float mag = sqrtf(sum_sq);
        if (mag > 1e-6) {
            for (int i = 0; i < SUBJECT_VEC_DIM; i++) out_vec[i] /= mag;
        }
        ESP_LOGI(TAG, "Classification success. Summary: %s", out_summary ? out_summary : "N/A");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Classification failed");
    return ESP_FAIL;
}

static float cosine_similarity(const float *v1, const float *v2)
{
    float dot = 0, m1 = 0, m2 = 0;
    for (int i = 0; i < SUBJECT_VEC_DIM; i++) {
        dot += v1[i] * v2[i];
        m1 += v1[i] * v1[i];
        m2 += v2[i] * v2[i];
    }
    if (m1 == 0 || m2 == 0) return 0;
    return dot / (sqrtf(m1) * sqrtf(m2));
}

/* FNV-1a 64-bit for internal use */
static uint64_t fnv1a_64(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (; *s; s++) {
        h ^= (uint64_t)(unsigned char)*s;
        h *= 0x100000001b3ULL;
    }
    return h;
}

esp_err_t subject_router_find_target(const char *chat_id, const float *msg_vec, 
                                     char *out_session_id, size_t size)
{
    if (!s_lock || !s_index || !chat_id || !msg_vec || !out_session_id) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    time_t now = time(NULL);
    float best_score = -1.0f;
    int best_idx = -1;
    uint64_t chat_h = fnv1a_64(chat_id);
    char chat_prefix[9];
    snprintf(chat_prefix, sizeof(chat_prefix), "%08llx", (unsigned long long)(chat_h & 0xFFFFFFFF));

    /* Find best match among user's sessions */
    for (int i = 0; i < s_session_count; i++) {
        if (strncmp(s_index[i].session_id, chat_prefix, 8) != 0) continue;

        float sim = cosine_similarity(msg_vec, s_index[i].vector);
        
        /* Time-based Recency Boost: Exponential Decay */
        double diff = difftime(now, s_index[i].last_updated);
        float boost = 0.0f;
        if (diff >= 0) {
            boost = SHRIMP_ROUTER_BOOST_INITIAL * expf(-(float)diff / SHRIMP_ROUTER_BOOST_TAU);
        }

        float score = sim + boost;
        ESP_LOGD(TAG, "Session %s: sim=%.2f, boost=%.2f, score=%.2f", 
                 s_index[i].session_id, sim, boost, score);

        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    /* Threshold for matching from config */
    if (best_idx != -1 && best_score > SHRIMP_ROUTER_MATCH_THRESHOLD) {
        strncpy(out_session_id, s_index[best_idx].session_id, size - 1);
        out_session_id[size - 1] = '\0';
        ESP_LOGI(TAG, "Matched to topic %s (score=%.2f): %s", 
                 out_session_id, best_score, s_index[best_idx].summary);
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    /* No match: generate a new unique session_id: <chat_h_8>_<ts> */
    snprintf(out_session_id, size, "%08llx_%08lx", (unsigned long long)(chat_h & 0xFFFFFFFF), (long)now);
    ESP_LOGI(TAG, "No match for user %s, created new topic %s", chat_id, out_session_id);
    
    /* Add to index with LRU logic */
    int target_idx = -1;
    if (s_session_count < MAX_SESSIONS) {
        target_idx = s_session_count++;
    } else {
        /* Find oldest to replace (LRU) */
        time_t oldest = now;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (s_index[i].last_updated < oldest) {
                oldest = s_index[i].last_updated;
                target_idx = i;
            }
        }
        ESP_LOGW(TAG, "Index full, replacing oldest session %s", s_index[target_idx].session_id);
    }

    if (target_idx != -1) {
        strncpy(s_index[target_idx].session_id, out_session_id, 32);
        memcpy(s_index[target_idx].vector, msg_vec, sizeof(float) * SUBJECT_VEC_DIM);
        s_index[target_idx].last_updated = now;
        
        /* Note: summary will be updated in subject_router_update_session 
           or we should pass it here. For new topics, we'll get it from classify */
        s_dirty = true;
    }

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t subject_router_update_session(const char *session_id, const float *msg_vec, const char *summary)
{
    if (!s_lock || !s_index || !session_id || !msg_vec) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (int i = 0; i < s_session_count; i++) {
        if (strcmp(s_index[i].session_id, session_id) == 0) {
            /* ALPHA-based Learning Rate Update */
            for (int j = 0; j < SUBJECT_VEC_DIM; j++) {
                s_index[i].vector[j] = s_index[i].vector[j] * (1.0f - SHRIMP_ROUTER_LEARNING_RATE) 
                                       + msg_vec[j] * SHRIMP_ROUTER_LEARNING_RATE;
            }
            if (summary && summary[0]) {
                strncpy(s_index[i].summary, summary, sizeof(s_index[i].summary) - 1);
                s_index[i].summary[sizeof(s_index[i].summary) - 1] = '\0';
            }
            s_index[i].last_updated = time(NULL);
            s_dirty = true;
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t subject_router_clear_user(const char *chat_id)
{
    if (!s_lock || !s_index || !chat_id) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    uint64_t chat_h = fnv1a_64(chat_id);
    char chat_prefix[9];
    snprintf(chat_prefix, sizeof(chat_prefix), "%08llx", (unsigned long long)(chat_h & 0xFFFFFFFF));

    int removed = 0;
    for (int i = 0; i < s_session_count; i++) {
        if (strncmp(s_index[i].session_id, chat_prefix, 8) == 0) {
            /* Delete the session file via session_mgr */
            extern esp_err_t session_clear(const char *chat_id); // Reusing the function
            session_clear(s_index[i].session_id);

            /* Shift remaining entries */
            if (i < s_session_count - 1) {
                memmove(&s_index[i], &s_index[i+1], sizeof(session_meta_t) * (s_session_count - i - 1));
            }
            s_session_count--;
            i--; // Check new entry at this index
            removed++;
        }
    }

    if (removed > 0) {
        ESP_LOGI(TAG, "Cleared %d topics for user %s", removed, chat_id);
        s_dirty = true;
    }

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t subject_router_sync_index(void)
{
    const char *tmp_path = INDEX_PATH ".tmp";
    FILE *f = fopen(tmp_path, "wb");
    if (!f) return ESP_FAIL;

    size_t n = fwrite(s_index, sizeof(session_meta_t), s_session_count, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (n != s_session_count) {
        unlink(tmp_path);
        return ESP_FAIL;
    }

    if (rename(tmp_path, INDEX_PATH) != 0) {
        unlink(tmp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}
