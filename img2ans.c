/*
 * img2ans - Convert images to ANSI art
 *
 * Converts PNG, JPG, or BMP images to CP437 ANSI art files suitable for
 * telnet BBSs, ANSI viewers, and terminals.
 *
 * License: GPL-3.0-or-later
 * Build:   cc -O2 -o img2ans img2ans.c -lm
 */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>

/* Fatal error + exit */
static void die(const char *msg) {
    fprintf(stderr, "img2ans: %s\n", msg);
    exit(1);
}

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */
#ifndef IMG2ANS_VERSION
#define IMG2ANS_VERSION "0.0.0-dev"
#endif


#define COLS          80
#define GLYPH_W       8
#define GLYPH_H       16
#define ANSI_COLORS   16
#define MAX_ROWS      200

/* CP437 block/shade characters used for rendering */
/*
 * CP437 shading glyphs (these are the ones that matter for ANSI art):
 *   0xB0 = light shade (???), 0xB1 = medium shade (???), 0xB2 = dark shade (???)
 *   0xDB = full block (???), 0xDC = lower half (???), 0xDD = left half (???)
 *   0xDE = right half (???), 0xDF = upper half (???)
 */
#define CP437_LIGHT    0xB0
#define CP437_MED      0xB1
#define CP437_DARK     0xB2
#define CP437_FULL     0xDB
#define CP437_LHALF    0xDC   /* lower half block */
#define CP437_UHALF    0xDF   /* upper half block */
#define CP437_LHALF2   0xDD   /* left half block  */
#define CP437_RHALF    0xDE   /* right half block */
#define CP437_SPACE    0x20

/* sRGB linearization: gamma 2.2 approximation via lookup table */
static float g_srgb_to_lin[256];
static uint8_t g_lin_to_srgb[4096]; /* 12-bit linear -> 8-bit sRGB */

static void init_gamma_tables(void) {
    for (int i = 0; i < 256; i++)
        g_srgb_to_lin[i] = (float)pow(i / 255.0, 2.2);
    for (int i = 0; i < 4096; i++) {
        double lin = i / 4095.0;
        int s = (int)(pow(lin, 1.0 / 2.2) * 255.0 + 0.5);
        g_lin_to_srgb[i] = (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
    }
}

static inline uint8_t linear_to_srgb(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return g_lin_to_srgb[(int)(v * 4095.0f + 0.5f)];
}

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

typedef struct { uint8_t r, g, b; } RGB;

/* Cell: glyph + color info. attr used for 16-color mode; fg_idx/bg_idx for 256;
 * fg_rgb/bg_rgb for 24-bit. All modes populate glyph ch. */
typedef struct {
    uint8_t ch;
    uint8_t attr;     /* 16-color: (bg<<4)|fg */
    int     fg_idx;   /* 256-color palette index */
    int     bg_idx;   /* 256-color palette index */
    RGB     fg_rgb;   /* 24-bit: exact foreground */
    RGB     bg_rgb;   /* 24-bit: exact background */
} Cell;

typedef enum { METRIC_RGB, METRIC_REDMEAN, METRIC_YCBCR } ColorMetric;
typedef enum { DITHER_NONE, DITHER_FS, DITHER_ATKINSON, DITHER_ORDERED, DITHER_JJN } DitherMode;
typedef enum { PAL_VGA, PAL_WIN } PaletteKind;
typedef enum { GLYPH_STANDARD, GLYPH_EXTENDED } GlyphSet;
typedef enum { COLOR_16, COLOR_256, COLOR_24BIT } ColorMode;
typedef enum { FMT_ANS, FMT_BIN } OutputFormat;
typedef enum { RESAMPLE_BOX, RESAMPLE_LANCZOS } ResampleMode;
typedef enum { CHARSET_ANSI, CHARSET_PETSCII } CharSet;

/* Double-buffered floating-point pixel for dithering */
typedef struct { double r, g, b; } FRGB;

/* Glyph descriptor - precomputed from the DOS font */
typedef struct {
    uint8_t ch;
    uint8_t rows[16];  /* bitmap rows, MSB = leftmost pixel */
    int     on_count;  /* number of lit pixels (0..128) */
} GlyphInfo;

/* Conversion options */
typedef struct {
    int          cols;
    int          rows;         /* 0 = auto */
    PaletteKind  palette;
    int          ice;          /* allow 16 background colors */
    DitherMode   dither;
    ColorMetric  metric;
    int          gamma_correct; /* linearize before averaging */
    double       sharpen;       /* unsharp mask strength (0 = off) */
    GlyphSet     glyph_set;     /* standard or extended */
    ColorMode    color_mode;    /* 16, 256, or 24-bit */
    OutputFormat format;        /* ans or bin */
    ResampleMode resample;      /* box (default) or lanczos */
    CharSet      charset;       /* ansi (default) or petscii */
    /* SAUCE */
    int          sauce;
    char         title[36];
    char         author[21];
    char         group[21];
} Options;

/* -------------------------------------------------------------------------
 * CP437 glyph bitmaps for ANSI art rendering
 *
 * Derived from the IBM VGA character generator ROM as documented in the
 * IBM PC/AT Technical Reference Manual (1984). The same data appears in
 * the Linux kernel (lib/fonts/font_8x16.c) and other open-source projects.
 * Only the nine glyphs used for ANSI art rendering are included.
 * ---------------------------------------------------------------------- */

/* Nine core glyphs: space, shade (B0/B1/B2), full block, four half-blocks */
typedef struct { uint8_t ch; uint8_t rows[16]; } FontGlyph;

static const FontGlyph FONT_GLYPHS_STANDARD[] = {
    /* 0x20 space - 0/128 = 0% */
    { 0x20, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
              0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } },
    /* 0xB0 light shade - 32/128 = 25% (IBM VGA ROM) */
    { 0xB0, { 0xAA,0x00,0x55,0x00,0xAA,0x00,0x55,0x00,
              0xAA,0x00,0x55,0x00,0xAA,0x00,0x55,0x00 } },
    /* 0xB1 medium shade - 64/128 = 50% */
    { 0xB1, { 0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,
              0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA } },
    /* 0xB2 dark shade - 96/128 = 75% */
    { 0xB2, { 0xDD,0x77,0xDD,0x77,0xDD,0x77,0xDD,0x77,
              0xDD,0x77,0xDD,0x77,0xDD,0x77,0xDD,0x77 } },
    /* 0xDB full block - 128/128 = 100% */
    { 0xDB, { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
              0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF } },
    /* 0xDC lower half - rows 8-15 filled */
    { 0xDC, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,
              0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF } },
    /* 0xDD left half - left 4 pixels */
    { 0xDD, { 0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,
              0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0 } },
    /* 0xDE right half - right 4 pixels */
    { 0xDE, { 0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,
              0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F } },
    /* 0xDF upper half - rows 0-7 filled */
    { 0xDF, { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,
              0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } },
};

#define NUM_STANDARD  (int)(sizeof(FONT_GLYPHS_STANDARD)/sizeof(FONT_GLYPHS_STANDARD[0]))

/* Extended glyphs: box-drawing, geometric shapes, and partial fills.
 * All bitmaps from the IBM VGA 8x16 character generator ROM. */
static const FontGlyph FONT_GLYPHS_EXTENDED[] = {
    /* 0xB3 box single vertical bar */
    { 0xB3, { 0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,
              0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18 } },
    /* 0xBA box double vertical bar */
    { 0xBA, { 0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,
              0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36 } },
    /* 0xC4 box single horizontal bar */
    { 0xC4, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,
              0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } },
    /* 0xCD box double horizontal bar */
    { 0xCD, { 0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0xFF,
              0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } },
    /* 0x16 right-pointing triangle (filled) */
    { 0x16, { 0x00,0x00,0x80,0xC0,0xE0,0xF0,0xF8,0xFC,
              0xF8,0xF0,0xE0,0xC0,0x80,0x00,0x00,0x00 } },
    /* 0x17 left-pointing triangle (filled) */
    { 0x17, { 0x00,0x00,0x02,0x06,0x0E,0x1E,0x3E,0x7E,
              0x3E,0x1E,0x0E,0x06,0x02,0x00,0x00,0x00 } },
    /* 0x1E up-pointing triangle */
    { 0x1E, { 0x00,0x00,0x18,0x18,0x3C,0x3C,0x7E,0x7E,
              0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00 } },
    /* 0x1F down-pointing triangle */
    { 0x1F, { 0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0x7E,
              0x7E,0x3C,0x3C,0x18,0x18,0x00,0x00,0x00 } },
    /* 0x2F forward slash (diagonal) */
    { 0x2F, { 0x00,0x00,0x02,0x04,0x08,0x08,0x10,0x10,
              0x20,0x20,0x40,0x80,0x00,0x00,0x00,0x00 } },
    /* 0x5C backslash (diagonal) */
    { 0x5C, { 0x00,0x00,0x80,0x40,0x20,0x20,0x10,0x10,
              0x08,0x08,0x04,0x02,0x00,0x00,0x00,0x00 } },
    /* 0xFE small square (centered) */
    { 0xFE, { 0x00,0x00,0x00,0x00,0x7E,0x7E,0x7E,0x7E,
              0x7E,0x7E,0x7E,0x00,0x00,0x00,0x00,0x00 } },
    /* 0x04 diamond */
    { 0x04, { 0x00,0x00,0x08,0x1C,0x3E,0x7F,0x3E,0x1C,
              0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } },
    /* 0x09 circle (outlined) */
    { 0x09, { 0x00,0x00,0x3C,0x42,0x42,0x42,0x42,0x42,
              0x42,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 } },
    /* 0x08 inverse bullet (filled circle) */
    { 0x08, { 0x00,0x00,0x3C,0x7E,0x7E,0x7E,0x7E,0x7E,
              0x7E,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 } },
    /* 0x13 double exclamation (vertical lines, ~12% coverage) */
    { 0x13, { 0x00,0x36,0x36,0x36,0x36,0x36,0x00,0x36,
              0x36,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } },
    /* 0xF0 triple bar / equivalence (~37% coverage) */
    { 0xF0, { 0x00,0x00,0x00,0xFF,0x00,0x00,0xFF,0x00,
              0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00 } },
    /* 0xF2 upper >= half block (bottom portion, ~37%) */
    { 0xF2, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
              0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF } },
    /* 0xBB top-right double corner */
    { 0xBB, { 0x00,0x36,0x36,0x36,0x36,0x36,0x36,0x36,
              0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36 } },
    /* 0x58 X character (~22% coverage diagonal cross) */
    { 0x58, { 0x00,0x00,0xC3,0x66,0x3C,0x18,0x3C,0x66,
              0xC3,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } },
};

