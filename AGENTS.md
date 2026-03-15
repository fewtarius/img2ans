# AGENTS.md - img2ans Technical Reference

**Version:** 1.0  
**Date:** 2026-03-14  
**Methodology:** See `.clio/instructions.md`

---

## Project Overview

**img2ans** converts raster images to CP437 ANSI art files. Single C99 source file,
no runtime dependencies beyond libc and `-lm`.

- **Source:** `img2ans.c` (~900 lines)
- **Headers:** `stb_image.h` (bundled, image loading)
- **Build:** CMake (primary), Make (secondary)
- **Output:** `.ans` files with optional SAUCE metadata
- **License:** GPL-3.0-or-later

---

## Quick Setup

```bash
# CMake (preferred)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Make (alternative)
make

# Install
sudo cmake --install build       # to /usr/local/bin + man1
sudo make install

# Syntax check (fast - no full build needed)
cc -std=c99 -Wall -Wextra -Wno-unused-parameter -fsyntax-only img2ans.c

# Debug build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
make debug
```

---

## Architecture

### Data Flow

```
Input image (PNG/JPG/BMP)
    |
    v stb_image (RGBA -> RGB strip alpha)
    |
    v convert_image()
    |   for each cell (cx, cy):
    |     1. sample_pixel() x128  <- area-average 8x16 subpixels from source
    |     2. Apply dither error from g_err[cy*cols+cx]
    |     3. best_cell()          <- full 16x8 palette search over all glyphs
    |     4. diffuse_error()      <- propagate quantization error to neighbors
    |
    v Cell array [rows * cols]
    |
    v save_ansi()         <- minimized SGR sequences + \r\n line endings
    |
    v write_sauce()       <- optional 128-byte SAUCE record at end of file
    |
    Output .ans file
```

### Key Types

| Type | Purpose |
|------|---------|
| `RGB` | 3-byte packed color `{r, g, b}` |
| `FRGB` | Float color for dither error accumulation |
| `Cell` | Output cell: `{ch, attr}` - CP437 char + VGA attribute byte |
| `GlyphInfo` | Glyph: `{ch, rows[16], on_count}` - bitmap + coverage count |
| `Options` | All CLI options in one struct |
| `DitherMode` | `DITHER_NONE`, `DITHER_FS`, `DITHER_ATKINSON` |
| `ColorMetric` | `METRIC_RGB`, `METRIC_REDMEAN`, `METRIC_YCBCR` |

### VGA Attribute Byte

```
Bit  7   6   5   4   3   2   1   0
     BG3 BG2 BG1 BG0 FG3 FG2 FG1 FG0

Without iCE: BG3=blink bit, BG 0..7
With iCE:    BG 0..15 (background colors 8-15 enabled)
```

### Glyph Set

| Char | Code | Coverage | Error model |
|------|------|----------|-------------|
| Space  | 0x20 | 0%  | solid BG |
| Light  | 0xB0 | 25% | coverage-blend |
| Medium | 0xB1 | 50% | coverage-blend |
| Dark   | 0xB2 | 75% | coverage-blend |
| Full   | 0xDB | 100%| solid FG |
| Lower  | 0xDC | 56% | pixel-by-pixel |
| Left   | 0xDD | 50% | pixel-by-pixel |
| Right  | 0xDE | 50% | pixel-by-pixel |
| Upper  | 0xDF | 44% | pixel-by-pixel |

**Shade glyphs** (B0/B1/B2) use a coverage-blend error model: the eye integrates
the stipple into a perceived average, so `cell_error` blends fg/bg by coverage
fraction and compares that single color against each sample. Block/half glyphs
use per-pixel matching (spatially coherent, so pixel-by-pixel is accurate).

### Cell Search (`best_cell`)

Full exhaustive search - no heuristic pruning:
- Solid candidates: space glyph x 8 (or 16) bg colors
- Block/shade candidates: 8 glyphs x 16 fg x 8 bg (= 1024 combos)
- ~1152 total candidates per cell
- ~1s for 79x39 output on modern hardware

