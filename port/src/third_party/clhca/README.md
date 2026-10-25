# clHCA

CRI HCA decoder, unmodified, from vgmstream (`src/coding/libs/clhca.c`, `clhca.h`, `clhca_data.h`) at commit
`f7f764155b227f683adbab25402fe8f2be0b2ec2`. Licence: vgmstream's ISC-style licence, in `COPYING`.

It is an independent decoder (decompiled and re-reverse-engineered by its authors, see the header of
`clhca.c`), not CRI's code. The port uses it for the PS Vita release's voices (`port/src/hca.cpp`).
