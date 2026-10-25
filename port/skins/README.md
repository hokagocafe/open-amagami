# Button pictures

`ps/`, `xbox/` and `kbd/`: the button glyphs of our own art, as PNG at four times the size of the game's own button sprites.
Drawn from scratch by `tools/hokago.py make_button_art` (hokago-tools), with the letters from M PLUS Rounded 1c (SIL OFL 1.1);
nothing of the game's art is in them. The port reduces them to the sprites' size and shows the ones of the controller in use
(docs/technical/input.md). To change one, change the generator and run it again:

    python tools/hokago.py make_button_art OUT --font MPLUSRounded1c-ExtraBold.ttf
    copy OUT/png/ps, OUT/png/xbox, OUT/png/kbd here

`ps_white` (white symbols, as the game's) is not copied: the port does not use it yet.

`keycap.ttf`: the letters of the keyboard keys' labels, a subset of M PLUS Rounded 1c ExtraBold (SIL Open Font Licence 1.1,
https://github.com/google/fonts/tree/main/ofl/mplusrounded1c): Latin letters, digits and common punctuation only.
