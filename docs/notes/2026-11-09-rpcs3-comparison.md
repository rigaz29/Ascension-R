# RPCS3 against the port in the first fight (2026-11-09)

Port `0190a52` (fork `ad3336e8`), RPCS3 0.0.43-20572 with the player's settings (Vulkan, 720p, PPU and SPU on LLVM, VSync off). Ryzen 5 4600H,
GTX 1650 Ti, 16 GB, plugged in.

| | Runs | Median of each run | Mean of the 10 s windows | Lowest window |
|---|---|---|---|---|
| RPCS3, Disable MLAA | 3 | 22.4, 17.5, 19.6 | 19.6 | 13.9 |
| Port, FSR 1, default patches | 2 | 37.6, 27.8 | 33.7 | 27.0 |
| Port, FSR 1, Disable MLAA only | 2 | 25.5, 26.8 | 25.5 | 21.5 |

FPS from RPCS3's window title, flips/s from the bench: the same thing, the game's frames.

The port's default patches turn off MLAA, motion blur and dynamic shadows, 1.72 times RPCS3's rate. That is not the same picture: RPCS3 has no patch
for the shadows. With the same patch on both, Disable MLAA alone, the port runs 1.30 times RPCS3's rate.

## Method

- Both on the same copy of the saves: Continue Game of Profile 1, the Megaera fight. Windows from 70 to 130 s after the load's thread starts, Cross
  24 s after it, Square every 3 s, as `gowa.py bench` drives the port. RPCS3 was driven the same way by posted key messages, and the load's
  thread read from its log.
- The fight has to stay where it is on both sides. In RPCS3 a test patch made every hit do nothing: `fmr f31,f1` at 0x87460 in
  func_00087440, a creature's damage, became `fsub f31,f1,f1`. The port ran with `cheats.damage_taken=0` and a temporary probe making
  Kratos's hits do nothing as well. Screenshots at 75 and 125 s show the same phase in all three.
- Port: FSR 1 from 720p to 1080p, render scale 1, no frame generation, VSync off, SMAA as in the player's settings.
- RPCS3 ran from a copy of its program, settings, firmware and caches, with a junction to the game, so the install was only read: its log, its settings
  and the game folder's listing were compared before and after. RPCS3 will not start from a temporary folder.

## How much to trust it

The spread is large. The boss and the camera differ from run to run, so a median moves by 20% or more: 17.5-22.4 for RPCS3, 27.8-37.6 for the port.
Taken run against run, the port with the default patches is 1.24 to 2.15 times RPCS3, and with Disable MLAA alone 1.14 to 1.53 times. No RPCS3 run
reached the slower of the like-for-like port runs. One scene, one PC.

Four more port runs before the enemies' damage was turned off, where the fight moved on to its next phase, gave medians of 31.3 and 33.8 with the default
patches and 27.2 and 25.6 with Disable MLAA alone: the same picture.

The lowest free RAM during an RPCS3 run was 3.4 GB of 15.7.

On 2026-10-31 the same fight was 24.0 FPS in RPCS3 and 16.3 flips/s in the port.
