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
typedef struct { uint8_t ch, attr; } Cell; /* attr = (bg<<4)|fg */

typedef enum { METRIC_RGB, METRIC_REDMEAN, METRIC_YCBCR } ColorMetric;
typedef enum { DITHER_NONE, DITHER_FS, DITHER_ATKINSON, DITHER_ORDERED, DITHER_JJN } DitherMode;
typedef enum { PAL_VGA, PAL_WIN } PaletteKind;

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

/* Nine glyphs: space, shade (B0/B1/B2), full block, four half-blocks */
typedef struct { uint8_t ch; uint8_t rows[16]; } FontGlyph;

static const FontGlyph FONT_GLYPHS[] = {
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
    /* 0xDC lower half - rows 7-15 filled */
    { 0xDC, { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,
              0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF } },
    /* 0xDD left half - left 4 pixels */
    { 0xDD, { 0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,
              0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0 } },
    /* 0xDE right half - right 4 pixels */
    { 0xDE, { 0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,
              0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F } },
    /* 0xDF upper half - rows 0-6 filled */
    { 0xDF, { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,
              0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } },
};

#define NUM_GLYPHS  (int)(sizeof(FONT_GLYPHS)/sizeof(FONT_GLYPHS[0]))

/* Precomputed glyph cache */
static GlyphInfo g_glyphs[9];   /* matches NUM_GLYPHS */
static int       g_num_glyphs = 0;

static void init_glyphs(void) {
    g_num_glyphs = NUM_GLYPHS;
    for (int i = 0; i < NUM_GLYPHS; i++) {
        g_glyphs[i].ch = FONT_GLYPHS[i].ch;
        memcpy(g_glyphs[i].rows, FONT_GLYPHS[i].rows, 16);
        int cnt = 0;
        for (int row = 0; row < 16; row++) {
            uint8_t b = FONT_GLYPHS[i].rows[row];
            while (b) { cnt += (b & 1); b >>= 1; }
        }
        g_glyphs[i].on_count = cnt;
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
    return (Cell){ best_ch, attr };
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

    /* Determine output dimensions */
    int cols = opt->cols > 0 ? opt->cols : COLS;
    int rows;
    if (opt->rows > 0) {
        rows = opt->rows;
    } else {
        /* Auto: preserve aspect ratio. Cell aspect is 8px wide x 16px tall.
         * img px -> cells: cols*8 px wide maps to img_w px, so scale factor is cols*8/img_w.
         * Height in cells: img_h * (cols*8/img_w) / 16, simplified:
         *   rows = img_h * cols / (img_w * 2)   (the /2 accounts for 2:1 cell aspect)
         */
        rows = (img_h * cols) / (img_w * 2);
        if (rows < 1) rows = 1;
        if (rows > MAX_ROWS) rows = MAX_ROWS;
    }

    *out_cols = cols;
    *out_rows = rows;

    /* Cell size in source image pixels */
    double cw = (double)img_w / cols;
    double ch = (double)img_h / rows;

    /* Allocate output */
    Cell *cells = malloc((size_t)(cols * rows) * sizeof(Cell));
    if (!cells) { fprintf(stderr, "img2ans: out of memory\n"); exit(1); }

    /* Dither error buffer (for error-diffusion modes) */
    if (opt->dither == DITHER_FS || opt->dither == DITHER_ATKINSON || opt->dither == DITHER_JJN) {
        err_alloc(cols, rows);
    }

    for (int cy = 0; cy < rows; cy++) {
        for (int cx = 0; cx < cols; cx++) {
            /* Collect 16x8 subpixel samples for glyph matching */
            RGB samples[16][8];
            double ox = cx * cw;
            double oy = cy * ch;
            for (int py = 0; py < 16; py++)
                for (int px = 0; px < 8; px++)
                    samples[py][px] = sample_pixel(img, img_w, img_h, ox, oy, cw, ch, px, py, opt->gamma_correct);

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
            Cell cell = best_cell(samples, pal, opt->ice, opt->metric);
            cells[cy * cols + cx] = cell;

            /* Propagate dithering error (error-diffusion modes only) */
            if (opt->dither == DITHER_FS || opt->dither == DITHER_ATKINSON || opt->dither == DITHER_JJN) {
                /* Compute the approximate RGB that this cell renders */
                int fg = cell.attr & 0x0F;
                int bg = (cell.attr >> 4) & 0x0F;
                const GlyphInfo *g = NULL;
                for (int gi = 0; gi < g_num_glyphs; gi++) {
                    if (g_glyphs[gi].ch == cell.ch) { g = &g_glyphs[gi]; break; }
                }
                RGB approx;
                if (!g || g->on_count == 0) {
                    approx = pal[bg];
                } else if (g->on_count == 128) {
                    approx = pal[fg];
                } else {
                    /* Blend: approximate rendered average as weighted avg */
                    double alpha = (double)g->on_count / 128.0;
                    approx.r = (uint8_t)clamp_byte((int)(alpha * pal[fg].r + (1-alpha) * pal[bg].r + 0.5));
                    approx.g = (uint8_t)clamp_byte((int)(alpha * pal[fg].g + (1-alpha) * pal[bg].g + 0.5));
                    approx.b = (uint8_t)clamp_byte((int)(alpha * pal[fg].b + (1-alpha) * pal[bg].b + 0.5));
                }
                RGB avg = avg_samples(samples);
                diffuse_error(cx, cy, avg, approx, opt->dither);
            }
        }
    }

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
    s.file_type    = 1;  /* ANSI */
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
        "  --sauce        embed SAUCE metadata record\n"
        "  --title STR    SAUCE title (implies --sauce)\n"
        "  --author STR   SAUCE author (implies --sauce)\n"
        "  --group STR    SAUCE group (implies --sauce)\n"
        "  --help         show this help\n"
        "\n"
        "output defaults to input filename with .ans extension.\n",
        prog);
}

static void die(const char *msg) {
    fprintf(stderr, "img2ans: %s\n", msg);
    exit(1);
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

    /* Build default output name: replace/append .ans */
    static char out_buf[4096];
    if (!out_file) {
        strncpy(out_buf, in_file, sizeof(out_buf) - 5);
        char *dot = strrchr(out_buf, '.');
        char *slash = strrchr(out_buf, '/');
        if (dot && (!slash || dot > slash)) *dot = '\0';
        strncat(out_buf, ".ans", sizeof(out_buf) - strlen(out_buf) - 1);
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
    init_glyphs();
    if (opt.gamma_correct)
        init_gamma_tables();

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

    /* Write output */
    FILE *f = fopen(out_file, "wb");
    if (!f) { fprintf(stderr, "img2ans: cannot open '%s' for writing\n", out_file); return 1; }

    save_ansi(f, cells, out_cols, out_rows, opt.ice, &opt);

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