#define NUM_EXTENDED  (int)(sizeof(FONT_GLYPHS_EXTENDED)/sizeof(FONT_GLYPHS_EXTENDED[0]))

#define MAX_GLYPHS  (NUM_STANDARD + NUM_EXTENDED)

/* Precomputed glyph cache */
static GlyphInfo g_glyphs[MAX_GLYPHS];
static int       g_num_glyphs = 0;

static void init_glyphs(GlyphSet set) {
    g_num_glyphs = 0;

    /* Always include standard glyphs */
    for (int i = 0; i < NUM_STANDARD; i++) {
        g_glyphs[g_num_glyphs].ch = FONT_GLYPHS_STANDARD[i].ch;
        memcpy(g_glyphs[g_num_glyphs].rows, FONT_GLYPHS_STANDARD[i].rows, 16);
        int cnt = 0;
        for (int row = 0; row < 16; row++) {
            uint8_t b = FONT_GLYPHS_STANDARD[i].rows[row];
            while (b) { cnt += (b & 1); b >>= 1; }
        }
        g_glyphs[g_num_glyphs].on_count = cnt;
        g_num_glyphs++;
    }

    /* Extended set adds box-drawing, geometric shapes */
    if (set == GLYPH_EXTENDED) {
        for (int i = 0; i < NUM_EXTENDED; i++) {
            g_glyphs[g_num_glyphs].ch = FONT_GLYPHS_EXTENDED[i].ch;
            memcpy(g_glyphs[g_num_glyphs].rows, FONT_GLYPHS_EXTENDED[i].rows, 16);
            int cnt = 0;
            for (int row = 0; row < 16; row++) {
                uint8_t b = FONT_GLYPHS_EXTENDED[i].rows[row];
                while (b) { cnt += (b & 1); b >>= 1; }
            }
            g_glyphs[g_num_glyphs].on_count = cnt;
            g_num_glyphs++;
        }
    }
}

/* -------------------------------------------------------------------------
 * 16-color palettes
 * ---------------------------------------------------------------------- */

/* VGA (DOS) 16-color palette */
static const RGB VGA16[16] = {
    {  0,  0,  0}, {  0,  0,170}, {  0,170,  0}, {  0,170,170},
    {170,  0,  0}, {170,  0,170}, {170, 85,  0}, {170,170,170},
    { 85, 85, 85}, { 85, 85,255}, { 85,255, 85}, { 85,255,255},
    {255, 85, 85}, {255, 85,255}, {255,255, 85}, {255,255,255},
};

/* Windows console palette */
static const RGB WIN16[16] = {
    {  0,  0,  0}, {  0,  0,128}, {  0,128,  0}, {  0,128,128},
    {128,  0,  0}, {128,  0,128}, {128,128,  0}, {192,192,192},
    {128,128,128}, {  0,  0,255}, {  0,255,  0}, {  0,255,255},
    {255,  0,  0}, {255,  0,255}, {255,255,  0}, {255,255,255},
};

static inline const RGB *get_palette(PaletteKind pk) {
    return (pk == PAL_WIN) ? WIN16 : VGA16;
}

/* xterm-256 palette: 16 system + 216 color cube + 24 grayscale */
static RGB XTERM256[256];

static void init_xterm256(void) {
    /* 0-15: same as VGA16 */
    memcpy(XTERM256, VGA16, 16 * sizeof(RGB));
    /* 16-231: 6x6x6 color cube */
    for (int i = 0; i < 216; i++) {
        int r = i / 36, g = (i / 6) % 6, b = i % 6;
        XTERM256[16+i].r = r ? (uint8_t)(r * 40 + 55) : 0;
        XTERM256[16+i].g = g ? (uint8_t)(g * 40 + 55) : 0;
        XTERM256[16+i].b = b ? (uint8_t)(b * 40 + 55) : 0;
    }
    /* 232-255: grayscale */
    for (int i = 0; i < 24; i++) {
        uint8_t v = (uint8_t)(8 + i * 10);
        XTERM256[232+i] = (RGB){v, v, v};
    }
}

/* C64 color palette (VICE default, widely accepted as canonical) */
static const RGB C64_PAL[16] = {
    {  0,  0,  0}, {255,255,255}, {136, 57, 50}, {103,182,189},
    {139, 63,150}, { 85,160, 73}, { 64, 49,141}, {191,206,114},
    {139, 84, 41}, { 87, 66,  0}, {184,105, 98}, { 80, 80, 80},
    {120,120,120}, {148,224,137}, {120,105,196}, {159,159,159},
};

/* PETSCII glyph bitmaps: 8x8 upper-case/graphics set (C64 ROM).
 * Each glyph is 8 rows of 8 bits. Subset most useful for image rendering. */
typedef struct { uint8_t code; uint8_t rows[8]; } PetsciiGlyph;

static const PetsciiGlyph PETSCII_GLYPHS[] = {
    /* space */
    { 0x20, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } },
    /* full block (reverse space) */
    { 0xA0, { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF } },
    /* upper half */
    { 0xA3, { 0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00 } },
    /* lower half */
    { 0xE2, { 0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF } },
    /* left half */
    { 0xA1, { 0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0 } },
    /* right half */
    { 0xE1, { 0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F } },
    /* upper-left quadrant */
    { 0xFE, { 0xF0,0xF0,0xF0,0xF0,0x00,0x00,0x00,0x00 } },
    /* upper-right quadrant */
    { 0xFC, { 0x0F,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00 } },
    /* lower-left quadrant */
    { 0xFB, { 0x00,0x00,0x00,0x00,0xF0,0xF0,0xF0,0xF0 } },
    /* lower-right quadrant */
    { 0xEC, { 0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x0F } },
    /* checkerboard */
    { 0xE6, { 0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55 } },
    /* upper-right triangle */
    { 0xBF, { 0x01,0x03,0x07,0x0F,0x1F,0x3F,0x7F,0xFF } },
    /* lower-left triangle */
    { 0xCE, { 0xFF,0x7F,0x3F,0x1F,0x0F,0x07,0x03,0x01 } },
};

#define NUM_PETSCII_GLYPHS  (int)(sizeof(PETSCII_GLYPHS)/sizeof(PETSCII_GLYPHS[0]))

/* Precomputed PETSCII glyph cache (GlyphInfo with 8x8, padded to 16 rows) */
static GlyphInfo g_petscii[64];
static int       g_petscii_num = 0;

static void init_petscii(void) {
    g_petscii_num = 0;
    for (int i = 0; i < NUM_PETSCII_GLYPHS; i++) {
        GlyphInfo *gi = &g_petscii[g_petscii_num];
        gi->ch = PETSCII_GLYPHS[i].code;
        memset(gi->rows, 0, 16);
        memcpy(gi->rows, PETSCII_GLYPHS[i].rows, 8);
        int cnt = 0;
        for (int row = 0; row < 8; row++) {
            uint8_t b = PETSCII_GLYPHS[i].rows[row];
            while (b) { cnt += (b & 1); b >>= 1; }
        }
        gi->on_count = cnt;
        g_petscii_num++;
    }
}

/* -------------------------------------------------------------------------
 * Color distance metrics
 * ---------------------------------------------------------------------- */

