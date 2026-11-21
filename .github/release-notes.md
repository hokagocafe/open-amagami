## Download

**[open-amagami-windows-x64.zip](https://github.com/hokagocafe/open-amagami/releases/download/TAG/open-amagami-windows-x64.zip)** (in *Assets* below), for Windows 10 and 11. Nothing else to install.

You need **your own copy** of *Amagami ebKore+* for PS Vita (`PCSG00291`), decrypted: the game's folder or a Vita3K install, Japanese or with the English fan translation v1.1. Nothing of the game is in this download.

## What's new

- A new launcher, drawn with SDL3 (the older Windows one stays as a fallback, `launcher_ui=win32`). It has a light and a dark look (it follows the system; **General → Window colours** forces one), its window carries the game's own icon once the game is loaded, Ctrl+Tab switches tabs, Esc closes it, and keyboard and pad reach the game after it closes.
- A shortcut called *Amagami*, with the game's own icon, is left next to `amagami.exe` once the game is loaded, and **Game files → Create desktop shortcut** (or `amagami.exe shortcut`) puts one on the desktop. The icon inside the `.exe` itself cannot change.
- Everything now lives in a `portable-data` folder next to `amagami.exe`: saves, settings and the game copy. The first start copies what an earlier version left in `%APPDATA%` and `%LOCALAPPDATA%` and asks whether to delete the old folders (default No). Keep the program in a folder you can write to.
- The program works from a terminal without the launcher: `amagami.exe help` lists `status`, `check`, `settings`, `find`, `use`, `add-variant`, `delete-copy`, `shortcut`, `update` and `start`.

## How to play

1. Unpack the zip anywhere (right-click, *Extract All...*).
2. Run `amagami.exe`. If the launcher found your copy, press the big button; otherwise press **Choose game folder...** (or drop the folder on the launcher) and pick the folder of your copy. Let it copy the game; required checks must pass. An unlisted version may show a warning.
3. Press **Start game**.

Windows may warn that the program is not recognised: press **More info**, then **Run anyway**.

Updating: unpack the new zip over the old one (keep the `portable-data` folder); saves and settings are kept. The launcher tells you when a new version is out. More help in the [README](https://github.com/hokagocafe/open-amagami#readme).

The zip's SHA-256 is in `SHA256SUMS.txt`, built by this repository's workflow from the tagged source.

---
