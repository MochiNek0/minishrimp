#include "json_utils.h"
#include <string.h>
#include <stdio.h>

bool shrimp_json_repair(const char *input, char *output, size_t size)
{
    if (!input || !output || size == 0) return false;

    /* 1. Find the first occurrence of '{' or '[' to skip preambles/markdown */
    const char *start = strpbrk(input, "{[");
    if (!start) return false;

    /* 2. Extract as much as possible, up to the buffer limit */
    size_t copy_len = strlen(start);
    if (copy_len >= size) copy_len = size - 1;
    strncpy(output, start, copy_len);
    output[copy_len] = '\0';

    /* 4. Find the last balanced point or repair truncation */
    char stack[64];
    int top = -1;

    for (int i = 0; output[i]; i++) {
        if (output[i] == '{' || output[i] == '[') {
            if (top < (int)sizeof(stack) - 1) {
                stack[++top] = output[i];
            }
        } else if (output[i] == '}') {
            if (top >= 0 && stack[top] == '{') {
                top--;
            }
        } else if (output[i] == ']') {
            if (top >= 0 && stack[top] == '[') {
                top--;
            }
        }
    }

    /* 5. Cleanup trailing commas before closure */
    if (top >= 0) {
        /* Truncation detected: find the last non-whitespace character */
        int len = (int)strlen(output);
        int p = len - 1;
        while (p >= 0 && (output[p] == ' ' || output[p] == '\n' || output[p] == '\r' || output[p] == '\t')) p--;
        
        /* If it's a comma, it will break parsing. Null it out. */
        if (p >= 0 && output[p] == ',') {
            output[p] = ' ';
        }

        /* 6. Append missing closing characters */
        while (top >= 0 && (size_t)strlen(output) < size - 1) {
            char closing = (stack[top] == '{') ? '}' : ']';
            size_t curr_len = strlen(output);
            output[curr_len] = closing;
            output[curr_len + 1] = '\0';
            top--;
        }
    }

    return true;
}
