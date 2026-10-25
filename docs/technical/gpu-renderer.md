# GPU renderer (F5-7, D-022, D-023)

Design for the GPU back end of the Windows window. The software renderer (`Graphics::render_scene` in
`port/src/gfx.cpp`) stays as the reference and the fallback (D-018). The design is written down here before any
code is. Everything below about the current code is **confirmed** by reading `gfx.cpp` and `win_main.cpp` on
2026-10-22. Everything about the GPU side is a **plan** until it is measured.

## What exists today

- The machine thread hands a `Scene` (a back-to-front list of `DrawItem`s) to `draw_thread` in `win_main.cpp`.
- `draw_thread` calls `render_scene`. That applies `view_of` (script units to screen pixels; on Vita
  `VIEW_UNIT = 2`) and rasterises into a 24-bit 960x544 frame in four horizontal bands. It then swaps R and B and
  hands the frame to `WM_PAINT`.
- `WM_PAINT` letterboxes the frame into the client area with GDI `StretchDIBits`: `COLORONCOLOR` at whole
  multiples and `HALFTONE` otherwise.
- Measured on 2026-10-20 (`AMG_RENDERBENCH`): median 6.3 ms and worst 13.1 ms per frame at 960x544. The cost of
  `StretchDIBits` has not been measured.

## Decisions taken here

1. **API: Direct3D 11 (D-023).** It ships with every Windows 10/11, needs no new dependency, has the WARP software
   device for running tests on machines without a GPU, and DXGI gives flip-model presentation with vsync and
   borderless full screen. The back end sits behind a small interface, so OpenGL or Vulkan can be added if a
   non-Windows port ever exists.
2. **Same input as the software renderer.** The GPU draws the list that `view_of` returns, item by item, in order.
   No reordering or batching across items changes the result: batching is only allowed between consecutive items
   with the same texture, blend and clip.
3. **Two stages.**
   - Stage 1 draws the scene into an off-screen target of `k * 960` by `k * 544`. `k = 1` now; `k` is the internal
     render scale of F5-3.
   - Stage 2 draws that target into the swap chain, letterboxed. Linear filtering is used when the size is not a
     whole multiple, nearest when it is.
   - This replaces `StretchDIBits`.
4. **The software path stays selectable.** `--renderer software|gpu`, default `gpu`. The window falls back to
   software by itself if device creation fails, and says so in the title bar.

## How each `DrawItem` field maps

| Field / behaviour in `draw()` | On the GPU |
|---|---|
| Quad from `centre`, `scale`, `rect`, `angle`, `scale_on_screen` | The vertex shader builds the 4 corners from the same formulas: `half_w`/`half_h` and the `ux, uy, vx, vy` basis. Per-item constants go in a constant buffer. |
| Texel lookup `u, v` in `[0, rw) x [0, rh)` offset by `rx, ry` | Texture coordinates in texels, divided by the texture size in the shader. |
| Bilinear "clamped to the sprite's rectangle" (`lo_u..hi_u`) | Point-sample four texels in the pixel shader and clamp each index to `[rx, rx+rw-1]`. This is the same arithmetic as the CPU path. A hardware sampler would bleed across neighbouring sprites in a sheet. |
| Texture alpha `0x80` = opaque (`a = tex.a * alpha * 2`) | Same in the shader: `a = saturate(tex.a * 255 / 128 * alpha)`. |
| `light` multiply, clamped at 255 | `rgb = saturate(tex.rgb * light)`. |
| No image: plain rectangle of colour `light`, alpha `alpha` | Same shader with a flag and no texture. |
| Normal blend `src*a + dst*(1-a)` | Blend state `SRC_ALPHA` / `INV_SRC_ALPHA`. |
| `additive`: `dst + src*a` clamped | Blend state `SRC_ALPHA` / `ONE`. The unorm target clamps. |
| `clipped` with `clip` in screen pixels | Scissor rectangle (times `k`). |
| Edge rounding of `x0..x1` (`floor(v + 0.5)`) | Rasterisation covers pixel centres, which gives the same coverage. |
| 1:1 fast path that skips bilinear | Needs nothing: bilinear at exact texel centres gives the same value. |
| `snow` (`paint_snow`) | One quad per flake. The pixel shader computes the box coverage `cx * cy` analytically and blends toward white with `k = a * 0.8 * coverage` (blend `SRC_ALPHA` / `INV_SRC_ALPHA` with white). |

