## Download

**[open-amagami-windows-x86.zip](https://github.com/hokagocafe/open-amagami/releases/download/TAG/open-amagami-windows-x86.zip)** (in *Assets* below), for Windows 10 and 11. Nothing else to install.

You need **your own copy** of *Amagami ebKore+* for PS Vita (`PCSG00291`), as a decrypted folder of files, or a Vita3K export containing `app/PCSG00291`. The launcher accepts `eboot.bin` or a prepared `extracted/eboot_decrypted.elf`. Nothing of the game is in this download.

## What's new

- The launcher and importer recognize Vita3K exports with an optional `patch/PCSG00291` overlay.
- Audio checks accept packed files, extracted voices and prepared WAVs; missing sound categories are named instead of treating every layout as broken.
- The port reads the Vita3K app executable for its engine tables and applies mapped voice entries from `VOICE_P.CPK` when present.

## How to play

1. Unpack the zip anywhere (right-click, *Extract All...*).
2. Run `amagami.exe`, press **Choose game folder...** and pick the folder of your copy. Let it copy the game; required checks must pass. An unlisted version may show a warning.
3. Press **Start game**.

Windows may warn that the program is not recognised: press **More info**, then **Run anyway**.

Updating: unpack the new zip over the old one; saves, settings and your shortcuts are kept. From v0.1.5 on, the launcher tells you when a new version is out. More help in the [README](https://github.com/hokagocafe/open-amagami#readme).

The zip's SHA-256 is in `SHA256SUMS.txt`, built by this repository's workflow from the tagged source.

---
