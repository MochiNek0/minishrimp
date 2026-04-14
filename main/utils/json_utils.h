#pragma once

#include <stddef.h>
#include <stdbool.h>

/**
 * Robustly extract and repair JSON from potentially noisy or truncated LLM output.
 * 
 * @param input   Raw string from LLM (may contain preambles, markdown tags, or be truncated)
 * @param output  Buffer to store the repaired/extracted JSON
 * @param size    Size of the output buffer
 * @return        True if a JSON-like structure was found and processed, False otherwise.
 */
bool shrimp_json_repair(const char *input, char *output, size_t size);