Expected differences: the GPU computes in float, the CPU in 8-bit fixed point (`>> 8`). That gives ±1 or 2 levels
per channel at some pixels. This is why D-022 says to compare with a threshold, not byte for byte.

## Textures

- An `Image` is RGBA8, uploaded as `DXGI_FORMAT_R8G8B8A8_UNORM` in its own texture.
- **Images can change after creation.** `NaviObject::mark` writes into its `work` image (`gfx.cpp`, map marks),
  and actor canvases are pasted together. So the cache cannot key on the pointer alone. Plan: add
  `uint32_t version` to `Image` and bump it at every write site (about 15, listed by
  `grep "px\[" port/src/gfx.cpp`).
  - Cache map: `Image*` → `{weak_ptr<Image>, version, texture}`.
  - Re-upload when the version differs.
  - Drop entries whose `weak_ptr` expired, checked once per frame.
- Images in a `Scene` snapshot are shared with the machine thread, so the draw thread only reads them. Making
  `version` atomic is enough to stay correct across threads.
- Upload cost is unknown. Measure it in step 3 with a counter of uploaded bytes per frame in the title bar.

## Present and timing

- Flip-model swap chain (`DXGI_SWAP_EFFECT_FLIP_DISCARD`), two buffers, `Present(1, 0)`: vsync on by default.
  Still to decide: an "uncapped" switch for testing.
- The draw thread still takes the newest `Scene` and skips stale ones, so a slow GPU never slows the game (the same
  rule as now).
- Resizing the window resizes the swap chain (`ResizeBuffers` on `WM_SIZE`), not the off-screen target.
- F9 screenshot: copy the off-screen target to a staging texture and read it back, so `build/shot.png` keeps the
  game's own size.

## Verification (step 4)

- `amagami.exe --renderer-compare`: every frame is drawn by both back ends, and the per-frame maximum and mean
  absolute difference are written to stderr in one compact line.
- Pass threshold proposed: mean under 1 level, and maximum under 8 outside edges. To be confirmed on the
  reference scenes of F4-22 played on Vita data.
- The tests in WSL keep using the software renderer. The GPU comparison runs on Windows, with WARP if needed.

## Status

- **Step 3a done 2026-10-22 (confirmed):** `port/src/d3d_present.cpp` (`amg::D3DPresenter`) puts the CPU frame in a dynamic texture and draws it into a flip-model swap chain, letterboxed by the viewport, with `Present(1, 0)` (vsync), linear sampling at every size (it was point sampling at whole multiples until 2026-10-23; the default filter, Smooth, is bilinear, and only Pixels samples the nearest texel), `WM_SIZE` resizes the swap chain. `amagami.exe` uses it by default; `--renderer software` or a failed device creation keeps the GDI path. Checked on the owner's machine: the window shows the game at 1540x845 with black side bars, 60 game fps and 60 shown fps, drawing 7.7 ms (the wait for vsync is not counted in that number). The scene is still drawn by the CPU; steps 3b onward are not started.
- **Steps 3b, 3c and 4 done 2026-10-22 (confirmed on Vita data):** `present_scene` draws the scene on the GPU: one scissored full-screen triangle per `DrawItem` with the pixel shader mapping each pixel back into the sprite's rectangle (the arithmetic of `draw()`), four `Load`s clamped to the sprite's rectangle and mixed bilinearly, alpha with the 0x80 convention, normal and additive blend states, clip as a scissor, snow flakes by analytic coverage. Textures are uploaded the first time an image is seen (immutable, keyed by the `Image` pointer plus a weak pointer: a changed picture is always a new `Image`, so no version number was needed) and dropped when the image is freed. It is the default; `--renderer present` keeps the CPU drawing with the GPU presenting, `--renderer software` the GDI path. **Comparison** (`AMG_GPU_COMPARE=1`, stderr): every 120th scene is also drawn by the CPU; over the logos, the title, the main menu and the name entry (items 1 to 162) the mean difference was 0.0 to 0.98 levels of 255, the largest 4, and 0.000 % of the values differed by more than 8. That meets the threshold proposed below (mean under 1, maximum under 8). Second run (confirmed, 2026-10-22): 170 s of scripted presses through the title, the new game, the name entry and the prologue (86 comparisons, up to 188 items): worst mean 0.58 levels, worst maximum 3, 0.000 % of values over 8. Not yet compared: light balls, snow, the mahjong table, the map.
- Not measured yet: upload cost per frame, GPU use, behaviour in full screen and with several monitors.

