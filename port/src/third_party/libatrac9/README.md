# LibAtrac9

Alex Barney's ATRAC9 decoder (the C implementation), from https://github.com/Thealexbarney/LibAtrac9 at commit
`efca2e3af35562a09a9bb6deed90e45b4b824dc4`. Licence: MIT, in `LICENSE`.

The sources are the repository's `C/src/*.c` and `*.h`, unchanged except for one edit in `libatrac9.h`: the
`DLLEXPORT` macro is defined empty (upstream exports it for a DLL, or imports it on MSVC), because the decoder is
built into the program as plain functions. The port uses it for the PS Vita release's music and ambience
(`port/src/at9.cpp`), which ship as ATRAC9 in RIFF files inside `MUSIC.CPK` and `SOUND.CPK`; before, `tools/vita_audio.py`
converted them with ffmpeg.
