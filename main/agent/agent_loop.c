#include "agent_loop.h"
#include "agent/context_builder.h"
#include "shrimp_config.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "memory/session_mgr.h"
#include "agent/subject_router.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "agent";

#define TOOL_OUTPUT_SIZE  (8 * 1024)

/* Build the assistant content array from llm_response_t for the messages history.
 * Returns a cJSON array with text and tool_use blocks. */
static cJSON *build_assistant_content(const llm_response_t *resp)
{
    cJSON *content = cJSON_CreateArray();

    /* Text block */
    if (resp->text && resp->text_len > 0) {
        cJSON *text_block = cJSON_CreateObject();
        cJSON_AddStringToObject(text_block, "type", "text");
        cJSON_AddStringToObject(text_block, "text", resp->text);
        cJSON_AddItemToArray(content, text_block);
    }

    /* Tool use blocks */
    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        cJSON *tool_block = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_block, "type", "tool_use");
        cJSON_AddStringToObject(tool_block, "id", call->id);
        cJSON_AddStringToObject(tool_block, "name", call->name);

        cJSON *input = cJSON_Parse(call->input);
        if (input) {
            cJSON_AddItemToObject(tool_block, "input", input);
        } else {
            cJSON_AddItemToObject(tool_block, "input", cJSON_CreateObject());
        }

        cJSON_AddItemToArray(content, tool_block);
    }

    return content;
}

static void json_set_string(cJSON *obj, const char *key, const char *value)
{
    if (!obj || !key || !value) {
        return;
    }
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddStringToObject(obj, key, value);
}

static void append_turn_context_prompt(char *prompt, size_t size, const shrimp_msg_t *msg, const char *topic_id)
{
    if (!prompt || size == 0 || !msg) {
        return;
    }

    size_t off = strnlen(prompt, size - 1);
    if (off >= size - 1) {
        return;
    }

    int n = snprintf(
        prompt + off, size - off,
        "\n## Current Turn Context\n"
        "- source_channel: %s\n"
        "- source_chat_id: %s\n"
        "- active_topic_id: %s\n"
        "- If using cron_add in this turn, set channel to source_channel and chat_id to source_chat_id.\n",
        msg->channel[0] ? msg->channel : "(unknown)",
        msg->chat_id[0] ? msg->chat_id : "(empty)",
        topic_id ? topic_id : "(none)");

    if (n < 0 || (size_t)n >= (size - off)) {
        prompt[size - 1] = '\0';
    }
}

static char *patch_tool_input_with_context(const llm_tool_call_t *call, const shrimp_msg_t *msg)
{
    if (!call || !msg || strcmp(call->name, "cron_add") != 0) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(call->input ? call->input : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (!root) {
        return NULL;
    }

    bool changed = false;

    cJSON *channel_item = cJSON_GetObjectItem(root, "channel");
    const char *channel = cJSON_IsString(channel_item) ? channel_item->valuestring : NULL;

    if ((!channel || channel[0] == '\0') && msg->channel[0] != '\0') {
        json_set_string(root, "channel", msg->channel);
        channel = msg->channel;
        changed = true;
    }

    if (channel && strcmp(channel, msg->channel) == 0 && msg->chat_id[0] != '\0') {
        cJSON *chat_item = cJSON_GetObjectItem(root, "chat_id");
        const char *chat_id = cJSON_IsString(chat_item) ? chat_item->valuestring : NULL;
        if (!chat_id || chat_id[0] == '\0' || strcmp(chat_id, "cron") == 0) {
            json_set_string(root, "chat_id", msg->chat_id);
            changed = true;
        }
    }

    char *patched = NULL;
    if (changed) {
        patched = cJSON_PrintUnformatted(root);
        if (patched) {
            ESP_LOGI(TAG, "Patched cron_add target to %s:%s", msg->channel, msg->chat_id);
        }
    }

    cJSON_Delete(root);
    return patched;
}

/* Build the user message with tool_result blocks */
static cJSON *build_tool_results(const llm_response_t *resp, const shrimp_msg_t *msg,
                                 char *tool_output, size_t tool_output_size)
{
    cJSON *content = cJSON_CreateArray();

    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        const char *tool_input = call->input ? call->input : "{}";
        char *patched_input = patch_tool_input_with_context(call, msg);
        if (patched_input) {
            tool_input = patched_input;
        }

        /* Execute tool */
        char *raw_output = heap_caps_calloc(1, tool_output_size, MALLOC_CAP_SPIRAM);
        if (raw_output) {
            tool_registry_execute(call->name, tool_input, raw_output, tool_output_size);
            filter_valid_utf8(raw_output, tool_output, tool_output_size);
            free(raw_output);
        } else {
            tool_registry_execute(call->name, tool_input, tool_output, tool_output_size);
            /* In-place filter if no PSRAM available */
            char *tmp = strdup(tool_output);
            if (tmp) {
                filter_valid_utf8(tmp, tool_output, tool_output_size);
                free(tmp);
            }
        }
        free(patched_input);

        ESP_LOGI(TAG, "Tool %s result: %d bytes (sanitized)", call->name, (int)strlen(tool_output));

        /* Build tool_result block */
        cJSON *result_block = cJSON_CreateObject();
        cJSON_AddStringToObject(result_block, "type", "tool_result");
        cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
        cJSON_AddStringToObject(result_block, "content", tool_output);
        cJSON_AddItemToArray(content, result_block);
    }

    return content;
}

