# The launcher, 8-bit and neon (2026-11-12)

- **What:** `port/launcher.cpp` drawn in the look picked from four mockups: deep purple and gold, 8-bit bevels,
  magenta and cyan neon, and the precompile screen's scene moving in the header. First the Play tab and the widgets
  every tab shares, then the other tabs; the keyboard window comes last.
- **Fonts:** Silkscreen for headings, buttons and captions, unchanged from google/fonts, in `third_party/silkscreen/`
  with its `OFL.txt`. `app.rc` carries it as RCDATA 101 and 102 and `fonts_load` reads it from memory into a collection
  of its own (IDWriteFactory5, Windows 10 1709 on). `check_repo.py` turns fonts away and lets these two in by path and
  SHA-256 (`FONTS`); a release puts the licence in `licenses/` and names it in `THIRD-PARTY.txt`. The rest of the text
  is Windows' own Bahnschrift at 14 and 15 (Segoe UI where it is missing). The mockup's Pixelify Sans was dropped: on
  its 5x7 grid 2, 5, S and Z share their outlines, and at 15 DIPs (1.4 pixels a dot) a 5 read as an S, "24%" as "84%"
  and G as B, smoothed or not, and its fi and fl ligatures as an A.
- **Sharp pixel fonts:** a Silkscreen dot is an eighth of its size, so 8, 16 and 24 DIPs are whole pixels at 100%,
  150% and 200%; there it is drawn aliased (`sharp`), and every text starts on a whole device pixel (`text`). At 125%
  16 DIPs are 20 pixels and come out a little soft.
- **The header:** `gowa_scene_frame` (`loading_screen.cpp`) draws the screen's 640x360 scene at a time, and the launcher
  copies it into a bitmap 30 times a second (timer 3). The header shows the part of it that fills its width with each
  scene pixel k x k device pixels, k the smallest that covers the width: 2 at 100% and 125%, 3 at 150%; the right edge
  is the scene's, where the temple stands. The title is "ASCENSION-R" in the screen's pixel font as the banner has it,
  a bitmap of a pixel a dot drawn at whole pixels a dot. The game's PIC1.PNG and ICON0.PNG are no longer read: the
  sidebar shows the program's icon.
- **Layers:** all but the scene is drawn into a layer (`layer_draw`) only when something changes (`redraw`); a frame is
  the scene, the layer over it, the glow and the scanlines (`frame_draw`), so the moving scene costs little more than
  the scene itself. What glows is listed while the layer is drawn (`glow_rect`, `glow_text`, `glow_bitmap`), drawn into
  a layer of its own, blurred at 3 and 11 DIPs (Direct2D's Gaussian blur, through ID2D1DeviceContext, Windows 8 on) and
  added over the frame. `glow_hole` keeps it off the gold buttons, which it washed out to white. The scanlines, a dark
  line every third device pixel or so, and the darker corners are one bitmap, made for each size.
- **Pixels:** fills, frames and bevels are snapped to device pixels (`snap`, `SR`, `thick`). Icons are 16x16 dots, a dot
  a DIP, filled aliased: whole pixels at 100% and 200%; at 150% a two-dot line is 3 pixels and a one-dot detail 1 or 2.
- **Retro effects:** `[launcher] effects`, a switch on the Display tab, on by default: the glow and the scanlines. The
  scene stands still while the window is minimised or when Windows' "Show animations" is off.
- **Cost:** one 10 s measurement of the window left open on the Play tab, on AC power, 12 threads: 0.67 s of CPU, 6.7%
  of one core or 0.56% of the processor, and a 55 MB working set. Most of it is the scene, drawn whole each frame.
- **The other tabs:** the tab's icon at twice its size beside its name in the header; segmented buttons as wide as their
  longest name; the open list framed in gold with a hard shadow; the decrypting's buttons as wide as their names; a toast
  a badge in the bar, green or red, with its glow. Where the view cuts a card that scrolls, an edge is drawn and the
  rows fade into it (opaque for its first quarter, so no stroke of a letter shows through); the rows are clipped under
  the edge; the scrollbar is a frame with a thumb.
- **Looking at it:** `GOWA_LAUNCHER_SHOT=<folder>` draws the tabs into PNG files, the scene standing still, and the
  states that are hard to reach by hand: a list open, a search, the end of a scrolled tab, a toast, the System tab
  asking for what the decrypting needs, a precompile under way, and in the Kit the game's code being built (on the Play
  and System tabs) and not built yet. Each state is set only while its shot is drawn. `GOWA_LAUNCHER_SHOT_DPI` sets the
  DPI (120 by default). The Kit's exe for this: `GOWA_BUILD_DIR=build-kit gowa.py build -D PORT_GAME_MODULE=ON`, as
  `package kit` builds it.
- **Also:** on Windows 11 the title bar takes the window's colours (DWMWA_BORDER_COLOR, _CAPTION_COLOR, _TEXT_COLOR;
  Windows 10 keeps its dark one); the header's chip of the internal resolution follows Internal resolution; segmented
  buttons are as wide as their longest name; a pressed button is drawn sunk.
