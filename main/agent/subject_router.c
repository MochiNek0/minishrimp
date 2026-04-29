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
#define MAX_ACTIVE_TOPICS 16

typedef struct {
    char chat_id[64];
    char session_id[33];
    time_t last_active;
    bool in_use;
} active_topic_t;

static session_meta_t *s_index = NULL;
static int s_session_count = 0;
static active_topic_t s_active_topics[MAX_ACTIVE_TOPICS];
static SemaphoreHandle_t s_lock = NULL;
static bool s_dirty = false;
static bool s_task_running = false;

/* The 64 Subject Dimensions */
static const char *SUBJECT_NAMES[SUBJECT_VEC_DIM] = {
"Daily Routines", "Household Management", "Parenting & Family", "Elderly Care", "Personal Finance", "Real Estate", "Culinary Arts", "Nutrition & Diet",
  "Physical Medicine", "Mental Health", "Pharmacology", "Fitness & Bodybuilding", "Competitive Sports", "Outdoor Adventures", "Pets & Zoology", "Gardening & Botany",
  "Theoretical Mathematics", "Physics & Chemistry", "Biology & Genetics", "Earth Sciences", "Space & Astronomy", "Ecology & Environment", "Micro-Electronics", "Mechanical Engineering",
  "Software Engineering", "Artificial Intelligence", "Cybersecurity", "Data Science", "Consumer Electronics", "Internet of Things", "Automotive Tech", "Aerospace Tech",
  "Macroeconomics", "Financial Markets", "Corporate Management", "Marketing & Branding", "E-commerce & Retail", "Logistics & Supply Chain", "Career Planning", "Industrial Design",
  "World History", "Archeology", "Political Science", "Jurisprudence & Law", "Sociology", "Cultural Anthropology", "Linguistics", "Philosophy & Logic",
  "Classic Literature", "Creative Writing", "Journalism & News", "Cinematography", "Music Theory & Audio", "Visual Arts & Painting", "Photography & Video", "Performing Arts",
  "Fashion & Textile", "Architecture & Interior", "Travel & Tourism", "Navigation & Cartography", "Video Games & Esports", "Handicrafts & DIY", "Board Games & Puzzles", "Mythology & Folklore"
};

