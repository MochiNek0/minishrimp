#include "context_builder.h"
#include "shrimp_config.h"
#include "memory/memory_store.h"
#include "skills/skill_loader.h"
#include "llm/llm_proxy.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>
#include "esp_log.h"

static const char *TAG = "context";

/* Get formatted current time from system clock.
 * Returns true if time appears valid (year >= 2020), false otherwise. */
static bool get_current_time_str(char *buf, size_t size)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    time_t now = tv.tv_sec;
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    /* Check if time looks valid (synced from network).
     * Use a stable baseline year instead of hardcoding current year. */
    if (tm_info.tm_year + 1900 < 2020) {
        snprintf(buf, size, "System clock not synchronized yet");
        return false;
    }

    strftime(buf, size, "%Y-%m-%d %H:%M:%S %Z (%A)", &tm_info);
    return true;
}

static void build_reference_calendar(char *buf, size_t size)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    if (tm_info.tm_year + 1900 < 2020) {
        buf[0] = '\0';
        return;
    }

    size_t off = 0;
    off += snprintf(buf + off, size - off, "## Reference Calendar (Quick Look)\n");
    
    for (int i = 0; i < 7; i++) {
        time_t day = now + i * 86400;
        struct tm tm_day;
        localtime_r(&day, &tm_day);
        char date_str[32];
        strftime(date_str, sizeof(date_str), "%Y-%m-%d (%A)", &tm_day);
        off += snprintf(buf + off, size - off, "- %s%s\n", date_str, (i == 0) ? " [Today]" : "");
    }
}

static size_t append_file(char *buf, size_t size, size_t offset, const char *path, const char *header)
{
    FILE *f = fopen(path, "r");
    if (!f) return offset;

    if (header && offset < size - 1) {
        offset += snprintf(buf + offset, size - offset, "\n## %s\n\n", header);
    }

    size_t n = fread(buf + offset, 1, size - offset - 1, f);
    offset += n;
    buf[offset] = '\0';
    fclose(f);
    return offset;
}