static inline int clamp_byte(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* Naive sRGB Euclidean (squared) */
static inline long dist_rgb2(RGB a, RGB b) {
    long dr = (int)a.r - (int)b.r;
    long dg = (int)a.g - (int)b.g;
    long db = (int)a.b - (int)b.b;
    return dr*dr + dg*dg + db*db;
}

/* Redmean weighted distance (squared) - good perceptual compromise */
static inline long dist_redmean2(RGB a, RGB b) {
    int dr = (int)a.r - (int)b.r;
    int dg = (int)a.g - (int)b.g;
    int db = (int)a.b - (int)b.b;
    int rm = ((int)a.r + (int)b.r) / 2;
    return (long)((512 + rm) * dr*dr) / 256
         + (long)(4 * dg*dg)
         + (long)((767 - rm) * db*db) / 256;
}

/* YCbCr-ish perceptual distance (BT.601, integer) */
static inline long dist_ycbcr2(RGB a, RGB b) {
    int ya  = (77*(int)a.r + 150*(int)a.g + 29*(int)a.b) >> 8;
    int yb  = (77*(int)b.r + 150*(int)b.g + 29*(int)b.b) >> 8;
    int cba = (-43*(int)a.r - 85*(int)a.g + 128*(int)a.b) >> 8;
    int cbb = (-43*(int)b.r - 85*(int)b.g + 128*(int)b.b) >> 8;
    int cra = (128*(int)a.r - 107*(int)a.g - 21*(int)a.b) >> 8;
    int crb = (128*(int)b.r - 107*(int)b.g - 21*(int)b.b) >> 8;
    int dy = ya - yb, dcb = cba - cbb, dcr = cra - crb;
    /* Saturation of a (0..255) */
    int mn = a.r < a.g ? a.r : a.g; if (a.b < mn) mn = a.b;
    int mx = a.r > a.g ? a.r : a.g; if (a.b > mx) mx = a.b;
    int sat = mx - mn;
    /* Boost chroma weight for saturated colors */
    int chromaScale = 256 + sat * 2;
    return (long)(2 * dy*dy)
         + (long)(((long)(dcb*dcb + dcr*dcr) * chromaScale) >> 8);
}

static long pal_dist2(RGB a, RGB b, ColorMetric m) {
    switch (m) {
        case METRIC_REDMEAN: return dist_redmean2(a, b);
        case METRIC_YCBCR:   return dist_ycbcr2(a, b);
        default:             return dist_rgb2(a, b);
    }
}

/* -------------------------------------------------------------------------
 * Image resampling - area-average downsample to cell grid
 * ---------------------------------------------------------------------- */

/* Sample average RGB over a region of the source image (bilinear area avg) */
static RGB sample_region(const uint8_t *img, int img_w, int img_h,
                          double x0, double y0, double x1, double y1,
                          int gamma_correct) {
    /* clamp */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > img_w) x1 = img_w;
    if (y1 > img_h) y1 = img_h;
    if (x1 <= x0 || y1 <= y0) return (RGB){0,0,0};

    double sumr = 0, sumg = 0, sumb = 0, sumw = 0;
    int ix0 = (int)x0, iy0 = (int)y0;
    int ix1 = (int)ceil(x1), iy1 = (int)ceil(y1);

    for (int py = iy0; py < iy1; py++) {
        double wy = 1.0;
        if (py < y0) wy = py + 1.0 - y0;
        else if (py + 1.0 > y1) wy = y1 - py;
        for (int px = ix0; px < ix1; px++) {
            double wx = 1.0;
            if (px < x0) wx = px + 1.0 - x0;
            else if (px + 1.0 > x1) wx = x1 - px;
            double w = wx * wy;
            int idx = (py * img_w + px) * 3;
            if (gamma_correct) {
                sumr += w * g_srgb_to_lin[img[idx+0]];
                sumg += w * g_srgb_to_lin[img[idx+1]];
                sumb += w * g_srgb_to_lin[img[idx+2]];
            } else {
                sumr += w * img[idx+0];
                sumg += w * img[idx+1];
                sumb += w * img[idx+2];
            }
            sumw += w;
        }
    }
    if (sumw < 1e-9) return (RGB){0,0,0};
    if (gamma_correct) {
        return (RGB){
            linear_to_srgb((float)(sumr / sumw)),
            linear_to_srgb((float)(sumg / sumw)),
            linear_to_srgb((float)(sumb / sumw)),
        };
    }
    return (RGB){
        (uint8_t)clamp_byte((int)(sumr / sumw + 0.5)),
        (uint8_t)clamp_byte((int)(sumg / sumw + 0.5)),
        (uint8_t)clamp_byte((int)(sumb / sumw + 0.5)),
    };
}

/* Sample a single pixel (8x16 subpixel) of the source image for glyph matching */
static RGB sample_pixel(const uint8_t *img, int img_w, int img_h,
                         double cx, double cy,     /* cell origin in img coords */
                         double cw, double ch_,    /* cell size in img coords */
                         int gx, int gy,           /* glyph pixel 0..7, 0..15 */
                         int gamma_correct)
{
    double x0 = cx + (double)gx * cw / GLYPH_W;
    double y0 = cy + (double)gy * ch_ / GLYPH_H;
    double x1 = cx + (double)(gx+1) * cw / GLYPH_W;
    double y1 = cy + (double)(gy+1) * ch_ / GLYPH_H;
    return sample_region(img, img_w, img_h, x0, y0, x1, y1, gamma_correct);
}

/* -------------------------------------------------------------------------
 * Lanczos-3 resampling: high-quality downscaler
 * ---------------------------------------------------------------------- */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double lanczos_kernel(double x, double a) {
    if (x == 0.0) return 1.0;
    if (x < -a || x > a) return 0.0;
    double px = M_PI * x;
    return a * sin(px) * sin(px / a) / (px * px);
}

/* Resize image to dst_w x dst_h using Lanczos-3 (separable, 2-pass) */
static uint8_t *resize_lanczos(const uint8_t *src, int sw, int sh,
                                 int dw, int dh) {
    const double a = 3.0; /* Lanczos-3 */

    /* Temporary buffer for horizontal pass */
    double *tmp = (double *)malloc((size_t)(dw * sh * 3) * sizeof(double));
    uint8_t *dst = (uint8_t *)malloc((size_t)(dw * dh * 3));
    if (!tmp || !dst) { free(tmp); free(dst); return NULL; }

    /* Horizontal pass: src (sw x sh) -> tmp (dw x sh) */
    double sx_ratio = (double)sw / dw;
    double support_x = (sx_ratio > 1.0) ? a * sx_ratio : a;

    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < dw; x++) {
            double center = (x + 0.5) * sx_ratio - 0.5;
            int x0 = (int)floor(center - support_x);
            int x1 = (int)ceil(center + support_x);
            double sum_r = 0, sum_g = 0, sum_b = 0, sum_w = 0;
            for (int sx = x0; sx <= x1; sx++) {
                int csx = sx < 0 ? 0 : (sx >= sw ? sw - 1 : sx);
                double w = lanczos_kernel((sx - center) / (sx_ratio > 1.0 ? sx_ratio : 1.0), a);
                sum_r += w * src[(y * sw + csx) * 3 + 0];
                sum_g += w * src[(y * sw + csx) * 3 + 1];
                sum_b += w * src[(y * sw + csx) * 3 + 2];
                sum_w += w;
            }
            if (sum_w != 0.0) { sum_r /= sum_w; sum_g /= sum_w; sum_b /= sum_w; }
            tmp[(y * dw + x) * 3 + 0] = sum_r;
            tmp[(y * dw + x) * 3 + 1] = sum_g;
            tmp[(y * dw + x) * 3 + 2] = sum_b;
        }
    }

    /* Vertical pass: tmp (dw x sh) -> dst (dw x dh) */
    double sy_ratio = (double)sh / dh;
    double support_y = (sy_ratio > 1.0) ? a * sy_ratio : a;

    for (int x = 0; x < dw; x++) {
        for (int y = 0; y < dh; y++) {
            double center = (y + 0.5) * sy_ratio - 0.5;
            int y0 = (int)floor(center - support_y);
            int y1 = (int)ceil(center + support_y);
            double sum_r = 0, sum_g = 0, sum_b = 0, sum_w = 0;
            for (int sy = y0; sy <= y1; sy++) {
                int csy = sy < 0 ? 0 : (sy >= sh ? sh - 1 : sy);
                double w = lanczos_kernel((sy - center) / (sy_ratio > 1.0 ? sy_ratio : 1.0), a);
                sum_r += w * tmp[(csy * dw + x) * 3 + 0];
                sum_g += w * tmp[(csy * dw + x) * 3 + 1];
                sum_b += w * tmp[(csy * dw + x) * 3 + 2];
                sum_w += w;
            }
            if (sum_w != 0.0) { sum_r /= sum_w; sum_g /= sum_w; sum_b /= sum_w; }
            dst[(y * dw + x) * 3 + 0] = (uint8_t)clamp_byte((int)(sum_r + 0.5));
            dst[(y * dw + x) * 3 + 1] = (uint8_t)clamp_byte((int)(sum_g + 0.5));
            dst[(y * dw + x) * 3 + 2] = (uint8_t)clamp_byte((int)(sum_b + 0.5));
        }
    }

    free(tmp);
    return dst;
}

/* -------------------------------------------------------------------------
 * Dithering: Floyd-Steinberg and Atkinson over the cell grid
 * ---------------------------------------------------------------------- */

/* 4x4 Bayer ordered dithering threshold matrix (normalized to -0.5..+0.5) */
static const double BAYER4[4][4] = {
    { -0.46875, +0.03125, -0.34375, +0.15625 },
    { +0.28125, -0.21875, +0.40625, -0.09375 },
    { -0.28125, +0.21875, -0.40625, +0.09375 },
    { +0.46875, -0.03125, +0.34375, -0.15625 },
};

/* Error buffer: one FRGB per cell, rows side-by-side */
static FRGB *g_err = NULL;
static int   g_err_cols = 0;
static int   g_err_rows = 0;

static void err_alloc(int cols, int rows) {
    free(g_err);
    g_err = calloc((size_t)(cols * rows), sizeof(FRGB));
    g_err_cols = cols;
    g_err_rows = rows;
}

static FRGB err_get(int x, int y) {
    if (x < 0 || x >= g_err_cols || y < 0 || y >= g_err_rows)
        return (FRGB){0,0,0};
    return g_err[y * g_err_cols + x];
}

static void err_add(int x, int y, double r, double g, double b) {
    if (x < 0 || x >= g_err_cols || y < 0 || y >= g_err_rows) return;
    g_err[y * g_err_cols + x].r += r;
    g_err[y * g_err_cols + x].g += g;
    g_err[y * g_err_cols + x].b += b;
}

