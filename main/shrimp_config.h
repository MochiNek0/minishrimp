#pragma once

/* MiniShrimp Global Configuration */

/* Build-time secrets (highest priority, override NVS) */
#if __has_include("shrimp_secrets.h")
#include "shrimp_secrets.h"
#endif

#ifndef SHRIMP_SECRET_WIFI_SSID
#define SHRIMP_SECRET_WIFI_SSID       ""
#endif
#ifndef SHRIMP_SECRET_WIFI_PASS
#define SHRIMP_SECRET_WIFI_PASS       ""
#endif
#ifndef SHRIMP_SECRET_TG_TOKEN
#define SHRIMP_SECRET_TG_TOKEN        ""
#endif
#ifndef SHRIMP_SECRET_API_KEY
#define SHRIMP_SECRET_API_KEY         ""
#endif
#ifndef SHRIMP_SECRET_MODEL
#define SHRIMP_SECRET_MODEL           ""
#endif
#ifndef SHRIMP_SECRET_MODEL_PROVIDER
#define SHRIMP_SECRET_MODEL_PROVIDER  "custom"
#endif
#ifndef SHRIMP_SECRET_PROXY_HOST
#define SHRIMP_SECRET_PROXY_HOST      ""
#endif
#ifndef SHRIMP_SECRET_PROXY_PORT
#define SHRIMP_SECRET_PROXY_PORT      ""
#endif
#ifndef SHRIMP_SECRET_PROXY_TYPE
#define SHRIMP_SECRET_PROXY_TYPE      ""
#endif
#ifndef SHRIMP_SECRET_SEARCH_KEY
#define SHRIMP_SECRET_SEARCH_KEY      ""
#endif
#ifndef SHRIMP_SECRET_FEISHU_APP_ID
#define SHRIMP_SECRET_FEISHU_APP_ID   ""
#endif
#ifndef SHRIMP_SECRET_FEISHU_APP_SECRET
#define SHRIMP_SECRET_FEISHU_APP_SECRET ""
#endif
#ifndef SHRIMP_SECRET_TAVILY_KEY
#define SHRIMP_SECRET_TAVILY_KEY      ""
#endif
#ifndef SHRIMP_SECRET_CUSTOM_URL
#define SHRIMP_SECRET_CUSTOM_URL      ""
#endif
#ifndef SHRIMP_SECRET_CUSTOM_HEADER
#define SHRIMP_SECRET_CUSTOM_HEADER   ""
#endif
#ifndef SHRIMP_SECRET_CUSTOM_PREFIX
#define SHRIMP_SECRET_CUSTOM_PREFIX   "Bearer "
#endif

/* WiFi */
#define SHRIMP_WIFI_MAX_RETRY          5
#define SHRIMP_WIFI_RETRY_BASE_MS      1000
#define SHRIMP_WIFI_RETRY_MAX_MS       30000

/* Telegram Bot */
#define SHRIMP_TG_POLL_TIMEOUT_S       30
#define SHRIMP_TG_MAX_MSG_LEN          4096
#define SHRIMP_TG_POLL_STACK           (12 * 1024)
#define SHRIMP_TG_POLL_PRIO            5
#define SHRIMP_TG_POLL_CORE            0
#define SHRIMP_TG_CARD_SHOW_MS         3000
#define SHRIMP_TG_CARD_BODY_SCALE      3

/* Feishu Bot */
#define SHRIMP_FEISHU_MAX_MSG_LEN          4096
#define SHRIMP_FEISHU_POLL_STACK           (12 * 1024)
#define SHRIMP_FEISHU_POLL_PRIO            5
#define SHRIMP_FEISHU_POLL_CORE            1
#define SHRIMP_FEISHU_WEBHOOK_PORT         18790
#define SHRIMP_FEISHU_WEBHOOK_PATH         "/feishu/events"
#define SHRIMP_FEISHU_WEBHOOK_MAX_BODY     (16 * 1024)

/* Agent Loop */
#define SHRIMP_AGENT_STACK             (24 * 1024)
#define SHRIMP_AGENT_PRIO              6
#define SHRIMP_AGENT_CORE              1
#define SHRIMP_AGENT_MAX_HISTORY       20
#define SHRIMP_AGENT_MAX_TOOL_ITER     10
#define SHRIMP_MAX_TOOL_CALLS          4
#define SHRIMP_AGENT_SEND_WORKING_STATUS 1
#define SHRIMP_AGENT_DEBOUNCE_MS       3000

/* Timezone (POSIX TZ format) - China Standard Time (UTC+8) */
#define SHRIMP_TIMEZONE                "CST-8"