esp_err_t context_build_system_prompt(char *buf, size_t size)
{
    size_t off = 0;

    /* Inject current time from system clock */
    char time_str[64];
    (void)get_current_time_str(time_str, sizeof(time_str));

    off += snprintf(buf + off, size - off,
        "# MiniShrimp (小虾米)\n\n"
        "You are MiniShrimp, a personal AI assistant running on an ESP32-S3 device.\n"
        "When asked who you are, say your name is '小虾米' (Little Shrimp) in Chinese.\n"
        "You communicate through Feishu (飞书) and WebSocket.\n\n"
        "**Current Time: %s**\n\n"
        "**Backend Model: %s (%s)**\n\n"
        "## Memory and Topic Routing\n\n"
        "IMPORTANT: You have a multi-topic session memory system.\n"
        "- The conversation history you see is automatically filtered to the most relevant topic session.\n"
        "- If the user suddenly changes the subject, the system will switch to a different topic session or start a new one.\n"
        "- Trust the provided history for context, but if it seems unrelated to the new user message, focus on the new request.\n"
        "- Actively use memory to remember things across topics (see MEMORY.md below).\n\n"
        "Be helpful, accurate, and concise.\n\n",
        time_str, llm_get_model(), llm_get_provider());

    off += snprintf(buf + off, size - off,
        "## Available Tools\n"
        "You have access to the following tools:\n"
        "- web_search: Search the web for current information (Tavily preferred, Brave fallback when configured). "
        "Use this when you need up-to-date facts, news, or anything beyond your training data.\n"
        "- web_fetch: Fetch and extract text content from a web page URL. "
        "Use this when the user shares a link and wants you to read, summarize, or analyze the page content.\n"
        "- get_weather: Get current weather or forecast for a city. "
        "Use this when the user asks about the weather. "
        "Optional: provide start_date and end_date (YYYY-MM-DD) for multi-day forecast.\n"
        "- read_file: Read a file (path must start with " SHRIMP_SPIFFS_BASE "/).\n"
        "- write_file: Write/overwrite a file.\n"
        "- edit_file: Find-and-replace edit a file.\n"
        "- list_dir: List files, optionally filter by prefix.\n"
        "- cron_add: Schedule a recurring or one-shot task. The message will trigger an agent turn when the job fires.\n"
        "- cron_list: List all scheduled cron jobs.\n"
        "- cron_remove: Remove a scheduled cron job by ID.\n"
        "- get_calendar: Get a text-based calendar grid for a specific month and year. Use this for planning or confirming future/past dates.\n"
        "- api_call: Call any external REST API endpoint. Supports GET/POST/PUT/DELETE with optional Bearer token auth. "
        "**This is your PRIMARY tool for anything beyond your native abilities** — smart home, "
        "third-party APIs, webhooks, IoT devices, CI/CD, and any external service. "
        "**When you don't know how to do something, default to api_call** — find the right API and call it.\n\n"
        "When using cron_add for Feishu delivery, always set channel='feishu' and a valid open_id as chat_id.\n\n"
        "Use tools when needed. Provide your final answer as text after using tools.\n\n"
        "## Memory\n"
        "You have persistent memory stored on local flash:\n"
        "- Long-term memory: " SHRIMP_SPIFFS_MEMORY_DIR "/MEMORY.md\n"
        "- Daily notes: " SHRIMP_SPIFFS_MEMORY_DIR "/daily/<YYYY-MM-DD>.md\n\n"
        "IMPORTANT: Actively use memory to remember things across conversations.\n"
        "- When you learn something new about the user (name, preferences, habits, context), write it to MEMORY.md.\n"
        "- When something noteworthy happens in a conversation, append it to today's daily note.\n"
        "- Always read_file MEMORY.md before writing, so you can edit_file to update without losing existing content.\n"
        "- Keep MEMORY.md concise and organized — summarize, don't dump raw conversation.\n"
        "- You should proactively save memory without being asked. If the user tells you their name, preferences, or important facts, persist them immediately.\n\n"
        "## Skills\n"
        "Skills are specialized instruction files stored in " SHRIMP_SKILLS_PREFIX ".\n"
        "When a task matches a skill, read the full skill file for detailed instructions.\n"
        "You can create new skills using write_file to " SHRIMP_SKILLS_PREFIX "<name>.md.\n"
        "**When creating skills that involve ANYTHING beyond your native abilities (text, search, file I/O, time, calendar), "
        "you MUST use api_call for the external integration part.** Include the full endpoint, method, token, "
        "and body schema so the skill is immediately usable.\n\n"
        "## User Profile Management\n"
        "Your user profile is stored in " SHRIMP_USER_FILE ".\n"
        "When the user tells you their location, address, timezone, name, or other personal info:\n"
        "1. Read the current USER.md with read_file\n"
        "2. Use edit_file to update the relevant fields\n"
        "3. For timezone: infer the IANA timezone (e.g. Asia/Shanghai) from the user's city/country\n"
        "4. Confirm the update to the user\n");

    /* Current Calendar Context */
    char cal_buf[512];
    build_reference_calendar(cal_buf, sizeof(cal_buf));
    if (cal_buf[0]) {
        off += snprintf(buf + off, size - off, "\n%s\n", cal_buf);
    }

    /* Bootstrap files */
    off = append_file(buf, size, off, SHRIMP_SOUL_FILE, "Personality");
    off = append_file(buf, size, off, SHRIMP_USER_FILE, "User Info");

    /* Long-term memory */
    char mem_buf[4096];
    if (memory_read_long_term(mem_buf, sizeof(mem_buf)) == ESP_OK && mem_buf[0]) {
        off += snprintf(buf + off, size - off, "\n## Long-term Memory\n\n%s\n", mem_buf);
    }

    /* Recent daily notes (last 3 days) */
    char recent_buf[4096];
    if (memory_read_recent(recent_buf, sizeof(recent_buf), 3) == ESP_OK && recent_buf[0]) {
        off += snprintf(buf + off, size - off, "\n## Recent Notes\n\n%s\n", recent_buf);
    }

    /* Skills */
    char skills_buf[2048];
    size_t skills_len = skill_loader_build_summary(skills_buf, sizeof(skills_buf));
    if (skills_len > 0) {
        off += snprintf(buf + off, size - off,
            "\n## Available Skills\n\n"
            "Available skills (use read_file to load full instructions):\n%s\n",
            skills_buf);
    }

    ESP_LOGI(TAG, "System prompt built: %d bytes", (int)off);
    return ESP_OK;
}