### Dithering

Floyd-Steinberg and Atkinson operate at cell granularity:
1. Fetch accumulated error `g_err[cx,cy]` (float FRGB)
2. Apply error to all 128 subpixel samples before `best_cell()`
3. After `best_cell()`, compute approximate rendered average color
4. Diffuse `(avg - approx)` error to neighboring cells

### ANSI Output

SGR emitter tracks current `{fg, bg, bold, blink}` state and only emits
codes that change. Full reset (`\e[0m`) when bold or blink needs to turn off.
Line endings: `\r\n` (required for ANSI/BBS compatibility).

---

## Directory Structure

| Path | Purpose |
|------|---------|
| `img2ans.c` | Full implementation (single source file) |
| `stb_image.h` | Bundled image loader (Sean Barrett / MIT) |
| `CMakeLists.txt` | CMake build configuration |
| `Makefile` | GNU Make build configuration |
| `img2ans.1` | Man page |
| `README.md` | User-facing documentation |
| `LICENSE` | GPL-3.0 license text |
| `.github/workflows/build.yml` | CI: ubuntu + macos build matrix |
| `.clio/instructions.md` | CLIO project methodology |
| `AGENTS.md` | This file - technical reference |

### Source File Layout (`img2ans.c`)

Sections appear in this order:

1. **Constants** - `COLS`, `GLYPH_W`, `GLYPH_H`, CP437 char codes
2. **Types** - `RGB`, `FRGB`, `Cell`, `GlyphInfo`, `Options`, enums
3. **Font data** - `FONT_GLYPHS[]` bitmaps + `init_glyphs()` precompute
4. **Palettes** - `VGA16[]`, `WIN16[]`, `get_palette()`
5. **Color metrics** - `dist_rgb2`, `dist_redmean2`, `dist_ycbcr2`, `pal_dist2`
6. **Resampling** - `sample_region`, `sample_pixel` (area-average)
7. **Dither buffer** - `g_err[]`, `err_alloc`, `err_get`, `err_add`, `diffuse_error`
8. **Cell rendering** - `cell_error` (shade vs block model), `avg_samples`, `best_cell`
9. **Conversion** - `convert_image` (outer loop: sample + dither + search + propagate)
10. **ANSI output** - `save_ansi` (minimized SGR sequences, `\r\n` endings)
11. **SAUCE** - `write_sauce` (128-byte record, EOF marker 0x1A)
12. **main** - arg parsing, stb_image load, convert, write, SAUCE

---

## Code Style

**C Conventions:**

- C99 strict: `-std=c99 -Wall -Wextra -Wno-unused-parameter`
- No external dependencies - `stb_image.h` is the only bundled header
- Single source file - everything in `img2ans.c`
- All functions `static` (no public API surface)
- Brief comments describing *what*, not *why* - git history handles *why*
- No `// TODO` or `// FIXME` in committed code

**Naming:**

| Kind | Convention | Examples |
|------|-----------|---------|
| Types | `PascalCase` | `ColorMetric`, `GlyphInfo`, `DitherMode` |
| Functions | `snake_case` | `best_cell`, `cell_error`, `save_ansi` |
| Constants/macros | `UPPER_CASE` | `CP437_FULL`, `GLYPH_W`, `MAX_ROWS` |
| Global statics | `g_` prefix | `g_glyphs`, `g_err`, `g_num_glyphs` |

**Section headers:**

```c
/* -------------------------------------------------------------------------
 * Section Name
 * ---------------------------------------------------------------------- */
```

No dramatic annotations - no `/* CRITICAL FIX */`, no AI slop language.

---

## Testing

