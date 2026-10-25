# SDL3

Simple DirectMedia Layer, release 3.4.18 (https://github.com/libsdl-org/SDL), the Visual C++ development package: the
headers (`include/SDL3`), and the 32-bit import library and DLL (`lib/x86`), unmodified. Licence: zlib, in `LICENSE.txt`.

The port uses it for gamepads only (docs/technical/input.md, D-025). The program is built 32-bit, so only `lib/x86` is
kept; the x64 and arm64 libraries are in the same release package if a build for them is made.
