#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <time.h>

#define SUBJECT_VEC_DIM 32

typedef struct {
    char session_id[33]; // Based on fnv1a_64 hex (16 chars) + "s_" + ".jsonl"
    float vector[SUBJECT_VEC_DIM];
    time_t last_updated;
} session_meta_t;

/**
 * Initialize subject router. Loads existing session index if available.
 */
esp_err_t subject_router_init(void);

/**
 * Classify a user message into a 32-dimensional semantic vector.
 * Uses the main LLM with a specialized prompt.
 */
esp_err_t subject_router_classify(const char *content, float *out_vec);

/**
 * Find the most relevant session for a new message.
 * @param chat_id   Target user/chat ID
 * @param msg_vec   Classified vector of the incoming message
 * @param out_session_id Buffer to store the resulting session ID
 * @param size      Size of out_session_id buffer
 * @return ESP_OK on success
 */
esp_err_t subject_router_find_target(const char *chat_id, const float *msg_vec, 
                                     char *out_session_id, size_t size);

/**
 * Update the vector of an existing session (Average Update).
 */
esp_err_t subject_router_update_session(const char *session_id, const float *msg_vec);

/**
 * Clear all topics and session files associated with a specific user.
 */
esp_err_t subject_router_clear_user(const char *chat_id);

/**
 * Sync the memory index to SPIFFS.
 */
esp_err_t subject_router_sync_index(void);
