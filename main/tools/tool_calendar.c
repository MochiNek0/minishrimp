#include "tool_calendar.h"
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdbool.h>
#include "cJSON.h"
#include "esp_log.h"

static const char *MONTH_NAMES[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

esp_err_t tool_calendar_init(void)
{
    return ESP_OK;
}

static bool is_leap(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int get_days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap(year)) return 29;
    if (month >= 1 && month <= 12) return days[month - 1];
    return 0;
}

esp_err_t tool_calendar_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: Invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    /* Default to current year/month if not specified */
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    int year = tm_now.tm_year + 1900;
    int month = tm_now.tm_mon + 1;

    cJSON *year_item = cJSON_GetObjectItem(root, "year");
    if (year_item && cJSON_IsNumber(year_item)) {
        year = year_item->valueint;
    }

    cJSON *month_item = cJSON_GetObjectItem(root, "month");
    if (month_item && cJSON_IsNumber(month_item)) {
        month = month_item->valueint;
    }

    cJSON_Delete(root);

    if (month < 1 || month > 12) {
        snprintf(output, output_size, "Error: Month must be between 1 and 12");
        return ESP_ERR_INVALID_ARG;
    }

    /* Find start day of the month using mktime */
    struct tm tm_month = {0};
    tm_month.tm_year = year - 1900;
    tm_month.tm_mon = month - 1;
    tm_month.tm_mday = 1;
    tm_month.tm_hour = 12; // Use noon to avoid DST edge cases
    
    mktime(&tm_month);
    int start_wday = tm_month.tm_wday; // 0=Sun, 1=Mon, ..., 6=Sat

    int days_in_month = get_days_in_month(year, month);
    
    /* Format the calendar grid */
    int len = 0;
    len += snprintf(output + len, output_size - len, "   %s %d\n", MONTH_NAMES[month - 1], year);
    len += snprintf(output + len, output_size - len, "Su Mo Tu We Th Fr Sa\n");

    /* Leading spaces */
    for (int i = 0; i < start_wday; i++) {
        len += snprintf(output + len, output_size - len, "   ");
    }

    for (int day = 1; day <= days_in_month; day++) {
        len += snprintf(output + len, output_size - len, "%2d ", day);
        if ((start_wday + day) % 7 == 0) {
            len += snprintf(output + len, output_size - len, "\n");
        }
        if (len >= output_size - 10) break; // Buffer safety
    }

    if (output[len - 1] != '\n') {
        len += snprintf(output + len, output_size - len, "\n");
    }

    return ESP_OK;
}