/* Distribute quantization error after choosing FG/BG for a cell */
static void diffuse_error(int x, int y, RGB avg, RGB approx, DitherMode dm) {
    double er = (double)(int)avg.r - (double)(int)approx.r;
    double eg = (double)(int)avg.g - (double)(int)approx.g;
    double eb = (double)(int)avg.b - (double)(int)approx.b;

    if (dm == DITHER_FS) {
        /* Floyd-Steinberg: right 7/16, down-left 3/16, down 5/16, down-right 1/16 */
        err_add(x+1, y,   er*7/16, eg*7/16, eb*7/16);
        err_add(x-1, y+1, er*3/16, eg*3/16, eb*3/16);
        err_add(x,   y+1, er*5/16, eg*5/16, eb*5/16);
        err_add(x+1, y+1, er*1/16, eg*1/16, eb*1/16);
    } else if (dm == DITHER_ATKINSON) {
        /* Atkinson: distributes 6/8 of error (keeps some "in the image") */
        double f = 1.0/8.0;
        err_add(x+1, y,   er*f, eg*f, eb*f);
        err_add(x+2, y,   er*f, eg*f, eb*f);
        err_add(x-1, y+1, er*f, eg*f, eb*f);
        err_add(x,   y+1, er*f, eg*f, eb*f);
        err_add(x+1, y+1, er*f, eg*f, eb*f);
        err_add(x,   y+2, er*f, eg*f, eb*f);
    } else if (dm == DITHER_JJN) {
        /* Jarvis-Judice-Ninke: 12 neighbors, divisor 48 */
        err_add(x+1, y,   er*7/48, eg*7/48, eb*7/48);
        err_add(x+2, y,   er*5/48, eg*5/48, eb*5/48);
        err_add(x-2, y+1, er*3/48, eg*3/48, eb*3/48);
        err_add(x-1, y+1, er*5/48, eg*5/48, eb*5/48);
        err_add(x,   y+1, er*7/48, eg*7/48, eb*7/48);
        err_add(x+1, y+1, er*5/48, eg*5/48, eb*5/48);
        err_add(x+2, y+1, er*3/48, eg*3/48, eb*3/48);
        err_add(x-2, y+2, er*1/48, eg*1/48, eb*1/48);
        err_add(x-1, y+2, er*3/48, eg*3/48, eb*3/48);
        err_add(x,   y+2, er*5/48, eg*5/48, eb*5/48);
        err_add(x+1, y+2, er*3/48, eg*3/48, eb*3/48);
        err_add(x+2, y+2, er*1/48, eg*1/48, eb*1/48);
    }
}

/* -------------------------------------------------------------------------
 * Cell rendering: pick best glyph + FG/BG pair for a cell
 * ---------------------------------------------------------------------- */

/* Compute error for a glyph+FG+BG against the 16x8 samples.
 *
 * For shade glyphs (B0/B1/B2) we use a coverage-blended error: the eye
 * integrates the stipple into a perceived average color, so compare the
 * blended average against each sample's color.
 * For block/half glyphs we use pixel-by-pixel (spatially coherent, so
 * the per-pixel model is accurate).
 */
static long cell_error(const GlyphInfo *g, int fg_idx, int bg_idx,
                        const RGB *pal, const RGB samples[16][8],
                        ColorMetric m) {
    /* Shade glyphs: perceived-blend mode */
    if (g->ch == CP437_LIGHT || g->ch == CP437_MED || g->ch == CP437_DARK) {
        double cov = (double)g->on_count / 128.0; /* fraction of fg pixels */
        RGB fg = pal[fg_idx], bg = pal[bg_idx];
        /* Blended color the eye perceives */
        RGB blend = {
            (uint8_t)(fg.r * cov + bg.r * (1.0 - cov) + 0.5),
            (uint8_t)(fg.g * cov + bg.g * (1.0 - cov) + 0.5),
            (uint8_t)(fg.b * cov + bg.b * (1.0 - cov) + 0.5),
        };
        long err = 0;
        for (int py = 0; py < 16; py++)
            for (int px = 0; px < 8; px++)
                err += pal_dist2(samples[py][px], blend, m);
        return err;
    }

    /* Block/half glyphs: pixel-by-pixel */
    long err = 0;
    for (int py = 0; py < 16; py++) {
        uint8_t row = g->rows[py];
        for (int px = 0; px < 8; px++) {
            int on = (row >> (7 - px)) & 1;
            int pi = on ? fg_idx : bg_idx;
            err += pal_dist2(samples[py][px], pal[pi], m);
        }
    }
    return err;
}

/* cell_error variant taking direct RGB colors (for 256-color/24-bit modes) */
static long cell_error_rgb(const GlyphInfo *g, RGB fg, RGB bg,
                            const RGB samples[16][8], ColorMetric m) {
    if (g->ch == CP437_LIGHT || g->ch == CP437_MED || g->ch == CP437_DARK) {
        double cov = (double)g->on_count / 128.0;
        RGB blend = {
            (uint8_t)(fg.r * cov + bg.r * (1.0 - cov) + 0.5),
            (uint8_t)(fg.g * cov + bg.g * (1.0 - cov) + 0.5),
            (uint8_t)(fg.b * cov + bg.b * (1.0 - cov) + 0.5),
        };
        long err = 0;
        for (int py = 0; py < 16; py++)
            for (int px = 0; px < 8; px++)
                err += pal_dist2(samples[py][px], blend, m);
        return err;
    }

    long err = 0;
    for (int py = 0; py < 16; py++) {
        uint8_t row = g->rows[py];
        for (int px = 0; px < 8; px++) {
            int on = (row >> (7 - px)) & 1;
            err += pal_dist2(samples[py][px], on ? fg : bg, m);
        }
    }
    return err;
}

/* Compute the average RGB of the 16x8 samples */
static RGB avg_samples(const RGB samples[16][8]) {
    long sr = 0, sg = 0, sb = 0;
    for (int py = 0; py < 16; py++)
        for (int px = 0; px < 8; px++) {
            sr += samples[py][px].r;
            sg += samples[py][px].g;
            sb += samples[py][px].b;
        }
    return (RGB){
        (uint8_t)(sr / 128),
        (uint8_t)(sg / 128),
        (uint8_t)(sb / 128),
    };
}

/*
 * Find best cell (glyph + FG + BG) for a given set of 16x8 pixel samples.
 *
 * Full search: all 8 glyphs × 16 FG × 8 BG (+ solid space candidates).
 * ~1152 combos × 128 pixel evals = ~147K distance calls per cell. Fast enough.
 */
static Cell best_cell(const RGB samples[16][8], const RGB *pal,
                       int ice, ColorMetric m) {
    int bg_max = ice ? 16 : 8;

    long best_err = LONG_MAX;
    int best_fg = 7, best_bg = 0;
    uint8_t best_ch = CP437_SPACE;

    /* --- Solid color (space glyph, any bg color) --- */
    for (int ci = 0; ci < bg_max; ci++) {
        long err = 0;
        for (int py = 0; py < 16; py++)
            for (int px = 0; px < 8; px++)
                err += pal_dist2(samples[py][px], pal[ci], m);
        if (err < best_err) { best_err = err; best_fg = ci; best_bg = ci; best_ch = CP437_SPACE; }
    }

    /* --- Block/shade glyphs (FG != BG) --- */
    for (int gi = 0; gi < g_num_glyphs; gi++) {
        const GlyphInfo *g = &g_glyphs[gi];
        if (g->ch == CP437_SPACE) continue;

        for (int fgi = 0; fgi < 16; fgi++) {
            for (int bgi = 0; bgi < bg_max; bgi++) {
                if (fgi == bgi) continue;
                long err = cell_error(g, fgi, bgi, pal, samples, m);
                if (err < best_err) {
                    best_err = err; best_fg = fgi; best_bg = bgi; best_ch = g->ch;
                }
            }
        }
    }

    uint8_t attr = (uint8_t)(((best_bg & 0x0F) << 4) | (best_fg & 0x0F));
    return (Cell){ best_ch, attr, 0, 0, {0,0,0}, {0,0,0} };
}

/* 256-color best_cell: same exhaustive search but over 256 palette entries.
 * Uses iCE-style approach (no blink bit concern with 256 colors). */
static Cell best_cell_256(RGB samples[16][8], const RGB *pal256, ColorMetric m) {
    long best_err = LONG_MAX;
    uint8_t best_ch = 0x20;
    int best_fg = 0, best_bg = 0;

    for (int gi = 0; gi < g_num_glyphs; gi++) {
        const GlyphInfo *g = &g_glyphs[gi];

        if (g->on_count == 0) {
            /* Solid BG only */
            for (int bg = 0; bg < 256; bg++) {
                long err = 0;
                for (int py = 0; py < 16 && err < best_err; py++)
                    for (int px = 0; px < 8; px++)
                        err += pal_dist2(samples[py][px], pal256[bg], m);
                if (err < best_err) {
                    best_err = err; best_ch = g->ch; best_fg = 0; best_bg = bg;
                }
            }
        } else if (g->on_count == 128) {
            /* Solid FG only */
            for (int fg = 0; fg < 256; fg++) {
                long err = 0;
                for (int py = 0; py < 16 && err < best_err; py++)
                    for (int px = 0; px < 8; px++)
                        err += pal_dist2(samples[py][px], pal256[fg], m);
                if (err < best_err) {
                    best_err = err; best_ch = g->ch; best_fg = fg; best_bg = 0;
                }
            }
        } else {
            /* For 256 colors, exhaustive FG*BG (256*256=65536) is too expensive.
             * Instead: find the 2 best-matching palette entries for the cell avg,
             * then search a neighborhood around them. */
            RGB avg = avg_samples(samples);
            int top[8];
            long top_d[8];
            for (int i = 0; i < 8; i++) { top[i] = 0; top_d[i] = LONG_MAX; }
            for (int ci = 0; ci < 256; ci++) {
                long d = pal_dist2(avg, pal256[ci], m);
                for (int i = 0; i < 8; i++) {
                    if (d < top_d[i]) {
                        for (int j = 7; j > i; j--) { top[j] = top[j-1]; top_d[j] = top_d[j-1]; }
                        top[i] = ci; top_d[i] = d;
                        break;
                    }
                }
            }

            /* Search top-N fg x top-N bg combinations */
            for (int fi = 0; fi < 8; fi++) {
                for (int bi = 0; bi < 8; bi++) {
                    int fg = top[fi], bg = top[bi];
                    long err = cell_error_rgb(g, pal256[fg], pal256[bg], samples, m);
                    if (err < best_err) {
                        best_err = err; best_ch = g->ch; best_fg = fg; best_bg = bg;
                    }
                }
            }
        }
    }

    Cell c = {0};
    c.ch = best_ch;
    c.fg_idx = best_fg;
    c.bg_idx = best_bg;
    c.fg_rgb = pal256[best_fg];
    c.bg_rgb = pal256[best_bg];
    return c;
}

