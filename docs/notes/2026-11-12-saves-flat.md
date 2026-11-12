# Saves with no folder of their own (issue #10, 2026-11-12)

- **What the reporter saw:** a Ryzen 7 8700F and an RX 9070 XT, the 0.1.0-alpha.7 Kit. After playing into the second
  level the launcher said "No saves yet" and the game found no save, while the save folder held 36 files with no
  folder of their own: a profile's DAT-1 to DAT-31 and DATFEST, the settings save's SAVEDATA, and ICON0.PNG,
  PARAM.SFO and PARAM.PFD that the two share names for.
- **Their log:** `AutoSave2(version=1, dir='BCUS98232-SYSDATA')`, then from the same call `AutoSave(version=1,
  dir='')`, `SAVE dir=''` and `backup of '': 36 file(s)`. All five of its saves, three of BCUS98232-SYSDATA and two of
  BCUS98232-PROFDATA-0, went into the save folder itself. AutoLoad2, which translates its name once, looked for the
  right folders and found none.
- **The cause:** cellSaveDataAutoSave2 translated dirName from a guest address to a host pointer and handed that to
  cellSaveDataAutoSave, which translated it again. `savedata_host_str` takes a value below 4 GB for a guest address.
  Windows reserved the guest's memory below 4 GB on their PC, so the name's host pointer read as a guest address,
  2 to 4 GB into the guest's memory, a page of zeros: an empty name. Here the reservation was above 4 GB, the pointer
  was left as it was, and the name, at guest 0x940920 in the game's data, came through.
- **The fix:** fork `88ac02a4`: AutoSave2 hands on the guest's address. `tools/test_savedata_autosave2.py` maps the
  guest's memory at 0x10000 and checks the name the stat callback gets: `TEST00002` with the fix, an empty one
  without it. And the port reserves the guest's memory top down (`alloc_guest_vm`), above 4 GB on every PC, as on
  those it was played on: cellRtc too takes a value below 4 GB for a guest address.
- **Tried:** the temple replay twice with the reservation top down, at 0x7FF3B5DC0000: the settings save went into
  BCUS98232-SYSDATA, nothing loose, no error. A reservation below 4 GB could not be had on this PC (nothing there is
  free for 4 GB), so the test above is what shows the fix where it went wrong.
- **Their saves:** the folder as it is holds the profile's and the settings save's files mixed, and of the three they
  share, whichever was written last.
