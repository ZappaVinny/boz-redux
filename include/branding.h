/* BOZ Redux branding built into the executables: the logo, the window icon and the UI fonts
 * (branding/), and the brand colours. */
#ifndef BOZ_BRANDING_H
#define BOZ_BRANDING_H

#ifdef __cplusplus
extern "C" {
#endif

struct brand_blob {
    const unsigned char *data;
    unsigned size;
};

extern const struct brand_blob BRAND_LOGO;          /* PNG, 640x260, transparent */
extern const struct brand_blob BRAND_ICON;          /* PNG, 256x256 */
extern const struct brand_blob BRAND_FONT_REGULAR;  /* Barlow Regular (OFL) */
extern const struct brand_blob BRAND_FONT_SEMIBOLD; /* Barlow SemiBold */
extern const struct brand_blob BRAND_FONT_HEADING;  /* Barlow Condensed Bold */

/* Brand colours, 0xRRGGBB. */
enum {
    BRAND_RED = 0xe31c29,
    BRAND_RED_DARK = 0xa8141f,
    BRAND_BLACK = 0x0d0e10,
    BRAND_PANEL = 0x16181b,
    BRAND_RAISED = 0x202328,
    BRAND_LINE = 0x2e3238,
    BRAND_WHITE = 0xe9eaeb,
    BRAND_GREY = 0x8b9098,
};

/* Decodes a PNG blob to 8-bit RGBA rows; NULL on failure. Free with brand_image_free. */
unsigned char *brand_image_decode(const struct brand_blob *blob, int *width, int *height);
void brand_image_free(unsigned char *pixels);

#ifdef __cplusplus
}
#endif

#endif