```bash
# Syntax check (fastest - use this first)
cc -std=c99 -Wall -Wextra -Wno-unused-parameter -fsyntax-only img2ans.c

# Full build
cmake --build build

# Smoke tests
./build/img2ans --help
./build/img2ans --cols 40 --rows 12 input.png /tmp/out.ans
./build/img2ans --sauce --title "T" --author "A" --cols 20 --rows 5 input.png /tmp/sauce.ans

# Verify SAUCE record
python3 -c "
d = open('/tmp/sauce.ans','rb').read()
idx = d.rfind(b'SAUCE')
r = d[idx:]
print('cols:', int.from_bytes(r[96:98], 'little'))
print('rows:', int.from_bytes(r[98:100], 'little'))
"

# CI smoke test generates its own PNG in pure Python
# See .github/workflows/build.yml for the full test script
```

**Before every commit:**
1. Zero compiler warnings (`-Wall -Wextra`)
2. `--help` prints without error
3. Converts a real image without crashing
4. If SAUCE code changed: verify SAUCE output with python3 snippet above

---

## Commit Format

```
img2ans: short description (imperative, <60 chars)

Longer explanation if needed. What changed, what was wrong before,
what's correct now. No filler.
```

**Examples:**
```
img2ans: add Atkinson dithering mode

Floyd-Steinberg diffuses 100% of error; Atkinson diffuses 6/8, which
retains more highlight detail and reduces edge ringing. New --dither
atkinson option. Defaults unchanged.
```

Always squash to one commit per logical change before pushing.
Do not push to origin without asking.

---

## Development Tools

```bash
# Quick syntax check
cc -std=c99 -Wall -Wextra -Wno-unused-parameter -fsyntax-only img2ans.c

# Build and run in one go
cmake --build build && ./build/img2ans --help

# Generate test PNG with Python (no ImageMagick needed)
python3 -c "
import struct, zlib
def png_chunk(name, data):
    crc = zlib.crc32(name + data) & 0xFFFFFFFF
    return struct.pack('>I', len(data)) + name + data + struct.pack('>I', crc)
w, h = 64, 64
raw = b''
for y in range(h):
    raw += b'\x00'
    for x in range(w):
        raw += bytes([int(x/w*255), int(y/h*255), 80])
compressed = zlib.compress(raw)
png = (b'\x89PNG\r\n\x1a\n'
       + png_chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
       + png_chunk(b'IDAT', compressed)
       + png_chunk(b'IEND', b''))
open('/tmp/test.png', 'wb').write(png)
print('wrote /tmp/test.png')
"

# Convert test image
./build/img2ans --cols 40 --rows 12 /tmp/test.png /tmp/test.ans

# Show output file info
wc -c /tmp/test.ans
xxd /tmp/test.ans | head -5
```

---

## Common Patterns

**Adding a new CLI option:**

```c
/* 1. Add field to Options struct */
typedef struct {
    // ...existing fields...
    int new_option;   /* description */
} Options;

/* 2. Set default in main() */
Options opt = { .cols = COLS, .new_option = 0, /* ... */ };

/* 3. Parse in the arg-loop */
} else if (strcmp(arg, "--new-option") == 0) {
    opt.new_option = 1;

/* 4. Use in conversion path */
```

**Adding a new color metric:**

```c
/* 1. Extend enum */
typedef enum { METRIC_RGB, METRIC_REDMEAN, METRIC_YCBCR, METRIC_NEW } ColorMetric;

/* 2. Implement distance function */
static inline long dist_new(RGB a, RGB b) { /* ... */ }

/* 3. Add case to pal_dist2 */
case METRIC_NEW: return dist_new(a, b);

/* 4. Add --metric new parsing in main() */
```

**Adding a new glyph:**

```c
/* 1. Add FontGlyph entry to FONT_GLYPHS[] */
{ 0xXX, { row0, row1, ..., row15 } },

/* 2. No other changes needed - init_glyphs() and best_cell() are generic */
```

**Verifying SAUCE record layout:**

