#include "str_util.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_trim_and_names(void) {
    char text[] = "  hello world \t\n";
    assert(strcmp(str_trim(text), "hello world") == 0);
    assert(strcmp(str_base_name("a/b\\C.bin"), "C.bin") == 0);
    assert(strcmp(str_base_name(NULL), "") == 0);
    char lower[16];
    str_lower_path(lower, sizeof(lower), "A\\B/C.BIN");
    assert(strcmp(lower, "a/b/c.bin") == 0);
}

static void test_parse_size(void) {
    uint32_t width = 0, height = 0;
    assert(str_parse_size("1280x720", &width, &height) && width == 1280 && height == 720);
    assert(str_parse_size("1920X1080", &width, &height) && width == 1920 && height == 1080);
    width = height = 7;
    assert(!str_parse_size("1280", &width, &height));
    assert(!str_parse_size("x720", &width, &height));
    assert(!str_parse_size("1280x", &width, &height));
    assert(!str_parse_size("0x720", &width, &height));
    assert(!str_parse_size("1280x720p", &width, &height));
    assert(!str_parse_size("70000x720", &width, &height));
    assert(width == 7 && height == 7);
}

int main(void) {
    test_trim_and_names();
    test_parse_size();
    puts("str_util tests passed");
    return 0;
}