/* 24-bit truecolor best_cell: compute optimal FG/BG analytically per glyph.
 * For each glyph, FG = avg of "on" pixels, BG = avg of "off" pixels. */
static Cell best_cell_24bit(RGB samples[16][8], ColorMetric m) {
    long best_err = LONG_MAX;
    Cell best = {0};
    best.ch = 0x20;

    for (int gi = 0; gi < g_num_glyphs; gi++) {
        const GlyphInfo *g = &g_glyphs[gi];
        double fg_r = 0, fg_g = 0, fg_b = 0;
        double bg_r = 0, bg_g = 0, bg_b = 0;
        int fg_cnt = 0, bg_cnt = 0;

        /* Compute optimal FG (on pixels) and BG (off pixels) as averages */
        for (int py = 0; py < 16; py++) {
            uint8_t row = g->rows[py];
            for (int px = 0; px < 8; px++) {
                int on = (row >> (7 - px)) & 1;
                if (on) {
                    fg_r += samples[py][px].r; fg_g += samples[py][px].g; fg_b += samples[py][px].b;
                    fg_cnt++;
                } else {
                    bg_r += samples[py][px].r; bg_g += samples[py][px].g; bg_b += samples[py][px].b;
                    bg_cnt++;
                }
            }
        }

        RGB fg_c, bg_c;
        if (fg_cnt > 0) {
            fg_c = (RGB){ (uint8_t)(fg_r / fg_cnt + 0.5), (uint8_t)(fg_g / fg_cnt + 0.5), (uint8_t)(fg_b / fg_cnt + 0.5) };
        } else {
            fg_c = (RGB){0, 0, 0};
        }
        if (bg_cnt > 0) {
            bg_c = (RGB){ (uint8_t)(bg_r / bg_cnt + 0.5), (uint8_t)(bg_g / bg_cnt + 0.5), (uint8_t)(bg_b / bg_cnt + 0.5) };
        } else {
            bg_c = (RGB){0, 0, 0};
        }

        /* Compute error with optimal colors */
        long err = 0;
        for (int py = 0; py < 16; py++) {
            uint8_t row = g->rows[py];
            for (int px = 0; px < 8; px++) {
                int on = (row >> (7 - px)) & 1;
                err += pal_dist2(samples[py][px], on ? fg_c : bg_c, m);
            }
        }

        if (err < best_err) {
            best_err = err;
            best.ch = g->ch;
            best.fg_rgb = fg_c;
            best.bg_rgb = bg_c;
        }
    }

    return best;
}

/* Best cell search for PETSCII: 8x8 glyphs, C64 16-color palette */
static Cell best_cell_petscii(const RGB samples[8][8], ColorMetric m) {
    long best_err = LONG_MAX;
    Cell best = { 0x20, 0, 0, 0, {0,0,0}, {0,0,0} };

    /* Solid space candidates: all 16 BG colors */
    for (int bg = 0; bg < 16; bg++) {
        long err = 0;
        for (int py = 0; py < 8; py++)
            for (int px = 0; px < 8; px++)
                err += pal_dist2(samples[py][px], C64_PAL[bg], m);
        if (err < best_err) {
            best_err = err;
            best.ch = 0x20;
            best.attr = (uint8_t)((bg << 4) | 0);
        }
    }

    /* All PETSCII glyphs x 16 FG x 16 BG */
    for (int gi = 0; gi < g_petscii_num; gi++) {
        const GlyphInfo *g = &g_petscii[gi];
        if (g->on_count == 0) continue; /* skip space, already covered */
        for (int fg = 0; fg < 16; fg++) {
            for (int bg = 0; bg < 16; bg++) {
                if (fg == bg && g->on_count != 64) continue;
                /* Use pixel-by-pixel error (8x8 glyphs are spatially coherent) */
                long err = 0;
                for (int py = 0; py < 8; py++) {
                    uint8_t row = g->rows[py];
                    for (int px = 0; px < 8; px++) {
                        int on = (row >> (7 - px)) & 1;
                        err += pal_dist2(samples[py][px], on ? C64_PAL[fg] : C64_PAL[bg], m);
                    }
                }
                if (err < best_err) {
                    best_err = err;
                    best.ch = g->ch;
                    best.attr = (uint8_t)((bg << 4) | fg);
                }
            }
        }
    }

    return best;
}

/* -------------------------------------------------------------------------
 * Pre-processing: unsharp mask for detail recovery
 * ---------------------------------------------------------------------- */

/* Apply unsharp mask: sharpened = original + amount * (original - blurred).
 * Uses a 3x3 Gaussian-like kernel [1 2 1; 2 4 2; 1 2 1] / 16. */
static uint8_t *sharpen_image(const uint8_t *img, int w, int h, double amount) {
    uint8_t *out = malloc((size_t)(w * h * 3));
    if (!out) return NULL;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < 3; c++) {
                /* 3x3 weighted average */
                double sum = 0;
                static const int kw[3] = {1, 2, 1};
                for (int ky = -1; ky <= 1; ky++) {
                    int sy = y + ky;
                    if (sy < 0) sy = 0;
                    if (sy >= h) sy = h - 1;
                    for (int kx = -1; kx <= 1; kx++) {
                        int sx = x + kx;
                        if (sx < 0) sx = 0;
                        if (sx >= w) sx = w - 1;
                        sum += kw[ky+1] * kw[kx+1] * img[(sy * w + sx) * 3 + c];
                    }
                }
                double blurred = sum / 16.0;
                double orig = img[(y * w + x) * 3 + c];
                double sharp = orig + amount * (orig - blurred);
                out[(y * w + x) * 3 + c] = (uint8_t)clamp_byte((int)(sharp + 0.5));
            }
        }
    }
    return out;
}

/* -------------------------------------------------------------------------
 * Main conversion: image -> cell array
 * ---------------------------------------------------------------------- */

