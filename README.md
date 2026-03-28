# img2ans

Convert images (PNG, JPEG, BMP) to ANSI art, PETSCII art, or BIN format.

Supports classic 16-color VGA, 256-color xterm, and 24-bit truecolor output
with multiple dithering algorithms, resampling methods, and character sets.

```
img2ans [options] input.{png,jpg,bmp} [output.ans]
```

## Options

```
--cols N         Width in character columns (default: 80, PETSCII: 40)
--rows N         Height in character rows (default: auto)
--palette vga    Color palette: vga (default) or win
--ice            Enable iCE colors (16 background colors)
--colors MODE    Color mode: 16 (default), 256, or 24bit
--dither MODE    Dithering: none, fs (Floyd-Steinberg, default), atkinson,
                 ordered (Bayer 4x4), jjn (Jarvis-Judice-Ninke)
--metric MODE    Color metric: rm (redmean, default), rgb, ycbcr
--gamma          Gamma-correct resampling (linear light averaging)
--no-gamma       Disable gamma correction (default)
--sharpen N      Unsharp mask strength (try 0.5-2.0)
--glyphs SET     Glyph set: standard (default) or extended
--charset C      Character set: ansi (default) or petscii
--format FMT     Output format: ans (default) or bin
--resample M     Resampling: box (default) or lanczos
--sauce          Embed SAUCE metadata record
--title STR      SAUCE title (implies --sauce)
--author STR     SAUCE author (implies --sauce)
--group STR      SAUCE group (implies --sauce)
--help           Show this help
```

## Examples

```sh
# Basic 80-column ANSI art
img2ans photo.png photo.ans

# Wide output with iCE colors and extended glyphs
img2ans --cols 132 --ice --glyphs extended photo.jpg wide.ans

# 256-color with Lanczos resampling for maximum sharpness
img2ans --colors 256 --resample lanczos photo.png photo.ans

# 24-bit truecolor (best quality, requires modern terminal)
img2ans --colors 24bit --dither atkinson photo.png photo.ans

# Gamma-correct with sharpening
img2ans --gamma --sharpen 1.5 photo.png photo.ans

# Ordered (Bayer) dithering for a retro halftone look
img2ans --dither ordered photo.png photo.ans

# PETSCII output for Commodore 64
img2ans --charset petscii --cols 40 --rows 25 photo.png photo.pet

# BIN format for PabloDraw / TheDraw
img2ans --format bin photo.png photo.bin

# SAUCE metadata
img2ans --title "My Art" --author "SysOp" --group "ACiD" banner.png banner.ans

# Kitchen sink: everything on
img2ans --cols 80 --ice --colors 256 --dither atkinson --metric ycbcr \
    --gamma --sharpen 1.0 --glyphs extended --resample lanczos \
    --sauce --title "Photo" --author "Me" photo.png photo.ans
```

## Color Modes

| Mode    | Colors | SGR codes        | Best for |
|---------|--------|------------------|----------|
| `16`    | 16     | Standard SGR 30-47 | BBS, classic terminals |
| `256`   | 256    | `\e[38;5;N` / `\e[48;5;N` | xterm, modern terminals |
| `24bit` | 16M    | `\e[38;2;R;G;B` / `\e[48;2;R;G;B` | truecolor terminals |

256-color mode uses the xterm-256 palette (216 color cube + 24 grayscale)
with a top-8 nearest neighbor search per cell. 24-bit mode analytically
computes the optimal FG/BG color per glyph with no palette constraint.

## Dithering

| Mode       | Algorithm | Character |
|------------|-----------|-----------|
| `fs`       | Floyd-Steinberg | Smooth gradients, default |
| `atkinson` | Atkinson | Preserves highlights, less banding |
| `jjn`      | Jarvis-Judice-Ninke | Wide diffusion kernel, smooth |
| `ordered`  | Bayer 4x4 | Retro halftone / stipple look |
| `none`     | No dithering | Sharpest but may show banding |

All dithering operates at cell granularity - error is diffused between
output cells, not individual pixels.

## Glyph Sets

### Standard (default)

| Character | Name | Coverage |
|-----------|------|----------|
| `0x20` | Space | 0% |
| `0xB0` | Light shade | 25% |
| `0xB1` | Medium shade | 50% |
| `0xB2` | Dark shade | 75% |
| `0xDB` | Full block | 100% |
| `0xDC` | Lower half | 56% |
| `0xDD` | Left half | 50% |
| `0xDE` | Right half | 50% |
| `0xDF` | Upper half | 44% |

### Extended (`--glyphs extended`)

Adds 20 additional CP437 glyphs: box-drawing, diagonals, triangles,
geometric shapes, and fills at various coverage percentages for finer
tonal resolution in photographic images.

## PETSCII Mode

`--charset petscii` enables Commodore 64 output:

- C64 16-color palette (VICE canonical values)
- 8x8 character cells (vs 8x16 ANSI)
- Square pixel aspect ratio (auto-rows preserves 1:1)
- 13 PETSCII graphics glyphs: blocks, quadrants, checkerboard, triangles
- Output uses PETSCII control codes (color bytes, RVS ON/OFF)
- Default 40 columns (C64 screen width)
- `.pet` file extension

## BIN Format

`--format bin` writes raw character+attribute byte pairs with no ANSI
escape sequences. Used by PabloDraw, TheDraw, and similar ANSI editors.
16-color only. SAUCE `file_type=5` when `--sauce` is used.

## Resampling

| Mode | Method | Speed | Quality |
|------|--------|-------|---------|
| `box` | Area average | Fast | Good (default) |
| `lanczos` | Lanczos-3 sinc | Slower | Sharper edges |

Lanczos-3 pre-resizes the source image to the exact pixel grid using a
separable sinc-windowed filter. Best for high-resolution source images
where sharpness matters.

## Image Quality Tips

- **Sharpen first:** `--sharpen 1.0` recovers detail lost to downsampling
- **Gamma correct:** `--gamma` produces more perceptually accurate results
- **Lanczos resampling:** `--resample lanczos` for sharp edges
- **Extended glyphs:** `--glyphs extended` for more tonal options
- **256 or 24-bit color:** dramatic improvement over 16-color
- **Atkinson dither:** better highlight preservation than Floyd-Steinberg
- **Try ycbcr metric:** `--metric ycbcr` for more perceptually uniform matching

## Build

### CMake (recommended)

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

### Make

```sh
make
sudo make install
```

Requires a C99 compiler and standard math library. No other dependencies -
image loading via [stb_image](https://github.com/nothings/stb).

## SAUCE

When `--sauce` is enabled, a 128-byte [SAUCE](https://www.acid.org/info/sauce/sauce.htm)
metadata record is appended to the output file. SAUCE records store title,
author, group, dimensions, and file type for compatibility with ANSI viewers
and BBS software.

## License

GPL-3.0-or-later
