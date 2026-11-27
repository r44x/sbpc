SBPC @VERSION@ - Saturn Bomberman on PC, Windows x64
https://github.com/r44x/sbpc

SBPC recompiles Saturn Bomberman's own code into native code on your PC, from your own
disc. This download contains no game data: you need your own copy of the game.

WHAT YOU NEED
- Windows 10 or 11, 64-bit.
- Your own dump of Saturn Bomberman (USA, Europe, Japan or the Japanese demo) as cue/bin,
  ISO + WAV or CHD.
- Nothing else: no BIOS file, no emulator, no Visual C++ runtime to install.

HOW TO PLAY
1. Unzip this folder anywhere.
2. Put your disc image next to sbpc.exe (subfolders are fine), or start sbpc.exe and press
   F2 to choose the folder where your games are.
3. Pick the game and press Enter. The first time, SBPC recompiles it (a minute or so),
   then the game starts. After that it starts at once.
4. In the game, F1 opens the menu (video, audio, controls, quit).

Your recompiled games (about 600 MB each), settings and logs go to %APPDATA%\SBPC.
Nothing is saved (the game has no save feature). To keep everything next to sbpc.exe
instead, use the portable download (SBPC-@VERSION@-windows-x64-portable.zip).

WHAT'S IN THIS FOLDER
- sbpc.exe       the launcher and the game window
- sbpc-cli.exe   the same program for the command line (sbpc-cli --cli help)
- sbpc-tcc.exe   TinyCC, the C compiler SBPC uses to build the recompiled code
- tcc\           headers sbpc-tcc needs
- LICENSE        SBPC's licence (MIT)
- licenses\      the licences of the parts SBPC includes:
    SDL3          zlib licence (libsdl.org)
    TinyCC        GNU LGPL 2.1 (sbpc-tcc.exe). Its source is the TinyCC project at
                  https://github.com/TinyCC/tinycc, commit @TCC_COMMIT@,
                  also linked from the SBPC repository (libs/tinycc).
    Half Eighties the launcher's pixel font, by Jayvee Enaguas (HarvettFox96), SIL Open Font
                  License 1.1

This is an early release: expect rough edges. Known gaps are listed in the repository
(program/docs/DESIGN.md).

SBPC is an independent fan project. It is not affiliated with, endorsed by or sponsored by
Hudson Soft, Konami or Sega. Saturn Bomberman is (c) Hudson Soft (now Konami); Sega Saturn
is a trademark of Sega.
