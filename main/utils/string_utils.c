#include "string_utils.h"
#include <string.h>
#include <stdbool.h>

void filter_valid_utf8(const char *src, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size == 0) return;

    size_t j = 0;
    const unsigned char *s = (const unsigned char *)src;
    size_t src_len = strlen(src);

    for (size_t i = 0; s[i] && j < dst_size - 1; ) {
        unsigned char c = s[i];
        size_t char_len;

        if (c < 0x80) {
            char_len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            char_len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            char_len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            char_len = 4;
        } else {
            i++;
            continue;
        }

        if (i + char_len > src_len) {
            break;
        }

        bool valid = true;
        for (size_t k = 1; k < char_len; k++) {
            if ((s[i + k] & 0xC0) != 0x80) {
                valid = false;
                break;
            }
        }

        if (valid && j + char_len < dst_size) {
            for (size_t k = 0; k < char_len; k++) {
                dst[j++] = s[i + k];
            }
        }
        i += char_len;
    }
    dst[j] = '\0';
}

size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    for (; *src && pos < dst_size - 3; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
    return pos;
}