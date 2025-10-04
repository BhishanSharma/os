// src/impl/lib/string_utils.c
#include "string_utils.h"

uint32_t kstr_to_uint32(const char* str) {
    uint32_t res = 0;
    for (int i = 0; str[i]; i++) {
        if (str[i] >= '0' && str[i] <= '9') {
            res = res * 10 + (str[i] - '0');
        } else {
            break; // stop at first non-digit
        }
    }
    return res;
}
