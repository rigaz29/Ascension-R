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
  (dev_hdd0\game\NPUA80918) works, and so does an unpacked PKG.
- The game's license file (.rap). If the game is installed in RPCS3, the launcher finds it.
- RPCS3, once, to decrypt your copy of the game. If you do not have it, the launcher offers to download the official
  build and removes it afterwards. RPCS3 needs the Microsoft Visual C++ Redistributable (x64).


FIRST START

1. Unpack this whole folder anywhere and start Ascension-R.exe.
2. In the System tab, pick the game's folder with Browse, or let Auto detect find it.
3. If the game still has to be decrypted, the same row says what is missing. Choose rpcs3.exe or press Download RPCS3,
   and choose the .rap file if it asks for one. Your game's folder and your RPCS3 are only read.
4. Press Play. Shaders are built the first time they are needed, so expect stutter at first. "Precompile shaders" in
   the launcher builds part of them ahead.


CONTROLLERS

Xbox controllers and other XInput pads, and through SDL3 the DualShock 4, the DualSense, Switch controllers and many
others. A DualShock 3 needs a driver such as DsHidMini. "Controller API" in the Controls tab chooses what reads them;
with DS4Windows running, set it to XInput, or the same controller counts twice.


@KIT@
WHERE YOUR FILES GO

Settings, saves, shaders and logs are in %LOCALAPPDATA%\Ascension-R. To keep them in this folder instead, put an empty
file named portable.txt beside Ascension-R.exe.


TO UPDATE

Unpack the new version over this folder, or into a new one. Your settings and saves stay where they are.
@KIT_UPDATE@


IF SOMETHING GOES WRONG

"Report a problem" in the launcher makes a zip of the logs, with your paths hidden and no game files. Attach it to an
issue at https://github.com/rigaz29/Ascension-R/issues


LICENSES

See the licenses folder, and THIRD-PARTY.txt in it first.
