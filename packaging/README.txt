Ascension-R @VERSION@ @RELEASE@

A native Windows port of God of War: Ascension (PS3, NPUA80918, version 1.12), made by static recompilation.
https://github.com/rigaz29/Ascension-R

This download holds none of the game's assets. You need your own copy of the game and its license.
This is an alpha: it has not been played from start to finish.


WHAT YOU NEED

- Windows 10 or 11, 64-bit.
- A CPU with AVX2 and FMA, and a GPU with Direct3D 12.
@KIT_NEEDS@
- Your copy of the game, the digital US release NPUA80918, updated to version 1.12. The folder of an RPCS3 install
  (dev_hdd0\game\NPUA80918) works, and so does an unpacked PKG. It has to be the whole folder: the launcher checks
  the game's files before it decrypts or builds anything, and says which are missing.
- The game's license file (.rap). If the game is installed in RPCS3, the launcher finds it.
- RPCS3, once, to decrypt your copy of the game. If you do not have it, the launcher offers to download the official
  build and removes it afterwards. RPCS3 needs the Microsoft Visual C++ Redistributable (x64).


FIRST START

1. Unpack this whole folder anywhere and start Ascension-R.exe.
2. In the System tab, pick the game's folder with Browse, or let Auto detect find it.
3. If the game still has to be decrypted, the same row says what is missing. Choose rpcs3.exe or press Download RPCS3,
   and choose the .rap file if it asks for one. Your game's folder and your RPCS3 are only read.
4. Press Play. The first start of each version builds the game's vertex shaders on a screen of its own, a minute or two
   the first time. The other shaders are built the first time they are needed, so expect some stutter at first.


CONTROLLERS

Xbox controllers and other XInput pads, and through SDL3 the DualShock 4, the DualSense, Switch controllers and many
others. A DualShock 3 needs a driver such as DsHidMini. "Controller API" in the Controls tab chooses what reads them;
with DS4Windows running, set it to XInput, or the same controller counts twice.


@KIT@
WHERE YOUR FILES GO

Settings, saves, shaders and logs are in %LOCALAPPDATA%\Ascension-R. To keep them in this folder instead, put an empty
file named portable.txt beside Ascension-R.exe.


TO UPDATE

The launcher says when a new version is out, with a link to its page: it asks GitHub at most every six hours, and
Check for updates, in its System tab, turns that off.

Unpack the new version into a new folder and start it from there; once it runs, the old folder can go. Unpacking it
over the old folder works too. Your settings, saves and shaders, and what the launcher made from your game, are in
%LOCALAPPDATA%\Ascension-R (see WHERE YOUR FILES GO), and the new version takes them from there: do not delete that
folder, your saves are in it. With portable.txt they are in this folder instead: unpack the new version over it.
@KIT_UPDATE@


IF SOMETHING GOES WRONG

"Report a problem" in the launcher makes a zip of the logs, with your paths hidden and no game files. Attach it to an
issue at https://github.com/rigaz29/Ascension-R/issues


LICENSES

See the licenses folder, and THIRD-PARTY.txt in it first.