static void subject_router_sync_task(void *arg)
{
    s_task_running = true;
    while (s_task_running) {
        vTaskDelay(pdMS_TO_TICKS(10000)); // Check every 10 seconds
        if (s_dirty) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            esp_err_t err = subject_router_sync_index();
            if (err == ESP_OK) {
                s_dirty = false;
                ESP_LOGI(TAG, "Background sync completed");
            } else {
                ESP_LOGE(TAG, "Background sync failed: %s", esp_err_to_name(err));
            }
            xSemaphoreGive(s_lock);
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
        ESP_LOGE(TAG, "Failed to allocate router index in PSRAM");
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(INDEX_PATH, "rb");
    if (f) {
        router_index_header_t head;
        if (fread(&head, sizeof(head), 1, f) == 1) {
            if (head.magick == SUBJECT_ROUTER_MAGICK && head.version == SUBJECT_ROUTER_VERSION) {
                s_session_count = fread(s_index, sizeof(session_meta_t), MAX_SESSIONS, f);
                ESP_LOGI(TAG, "Loaded %d session vectors from index", s_session_count);
            } else {
                ESP_LOGW(TAG, "Index Magick/Version mismatch (0x%08x vs 0x%08x), skipping load", 
                         (unsigned int)head.magick, (unsigned int)SUBJECT_ROUTER_MAGICK);
                s_session_count = 0;
            }
        } else {
            ESP_LOGW(TAG, "Index header read failed, skipping load");
            s_session_count = 0;
        }
        fclose(f);
    } else {
        ESP_LOGI(TAG, "No existing session index found");
    }

    /* Increased stack from 3072 to 4096 to prevent overflow during file I/O */
    xTaskCreate(subject_router_sync_task, "router_sync", 4096, NULL, 3, NULL);
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

    /* Get current time for temporal context */
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S (%A)", &tm_info);

    snprintf(prompt, 4096,
        "Current Time: %s\n\n"
        "Classify the user message by picking the EXACTLY 5 most relevant subject indices (1-64) in descending order of relevance. "
        "Also provide a 10-word summary. "
        "IMPORTANT: You MUST return your response as a JSON object: {\"indices\": [rank1, rank2, rank3, rank4, rank5], \"summary\": \"...\"}\n"
        "Subjects:\n%s",
        time_str, subject_list);

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

static bool contains_any(const char *s, const char *const *needles, size_t count)
{
    if (!s) return false;
    for (size_t i = 0; i < count; i++) {
        if (strstr(s, needles[i])) {
            return true;
        }
    }
    return false;
}

static bool message_is_context_dependent(const char *content)
{
    static const char *const patterns[] = {
        "这个", "那个", "这些", "那些", "它", "他们", "她们", "它们",
        "刚才", "上面", "前面", "之前", "这里", "这条", "这句话",
        "这些信息", "这个信息", "这个问题", "那个问题",
        "调用了什么工具", "用了什么工具", "什么工具", "怎么拿到", "怎么获取",
        "this", "that", "these", "those", "it", "they", "above", "previous",
        "earlier", "same", "which tool", "what tool"
    };
    return contains_any(content, patterns, sizeof(patterns) / sizeof(patterns[0]));
}

static bool message_is_explicit_topic_shift(const char *content)
{
    static const char *const patterns[] = {
        "换个话题", "换一个话题", "另一个话题", "新话题", "新问题",
        "说完了", "先不说", "先不聊", "不聊这个", "回到正题",
        "另外一个", "还有一个问题", "再问一个",
        "new topic", "different topic", "another topic", "new question",
        "separate question", "forget that", "ignore that"
    };
    return contains_any(content, patterns, sizeof(patterns) / sizeof(patterns[0]));
}

static int find_session_index_locked(const char *session_id)
{
    if (!session_id) return -1;
    for (int i = 0; i < s_session_count; i++) {
        if (strcmp(s_index[i].session_id, session_id) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_active_topic_locked(const char *chat_id)
{
    if (!chat_id) return -1;
    for (int i = 0; i < MAX_ACTIVE_TOPICS; i++) {
        if (s_active_topics[i].in_use && strcmp(s_active_topics[i].chat_id, chat_id) == 0) {
            return i;
        }
    }
    return -1;
}

static void set_active_topic_locked(const char *chat_id, const char *session_id, time_t now)
{
    if (!chat_id || !session_id) return;

    int idx = find_active_topic_locked(chat_id);
    if (idx < 0) {
        time_t oldest = now;
        idx = 0;
        for (int i = 0; i < MAX_ACTIVE_TOPICS; i++) {
            if (!s_active_topics[i].in_use) {
                idx = i;
                break;
            }
            if (s_active_topics[i].last_active <= oldest) {
                oldest = s_active_topics[i].last_active;
                idx = i;
            }
        }
    }

    strncpy(s_active_topics[idx].chat_id, chat_id, sizeof(s_active_topics[idx].chat_id) - 1);
    s_active_topics[idx].chat_id[sizeof(s_active_topics[idx].chat_id) - 1] = '\0';
    strncpy(s_active_topics[idx].session_id, session_id, sizeof(s_active_topics[idx].session_id) - 1);
    s_active_topics[idx].session_id[sizeof(s_active_topics[idx].session_id) - 1] = '\0';
    s_active_topics[idx].last_active = now;
    s_active_topics[idx].in_use = true;
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
    return subject_router_find_target_for_message(chat_id, NULL, msg_vec, out_session_id, size, NULL);
}

esp_err_t subject_router_find_target_for_message(const char *chat_id, const char *content,
                                                 const float *msg_vec,
                                                 char *out_session_id, size_t size,
                                                 subject_route_info_t *out_info)
{
    if (!s_lock || !s_index || !chat_id || !msg_vec || !out_session_id) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    time_t now = time(NULL);
    float best_score = -1.0f;
    float active_similarity = 0.0f;
    float active_score = 0.0f;
    int best_idx = -1;
    int active_session_idx = -1;
    bool context_dependent = message_is_context_dependent(content);
    bool explicit_shift = message_is_explicit_topic_shift(content);
    uint64_t chat_h = fnv1a_64(chat_id);
    char chat_prefix[9];
    snprintf(chat_prefix, sizeof(chat_prefix), "%08llx", (unsigned long long)(chat_h & 0xFFFFFFFF));

    int active_idx = find_active_topic_locked(chat_id);
    bool active_is_recent = false;
    if (active_idx >= 0) {
        active_session_idx = find_session_index_locked(s_active_topics[active_idx].session_id);
        if (active_session_idx >= 0) {
            double active_age = difftime(now, s_active_topics[active_idx].last_active);
            active_is_recent = (active_age >= 0 && active_age <= SHRIMP_ROUTER_ACTIVE_TTL_SEC);
            active_similarity = cosine_similarity(msg_vec, s_index[active_session_idx].vector);
            if (active_is_recent) {
                active_score = active_similarity +
                    SHRIMP_ROUTER_BOOST_INITIAL * expf(-(float)active_age / SHRIMP_ROUTER_BOOST_TAU);
            }
        }
    }

    /* Find best match among user's sessions */
    for (int i = 0; i < s_session_count; i++) {
        if (strncmp(s_index[i].session_id, chat_prefix, 8) != 0) continue;
        if (explicit_shift && i == active_session_idx) continue;

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

    subject_route_kind_t route_kind = SUBJECT_ROUTE_NEW;

    if (!explicit_shift && active_is_recent && active_session_idx >= 0) {
        bool active_is_best = (best_idx == active_session_idx);
        bool best_barely_better = (best_idx >= 0 && (best_score - active_score) < SHRIMP_ROUTER_SHIFT_MARGIN);

        if (context_dependent) {
            strncpy(out_session_id, s_index[active_session_idx].session_id, size - 1);
            out_session_id[size - 1] = '\0';
            set_active_topic_locked(chat_id, out_session_id, now);
            route_kind = SUBJECT_ROUTE_ACTIVE_FOLLOWUP;
            ESP_LOGI(TAG, "Routed follow-up to active topic %s (active_sim=%.2f, best=%.2f)",
                     out_session_id, active_similarity, best_score);
            goto done;
        }

        if (active_similarity >= SHRIMP_ROUTER_CONTINUE_THRESHOLD &&
            (active_is_best || best_barely_better)) {
            strncpy(out_session_id, s_index[active_session_idx].session_id, size - 1);
            out_session_id[size - 1] = '\0';
            set_active_topic_locked(chat_id, out_session_id, now);
            route_kind = SUBJECT_ROUTE_ACTIVE_CONTINUATION;
            ESP_LOGI(TAG, "Continued active topic %s (active_sim=%.2f, best=%.2f)",
                     out_session_id, active_similarity, best_score);
            goto done;
        }
    }

    /* Threshold for matching from config */
    if (best_idx != -1 && best_score > SHRIMP_ROUTER_MATCH_THRESHOLD) {
        strncpy(out_session_id, s_index[best_idx].session_id, size - 1);
        out_session_id[size - 1] = '\0';
        set_active_topic_locked(chat_id, out_session_id, now);
        route_kind = SUBJECT_ROUTE_MATCHED;
        ESP_LOGI(TAG, "Matched to topic %s (score=%.2f): %s", 
                 out_session_id, best_score, s_index[best_idx].summary);
        goto done;
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
    set_active_topic_locked(chat_id, out_session_id, now);

done:
    if (out_info) {
        out_info->kind = route_kind;
        out_info->best_score = best_score;
        out_info->active_similarity = active_similarity;
        out_info->context_dependent = context_dependent;
        out_info->explicit_shift = explicit_shift;
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
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for writing", tmp_path);
        return ESP_FAIL;
    }

    router_index_header_t head = {
        .magick = SUBJECT_ROUTER_MAGICK,
        .version = SUBJECT_ROUTER_VERSION,
        .count = s_session_count
    };

    size_t written = 0;
    written += fwrite(&head, sizeof(head), 1, f);
    written += fwrite(s_index, sizeof(session_meta_t), s_session_count, f);

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (written != (s_session_count + 1)) {
        ESP_LOGE(TAG, "Index write incomplete: %d/%d objects", (int)written, s_session_count + 1);
        unlink(tmp_path);
        return ESP_FAIL;
    }

    unlink(INDEX_PATH);
    if (rename(tmp_path, INDEX_PATH) != 0) {
        ESP_LOGE(TAG, "Failed to update index file");
        unlink(tmp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}