static Cell *convert_image(const uint8_t *img, int img_w, int img_h,
                            const Options *opt, int *out_cols, int *out_rows) {
    const RGB *pal = get_palette(opt->palette);

    /* PETSCII vs ANSI cell geometry */
    int glyph_w = GLYPH_W;  /* always 8 */
    int glyph_h = (opt->charset == CHARSET_PETSCII) ? 8 : GLYPH_H;

    /* Determine output dimensions */
    int cols = opt->cols > 0 ? opt->cols : ((opt->charset == CHARSET_PETSCII) ? 40 : COLS);
    int rows;
    if (opt->rows > 0) {
        rows = opt->rows;
    } else {
        /* Auto: preserve aspect ratio.
         * ANSI cells are 8x16 (2:1 aspect), PETSCII are 8x8 (1:1).
         */
        if (opt->charset == CHARSET_PETSCII) {
            rows = (img_h * cols) / img_w;
        } else {
            rows = (img_h * cols) / (img_w * 2);
        }
        if (rows < 1) rows = 1;
        if (rows > MAX_ROWS) rows = MAX_ROWS;
    }

    *out_cols = cols;
    *out_rows = rows;

    /* Cell size in source image pixels */
    double cw = (double)img_w / cols;
    double ch = (double)img_h / rows;

    /* Lanczos pre-resize: resample source to exact pixel grid */
    uint8_t *resized = NULL;
    int r_w = cols * glyph_w, r_h = rows * glyph_h;
    if (opt->resample == RESAMPLE_LANCZOS) {
        resized = resize_lanczos(img, img_w, img_h, r_w, r_h);
        if (!resized) die("out of memory (lanczos resize)");
        fprintf(stderr, "img2ans: lanczos resampled %dx%d -> %dx%d\n", img_w, img_h, r_w, r_h);
    }

    /* Allocate output */
    Cell *cells = malloc((size_t)(cols * rows) * sizeof(Cell));
    if (!cells) { fprintf(stderr, "img2ans: out of memory\n"); exit(1); }

    /* Dither error buffer (for error-diffusion modes) */
    if (opt->dither == DITHER_FS || opt->dither == DITHER_ATKINSON || opt->dither == DITHER_JJN) {
        err_alloc(cols, rows);
    }

    for (int cy = 0; cy < rows; cy++) {
        for (int cx = 0; cx < cols; cx++) {
            /* Collect subpixel samples: 16x8 for ANSI, 8x8 for PETSCII */
            RGB samples[16][8];
            memset(samples, 0, sizeof(samples));
            if (resized) {
                for (int py = 0; py < glyph_h; py++)
                    for (int px = 0; px < glyph_w; px++) {
                        int rx = cx * glyph_w + px;
                        int ry = cy * glyph_h + py;
                        const uint8_t *p = &resized[(ry * r_w + rx) * 3];
                        samples[py][px] = (RGB){p[0], p[1], p[2]};
                    }
            } else {
                double ox = cx * cw;
                double oy = cy * ch;
                for (int py = 0; py < glyph_h; py++)
                    for (int px = 0; px < glyph_w; px++)
                        samples[py][px] = sample_pixel(img, img_w, img_h, ox, oy, cw, ch, px, py, opt->gamma_correct);
            }

            /* Apply dither: ordered uses Bayer threshold, others use error buffer */
            if (opt->dither == DITHER_ORDERED) {
                /* Bayer threshold: bias each sample by the matrix value.
                 * Spread = palette step size (~85 for VGA). Scale to move
                 * samples toward the nearest palette boundary. */
                double threshold = BAYER4[cy & 3][cx & 3] * 64.0;
                for (int py = 0; py < 16; py++)
                    for (int px = 0; px < 8; px++) {
                        samples[py][px].r = (uint8_t)clamp_byte((int)(samples[py][px].r + threshold));
                        samples[py][px].g = (uint8_t)clamp_byte((int)(samples[py][px].g + threshold));
                        samples[py][px].b = (uint8_t)clamp_byte((int)(samples[py][px].b + threshold));
                    }
            } else if (opt->dither != DITHER_NONE) {
                FRGB ferr = err_get(cx, cy);
                if (ferr.r != 0.0 || ferr.g != 0.0 || ferr.b != 0.0) {
                    for (int py = 0; py < 16; py++)
                        for (int px = 0; px < 8; px++) {
                            samples[py][px].r = (uint8_t)clamp_byte((int)((double)samples[py][px].r + ferr.r + 0.5));
                            samples[py][px].g = (uint8_t)clamp_byte((int)((double)samples[py][px].g + ferr.g + 0.5));
                            samples[py][px].b = (uint8_t)clamp_byte((int)((double)samples[py][px].b + ferr.b + 0.5));
                        }
                }
            }

            /* Find best cell */
            Cell cell;
            if (opt->charset == CHARSET_PETSCII) {
                /* Cast to 8x8 - only top 8 rows used */
                cell = best_cell_petscii((const RGB (*)[8])samples, opt->metric);
            } else if (opt->color_mode == COLOR_256) {
                cell = best_cell_256(samples, XTERM256, opt->metric);
            } else if (opt->color_mode == COLOR_24BIT) {
                cell = best_cell_24bit(samples, opt->metric);
            } else {
                cell = best_cell(samples, pal, opt->ice, opt->metric);
            }
            cells[cy * cols + cx] = cell;

            /* Propagate dithering error (error-diffusion modes only) */
            if (opt->dither == DITHER_FS || opt->dither == DITHER_ATKINSON || opt->dither == DITHER_JJN) {
                /* Compute the approximate RGB that this cell renders */
                const GlyphInfo *g = NULL;
                int total_pixels = glyph_w * glyph_h; /* 128 for ANSI, 64 for PETSCII */

                if (opt->charset == CHARSET_PETSCII) {
                    for (int gi = 0; gi < g_petscii_num; gi++) {
                        if (g_petscii[gi].ch == cell.ch) { g = &g_petscii[gi]; break; }
                    }
                } else {
                    for (int gi = 0; gi < g_num_glyphs; gi++) {
                        if (g_glyphs[gi].ch == cell.ch) { g = &g_glyphs[gi]; break; }
                    }
                }

                RGB fg_c, bg_c;
                if (opt->charset == CHARSET_PETSCII) {
                    int fg = cell.attr & 0x0F;
                    int bg = (cell.attr >> 4) & 0x0F;
                    fg_c = C64_PAL[fg];
                    bg_c = C64_PAL[bg];
                } else if (opt->color_mode == COLOR_24BIT) {
                    fg_c = cell.fg_rgb;
                    bg_c = cell.bg_rgb;
                } else if (opt->color_mode == COLOR_256) {
                    fg_c = XTERM256[cell.fg_idx];
                    bg_c = XTERM256[cell.bg_idx];
                } else {
                    int fg = cell.attr & 0x0F;
                    int bg = (cell.attr >> 4) & 0x0F;
                    fg_c = pal[fg];
                    bg_c = pal[bg];
                }

                RGB approx;
                if (!g || g->on_count == 0) {
                    approx = bg_c;
                } else if (g->on_count == total_pixels) {
                    approx = fg_c;
                } else {
                    double alpha = (double)g->on_count / (double)total_pixels;
                    approx.r = (uint8_t)clamp_byte((int)(alpha * fg_c.r + (1-alpha) * bg_c.r + 0.5));
                    approx.g = (uint8_t)clamp_byte((int)(alpha * fg_c.g + (1-alpha) * bg_c.g + 0.5));
                    approx.b = (uint8_t)clamp_byte((int)(alpha * fg_c.b + (1-alpha) * bg_c.b + 0.5));
                }

                /* Average of actual samples (use glyph_h rows, not always 16) */
                long sr = 0, sg = 0, sb = 0;
                for (int py = 0; py < glyph_h; py++)
                    for (int px = 0; px < glyph_w; px++) {
                        sr += samples[py][px].r;
                        sg += samples[py][px].g;
                        sb += samples[py][px].b;
                    }
                RGB avg = {
                    (uint8_t)(sr / total_pixels),
                    (uint8_t)(sg / total_pixels),
                    (uint8_t)(sb / total_pixels),
                };
                diffuse_error(cx, cy, avg, approx, opt->dither);
            }
        }
    }

    free(resized);
    return cells;
}

/* -------------------------------------------------------------------------
 * ANSI output writer
 * ---------------------------------------------------------------------- */

/*
 * VGA attribute color index -> ANSI SGR color code mapping.
 * VGA palette order: 0=black,1=blue,2=green,3=cyan,4=red,5=magenta,6=brown,7=lgray
 * ANSI SGR order:    0=black,1=red, 2=green,3=yellow,4=blue,5=magenta,6=cyan,7=white
 */
static const int VGA_TO_SGR[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

/* Write bytes to file, abort on error */
static void write_bytes(FILE *f, const void *buf, size_t len) {
    if (len == 0) return;
    if (fwrite(buf, 1, len, f) != len) {
        fprintf(stderr, "img2ans: write error\n");
        exit(1);
    }
}

static void write_str(FILE *f, const char *s) {
    write_bytes(f, s, strlen(s));
}

/* Emit an SGR sequence: ESC [ codes... m */
static void emit_sgr(FILE *f, const int *codes, int n) {
    if (n == 0) return;
    char buf[64];
    int pos = 0;
    buf[pos++] = 0x1B; buf[pos++] = '[';
    for (int i = 0; i < n; i++) {
        if (i > 0) buf[pos++] = ';';
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%d", codes[i]);
    }
    buf[pos++] = 'm';
    write_bytes(f, buf, pos);
}

static void save_ansi(FILE *f, const Cell *cells, int cols, int rows,
                       int ice, const Options *opt) {
    /* Reset all attributes */
    int reset[] = {0};
    emit_sgr(f, reset, 1);

    if (opt->color_mode == COLOR_24BIT) {
        /* 24-bit truecolor output: ESC[38;2;R;G;Bm / ESC[48;2;R;G;Bm */
        RGB cur_fg = {0, 0, 0}, cur_bg = {0, 0, 0};
        int first = 1;

        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                const Cell *cell = &cells[r * cols + c];
                uint8_t ch = cell->ch;
                if (ch < 32 || ch == 127) ch = 32;

                RGB fg = cell->fg_rgb, bg = cell->bg_rgb;
                char buf[64];
                if (first || fg.r != cur_fg.r || fg.g != cur_fg.g || fg.b != cur_fg.b) {
                    snprintf(buf, sizeof(buf), "\x1B[38;2;%d;%d;%dm", fg.r, fg.g, fg.b);
                    write_str(f, buf);
                    cur_fg = fg;
                }
                if (first || bg.r != cur_bg.r || bg.g != cur_bg.g || bg.b != cur_bg.b) {
                    snprintf(buf, sizeof(buf), "\x1B[48;2;%d;%d;%dm", bg.r, bg.g, bg.b);
                    write_str(f, buf);
                    cur_bg = bg;
                }
                first = 0;
                write_bytes(f, &ch, 1);
            }
            if (r < rows - 1) {
                write_str(f, "\x1B[0m\r\n");
                first = 1;
            }
        }
        write_str(f, "\x1B[0m");
        return;
    }

    if (opt->color_mode == COLOR_256) {
        /* 256-color output: ESC[38;5;Nm / ESC[48;5;Nm */
        int cur_fg = -1, cur_bg = -1;

        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                const Cell *cell = &cells[r * cols + c];
                uint8_t ch = cell->ch;
                if (ch < 32 || ch == 127) ch = 32;

                int fg = cell->fg_idx, bg = cell->bg_idx;
                char buf[32];
                if (fg != cur_fg) {
                    snprintf(buf, sizeof(buf), "\x1B[38;5;%dm", fg);
                    write_str(f, buf);
                    cur_fg = fg;
                }
                if (bg != cur_bg) {
                    snprintf(buf, sizeof(buf), "\x1B[48;5;%dm", bg);
                    write_str(f, buf);
                    cur_bg = bg;
                }
                write_bytes(f, &ch, 1);
            }
            if (r < rows - 1) {
                write_str(f, "\x1B[0m\r\n");
                cur_fg = cur_bg = -1;
            }
        }
        write_str(f, "\x1B[0m");
        return;
    }

    /* 16-color mode (original) */
    int cur_attr = -1;

    for (int r = 0; r < rows; r++) {
        /* Trim trailing default-colored spaces from each row */
        int last_col = -1;
        for (int c = cols - 1; c >= 0; c--) {
            const Cell *cell = &cells[r * cols + c];
            uint8_t ch = cell->ch;
            if (ch < 32 || ch == 127) ch = 32;
            int fg_raw = cell->attr & 0x0F;
            int bg_raw = (cell->attr >> 4) & 0x0F;
            /* Not a trailing default space if non-space, or non-default colors */
            if (ch != 32 || fg_raw != 7 || bg_raw != 0) {
                last_col = c;
                break;
            }
        }

        for (int c = 0; c <= last_col; c++) {
            const Cell *cell = &cells[r * cols + c];
            uint8_t ch = cell->ch;
            if (ch < 32 || ch == 127) ch = 32;

            int attr = cell->attr;
            int fg_raw = attr & 0x0F;
            int bg_raw = (attr >> 4) & 0x0F;
            if (!ice) bg_raw &= 0x07;

            if (attr != cur_attr) {
                /* Build SGR codes */
                int codes[8];
                int nc = 0;

                int want_bold  = (fg_raw >= 8);
                int want_blink = (!ice && bg_raw >= 8); /* blink mode for high-BG without iCE */
                int fg_base    = fg_raw & 0x07;
                int bg_base    = bg_raw & 0x07;
                int want_fg    = 30 + VGA_TO_SGR[fg_base];
                int want_bg    = 40 + VGA_TO_SGR[bg_base];

                /* Check what we need to change from cur_attr */
                int prev_bold  = (cur_attr >= 0) && ((cur_attr & 0x0F) >= 8);
                int prev_blink = (cur_attr >= 0) && (!ice && ((cur_attr >> 4) & 0x0F) >= 8);

                /* If bold/blink state needs to turn OFF, we must reset */
                int need_reset = (cur_attr < 0)
                    || (prev_bold && !want_bold)
                    || (prev_blink && !want_blink);

                if (need_reset) {
                    codes[nc++] = 0;
                    cur_attr = 0; /* track that we've reset */
                    /* After reset, emit everything */
                    if (want_bold)  codes[nc++] = 1;
                    if (want_blink) codes[nc++] = 5;
                    codes[nc++] = want_fg;
                    codes[nc++] = want_bg;
                } else {
                    /* Just emit what changed */
                    int prev_fg_r = cur_attr & 0x0F;
                    int prev_bg_r = (cur_attr >> 4) & 0x0F;
                    if (!prev_bold && want_bold)   codes[nc++] = 1;
                    if (!prev_blink && want_blink) codes[nc++] = 5;
                    if (prev_fg_r != fg_raw)       codes[nc++] = want_fg;
                    if (prev_bg_r != bg_raw)       codes[nc++] = want_bg;
                }

                if (nc > 0) emit_sgr(f, codes, nc);
                cur_attr = attr;
            }

            /* Write the character byte directly (CP437) */
            uint8_t out_ch = ch;
            write_bytes(f, &out_ch, 1);
        }

        /* End of row: reset and newline (CR+LF for ANSI compatibility) */
        if (r < rows - 1) {
            if (cur_attr != 0) {
                int rst[] = {0};
                emit_sgr(f, rst, 1);
                cur_attr = 0;
            }
            write_str(f, "\r\n");
        }
    }

    /* Final reset */
    {
        int rst[] = {0};
        emit_sgr(f, rst, 1);
    }
}

