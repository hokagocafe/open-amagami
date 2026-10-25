# Images

Confidence levels: **confirmed**, **hypothesis**, **pending**. See [disc.md](disc.md).

## TIM2 as used by the game (confirmed)

All 2,848 images in `GRAPH0.ARC`, `GRAPH1.ARC` and `GRAPH2.ARC` are TIM2 files of one narrow kind:

- version 4, 16-byte alignment, one picture per file, no mipmaps;
- a 48-byte picture header;
- indexed pixels, with a 32-bit palette.

| Archive | Images | Pixels | Palette | Notes |
|---|---|---|---|---|
| `GRAPH0.ARC` | 747 | 8-bit | Declared, but **not stored in the file** | Interface art; colours come from `GRAPH0.CLT` |
| `GRAPH0.ARC` | 2 | 4-bit | 64 colours | The two fonts (`font/…`), 672×4384 |
| `GRAPH1.ARC` | 1,164 | 8-bit | 256 colours | All 640×448: backgrounds and event pictures |
| `GRAPH2.ARC` | 935 | 8-bit | 256 colours | Character sprite sheets, up to 512×512 |

### Layout

| Offset | Size | Field |
|---|---|---|
| `0x00` | 4 | Magic `TIM2` |
| `0x04` | 1 | Version (4) |
| `0x05` | 1 | Alignment (0) |
| `0x06` | 2 | Picture count (1) |
| `0x10` | 4 | Total size of the picture |
| `0x14` | 4 | Palette size in bytes |
| `0x18` | 4 | Pixel data size in bytes |
| `0x1C` | 2 | Header size (48) |
| `0x1E` | 2 | Palette colours |
| `0x20` | 1 | Picture format |
| `0x21` | 1 | Mipmap count (1) |
| `0x22` | 1 | Palette type (3: 32-bit) |
| `0x23` | 1 | Pixel type (4: 4-bit indexed, 5: 8-bit indexed) |
| `0x24` | 2 + 2 | Width, height |
| `0x28` | 24 | Graphics chip register values |
| `0x40` | — | Pixel data, then the palette |

### Details that matter

- **Palette order.** For 8-bit images the 256 entries are stored in the PS2's shuffled order: within every block of 32, entries 8 to 15 and 16 to 23 are swapped.
- **Alpha.** `0x80` means opaque. A few images (18 in `GRAPH1.ARC`, 3 in `GRAPH2.ARC`) carry alpha `0xFF` in some entries.
- **4-bit pixels.** The low nibble of each byte is the left pixel.
- **`GRAPH0.ARC` images have no palette.** Their header declares one, but the file ends after the pixels.

## `GRAPH0.CLT` (confirmed for the entry order)

8,192 bytes: eight 256-colour palettes of 4 bytes per entry, shared by the `GRAPH0.ARC` interface images.

- Rendering one image (`idou/calendar_base.tm2`) with each palette gives six coherent colour themes of the same picture; the other two palettes are plain white-on-black. So a palette is a theme, chosen at run time, not a property of the image.
- The entries are in the same shuffled order as the palettes inside TIM2 files. With that order the title screen's sprites get a transparent background; in plain order it comes out as an opaque grey box.
- The texture table in the executable gives a palette number for every interface texture (see [objects.md](objects.md)).
- Compared with the emulator on the action map, the title and the menus, every texture appears with the palette its table record names (palette 0 for the confirm bar, whose text is red, blue and pink, and palette 1 for the calendar). No runtime theme choice has been seen so far, so the earlier idea of a theme chosen at run time stays a hypothesis.

## Character sprites: sheets and `.zb` layouts (confirmed)

Each `stand/<name>x.tm2` in `GRAPH2.ARC` is a sheet with the picture cut into rectangles packed together. Its companion `stand/<name>z.zb` says how to put them back. It is a list of 32-byte records with no header:

