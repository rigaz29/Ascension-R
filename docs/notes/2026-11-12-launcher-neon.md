# The launcher, 8-bit and neon (2026-11-12)

- **What:** `port/launcher.cpp` drawn in the look picked from four mockups: deep purple and gold, 8-bit bevels,
  magenta and cyan neon, and the precompile screen's scene moving in the header. First the Play tab and the widgets
  every tab shares; the other tabs' layouts and the keyboard window come after.
- **Fonts:** Silkscreen (headings, buttons, captions) and Pixelify Sans (the rest), unchanged from google/fonts, in
  `third_party/silkscreen/` and `third_party/pixelify-sans/` with their `OFL.txt`. `app.rc` carries them as RCDATA
  101 to 103 and `fonts_load` reads them from memory into a collection of their own (IDWriteFactory5, Windows 10 1709
  on); without it the text is Segoe UI. `check_repo.py` turns fonts away and lets these three in by path and SHA-256
  (`FONTS`). A release puts the two licences in `licenses/` and names them in `THIRD-PARTY.txt`.
- **Sharp pixel fonts:** a Silkscreen dot is an eighth of its size, so 8, 16 and 24 DIPs are whole pixels at 100%,
  150% and 200%; there it is drawn aliased (`sharp`), and every text starts on a whole device pixel (`text`). At 125%
  16 DIPs are 20 pixels and come out a little soft. Pixelify Sans is drawn smoothed at 14 and 15.
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
- **Looking at it:** `GOWA_LAUNCHER_SHOT=<folder>` draws the tabs into PNG files, the scene standing still;
  `GOWA_LAUNCHER_SHOT_DPI` sets their DPI (120 by default).
- **Also:** on Windows 11 the title bar takes the window's colours (DWMWA_BORDER_COLOR, _CAPTION_COLOR, _TEXT_COLOR;
  Windows 10 keeps its dark one); the header's chip of the internal resolution follows Internal resolution; segmented
  buttons are as wide as their longest name; a pressed button is drawn sunk.
