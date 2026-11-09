/* The APK extractor inside another program (the Android launcher, which cannot start one).
 * CMake builds codboz_apk_extract.c for it with main renamed boz_apk_extract_main and exit
 * renamed boz_apk_extract_exit, which jumps back here instead of ending the program. */
#include <setjmp.h>
#include <stdnoreturn.h>

int boz_apk_extract_main(int argc, char **argv);
noreturn void boz_apk_extract_exit(int code);
int boz_apk_extract_run(const char *apk, const char *asset_dir);

static _Thread_local jmp_buf *g_return;
static _Thread_local int g_code;

noreturn void boz_apk_extract_exit(int code) {
    g_code = code;
    longjmp(*g_return, 1);
}

/* 0 when the APK was extracted. A failed run may leave an open file behind, which is fine for
 * the few installs a launcher does. */
int boz_apk_extract_run(const char *apk, const char *asset_dir) {
    jmp_buf here;
    jmp_buf *outer = g_return;
    g_return = &here;
    int result;
    if (setjmp(here)) {
        result = g_code ? g_code : 1;
    } else {
        char *argv[] = {"codboz_apk_extract", "extract", (char *)apk, (char *)asset_dir, 0};
        result = boz_apk_extract_main(4, argv);
    }
    g_return = outer;
    return result;
}
