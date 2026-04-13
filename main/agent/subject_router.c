#include "subject_router.h"
#include "shrimp_config.h"
#include "llm/llm_proxy.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

static const char *TAG = "router";

#define INDEX_PATH SHRIMP_SPIFFS_SESSION_DIR "/sessions_idx.bin"
#define MAX_SESSIONS 100

static session_meta_t *s_index = NULL;
static int s_session_count = 0;

/* The 32 Subject Dimensions */
static const char *SUBJECT_NAMES[SUBJECT_VEC_DIM] = {
    "Daily Life", "Schedule", "Weather", "Travel", "Food", "Health", "Finance", "Shopping",
    "Programming", "Hardware", "Math", "Science", "History", "Arts", "Music", "Sports",
    "Emotions", "Language", "Search", "Files", "Automation", "News", "Education", "Business",
    "Legal", "Philosophy", "Technology", "Humor", "Social", "Security", "Profile", "Planning"
};

esp_err_t subject_router_init(void)
{
    if (s_index) return ESP_OK;

    s_index = heap_caps_calloc(MAX_SESSIONS, sizeof(session_meta_t), MALLOC_CAP_SPIRAM);
    if (!s_index) return ESP_ERR_NO_MEM;

    FILE *f = fopen(INDEX_PATH, "rb");
    if (f) {
        s_session_count = fread(s_index, sizeof(session_meta_t), MAX_SESSIONS, f);
        fclose(f);
        ESP_LOGI(TAG, "Loaded %d session vectors from index", s_session_count);
    } else {
        ESP_LOGI(TAG, "No existing session index found");
    }

    return ESP_OK;
}

