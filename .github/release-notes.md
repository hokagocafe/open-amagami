## Download

**[open-amagami-windows-x64.zip](https://github.com/hokagocafe/open-amagami/releases/download/TAG/open-amagami-windows-x64.zip)** (in *Assets* below), for Windows 10 and 11. Nothing else to install.

You need **your own copy** of *Amagami ebKore+* for PS Vita (`PCSG00291`), decrypted: the game's folder or a Vita3K install, Japanese or with the English fan translation. Nothing of the game is in this download.

## What's new

- **Easier first start.** The launcher finds your copy of the game by itself (a Vita3K install, the Desktop, Downloads, the folders of the last run), takes a folder dropped on it, and says in plain words what is missing and what to do. Folders with accents or Japanese letters work.
- **The program is now 64-bit**, and has a portable mode (an empty `portable.txt` beside `amagami.exe`).
- **No dead ends.** A damaged or unreadable save is moved aside (a folder called `unreadable-...`) and the next start is clean. If the program fails to start it leaves a report and opens the launcher next time.
- **A new launcher:** a header with the status and the Start button always in view, and four tabs (Game files, General, Display, Controls), sharp at any Windows scaling. It and its messages come in English, Spanish and Japanese. They follow Windows; **Settings → Launcher language** chooses one.
- **All the program's options are in the game's own menu** (the last entry of the main menu, *Port options*) and in F8 over the game: window mode, screen filters, brightness, gamma, vertical sync and a frame counter, among fifteen in all.
- **The mouse works like the game's own cursor:** hover an entry and it lights at once, click chooses, right click goes back, the wheel moves; on the title, every menu, Settings, the name entry, the day map and the gallery, with the game's own arrow as the pointer.
- **Interface size** (50 to 100 %) keeps every panel where the game puts it instead of drifting, and a Continue with no save now says so in the game's own message box.
- **One-click update:** the launcher's *Get the new version* button can download, check and install the new version for you.
- **Fidelity fixes**, from reading the console version's own code: the mahjong's cut-in window, shaking is vertical only, the dark text colour, the density of snow and the sweat drop. The left stick follows the console's order on the day map and in the album.
- **Pad buttons can be remapped** (Settings → Controls..., a button next to each key: click it and press the pad's button; right click goes back to automatic), and **Pad mapping...** takes the mapping line of a pad the program does not know.
- **F8 opens the settings while you play**; the game waits until you close them.
- Copies from other decryption tools work: the executable can be a plain or compressed one, and you can pick a folder above the game's own.
- In the game's own Settings, the program's rows match the game's text size, and are in Japanese on the Japanese version.
- Far fewer hitches: the sound no longer drops out during the half second of work at the start, voices and backgrounds are prepared ahead, the game's first frames run behind the splash, and pictures that cost a frame are remembered for next time.

## How to play

1. Unpack the zip anywhere (right-click, *Extract All...*).
2. Run `amagami.exe`. If the launcher found your copy, press the big button; otherwise press **Choose game folder...** (or drop the folder on the launcher) and pick the folder of your copy. Let it copy the game; required checks must pass. An unlisted version may show a warning.
3. Press **Start game**.

Windows may warn that the program is not recognised: press **More info**, then **Run anyway**.

Updating: unpack the new zip over the old one; saves, settings and your shortcuts are kept. From v0.1.5 on, the launcher tells you when a new version is out. More help in the [README](https://github.com/hokagocafe/open-amagami#readme).

The zip's SHA-256 is in `SHA256SUMS.txt`, built by this repository's workflow from the tagged source.

---
