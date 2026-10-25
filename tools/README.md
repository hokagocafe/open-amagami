# Tools

The part of the project's shared tools that the repository needs.

- `hokago/enterbrain/scf_src.py`, `scf.py`, `scf_asm.py`, `lzss.py`: read and write the engine's script classes. The classes of our own
  in `port/script` are written as `.scfs` source and compiled with them:

      python -m hokago scf_src enc port/script/PortSwitch.scfs port/script/PortSwitch.scf

  (run from `tools`, or with `tools` on `PYTHONPATH`; the language is described in `docs/technical/scf-source.md`). The compiler reads
  the game's own archives to resolve a class's parent (see its usage), which is why the compiled `.scf` files are in the repository
  too: building the program does not need any of this.
- `hokago/art/make_button_art.py` redraws the button pictures in `port/skins`; `make_splash.py` draws the splash picture. They need
  `resvg-py` and `Pillow`, and the M PLUS Rounded 1c font (see `licenses/`).

`python -m hokago` lists the tools.
