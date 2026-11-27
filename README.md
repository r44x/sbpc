# SBPC: Saturn Bomberman on PC

Play *Saturn Bomberman* natively on Windows and Linux. SBPC recompiles the game's own code into native code **on your PC, from your own disc**, with no BIOS file needed. This repository contains no game data.

> SBPC is an independent fan project. It is not affiliated with, endorsed by or sponsored by Hudson Soft, Konami or Sega. *Saturn Bomberman* is © Hudson Soft (now Konami); Sega Saturn is a trademark of Sega.

## Status

Playable, still in progress. The USA, European and Japanese releases (plus the Japanese demo) run their menus, Normal Game, Battle Game and Master Game with sound and music, the same way from cue/bin, ISO + WAV or CHD.

- Up to 10 players on keyboard and gamepads (two emulated multitaps), with rebinding. Players 9 and 10 need the game's **Wide** stage
- An in-game menu for video, audio and controls, including experimental 16:9 widescreen

How it works, known gaps and next steps are in [program/docs/DESIGN.md](program/docs/DESIGN.md).

## Download

Releases are on the [Releases page](https://github.com/r44x/sbpc/releases): a zip for Windows (x64, nothing to install; a portable zip keeps everything next to `sbpc.exe`) and an AppImage for Linux (x86-64; make it executable and run it). They contain no game data.

## What you need

- Your own copy of *Saturn Bomberman* (USA, Europe or Japan, including the Japanese demo), dumped as **cue/bin**, **ISO + WAV** or **CHD**.
- Nothing else: no BIOS file, no emulator.

## Build

**Windows.** Install the tools once, from PowerShell:

```powershell
winget install --id Git.Git -e
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended"
```

Then run `program\scripts\bootstrap.bat` once (it fetches SDL3 and TinyCC) and double-click **`rebuild.bat`**: it finds Visual Studio, builds, runs the tests and starts SBPC. By hand, in **Developer PowerShell for VS 2022**:

```powershell
cd program
cmake --preset windows-msvc
cmake --build --preset windows-msvc
```

**Linux.** You need `git cmake ninja-build gcc` and SDL3's usual development packages (the list is in [.github/workflows/ci.yml](.github/workflows/ci.yml)).

```sh
./program/scripts/bootstrap.sh        # first time only
cd program
cmake --preset linux-release
cmake --build --preset linux-release
```

The build puts `sbpc` (`sbpc.exe`) and its compiler, `sbpc-tcc`, in `builds/`. Run the tests with `ctest --preset <preset>` from `program/`; they need no game data.

## Play

1. Put your dump in `games/` and start `builds/sbpc`. The launcher lists the discs in `games/` and its subfolders; **Change folder…** points it elsewhere.
2. Pick your disc and press **Build & Play**. The first time, SBPC recompiles the game's code and stores its own copy of the disc (a few seconds, about 530 MB). After that the button reads **Play**, and the game is listed as *(installed)*: it plays without your original dump.
3. Press **F1** (or **L3+R3** on a pad) at any time for the in-game menu: video, audio, controls, and return to the launcher.

| Saturn | Keyboard | Gamepad |
|---|---|---|
| D-pad | Arrow keys | D-pad or left stick |
| A / B / C | Z / X / C | bottom / right face buttons, right shoulder |
| X / Y / Z | A / S / D | left / top face buttons, left shoulder |
| L / R | Q / E | triggers |
| Start | Enter | Start |

The keyboard and the first gamepad are player 1; each extra gamepad is the next player. Rebind under **F1 → Input**.

The recompiled games, settings and logs live in `recompiled/`. A copy of `sbpc` outside this folder layout uses `%APPDATA%\SBPC` on Windows or `~/.local/share/sbpc` on Linux instead, and `--portable` keeps them in `data/` next to the program.

## Command line

```sh
sbpc --cli scan <folder>          # identify every disc in a folder
sbpc --cli identify <disc>        # details of one disc (.cue or .chd)
sbpc --cli generate <disc>        # recompile without playing
sbpc --cli extract <x.chd> <out.cue>  # convert a CHD to cue/bin
sbpc --cli boot <disc> 600 --screenshot   # run headless for 600 frames
sbpc --cli help                   # everything else
```

On Windows, use `sbpc-cli.exe` for these, so the terminal shows the output.

## How it works

SBPC finds the game's SH-2 code on your disc, turns it into C, and compiles that C on your PC with the bundled TinyCC. The rest of the Saturn (video, sound chip, CD drive and the BIOS functions the game calls) is reimplemented in C. Any code it doesn't recompile runs on a built-in interpreter instead. Details are in [program/docs/DESIGN.md](program/docs/DESIGN.md).

## Folders

```
games/          your dumps (not committed)
builds/         sbpc, sbpc-cli, sbpc-tcc (built here, shipped as releases, not committed)
recompiled/     built games (saturn-bomberman-usa/, -europe/, -japan/, -japan-demo/), settings and logs (not committed)
libs/           external code: SDL3 and TinyCC (git submodules)
program/        SBPC itself: CMakeLists.txt, src/, tests/, scripts/, docs/
rebuild.bat     Windows: build, test and start
```

Inside `program/src/`:

```
core            logging, paths, settings, files, processes
disc            disc images (cue/bin, ISO+WAV, CHD), ISO 9660, version table
analysis        finding the game's code
codegen         turning SH-2 code into C
sh2             SH-2 decoder, interpreter, shared instruction semantics
runtime         running recompiled code, loading and building modules
saturn          the machine: CPUs, VDP1/VDP2, sound, CD, SMPC, saves, BIOS
m68k            the sound CPU
launcher        launcher, game window, in-game menu, input
```

## Licence

SBPC is MIT-licensed. It uses [SDL3](https://libsdl.org) (zlib) and [TinyCC](https://github.com/TinyCC/tinycc) (LGPL). TinyCC is built as a separate program, `sbpc-tcc`, and never linked into SBPC. The launcher font is [Half Eighties](https://harvettfox96.neocities.org/) by Jayvee Enaguas (SIL Open Font License 1.1, see `libs/half-eighties/`).

SBPC is an independent fan project. It is not affiliated with, endorsed by or sponsored by Hudson Soft, Konami or Sega. *Saturn Bomberman* is © Hudson Soft (now Konami); Sega Saturn is a trademark of Sega.