/* LLM */
#define SHRIMP_LLM_DEFAULT_MODEL       "claude-opus-4-5"
#define SHRIMP_LLM_PROVIDER_DEFAULT    "anthropic"
#define SHRIMP_LLM_MAX_TOKENS          4096
#define SHRIMP_LLM_API_URL             "https://api.anthropic.com/v1/messages"
#define SHRIMP_OPENAI_API_URL          "https://api.openai.com/v1/chat/completions"
#define SHRIMP_QWEN_API_URL            "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
#define SHRIMP_GEMINI_API_URL          "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions"
#define SHRIMP_DEEPSEEK_API_URL        "https://api.deepseek.com/chat/completions"
#define SHRIMP_ZHIPU_API_URL           "https://open.bigmodel.cn/api/paas/v4/chat/completions"
#define SHRIMP_MOONSHOT_API_URL        "https://api.moonshot.cn/v1/chat/completions"
#define SHRIMP_MINIMAX_API_URL         "https://api.minimax.chat/v1/chat/completions"
#define SHRIMP_YI_API_URL              "https://api.01.ai/v1/chat/completions"
#define SHRIMP_DOUBAO_API_URL          "https://ark.cn-beijing.volces.com/api/v3/chat/completions"
#define SHRIMP_HUNYUAN_API_URL         "https://api.hunyuan.cloud.tencent.com/v1/chat/completions"
#define SHRIMP_BAICHUAN_API_URL        "https://api.baichuan-ai.com/v1/chat/completions"
#define SHRIMP_QIANFAN_API_URL         "https://aip.baidubce.com/rpc/2.0/ai_custom/v1/wenxinworkshop/chat/completions"
#define SHRIMP_SPARK_API_URL           "https://spark-api-open.xf-yun.com/v1/chat/completions"
#define SHRIMP_LLM_API_VERSION         "2023-06-01"
#define SHRIMP_LLM_STREAM_BUF_SIZE     (32 * 1024)
#define SHRIMP_LLM_LOG_VERBOSE_PAYLOAD 0
#define SHRIMP_LLM_LOG_PREVIEW_BYTES   160

/* Message Bus */
#define SHRIMP_BUS_QUEUE_LEN           16
#define SHRIMP_OUTBOUND_STACK          (12 * 1024)
#define SHRIMP_OUTBOUND_PRIO           5
#define SHRIMP_OUTBOUND_CORE           0

/* Memory / SPIFFS */
#define SHRIMP_SPIFFS_BASE             "/spiffs"
#define SHRIMP_SPIFFS_CONFIG_DIR       SHRIMP_SPIFFS_BASE "/config"
#define SHRIMP_SPIFFS_MEMORY_DIR       SHRIMP_SPIFFS_BASE "/memory"
#define SHRIMP_SPIFFS_SESSION_DIR      SHRIMP_SPIFFS_BASE "/sessions"
#define SHRIMP_MEMORY_FILE             SHRIMP_SPIFFS_MEMORY_DIR "/MEMORY.md"
#define SHRIMP_SOUL_FILE               SHRIMP_SPIFFS_CONFIG_DIR "/SOUL.md"
#define SHRIMP_USER_FILE               SHRIMP_SPIFFS_CONFIG_DIR "/USER.md"
#define SHRIMP_CONTEXT_BUF_SIZE        (16 * 1024)
#define SHRIMP_SESSION_MAX_MSGS        20
#define SHRIMP_SESSION_MAX_FILE_SIZE   (50 * 1024)  /* 50KB max per session file */

/* Cron / Heartbeat */
#define SHRIMP_CRON_FILE               SHRIMP_SPIFFS_BASE "/cron.json"
#define SHRIMP_CRON_MAX_JOBS           16
#define SHRIMP_CRON_CHECK_INTERVAL_MS  (60 * 1000)
#define SHRIMP_HEARTBEAT_FILE          SHRIMP_SPIFFS_BASE "/HEARTBEAT.md"
#define SHRIMP_HEARTBEAT_INTERVAL_MS   (30 * 60 * 1000)

/* Skills */
#define SHRIMP_SKILLS_PREFIX           SHRIMP_SPIFFS_BASE "/skills/"

/* WebSocket Gateway */
#define SHRIMP_WS_PORT                 18789
#define SHRIMP_WS_MAX_CLIENTS          4

/* Config AP (fallback when no WiFi credentials) */
#define SHRIMP_CONFIG_AP_SSID          "MiniShrimp-Setup"
#define SHRIMP_CONFIG_AP_PASS          "shrimp1234"

/* Serial CLI */
#define SHRIMP_CLI_STACK               (4 * 1024)
#define SHRIMP_CLI_PRIO                3
#define SHRIMP_CLI_CORE                0

/* NVS Namespaces */
#define SHRIMP_NVS_WIFI                "wifi_config"
#define SHRIMP_NVS_TG                  "tg_config"
#define SHRIMP_NVS_FEISHU              "feishu_config"
#define SHRIMP_NVS_LLM                 "llm_config"
#define SHRIMP_NVS_PROXY               "proxy_config"
#define SHRIMP_NVS_SEARCH              "search_config"

/* NVS Keys */
#define SHRIMP_NVS_KEY_SSID            "ssid"
#define SHRIMP_NVS_KEY_PASS            "password"
#define SHRIMP_NVS_KEY_WIFI_LIST       "wifi_list"
#define SHRIMP_NVS_KEY_TG_TOKEN        "bot_token"
#define SHRIMP_NVS_KEY_FEISHU_APP_ID   "app_id"
#define SHRIMP_NVS_KEY_FEISHU_APP_SECRET "app_secret"
#define SHRIMP_NVS_KEY_API_KEY         "api_key"
#define SHRIMP_NVS_KEY_TAVILY_KEY      "tavily_key"
#define SHRIMP_NVS_KEY_MODEL           "model"
#define SHRIMP_NVS_KEY_PROVIDER        "provider"
#define SHRIMP_NVS_KEY_CUSTOM_URL      "custom_url"
#define SHRIMP_NVS_KEY_CUSTOM_HEADER   "custom_header"
#define SHRIMP_NVS_KEY_CUSTOM_PREFIX   "custom_prefix"
#define SHRIMP_NVS_KEY_PROXY_HOST      "host"
#define SHRIMP_NVS_KEY_PROXY_PORT      "port"
