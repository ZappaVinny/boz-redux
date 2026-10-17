#include "str_util.h"

#include <ctype.h>
#include <string.h>

char *str_trim(char *text) {
    while (isspace((unsigned char)*text)) {
        ++text;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

const char *str_base_name(const char *path) {
    if (!path) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) {
        slash = backslash;
    }
    return slash ? slash + 1 : path;
}

static void lower(char *out, size_t out_size, const char *in, int paths) {
    if (!out_size) {
        return;
    }
    size_t i = 0;
    for (; in && in[i] && i + 1 < out_size; ++i) {
        out[i] = paths && in[i] == '\\' ? '/' : (char)tolower((unsigned char)in[i]);
    }
    out[i] = '\0';
}

void str_lower(char *out, size_t out_size, const char *in) {
    lower(out, out_size, in, 0);
}

void str_lower_path(char *out, size_t out_size, const char *in) {
    lower(out, out_size, in, 1);
}
