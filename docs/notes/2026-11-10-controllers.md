# Controllers through XInput and SDL3 (2026-11-10)

Until now the Windows build read controllers through XInput alone: Xbox controllers and the PC pads in XInput mode. A
DualShock 4, a DualSense or a Switch controller needed DS4Windows, Steam Input or the like. SDL 3 now reads the
controllers XInput does not, beside it.

## How it works (`libs/input/cellPad.c` in the fork)

- With SDL3's headers given to the build (`PS3RECOMP_SDL3_INCLUDE_DIR`, which `gowa.py build` sets from `../sdl3`),
  `cellPad.c` is compiled with `PS3RECOMP_PAD_SDL3`. Without them it is what it was.
- `SDL3.dll` is loaded at run time, by its full path beside the exe, with what it imports from there or System32.
  Nothing is linked: without the DLL, every controller is XInput's as before.
- `input.api` (Controller API in the Controls tab):
  - `auto`, the default: XInput's controllers through XInput, at their slot (slot n is port n), and the others
    through SDL3 on the ports left free, the lowest first, and the same one back for a controller that left.
  - `xinput`: SDL3 is not loaded.
  - `sdl3`: every controller through SDL3, Xbox ones too.
- An XInput controller, to SDL3: its XInput driver's path, `XInput#<slot>`, or a HID path with Microsoft's `IG_` in it.
  The hints keep XInput's controllers on that driver: RawInput, Windows.Gaming.Input, GameInput and HIDAPI's Xbox
  driver are off, as SDL 3.4.20 has them by default on Windows, and stay off whatever a later SDL does.
- `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS` is on: SDL has no window of its own here and would take the program for one
  in the background and drop the input. SDL is polled, with its events off.
- Every controller goes through one function, `pad_fill`: the same round dead zone, `stick_full`, flick hold and
  trigger point as XInput's, so the evade works the same on any of them. SDL's Y axis is down positive; it is turned
  round first. The face buttons go by position: the bottom one is Cross, B on a Nintendo pad.
- Rumble goes to whatever reads the port: XInputSetState, or SDL_RumbleGamepad, renewed while it lasts.
- The launcher names the controller in the Play tab's ready check, as the Controller API chosen in the window would read
  it, saved or not (`ps3_pad_host_describe`: it only looks, opens nothing).

## Packaging

`SDL3.dll` 3.4.20, the official build from `SDL3-devel-3.4.20-VC.zip` (SHA-256
`1ec3009655ee37b1f73e52b3b120acc5afd8abaf9532f3303b1bf1dfe91b5969`), goes beside the exe in both releases, with its
license as `licenses/SDL3.txt` and a line in THIRD-PARTY.txt. It imports only System32's DLLs. The README has a
CONTROLLERS section.

## Found on the way

- `LoadLibraryExW(L"SDL3.dll", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR)` fails with error 126 although the file is
  there: that flag is where the DLL's own imports are looked for too, and SETUPAPI and the rest are in System32. By
  its full path with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32` it loads.

## Measured (Ryzen 5 4600H, no controller plugged in)

- Starting SDL's gamepads: 100 ms, once.
- `SDL_UpdateGamepads`: 1.06 us a call; `SDL_GetGamepads`: 0.12 us. A poll comes every 4 ms at most.
- One run of the Prebuilt to the fight: 35.2 frames a second, no crash, SDL3 loaded and the recorded inputs played.

## Not tested yet

- A real controller through either path. The user's Xbox controller will be tried in Auto (XInput) and in SDL3 (SDL's
  XInput driver, then this file's SDL path), with the sticks, the evade and the rumble.
- A DualShock 4, a DualSense or a Switch controller: none is here. Their SDL drivers differ, the path in this file
  does not.
- Whether the game reads ports other than the first. With an Xbox controller and a DualSense plugged in together in
  Auto, the Xbox one is the first port.
