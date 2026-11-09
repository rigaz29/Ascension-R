# What a player's session leaves behind, and the problem report (2026-11-09)

Before the repository goes public. Everyone builds the port on their own PC, so the problems worth hearing about are the ones
that do not happen here.

## What was already recorded

The session log (`logs\session-*.log`, five kept), the settings, the GPU's name, a crash's exception and stack, and the pad
recording beside the log. The crash at the CYN050 cutscene was found from those.

## What was missing

- A freeze. `hang_watchdog` only runs with `GOWA_WATCHDOG` set, so a game that hung and was closed by force left a log that just stops.
- A session that stops with no exception. Nothing looked at how the last session ended.
- Windows version, CPU, memory, driver version, which commits the build came from, the game's version.
- A way to send any of it. A stack is offsets against the player's own `GowARecomp.map`, and the log's paths hold the Windows
  account name (17 times in one short log).

## What was added

`report.cpp`, no fork change.

**`[system]` lines** at the start of a session's log: the build (`git describe` of both repositories, compiler, the exe's link
stamp), Windows, CPU, memory, each GPU with its driver, the display, plugged in or on battery, the game's id and version.
`CMakeLists.txt` gives the commits to `report.cpp` alone, so a commit does not rebuild the port.

**The freeze watch.** No new guest flip for 15 s (`GOWA_FREEZE_WATCH=<seconds>`, 0 = off), once the first frame has come: the HLE
call each guest thread is inside, its `lr`, `r3`, `sp`, and each host thread's name, CPU over a second and stack as module+offset.
Once more at 60 s. `[freeze] frames again after N s` when they resume. A long load only costs some lines in the log. Each thread is
suspended only to read its stack; nothing that allocates or prints runs while it is. It runs in a session with a log, or with
the variable set, so a script's runs and the benches are as they were.

**How a session ended** is read back from its log: crashed, lost the graphics device, froze, stopped with no reason in the log,
ended normally. Closing the console now leaves a `[session] ended` line. A launcher that opens after one of the first four offers a
report, once: the launcher's own session then becomes the last one.

**Report a problem** in the sidebar, or `GowARecomp --report [--with-saves]`. A zip in `logs\`, and a dialog to open the issue page
or show the file. Nothing is sent. In it:

| | |
|---|---|
| `report.txt` | the `[system]` lines, whether `GOWA.elf` is the one the port is lifted from, how each session ended |
| `logs/` | the last three sessions the game ran in (first 256 KB and last 8 MB of each) with their pad recordings |
| `settings.ini` | the settings |
| `windows_events.txt` | Windows' Application Error and Application Hang records of this exe, two weeks |
| `save/` | only with the box ticked, without `.png`, `.pam`, `.at3` |

Offsets in the game's own code get their function name from the map, if the log's link stamp is this exe's. Paths from the settings
and the environment become `<hdd0>`, `<exe>`, `<home>` and so on, any other `X:\Users\<name>` becomes `<user>`, and the account and
PC names are hidden as whole words. `missed_code.txt` stays out: its rows start with 16 bytes of the game's code.

The zip uses DEFLATE with the fixed codes, about 60 lines and no new dependency. A session's log and pad recording go to a sixth or
better, and 13.7 MB of saves to 137 KB.

`.github/ISSUE_TEMPLATE/problem.yml` is the page it opens.

Not taken from Soulbrandt: the in-game menu and reporting from inside a frozen game, its runtime snapshot of SPU history, and a
screenshot. The save is opt-in here, not always included.

## Verification

- Reports from copies of the real logs, with and without saves: `zipfile.testzip` passes, and the account name, the PC name and the game's folder appear nowhere outside the saves.
- Three real sessions, started as a player would, each judged correctly by the report. `ppu main` suspended from outside: `[freeze]` after 15 s with
  named frames (`ppu_gcm_pump+0x30B < ps3_hle_call+0x1AB < func_0049D5E8+0x303`), `frames again` after it was resumed, then suspended again and
  killed: "froze". Killed with no freeze: "stopped with no reason". Window closed: "ended normally". The older crash log: "crashed".
- The launcher through UI Automation on a copy of the settings: the offer after the frozen session, saves ticked, the zip made, the ready dialog,
  the sidebar button (Cancel), and no offer at the next start.

Not tested: pressing Open GitHub and Show the file, a real hang rather than a suspended thread, and a non-ASCII account name.
The issue page exists once the repository is public.