esp_err_t subject_router_classify(const char *content, float *out_vec)
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

    snprintf(prompt, 4096,
        "Analyze the user message and score its relevance to the following 32 subjects. "
        "Each score must be a float between 0.0 and 1.0. "
        "Output ONLY a JSON object with a 'vector' key containing exactly 32 floats in this order:\n"
        "%s\n\n"
        "Example output: {\"vector\": [0.8, 0.1, 0.0, ...]}",
        subject_list);

    cJSON *messages = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", content);
    cJSON_AddItemToArray(messages, msg);

    llm_response_t resp;
    esp_err_t err = llm_chat_tools(prompt, messages, NULL, &resp);
    free(prompt);
    cJSON_Delete(messages);

    if (err != ESP_OK) return err;

    /* Parse vector from response text - handle potential markdown code blocks and objects */
    const char *json_ptr = resp.text;
    if (json_ptr) {
        const char *start = strchr(json_ptr, '{');
        if (!start) start = strchr(json_ptr, '[');
        if (start) json_ptr = start;
    }

    cJSON *root = cJSON_Parse(json_ptr);
    bool success = false;
    float sum_sq = 0;

    if (root) {
        cJSON *vec_arr = cJSON_IsObject(root) ? cJSON_GetObjectItem(root, "vector") : (cJSON_IsArray(root) ? root : NULL);
        if (vec_arr && cJSON_IsArray(vec_arr) && cJSON_GetArraySize(vec_arr) == SUBJECT_VEC_DIM) {
            for (int i = 0; i < SUBJECT_VEC_DIM; i++) {
                cJSON *item = cJSON_GetArrayItem(vec_arr, i);
                float val = (float)(item ? item->valuedouble : 0.0);
                if (val < 0) {
                    val = 0;
                }
                if (val > 1) {
                    val = 1;
                }
                out_vec[i] = val;
                sum_sq += val * val;
            }
            success = true;
        }
        cJSON_Delete(root);
    }

    /* Fallback: Lazy Parsing (方案 C) - scan text for floats if JSON parsing failed */
    if (!success) {
        ESP_LOGW(TAG, "JSON parse failed, attempting heuristic number scanning...");
        int count = 0;
        const char *p = (json_ptr ? json_ptr : resp.text);
        while (*p && count < SUBJECT_VEC_DIM) {
            /* Find start of a number: digit, dot, or minus */
            while (*p && !((*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) p++;
            if (!*p) break;

            char *endptr;
            float val = strtof(p, &endptr);
            if (p != endptr) {
                /* Basic sanity check: scores are usually 0-1, 
                   ignore numbers like '32' in '32 dimensions' */
                if (val >= 0.0f && val <= 1.5f) {
                    out_vec[count++] = (val > 1.0f) ? 1.0f : val;
                    sum_sq += out_vec[count-1] * out_vec[count-1];
                }
                p = endptr;
            } else {
                p++;
            }
        }
        if (count == SUBJECT_VEC_DIM) {
            ESP_LOGI(TAG, "Heuristic scan matched %d dimensions", count);
            success = true;
        }
    }

    llm_response_free(&resp);

    if (success) {
        /* Normalization to unit length for stable cosine similarity */
        float mag = sqrtf(sum_sq);
        if (mag > 1e-6) {
            for (int i = 0; i < SUBJECT_VEC_DIM; i++) {
                out_vec[i] /= mag;
            }
        }
        ESP_LOGI(TAG, "Successfully generated stabilized 32-dim vector");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to extract 32-dim vector via all methods");
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
    if (!s_index || !chat_id || !msg_vec || !out_session_id) return ESP_ERR_INVALID_ARG;

    time_t now = time(NULL);
    float best_score = -1.0f;
    int best_idx = -1;
    uint64_t chat_h = fnv1a_64(chat_id);
    char chat_prefix[9];
    snprintf(chat_prefix, sizeof(chat_prefix), "%08llx", (unsigned long long)(chat_h & 0xFFFFFFFF));

    /* Find best match among user's sessions */
    for (int i = 0; i < s_session_count; i++) {
        /* Only check sessions belonging to this user (prefix match) */
        if (strncmp(s_index[i].session_id, chat_prefix, 8) != 0) continue;

        float sim = cosine_similarity(msg_vec, s_index[i].vector);
        
        /* Time-based Recency Boost: +0.4 if < 5 mins ago */
        float boost = 0.0f;
        double diff = difftime(now, s_index[i].last_updated);
        if (diff >= 0 && diff < 300) {
            boost = 0.4f;
        }

        float score = sim + boost;
        ESP_LOGD(TAG, "Session %s: sim=%.2f, boost=%.2f, score=%.2f", 
                 s_index[i].session_id, sim, boost, score);

        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    /* Threshold for matching: 0.6 */
    if (best_idx != -1 && best_score > 0.6f) {
        strncpy(out_session_id, s_index[best_idx].session_id, size - 1);
        out_session_id[size - 1] = '\0';
        ESP_LOGI(TAG, "Matched to user %s session %s (score=%.2f)", chat_id, out_session_id, best_score);
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
        subject_router_sync_index();
    }

    return ESP_OK;
}

esp_err_t subject_router_update_session(const char *session_id, const float *msg_vec)
{
    if (!s_index || !session_id || !msg_vec) return ESP_ERR_INVALID_ARG;

    for (int i = 0; i < s_session_count; i++) {
        if (strcmp(s_index[i].session_id, session_id) == 0) {
            /* Average Update */
            for (int j = 0; j < SUBJECT_VEC_DIM; j++) {
                s_index[i].vector[j] = (s_index[i].vector[j] + msg_vec[j]) / 2.0f;
            }
            s_index[i].last_updated = time(NULL);
            return subject_router_sync_index();
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t subject_router_clear_user(const char *chat_id)
{
    if (!s_index || !chat_id) return ESP_ERR_INVALID_ARG;

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
        return subject_router_sync_index();
    }

    return ESP_OK;
}

esp_err_t subject_router_sync_index(void)
{
    FILE *f = fopen(INDEX_PATH, "wb");
    if (!f) return ESP_FAIL;

    size_t n = fwrite(s_index, sizeof(session_meta_t), s_session_count, f);
    fclose(f);

    if (n != s_session_count) return ESP_FAIL;
    return ESP_OK;
}