```python
# SAUCE record is 128 bytes starting at "SAUCE00" magic
# Offsets from the SAUCE spec:
#   0-6:   ID "SAUCE00"
#   7-41:  title (35 bytes, space-padded)
#  42-61:  author (20 bytes)
#  62-81:  group (20 bytes)
#  82-89:  date (8 bytes YYYYMMDD)
#  90-93:  filesize (4 bytes LE)
#     94:  datatype (1 = character)
#     95:  filetype (1 = ANSI)
#  96-97:  width (LE uint16)
#  98-99:  height (LE uint16)
d = open('output.ans', 'rb').read()
s = d[d.rfind(b'SAUCE'):]
print('title:', s[7:42].rstrip(b' ').decode('ascii', errors='replace'))
print('cols:', int.from_bytes(s[96:98], 'little'))
print('rows:', int.from_bytes(s[98:100], 'little'))
```

---

## CLI Reference

```
img2ans [options] input.{png,jpg,bmp} [output.ans]

--cols N       Width in columns (default: 80)
--rows N       Height in rows (default: auto, preserves aspect at 2:1 cell ratio)
--palette vga  Palette: vga (default) or win
--ice          Enable iCE colors (16 background colors, no blink)
--dither MODE  none | fs (Floyd-Steinberg, default) | atkinson
--metric MODE  rm (redmean, default) | rgb | ycbcr
--sauce        Embed SAUCE record
--title STR    SAUCE title (implies --sauce, max 35 chars)
--author STR   SAUCE author (implies --sauce, max 20 chars)
--group STR    SAUCE group (implies --sauce, max 20 chars)
-h, --help     Print usage
```

Output defaults to `input.ans` when no output path given.

---

## Anti-Patterns (What NOT To Do)

| Anti-Pattern | Why It's Wrong | What To Do |
|--------------|----------------|------------|
| Split into multiple source files | Project goal is single-file simplicity | Keep everything in `img2ans.c` |
| Add external library deps | Zero deps is a design requirement | Use bundled stb_image only |
| Heuristic pruning in `best_cell` | Accuracy > speed for this tool | Keep full exhaustive search |
| Per-pixel dither (not per-cell) | Cell is the atomic output unit | Dither at cell granularity |
| `// TODO` in committed code | Signals incomplete work | Finish it or leave it out |
| Push without squashing | Clutters history | Squash to one commit per change |
| Push without asking | User policy | Always ask before push |

---

## CI / GitHub Actions

`.github/workflows/build.yml` builds on `ubuntu-latest` and `macos-latest` on every push to `main`.

Steps:
1. `cmake -B build -DCMAKE_BUILD_TYPE=Release`
2. `cmake --build build`
3. Smoke test: generates a 64x64 test PNG with pure Python, converts it, verifies SAUCE
4. Uploads binary as artifact

The CI test PNG is generated inline with Python's `struct` + `zlib` - no ImageMagick or other tools needed.

---

## Potential Future Work

- **BIN output** - raw 2-byte attr+char pairs (no ANSI escapes), used by PabloDraw
- **Palette from image** - k-means to derive a custom 16-color palette per image
- **Faster search** - precompute coverage-weighted palette blends to prune candidates
- **More glyphs** - diagonal fills, quadrant blocks (Unicode, but some BBS fonts have them)
- **Video input** - frame-by-frame for ANSI animations

---

## Quick Reference

```bash
# Syntax check
cc -std=c99 -Wall -Wextra -Wno-unused-parameter -fsyntax-only img2ans.c

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build

# Basic convert
./build/img2ans input.png output.ans

# All options
./build/img2ans --cols 80 --rows 40 --ice --dither atkinson --metric ycbcr \
    --sauce --title "My Art" --author "SysOp" --group "ACiD" input.png output.ans

# Verify SAUCE
python3 -c "
d = open('output.ans','rb').read()
s = d[d.rfind(b'SAUCE'):]
print(int.from_bytes(s[96:98],'little'), 'x', int.from_bytes(s[98:100],'little'))
"

# Git status before commit
git status && git diff
```

---

*For project methodology and workflow, see `.clio/instructions.md`*
