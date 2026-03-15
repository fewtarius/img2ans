# img2ans

Convert images (PNG, JPEG, BMP) to ANSI art for telnet BBSs and terminals.

Uses the IBM VGA 8x16 character set - full block, half-block, and shade glyphs
with the 16-color EGA/VGA palette - to represent source images as CP437 ANSI art.

```
img2ans [options] input.{png,jpg,bmp} [output.ans]
```

## Options

```
--cols N       Width in character columns (default: 80)
--rows N       Height in character rows (default: auto)
--palette vga  Color palette: vga (default) or win
--ice          Enable iCE colors (16 background colors)
--dither MODE  Dithering: none, fs (Floyd-Steinberg, default), atkinson
--metric MODE  Color metric: rm (redmean, default), rgb, ycbcr
--sauce        Embed SAUCE metadata record
--title STR    SAUCE title (implies --sauce)
--author STR   SAUCE author (implies --sauce)
--group STR    SAUCE group (implies --sauce)
--help         Show this help
```

## Examples

```sh
# Convert to 80-column ANSI art
img2ans photo.png photo.ans

# Wider output with iCE colors
img2ans --cols 132 --ice photo.jpg wide.ans

# Embed SAUCE metadata
img2ans --title "My Art" --author "SysOp" --group "ACiD" banner.png banner.ans

# No dithering, Atkinson dither
img2ans --dither none logo.png logo.ans
img2ans --dither atkinson photo.png photo.ans
```

## Output

Writes a CP437 ANSI art file with minimized SGR escape sequences and `\r\n`
line endings. Optionally appends a [SAUCE](https://www.acid.org/info/sauce/sauce.htm)
metadata record for compatibility with ANSI viewers and BBS software.

## Glyphs

| Character | Name        | Coverage |
|-----------|-------------|----------|
| `0xB0`    | Light shade | 25%      |
| `0xB1`    | Medium shade| 50%      |
| `0xB2`    | Dark shade  | 75%      |
| `0xDB`    | Full block  | 100%     |
| `0xDC`    | Lower half  | 56%      |
| `0xDD`    | Left half   | 50%      |
| `0xDE`    | Right half  | 50%      |
| `0xDF`    | Upper half  | 44%      |
| `0x20`    | Space       | 0%       |

For shade glyphs (B0/B1/B2), the renderer uses a perceptual blend model - the
eye integrates the stipple into an average color, so the blend is compared
against the cell average rather than per-pixel.

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

## License

GPL-3.0-or-later
