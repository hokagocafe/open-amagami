# Third-party software and fonts

The code of this project is under the MIT licence (see `LICENSE`). It includes, and is built with, the following. Each keeps
its own licence, shown here or in its folder.

| Component | What it is used for | Licence | Where |
|---|---|---|---|
| LibAtrac9 by Alex Barney | decoding the game's music (ATRAC9) | MIT | `port/src/third_party/libatrac9/` |
| clHCA from vgmstream | decoding the game's voices (CRI HCA) | ISC-style, see `COPYING` | `port/src/third_party/clhca/` |
| HE-VAG prediction filters from vgmstream (`psv_decoder.c`) | decoding the game's sound effects | ISC-style, the same licence as clHCA (`port/src/third_party/clhca/COPYING`) | `port/src/hevag_coefs.inc` |
| SDL3 (Simple DirectMedia Layer) | gamepads | zlib | `port/src/third_party/sdl3/` (the library is downloaded when building; the program's package carries `SDL3.dll`) |
| stb_image and stb_truetype by Sean Barrett | reading our PNG pictures, drawing the keyboard keys' labels | public domain or MIT | `port/src/third_party/stb/` |
| Tables and the 32-point transform of PL_MPEG by Dominic Szablewski (from kjmp2 by Martin J. Fiedler) | decoding MPEG audio layer II | MIT | `port/src/mpa_tables.inc`, `port/src/mpa_idct.inc` |
| M PLUS Rounded 1c (the Rounded M+ fonts) | the letters of our button pictures and key labels, and of the splash picture | SIL Open Font License 1.1 | `licenses/OFL-MPLUSRounded1c.txt`; `port/skins/keycap.ttf` is a subset of the font |

Copyright lines, as the components state them:

- LibAtrac9: Copyright (c) 2018 Alex Barney.
- clHCA / vgmstream: Copyright (c) 2008-2025 Adam Gashlin, Fastelbja, Ronny Elfert, bnnm, Christopher Snowhill, NicknineTheEagle,
  bxaimc, Thealexbarney, CyberBotX, EdnessP, et al, and the portions listed in `port/src/third_party/clhca/COPYING`.
- SDL3: Copyright (C) 1997-2026 Sam Lantinga.
- PL_MPEG: Copyright (c) 2019 Dominic Szablewski.
- M PLUS Rounded 1c: Copyright 2016 The Rounded M+ Project Authors.

*Amagami* and the names of its characters, companies and platforms belong to their owners; they are mentioned only to say what
this software is for.
