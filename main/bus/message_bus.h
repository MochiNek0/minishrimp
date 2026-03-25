#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Channel identifiers */
#define SHRIMP_CHAN_TELEGRAM   "telegram"
#define SHRIMP_CHAN_FEISHU     "feishu"
#define SHRIMP_CHAN_WEBSOCKET  "websocket"
#define SHRIMP_CHAN_CLI        "cli"
#define SHRIMP_CHAN_SYSTEM     "system"

/* Message types on the bus */
typedef struct {
    char channel[16];       /* "telegram", "websocket", "cli" */
    char chat_id[96];       /* Telegram/Feishu chat_id, open_id, or WS client id */
    char *content;          /* Heap-allocated message text (caller must free) */
    char *image_url;        /* Heap-allocated image URL or data URI (optional, caller must free) */
} shrimp_msg_t;

/**
 * Initialize the message bus (inbound + outbound FreeRTOS queues).
 */
esp_err_t message_bus_init(void);

/**
 * Push a message to the inbound queue (towards Agent Loop).
 * The bus takes ownership of msg->content.
 */
esp_err_t message_bus_push_inbound(const shrimp_msg_t *msg);

/**
 * Pop a message from the inbound queue (blocking).
 * Caller must free msg->content when done.
 */
esp_err_t message_bus_pop_inbound(shrimp_msg_t *msg, uint32_t timeout_ms);

/**
 * Push a message to the outbound queue (towards channels).
 * The bus takes ownership of msg->content.
 */
esp_err_t message_bus_push_outbound(const shrimp_msg_t *msg);

/**
 * Pop a message from the outbound queue (blocking).
 * Caller must free msg->content when done.
 */
esp_err_t message_bus_pop_outbound(shrimp_msg_t *msg, uint32_t timeout_ms);
