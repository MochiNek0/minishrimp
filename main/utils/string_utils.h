#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include <stddef.h>

/**
 * @brief Filter invalid UTF-8 bytes from a string.
 *
 * Removes invalid UTF-8 sequences, ensuring the output is valid UTF-8
 * for API safety.
 *
 * @param src Source string (null-terminated)
 * @param dst Destination buffer
 * @param dst_size Size of destination buffer
 */
void filter_valid_utf8(const char *src, char *dst, size_t dst_size);

/**
 * @brief URL-encode a query string.
 *
 * @param src Source string (null-terminated)
 * @param dst Destination buffer
 * @param dst_size Size of destination buffer
 * @return Number of characters written (excluding null terminator)
 */
size_t url_encode(const char *src, char *dst, size_t dst_size);

#endif /* STRING_UTILS_H */