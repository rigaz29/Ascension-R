# The stats panel (2026-11-10)

`display.show_fps = overlay` drew a small panel in the top left corner. It is now a larger panel of dark glass in the top
right corner, with:

- GPU load; video memory in use and how much the card has; the whole machine's CPU load; RAM in use and installed;
- PPU, SPU and RSX load; frame rate and mean frame time;
- a graph of the last 96 frame times, 0 to 66.7 ms;
- the GPU and CPU names, what scales and antialiases the picture, the internal and output resolution;
- where Kratos is.

The panel is as wide as its longest line, so the names of the hardware are shortened: trademarks go, and so does what
follows the model (` with Radeon Graphics`, ` CPU @ 3.70GHz`, `6-Core Processor`).

## Where Kratos is

A creature's controller is at `+0x1304`, and its first word says what it is (`cheats.cpp`, `goPlayer` for Kratos). A
temporary probe walked the title's heap, `0x70000000` to `0x80000000`, for a creature with a `goPlayer` controller and a
plausible health, wrote 8 KB of it every half second, and looked in the title's data for what pointed at it.

- Two globals point at him in a level: `0x009BC3D0` and `0x009C1830`. The other hits were on the stack.
- `+0x10` to `+0x4F` of the creature is his world matrix: three unit rows, then the position at `+0x40`, `+0x44`, `+0x48`
  and a `1.0`. It goes from `0, 0, 0` to the spawn point and then moves with the stick.

`position_line` in `cheats.cpp` follows either global and checks the controller. Between levels the pointer may be stale,
so it asks `VirtualQuery` before reading, and the line shows dashes until he is there. Read on the bridge and in the
CYN050 scene.

## What it costs

Text is drawn with GDI at twice the size and box-filtered down: at 1/60 of the window's height GDI's whole-pixel advances
spaced the letters unevenly. The text changes four times a second and the graph ten times.

| Where it is painted | Mean per update on the presenting thread | Worst |
|---|---|---|
| On the presenting thread | 2.8 to 3.6 ms in the level | 11.3 ms |
| On a thread of its own, with the presenting thread only copying the image | 0.08 to 0.15 ms | 0.65 ms |

One run of the bridge route each, 300 updates a line. The panel is painted into two images in turn, and a lock covers only
the swap and the copy.