static void agent_loop_task(void *arg)
{
    ESP_LOGI(TAG, "Agent loop started on core %d", xPortGetCoreID());

    /* Allocate large buffers from PSRAM */
    char *system_prompt = heap_caps_calloc(1, SHRIMP_CONTEXT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *history_json = heap_caps_calloc(1, SHRIMP_LLM_STREAM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *tool_output = heap_caps_calloc(1, TOOL_OUTPUT_SIZE, MALLOC_CAP_SPIRAM);

    if (!system_prompt || !history_json || !tool_output) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
        vTaskDelete(NULL);
        return;
    }

    const char *tools_json = tool_registry_get_tools_json();

    while (1) {
        shrimp_msg_t msg;
        esp_err_t err = message_bus_pop_inbound(&msg, UINT32_MAX);
        if (err != ESP_OK) continue;

        /* ── Debounce: wait for more messages from the same chat_id ── */
#if SHRIMP_AGENT_DEBOUNCE_MS > 0
        {
            shrimp_msg_t extra;
            while (message_bus_pop_inbound(&extra, SHRIMP_AGENT_DEBOUNCE_MS) == ESP_OK) {
                if (strcmp(extra.chat_id, msg.chat_id) == 0) {
                    /* Same user — append content with newline */
                    size_t old_len = strlen(msg.content);
                    size_t add_len = strlen(extra.content);
                    char *old_content = msg.content;
                    char *merged = realloc(old_content, old_len + 1 + add_len + 1);
                    if (merged) {
                        merged[old_len] = '\n';
                        memcpy(merged + old_len + 1, extra.content, add_len + 1);
                        msg.content = merged;
                    }
                    /* If extra also has an image and msg doesn't, take it */
                    if (extra.image_url && !msg.image_url) {
                        msg.image_url = extra.image_url;
                        extra.image_url = NULL;
                    }
                    free(extra.content);
                    free(extra.image_url);
                    ESP_LOGI(TAG, "Debounce: merged message from %s:%s", extra.channel, extra.chat_id);
                } else {
                    /* Different user — push back and stop debouncing */
                    message_bus_push_inbound(&extra);
                    break;
                }
            }
        }
#endif

        ESP_LOGI(TAG, "Processing message from %s:%s", msg.channel, msg.chat_id);

        /* 1. Build base system prompt */
        context_build_system_prompt(system_prompt, SHRIMP_CONTEXT_BUF_SIZE);

        /* 2. Semantic Routing */
        float msg_vec[SUBJECT_VEC_DIM] = {0};
        char topic_id[128];
        char summary[64] = {0};
        if (subject_router_classify(msg.content, msg_vec, summary, sizeof(summary)) == ESP_OK) {
            subject_router_find_target(msg.chat_id, msg_vec, topic_id, sizeof(topic_id));
        } else {
            /* Fallback to user-specific general session if classification fails */
            snprintf(topic_id, sizeof(topic_id), "%s_general", msg.chat_id);
            ESP_LOGW(TAG, "Classification failed, fallback to general session: %s", topic_id);
        }

        /* 3. Build turn-specific context and inject topic info */
        append_turn_context_prompt(system_prompt, SHRIMP_CONTEXT_BUF_SIZE, &msg, topic_id);
        ESP_LOGI(TAG, "LLM turn context: topic=%s", topic_id);

        /* 4. Load session history into cJSON array */
        session_get_history_json(topic_id, history_json,
                                 SHRIMP_LLM_STREAM_BUF_SIZE, SHRIMP_AGENT_MAX_HISTORY);

        cJSON *messages = cJSON_Parse(history_json);
        if (!messages) messages = cJSON_CreateArray();

        /* 3. Append current user message (with optional image) */
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");

        if (msg.image_url && msg.image_url[0]) {
            /* Multimodal: build content array with image + text */
            cJSON *content_arr = cJSON_CreateArray();

            /* Image block */
            cJSON *img_block = cJSON_CreateObject();
            cJSON_AddStringToObject(img_block, "type", "image");
            cJSON *source = cJSON_CreateObject();
            if (strncmp(msg.image_url, "data:", 5) == 0) {
                /* data URI: extract media type and base64 data */
                cJSON_AddStringToObject(source, "type", "base64");
                /* Parse: data:<media_type>;base64,<data> */
                const char *semi = strchr(msg.image_url + 5, ';');
                if (semi) {
                    size_t mt_len = semi - (msg.image_url + 5);
                    char *media_type = malloc(mt_len + 1);
                    if (media_type) {
                        memcpy(media_type, msg.image_url + 5, mt_len);
                        media_type[mt_len] = '\0';
                        cJSON_AddStringToObject(source, "media_type", media_type);
                        free(media_type);
                    }
                    const char *comma = strchr(semi, ',');
                    if (comma) {
                        cJSON_AddStringToObject(source, "data", comma + 1);
                    }
                }
            } else {
                /* HTTP URL */
                cJSON_AddStringToObject(source, "type", "url");
                cJSON_AddStringToObject(source, "url", msg.image_url);
            }
            cJSON_AddItemToObject(img_block, "source", source);
            cJSON_AddItemToArray(content_arr, img_block);

            /* Text block */
            cJSON *text_block = cJSON_CreateObject();
            cJSON_AddStringToObject(text_block, "type", "text");
            cJSON_AddStringToObject(text_block, "text", msg.content);
            cJSON_AddItemToArray(content_arr, text_block);

            cJSON_AddItemToObject(user_msg, "content", content_arr);

            /* Free image_url now — it's been copied into the JSON */
            free(msg.image_url);
            msg.image_url = NULL;

            ESP_LOGI(TAG, "Built multimodal user message with image");
        } else {
            cJSON_AddStringToObject(user_msg, "content", msg.content);
        }
        cJSON_AddItemToArray(messages, user_msg);

        /* 4. ReAct loop */
        char *final_text = NULL;
        int iteration = 0;
        bool sent_working_status = false;
        char error_reason[128] = {0};

        while (iteration < SHRIMP_AGENT_MAX_TOOL_ITER) {
            /* Send "working" indicator before each API call */
#if SHRIMP_AGENT_SEND_WORKING_STATUS
            if (!sent_working_status && strcmp(msg.channel, SHRIMP_CHAN_SYSTEM) != 0) {
                shrimp_msg_t status = {0};
                strncpy(status.channel, msg.channel, sizeof(status.channel) - 1);
                strncpy(status.chat_id, msg.chat_id, sizeof(status.chat_id) - 1);
                status.content = strdup("小虾米正在处理...");
                if (status.content) {
                    if (message_bus_push_outbound(&status) != ESP_OK) {
                        ESP_LOGW(TAG, "Outbound queue full, drop working status");
                        free(status.content);
                    } else {
                        sent_working_status = true;
                    }
                }
            }
#endif

            llm_response_t resp;
            err = llm_chat_tools(system_prompt, messages, tools_json, &resp);

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "LLM call failed: %s", esp_err_to_name(err));
                snprintf(error_reason, sizeof(error_reason), "LLM 调用失败 (%s)", esp_err_to_name(err));
                break;
            }

            if (!resp.tool_use) {
                /* Normal completion — save final text and break */
                if (resp.text && resp.text_len > 0) {
                    final_text = strdup(resp.text);
                    if (!final_text) {
                        ESP_LOGE(TAG, "Failed to strdup final text (%d bytes)", (int)resp.text_len);
                        snprintf(error_reason, sizeof(error_reason), "内存分配失败");
                    }
                } else {
                    snprintf(error_reason, sizeof(error_reason), "未返回有效内容");
                }
                llm_response_free(&resp);
                break;
            }

            ESP_LOGI(TAG, "Tool use iteration %d: %d calls", iteration + 1, resp.call_count);

            /* Append assistant message with content array */
            cJSON *asst_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(asst_msg, "role", "assistant");
            cJSON_AddItemToObject(asst_msg, "content", build_assistant_content(&resp));
            cJSON_AddItemToArray(messages, asst_msg);

            /* Execute tools and append results */
            cJSON *tool_results = build_tool_results(&resp, &msg, tool_output, TOOL_OUTPUT_SIZE);
            cJSON *result_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(result_msg, "role", "user");
            cJSON_AddItemToObject(result_msg, "content", tool_results);
            cJSON_AddItemToArray(messages, result_msg);

            llm_response_free(&resp);
            iteration++;
        }
        
        if (iteration >= SHRIMP_AGENT_MAX_TOOL_ITER && !error_reason[0]) {
            snprintf(error_reason, sizeof(error_reason), "工具调用次数达到上限");
        }

        cJSON_Delete(messages);

        /* 5. Send response */
        if (final_text && final_text[0]) {
            /* Save to session (only user text + final assistant text) */
            esp_err_t save_user = session_append(topic_id, "user", msg.content);
            esp_err_t save_asst = session_append(topic_id, "assistant", final_text);
            
            /* Update topic vector and summary */
            subject_router_update_session(topic_id, msg_vec, summary);

            if (save_user != ESP_OK || save_asst != ESP_OK) {
                ESP_LOGW(TAG, "Session save failed for topic %s", topic_id);
            } else {
                ESP_LOGI(TAG, "Session saved for topic %s", topic_id);
            }

            /* Push response to outbound */
            shrimp_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = final_text;  /* transfer ownership */
            ESP_LOGI(TAG, "Queue final response to %s:%s (%d bytes)",
                     out.channel, out.chat_id, (int)strlen(final_text));
            if (message_bus_push_outbound(&out) != ESP_OK) {
                ESP_LOGW(TAG, "Outbound queue full, drop final response");
                free(final_text);
            } else {
                final_text = NULL;
            }
        } else {
            /* Error or empty response */
            free(final_text);
            shrimp_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), "抱歉，处理出错了：%s", 
                     error_reason[0] ? error_reason : "未知错误");
            out.content = strdup(err_msg);
            
            if (out.content) {
                if (message_bus_push_outbound(&out) != ESP_OK) {
                    ESP_LOGW(TAG, "Outbound queue full, drop error response");
                    free(out.content);
                }
            }
        }

        /* Free inbound message content */
        free(msg.content);
        free(msg.image_url);

        /* Log memory status */
        ESP_LOGI(TAG, "Free PSRAM: %d bytes",
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}

esp_err_t agent_loop_init(void)
{
    subject_router_init();
    ESP_LOGI(TAG, "Agent loop initialized");
    return ESP_OK;
}

esp_err_t agent_loop_start(void)
{
    const uint32_t stack_candidates[] = {
        SHRIMP_AGENT_STACK,
        20 * 1024,
        16 * 1024,
        14 * 1024,
        12 * 1024,
    };

    for (size_t i = 0; i < (sizeof(stack_candidates) / sizeof(stack_candidates[0])); i++) {
        uint32_t stack_size = stack_candidates[i];
        BaseType_t ret = xTaskCreatePinnedToCore(
            agent_loop_task, "agent_loop",
            stack_size, NULL,
            SHRIMP_AGENT_PRIO, NULL, SHRIMP_AGENT_CORE);

        if (ret == pdPASS) {
            ESP_LOGI(TAG, "agent_loop task created with stack=%u bytes", (unsigned)stack_size);
            return ESP_OK;
        }

        ESP_LOGW(TAG,
                 "agent_loop create failed (stack=%u, free_internal=%u, largest_internal=%u), retrying...",
                 (unsigned)stack_size,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    return ESP_FAIL;
}
