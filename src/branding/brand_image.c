#include "branding.h"

#include <stdio.h>
#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

unsigned char *brand_image_decode(const struct brand_blob *blob, int *width, int *height) {
    int channels = 0;
    return stbi_load_from_memory(blob->data, (int)blob->size, width, height, &channels, 4);
}

unsigned char *brand_image_load_file(const char *path, int *width, int *height) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    unsigned char *data = NULL;
    long size = 0;
    if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) > 0 && size < 32L * 1024 * 1024 &&
        fseek(file, 0, SEEK_SET) == 0 && (data = malloc((size_t)size)) != NULL &&
        fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(file);
    if (!data) {
        return NULL;
    }
    int channels = 0;
    unsigned char *pixels = stbi_load_from_memory(data, (int)size, width, height, &channels, 4);
    free(data);
    return pixels;
}

void brand_image_free(unsigned char *pixels) {
    stbi_image_free(pixels);
}
