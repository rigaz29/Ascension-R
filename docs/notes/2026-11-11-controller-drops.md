# A controller that dropped off the USB bus (2026-11-11)

The player reported that with the controller API on Auto or XInput the buttons did nothing, and that SDL3 worked.

## What the logs and Windows show

- The controller is a wired Xbox 360 pad of another maker: USB VID 0284, PID 028C, "Controller (XBOX 360 For
  Windows)", on XInput slot 0. SDL3 names it "XInput Controller #1".
- Windows' Kernel-PnP log (Device Management, event 1010): "surprise removed as it is reported as missing on the
  bus" at 19:04:12 and again at 19:04:39. Every instance of the device last arrived at 19:17:57.
- The session started at 19:02 (Auto): XInput read the pad, buttons and sticks, until 88 s on the pad clock; then
  nothing for 13 s, and XInput read it again from 101 s on. That is the drop at 19:04:12 and its return, with no help.
- The sessions started at 19:04 (Auto) and 19:06 (XInput) came after the second drop: XInput had no controller, and
  the keyboard fallback took the A and S keys the player pressed instead (Square, Triangle).
- The session started at 19:10 (SDL3): SDL3 found the controller at 39.5 s on the pad clock, 19:17:58, the second
  Windows saw the device arrive.

Both APIs read the pad whenever it was on the bus. The drops were the pad's own (its cable, the port, or the pad), and
the SDL3 session was the one it came back in.

## What changed

- `cellPad.c` logs XInput controllers as they connect and disconnect, per port, as it does SDL3's. The 19:04 and
  19:06 logs said nothing about the pad; the next one will.
- The keyboard fallback's line says "no controller there", not "no XInput device": it is the same with SDL3.