/* -------------------------------------------------------------------------
 * SAUCE record writer
 * Reference: https://www.acid.org/info/sauce/sauce.htm
 * ---------------------------------------------------------------------- */

#pragma pack(push, 1)
typedef struct {
    char     id[5];        /* "SAUCE" */
    char     version[2];   /* "00"    */
    char     title[35];
    char     author[20];
    char     group[20];
    char     date[8];      /* YYYYMMDD */
    uint32_t file_size;
    uint8_t  data_type;    /* 1 = Character */
    uint8_t  file_type;    /* 1 = ANSI      */
    uint16_t tinfo1;       /* cols          */
    uint16_t tinfo2;       /* rows          */
    uint16_t tinfo3;
    uint16_t tinfo4;
    uint8_t  comment_count;
    uint8_t  tflags;       /* bit0=iCE colors */
    char     tinfos[22];
} SauceRecord;
#pragma pack(pop)

/* BIN format output: raw character+attribute pairs, 16-color only */
static void save_bin(FILE *f, const Cell *cells, int cols, int rows) {
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const Cell *cell = &cells[r * cols + c];
            uint8_t ch = cell->ch;
            if (ch < 32 || ch == 127) ch = 32;
            uint8_t pair[2] = { ch, cell->attr };
            write_bytes(f, pair, 2);
        }
    }
}

/* PETSCII color control codes (C64):
 * These are the PETSCII byte values that set the current text color. */
static const uint8_t PETSCII_COLOR_CODES[16] = {
    0x90, /* 0  black */
    0x05, /* 1  white */
    0x1C, /* 2  red */
    0x9F, /* 3  cyan */
    0x9C, /* 4  purple */
    0x1E, /* 5  green */
    0x1F, /* 6  dark blue */
    0x9E, /* 7  yellow */
    0x81, /* 8  orange */
    0x95, /* 9  brown */
    0x96, /* 10 light red */
    0x97, /* 11 dark grey */
    0x98, /* 12 grey */
    0x99, /* 13 light green */
    0x9A, /* 14 light blue */
    0x9B, /* 15 light grey */
};

/* Save PETSCII output with C64 color control codes.
 * Format: sequential stream of color codes + characters.
 * Uses reverse mode (RVS ON = 0x12, RVS OFF = 0x92) for background colors.
 * A PETSCII file is read by C64 terminal programs. */
static void save_petscii(FILE *f, const Cell *cells, int cols, int rows) {
    int cur_fg = -1;
    int rvs_on = 0;

    /* Clear screen: 0x93 */
    uint8_t clr = 0x93;
    write_bytes(f, &clr, 1);

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const Cell *cell = &cells[r * cols + c];
            int fg = cell->attr & 0x0F;
            int bg = (cell->attr >> 4) & 0x0F;
            uint8_t ch = cell->ch;

            /* For space (empty), just set BG color as the "ink" in reverse mode */
            if (ch == 0x20 || ch == 0xA0) {
                /* Space: show bg color via reverse space, or normal space */
                if (bg == 0 && !rvs_on) {
                    /* Black bg, just emit space */
                    if (cur_fg != fg) {
                        write_bytes(f, &PETSCII_COLOR_CODES[fg], 1);
                        cur_fg = fg;
                    }
                    uint8_t sp = 0x20;
                    write_bytes(f, &sp, 1);
                } else {
                    /* Use reverse space to show bg color */
                    if (cur_fg != bg) {
                        write_bytes(f, &PETSCII_COLOR_CODES[bg], 1);
                        cur_fg = bg;
                    }
                    if (!rvs_on) {
                        uint8_t rv = 0x12;
                        write_bytes(f, &rv, 1);
                        rvs_on = 1;
                    }
                    uint8_t sp = 0x20;
                    write_bytes(f, &sp, 1);
                }
            } else {
                /* For other glyphs: fg color, with reverse off */
                if (rvs_on) {
                    uint8_t rv = 0x92;
                    write_bytes(f, &rv, 1);
                    rvs_on = 0;
                }
                if (cur_fg != fg) {
                    write_bytes(f, &PETSCII_COLOR_CODES[fg], 1);
                    cur_fg = fg;
                }
                write_bytes(f, &ch, 1);
            }
        }
        /* Newline: CR */
        if (r < rows - 1) {
            if (rvs_on) {
                uint8_t rv = 0x92;
                write_bytes(f, &rv, 1);
                rvs_on = 0;
            }
            uint8_t cr = 0x0D;
            write_bytes(f, &cr, 1);
        }
    }
}

static void write_sauce(FILE *f, int cols, int rows, int ice,
                         long file_size_before, const Options *opt) {
    /* EOF marker */
    uint8_t eof_marker = 0x1A;
    write_bytes(f, &eof_marker, 1);

    SauceRecord s;
    memset(&s, ' ', sizeof(s));

    memcpy(s.id,      "SAUCE", 5);
    memcpy(s.version, "00",    2);

    /* Title, author, group - space-padded */
    memset(s.title,  ' ', 35); if (opt->title[0])  memcpy(s.title,  opt->title,  strlen(opt->title)  < 35 ? strlen(opt->title)  : 35);
    memset(s.author, ' ', 20); if (opt->author[0]) memcpy(s.author, opt->author, strlen(opt->author) < 20 ? strlen(opt->author) : 20);
    memset(s.group,  ' ', 20); if (opt->group[0])  memcpy(s.group,  opt->group,  strlen(opt->group)  < 20 ? strlen(opt->group)  : 20);

       /* Date: 8 bytes YYYYMMDD, no NUL terminator */
    {
        char tmp[9];
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        snprintf(tmp, sizeof(tmp), "%04d%02d%02d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
        memcpy(s.date, tmp, 8);
    }

    s.file_size    = (uint32_t)file_size_before;
    s.data_type    = 1;  /* Character */
    s.file_type    = (opt->format == FMT_BIN) ? 5 : 1;  /* 5=BIN, 1=ANSI */
    s.tinfo1       = (uint16_t)cols;
    s.tinfo2       = (uint16_t)rows;
    s.tinfo3       = 0;
    s.tinfo4       = 0;
    s.comment_count = 0;
    s.tflags       = ice ? 1 : 0;
    memset(s.tinfos, 0, 22);

    write_bytes(f, &s, sizeof(s));
}

/* -------------------------------------------------------------------------
 * CLI parsing & main
 * ---------------------------------------------------------------------- */

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [options] input.{png,jpg,bmp} [output.ans]\n"
        "\n"
        "version: " IMG2ANS_VERSION "\n"
        "\n"
        "options:\n"
        "  --cols N       width in columns (default: 80)\n"
        "  --rows N       height in rows (default: auto)\n"
        "  --palette vga  color palette: vga (default) or win\n"
        "  --ice          enable iCE colors (16 background colors)\n"
        "  --dither MODE  dithering: none, fs (default), atkinson, ordered, jjn\n"
        "  --metric MODE  color metric: rm (default), rgb, ycbcr\n"
        "  --gamma        gamma-correct resampling (linear light averaging)\n"
        "  --no-gamma     disable gamma correction (default)\n"
        "  --sharpen N    unsharp mask strength (default: off, try 0.5-2.0)\n"
        "  --glyphs SET   glyph set: standard (default) or extended\n"
        "  --colors MODE  color mode: 16 (default), 256, or 24bit\n"
        "  --format FMT   output format: ans (default) or bin\n"
        "  --resample M   resampling: box (default) or lanczos\n"
        "  --charset C    character set: ansi (default) or petscii\n"
        "  --sauce        embed SAUCE metadata record\n"
        "  --title STR    SAUCE title (implies --sauce)\n"
        "  --author STR   SAUCE author (implies --sauce)\n"
        "  --group STR    SAUCE group (implies --sauce)\n"
        "  --help         show this help\n"
        "\n"
        "output defaults to input filename with .ans extension.\n",
        prog);
}