## Work split

| Step | What | Who |
|---|---|---|
| 3a | `Image::version` and the write sites; `--renderer` flag; the D3D11 device, swap chain and present path (stage 2 only, still drawing the CPU frame as a texture) | Sonnet |
| 3b | Stage 1: shaders, per-item constants, blend states, scissor, the texture cache | Sonnet, with this table as the specification |
| 3c | Snow | Sonnet |
| 4 | `--renderer-compare` and the threshold run on the reference scenes | Sonnet, then CPU |
| 5 | Internal scale `k` (F5-3) | Opus for the design of what changes with `k` (clip, snow, text) |

Step 3a already pays for itself: GPU scaling and vsync replace `StretchDIBits`, even before the scene is drawn on
the GPU.

## Render scale, screen shape and filter (2026-10-23)

Window options on the GPU path, rows of the game's Settings screen (docs/technical/input.md) and config.txt keys, each with an environment override:
- **Render scale** (`render`, `AMG_RENDER`: auto, 1 to 4). The scene texture is `k` times the frame; each item's centre grows by `k`, the map back to the sprite's texels shrinks by `k`, scissors and snow flakes scale likewise. Sprites are sampled bilinearly at the new resolution (confirmed by running at 3x; no pixel comparison yet). Auto picks the window's height over the frame's, rounded up, at most 4. `read_scene` (the comparison with the software frame) works at 1 only: compare with `AMG_RENDER=1`.
- **Screen shape** (`aspect`, `AMG_ASPECT`): fit (bars), fill (crops), stretch, whole (largest whole multiple of the frame).
- **Screen filter** (`filter`, `AMG_FILTER`): smooth, sharp bilinear (pixel shader from the coordinate derivatives), pixels.

## Interface size (2026-10-23)

`UI_SCALE` (gfx.hpp; `uiscale` in config.txt, `AMG_UI_SCALE` in percent, on both back ends) shrinks the interface relative to the picture, in `view_of`. Interface = items of layers from `UI_FIRST_LAYER` (4, `AMG_UI_LAYER`) up that do not cover the screen (height under 0.9 of it, and not a wide panel over 0.6 of it). **Hypothesis from two screens** (event: backgrounds 0, characters 2, text window 7, text 8; settings: 0 and 4 up): the layer rule may need exceptions elsewhere. One anchor per screen: the middle horizontally; vertically the bottom (top) edge when the **area-weighted centre** of the interface lies in the lower (upper) third, else the middle. The first version took the extent (highest and lowest edge) of every interface item, and a few small stray items (markers at the left edge of an event scene) stretched it to the middle, so the text window shrank towards the middle of the screen instead of staying at the bottom (found 2026-10-23 on `n1ako_se01C` at 70 and 50 %, confirmed fixed there; the settings screen at 70 and 50 % is pixel-identical before and after). A per-item anchor was tried first and tore menus apart (rows and their clip rectangles drifted differently). Checked at 70 and 80 % on an event's text window and on the settings screen. Not done: real widescreen (F5-4/F5-5), where the background would also fill.