| Offset | Type | Field |
|---|---|---|
| `0` | `f32`, `f32` | Destination x, y (top-left corner, in a canvas shared by all of a character's files) |
| `8` | `f32`, `f32` | Source x, y on the sheet |
| `16` | `f32`, `f32` | Width, height |
| `24` | `u32` | Group: 0, 1, 2 or 3 |
| `28` | `u32` | Always 0 |

All 927 sheet/layout pairs with known names assemble without a record reaching outside its sheet, and the assembled pictures look right.

File name prefixes (confirmed for `b`, `f` and `x`):

| Prefix | Contents |
|---|---|
| `stand/b…` | Bodies, with no face. Group 0 is the body; the few records in groups 1 and 3 are the blush and the sweat drop |
| `stand/f…` | Face parts for a body: group 1 and group 2 are the two eyes, group 3 the mouth, each with 9 faces × 3 frames |
| `stand/x…` | Whole pictures with no separate face, 512×512, all in group 0 |
| `stand/e…` | Other standing pictures |

Bodies and faces share one coordinate space, wider than what is shown: bodies reach x = 673 and y = 966. The engine shifts every record by an offset from the scripts, (−164, −180) for characters, and shows the 512×768 that remain. Which records are drawn at a given moment, and where the result goes on screen, is in [objects.md](objects.md).

`tools/hokago.py sprite compose` draws every record in file order, which gives a correct picture for bodies and an overlay of all variants for faces.

## Other files

- `mc_icon/amagami.ico` in `GRAPH0.ARC` (24,678 bytes) is the only non-TIM2 entry: by its name, the memory card icon. Format pending.

## Upscaling pilot (measured on 5 textures)

`tools/hokago.py upscale` exports a TIM2 to RGBA, pads it 8 px by replicating the edge, runs it through `realesrgan-ncnn-vulkan` (model `realesr-animevideov3`, x2), scales alpha separately with Lanczos, and crops the padding. Output goes to `assets/upscaled/x2/<ARC>/<HASH>[_p<palette>].png` (untracked).

| Texture | Size | Time |
|---|---|---|
| `GRAPH1` background | 640×448 → 1280×896 | 13.9 s |
| `GRAPH2` sheet | 512×512 → 1024×1024 | 6.2 s |
| `GRAPH2` sheet | 512×256 → 1024×512 | 3.6 s |
| `GRAPH0` interface, palette 0 | 96×72 and 112×88 | 2.1 s each |

Hardware: Ryzen 5 3450U, Vega iGPU through Vulkan, no dedicated GPU. Extrapolated for the whole set: about 6 h (hypothesis, not run). Visually (confirmed on one sheet) the model gives cleaner lines than nearest or Lanczos.

Things the pilot does not settle:

- **Palette variants.** A `GRAPH0` image is one TIM2 shown with one of eight `GRAPH0.CLT` palettes; each palette in use needs its own upscaled file (hence the `_p<N>` suffix). Only the palettes the texture table names are needed.
- **Colour under transparent texels** (confirmed). The palette stores pure black under every transparent texel (100 % in a face sheet checked). Fed to the model as is, that black bleeds into the sprite's edge: on two sheets the rim luminance was 27.8 and 45.4 against 78.8 and 77.9 once the transparent texels take the nearest opaque colour. `tools/hokago.py upscale` now does that fill (`--no-bleed` turns it off); the model sees colour only, alpha is scaled apart.
- **Denoise level** of waifu2x cunet (n = -1, 0, 1) changes the result by 0.1–0.2 on a 0–255 scale: not a useful knob.
- **Bitmap fonts** (`font/…`, 4-bit, 672×4384) are not covered; they probably need a different model or none.

### Integration in the port (design, not implemented)

The port draws into a fixed 640×448 frame that Windows stretches to the window (`win_main.cpp`), and samples textures in the table's source coordinates (`gfx.cpp`, `paint` path). So an upscaled texture alone changes nothing visible. Two parts are needed:

1. **Internal render scale `k`.** Frame of `640k × 448k`; screen-space geometry and text multiplied by `k`; `StretchDIBits` source size follows. Software cost grows by `k²`.
2. **Texture scale.** `Image` gets a `scale` field; the sampler multiplies `rx, ry, rw, rh` and the clamp bounds by it. Loader order: `assets/upscaled/x<k>/…` first, original TIM2 otherwise. The port has no PNG reader, so the pipeline should add a raw RGBA cache (`.rgba`: w, h, pixels with alpha in 0..128) next to the PNG, or the port takes a single-header decoder.

Pixel-exact comparison against the emulator (D-016) keeps using `k = 1` and the original textures.

## Tool

```sh
python tools/hokago.py tim2 to-png   picture.tm2 picture.png
python tools/hokago.py tim2 to-png   icon.tm2 icon.png --clut extracted/GRAPH/GRAPH0.CLT --palette 0
python tools/hokago.py tim2 from-png picture.png picture.tm2
python tools/hokago.py tim2 verify   extracted/GRAPH/GRAPH1.ARC

python tools/hokago.py sprite info    layout.zb
python tools/hokago.py sprite compose sheet.tm2 layout.zb sprite.png
```

The PNG is 8-bit indexed: it keeps the pixel indices and the palette, and stores the original TIM2 header in a text chunk. Converting back is therefore exact, and `verify` confirms it for all 2,848 images. An image edited in a program that keeps the palette and that text chunk converts back with the edit applied; resizing is not supported.

Images without a palette are exported in greyscale unless `--clut` is given.