int main(int argc, char *argv[]) {
    Options opt = {0};
    opt.cols    = 80;
    opt.rows    = 0;
    opt.palette = PAL_VGA;
    opt.ice     = 0;
    opt.dither  = DITHER_FS;
    opt.metric  = METRIC_REDMEAN;
    opt.sauce   = 0;
    opt.gamma_correct = 0;
    opt.sharpen = 0.0;
    opt.glyph_set = GLYPH_STANDARD;
    opt.color_mode = COLOR_16;
    opt.format = FMT_ANS;
    opt.resample = RESAMPLE_BOX;
    opt.charset = CHARSET_ANSI;

    const char *in_file  = NULL;
    const char *out_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]); return 0;
        } else if (strcmp(argv[i], "--cols") == 0 && i+1 < argc) {
            opt.cols = atoi(argv[++i]);
            if (opt.cols < 1 || opt.cols > 320) die("--cols must be 1..320");
        } else if (strcmp(argv[i], "--rows") == 0 && i+1 < argc) {
            opt.rows = atoi(argv[++i]);
            if (opt.rows < 0 || opt.rows > MAX_ROWS) die("--rows must be 0..200");
        } else if (strcmp(argv[i], "--palette") == 0 && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "win") == 0)      opt.palette = PAL_WIN;
            else if (strcmp(argv[i], "vga") == 0) opt.palette = PAL_VGA;
            else die("unknown palette (vga|win)");
        } else if (strcmp(argv[i], "--ice") == 0) {
            opt.ice = 1;
        } else if (strcmp(argv[i], "--gamma") == 0) {
            opt.gamma_correct = 1;
        } else if (strcmp(argv[i], "--no-gamma") == 0) {
            opt.gamma_correct = 0;
        } else if (strcmp(argv[i], "--sharpen") == 0 && i+1 < argc) {
            opt.sharpen = atof(argv[++i]);
            if (opt.sharpen < 0.0) die("--sharpen must be >= 0");
        } else if (strcmp(argv[i], "--glyphs") == 0 && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "standard") == 0)      opt.glyph_set = GLYPH_STANDARD;
            else if (strcmp(argv[i], "extended") == 0)  opt.glyph_set = GLYPH_EXTENDED;
            else die("unknown glyph set (standard|extended)");
        } else if (strcmp(argv[i], "--colors") == 0 && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "16") == 0)        opt.color_mode = COLOR_16;
            else if (strcmp(argv[i], "256") == 0)  opt.color_mode = COLOR_256;
            else if (strcmp(argv[i], "24bit") == 0) opt.color_mode = COLOR_24BIT;
            else die("unknown color mode (16|256|24bit)");
        } else if (strcmp(argv[i], "--format") == 0 && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "ans") == 0)       opt.format = FMT_ANS;
            else if (strcmp(argv[i], "bin") == 0)  opt.format = FMT_BIN;
            else die("unknown format (ans|bin)");
        } else if (strcmp(argv[i], "--resample") == 0 && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "box") == 0)        opt.resample = RESAMPLE_BOX;
            else if (strcmp(argv[i], "lanczos") == 0) opt.resample = RESAMPLE_LANCZOS;
            else die("unknown resample mode (box|lanczos)");
        } else if (strcmp(argv[i], "--charset") == 0 && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "ansi") == 0)        opt.charset = CHARSET_ANSI;
            else if (strcmp(argv[i], "petscii") == 0) opt.charset = CHARSET_PETSCII;
            else die("unknown charset (ansi|petscii)");
        } else if (strcmp(argv[i], "--dither") == 0 && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "none") == 0)         opt.dither = DITHER_NONE;
            else if (strcmp(argv[i], "fs") == 0)      opt.dither = DITHER_FS;
            else if (strcmp(argv[i], "atkinson") == 0) opt.dither = DITHER_ATKINSON;
            else if (strcmp(argv[i], "ordered") == 0) opt.dither = DITHER_ORDERED;
            else if (strcmp(argv[i], "jjn") == 0)     opt.dither = DITHER_JJN;
            else die("unknown dither mode (none|fs|atkinson|ordered|jjn)");
        } else if (strcmp(argv[i], "--metric") == 0 && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "rgb") == 0)       opt.metric = METRIC_RGB;
            else if (strcmp(argv[i], "rm") == 0)   opt.metric = METRIC_REDMEAN;
            else if (strcmp(argv[i], "ycbcr") == 0) opt.metric = METRIC_YCBCR;
            else die("unknown metric (rgb|rm|ycbcr)");
        } else if (strcmp(argv[i], "--sauce") == 0) {
            opt.sauce = 1;
        } else if (strcmp(argv[i], "--title") == 0 && i+1 < argc) {
            strncpy(opt.title, argv[++i], 35);
            opt.sauce = 1;
        } else if (strcmp(argv[i], "--author") == 0 && i+1 < argc) {
            strncpy(opt.author, argv[++i], 20);
            opt.sauce = 1;
        } else if (strcmp(argv[i], "--group") == 0 && i+1 < argc) {
            strncpy(opt.group, argv[++i], 20);
            opt.sauce = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "img2ans: unknown option: %s\n", argv[i]);
            usage(argv[0]); return 1;
        } else if (!in_file) {
            in_file = argv[i];
        } else if (!out_file) {
            out_file = argv[i];
        } else {
            fprintf(stderr, "img2ans: unexpected argument: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    if (!in_file) { usage(argv[0]); return 1; }

    /* Build default output name: replace/append extension */
    static char out_buf[4096];
    const char *ext = (opt.format == FMT_BIN) ? ".bin" :
                      (opt.charset == CHARSET_PETSCII) ? ".pet" : ".ans";
    if (!out_file) {
        strncpy(out_buf, in_file, sizeof(out_buf) - 5);
        char *dot = strrchr(out_buf, '.');
        char *slash = strrchr(out_buf, '/');
        if (dot && (!slash || dot > slash)) *dot = '\0';
        strncat(out_buf, ext, sizeof(out_buf) - strlen(out_buf) - 1);
        out_file = out_buf;
    }

    /* Load image */
    int img_w, img_h, img_ch;
    uint8_t *img = stbi_load(in_file, &img_w, &img_h, &img_ch, 3);
    if (!img) {
        fprintf(stderr, "img2ans: cannot load '%s': %s\n", in_file, stbi_failure_reason());
        return 1;
    }

    fprintf(stderr, "img2ans: loaded %dx%d from '%s'\n", img_w, img_h, in_file);

    /* Init glyph cache */
    if (opt.charset == CHARSET_PETSCII) {
        init_petscii();
    } else {
        init_glyphs(opt.glyph_set);
    }
    if (opt.gamma_correct)
        init_gamma_tables();
    if (opt.color_mode == COLOR_256)
        init_xterm256();

    /* Pre-process: sharpen if requested */
    uint8_t *sharp_img = NULL;
    if (opt.sharpen > 0.0) {
        sharp_img = sharpen_image(img, img_w, img_h, opt.sharpen);
        if (!sharp_img) die("out of memory (sharpen)");
        fprintf(stderr, "img2ans: sharpened (amount=%.1f)\n", opt.sharpen);
    }

    /* Convert */
    int out_cols, out_rows;
    const uint8_t *src = sharp_img ? sharp_img : img;
    Cell *cells = convert_image(src, img_w, img_h, &opt, &out_cols, &out_rows);
    free(sharp_img);
    stbi_image_free(img);

    fprintf(stderr, "img2ans: rendering %dx%d cells -> '%s'\n", out_cols, out_rows, out_file);

    /* Validate format/mode combos */
    if (opt.format == FMT_BIN && opt.color_mode != COLOR_16) {
        die("BIN format only supports 16-color mode");
    }

    /* Write output */
    FILE *f = fopen(out_file, "wb");
    if (!f) { fprintf(stderr, "img2ans: cannot open '%s' for writing\n", out_file); return 1; }

    if (opt.format == FMT_BIN) {
        save_bin(f, cells, out_cols, out_rows);
    } else if (opt.charset == CHARSET_PETSCII) {
        save_petscii(f, cells, out_cols, out_rows);
    } else {
        save_ansi(f, cells, out_cols, out_rows, opt.ice, &opt);
    }

    long file_size = ftell(f);

    if (opt.sauce) {
        write_sauce(f, out_cols, out_rows, opt.ice, file_size, &opt);
    }

    fclose(f);
    free(cells);
    free(g_err);

    fprintf(stderr, "img2ans: done (%ld bytes)\n", file_size);
    return 0;
}
