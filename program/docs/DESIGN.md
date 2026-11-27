# SBPC design

The single reference for SBPC: what it is, the decisions behind it, how each part works, what the game's discs showed, and what's left. Change a decision here first, then in code. Working rules for contributors (and Claude) are in `CLAUDE.md` at the repository root.

**Contents:** 1 Goals and rules · 2 Layout and data · 3 Supported versions · 4 Architecture · 5 Discs · 6 Analysis · 7 Code generation and runtime · 8 The Saturn model · 9 Launcher · 10 Game window and overlay · 11 Quality, CI and releases · 12 Developer tools · 13 What the discs showed · 14 Known gaps and next steps

## 1. Goals and rules

- A PC port of **Saturn Bomberman** for **Windows and Linux**, made by static recompilation of the game's SH-2 code on the player's own PC.
- **Bring your own disc.** The repository and the released downloads contain no game data: no assets, no code bytes, no translated code. The only per-game facts stored are identifiers (product, version, hashes) and addresses. Everything derived from a disc stays in the player's data folder.
- **Dependencies:** only **SDL3** (platform layer) and **TinyCC** (compiler on the player's PC), as git submodules in `libs/`, plus the Half Eighties pixel font (OFL 1.1) for the launcher. No BIOS file. Codecs (CHD's LZMA, Deflate and FLAC) are SBPC's own code.
- **Licence:** MIT. TinyCC (LGPL) is built as a **separate program**, `sbpc-tcc`, and never linked into `sbpc`.
- **Language and build:** C11 with CMake presets. MSVC (VS 2022 Build Tools) on Windows, gcc or clang on Linux. Warning-free under `-Wall -Wextra -Wpedantic` and MSVC `/W4`.
- **One program:** `sbpc` is the launcher, the recompiler and the game runtime. On Windows, `sbpc-cli.exe` is the same program as a console application (the GUI build doesn't hold the terminal).
- **Notice:** SBPC is an independent fan project, not affiliated with, endorsed by or sponsored by Hudson Soft, Konami or Sega. *Saturn Bomberman* is © Hudson Soft (now Konami); Sega Saturn is a trademark of Sega. The README, the release notes and every download say so.

## 2. Layout and data

**Repository:**

```
SBPC/
  program/     SBPC itself: CMakeLists.txt, presets, src/, tests/, scripts/, packaging/, res/, docs/
  libs/        SDL (submodule), tinycc (submodule), half-eighties (font)
  games/       the player's dumps (ignored, except its README)
  builds/      sbpc, sbpc-cli, sbpc-tcc and tcc/ copied by the build (ignored; releases ship them)
  recompiled/  data folder in the repository layout (ignored)
  rebuild.bat  Windows: build, test and start the launcher
```

**Data folder** (`src/core/paths.c`):

| How SBPC runs | Data folder | Built games |
|---|---|---|
| From `SBPC/builds` (repository layout: `libs/` and `games/` beside it) | `SBPC/recompiled` | directly in it |
| Windows, anywhere else | `%APPDATA%\SBPC` | `generated\` |
| Linux, anywhere else (AppImage included) | `$XDG_DATA_HOME/sbpc`, else `~/.local/share/sbpc` | `generated/` |
| Portable: a `portable.txt` next to the executable, or `--portable` | `data/` beside the executable | `generated/` |

The data folder also holds `sbpc.ini` (settings), `logs/` (`sbpc.log` for people, `sbpc.jsonl` one JSON object per line, the previous run kept as `.1`, `boot_report.txt` from `--cli boot`, `input.rec`) and `analysis/` (reports from `--cli analyse`).

**One folder per built game**, named by `game_version.folder`: `saturn-bomberman-usa`, `saturn-bomberman-europe`, `saturn-bomberman-japan`, `saturn-bomberman-japan-demo`. `launcher_game_dir` builds every path into it and renames a folder left by an older SBPC under the short version id (`usa`, `eur`, `jpn`, `jdemo`). Each holds:
- the recompiled code: one library per module (`kernel.dll` / `.so`, and on the USA disc also `boot` and `fld_knl`), with its generated C and `<module>.compile.log`;
- `disc/`: SBPC's own copy of the game as ISO + WAV (about 600 MB), so the original dump isn't needed after a build;
- `build.stamp`: SBPC version, `SBPC_CODEGEN_VERSION` and the disc's data SHA-1;
- `launcher_bg.bmp` (or `launcher_bg.none`): the launcher wallpaper (section 9).

**No save files.** The game has no save feature: it only reads backup memory (it looks for `BOMBERSS_01`). The game window keeps backup memory in memory only and never writes a file. The backup format and library stay implemented and tested (section 8).

## 3. Supported versions

Each version is identified by the SHA-1 of its data track's user data (2048 bytes per sector, so `.bin` and `.iso` dumps hash the same) and listed in `src/disc/versions.c` with its modules, hints, player limit, PAL flag and folder. Other discs are refused: another game, or an unknown version of this one ("please report its product, version and SHA-1"). A cue that doesn't list all 30 tracks is refused too (the music would be missing).

**Support** = checks passed out of 8: **ID** the disc is recognised · **Build** every module recompiles · **Menus** boots to the title and menus · **Normal** / **Battle** / **Master** the mode plays (Battle: setup and a match) · **Sound** effects and music play · **User** played in the game window by the project owner. Keep this table current, and show it whenever work touches a specific version.

| Version | id | Product / version | Data-track SHA-1 | ID | Build | Menus | Normal | Battle | Master | Sound | User | Support |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| USA (1997) | `usa` | MK-81070 V1.003 | `65a3bfeb654e5b3bb0f47105185298a62f50000e` | yes | yes | yes | yes | yes | yes | yes | yes | **100%** |
| Europe (1997, PAL) | `eur` | MK-81070 V1.000 | `540c983bccea487f1f5aa602505fc2690f971eb2` | yes | yes | yes | yes | yes | yes | yes | yes | **100%** |
| Japan (1996-05-31) | `jpn` | T-14302G V1.000 | `09a5d7e2620c147a7b0d86d08d6ff111b5d19132` | yes | yes | yes | yes | yes | yes | yes | yes | **100%** |
| Japan Demo (1996-03-22) | `jdemo` | T-14302G V1.000 | `f79955c8d555841d66e58331e58bdc88540257e3` | yes | yes | yes | yes | yes | yes | yes | yes | **100%** |

The version ids stay short and internal: module names, area codes (`j…` = Japan) and logs use them.

**Per version:**
- **USA:** a NetLink front end (`/0`), then a loader chain to `/BOMSS/0KRNL.BIN` at 0x06006000 (see "How the USA disc boots", section 13). Modules: `boot` (`/0`), `kernel`, `fld_knl` (`/FLD_KNL.BIN` at 0x00200000, entry 0x002001A4 from its first longword). Kernel hints: 0x0600623A (the main loop, entered only by `longjmp`; `setjmp` at 0x060061C4, `longjmp` at 0x060061E6) and 0x060B0E8E (an in-battle routine nothing points at).
- **Europe:** no front end. The 1st read file is `/0KRNL.BIN`, loaded by the BIOS at 0x06006000. It's the USA code base: identical up to 0x06007382, so the main-loop hint 0x0600623A fits; the battle hint is 0x060B09EE. The IP is only 0xF00 bytes (one area block). The machine runs PAL (313 lines, 50 Hz). From the intro skip on, it produces the same frames as the USA version at the same frame numbers, even at 50 Hz.
- **Japan:** no front end. `/0KRNL.BIN` is linked for and loaded at 0x0600A000 (entry 0x0600A114). Found by the USA bytes: `setjmp` 0x0600A164, `longjmp` 0x0600A186, main loop 0x0600A1DA. Hints: 0x0600A1DA, 0x0600AF6A (code nothing points at), 0x060B1DF6 (battle routine). Its CD audio matches the USA tracks sample for sample, except a slightly longer silent tail. Game differences (not SBPC's): the title leads to GAME MODE / OPTION and GAME MODE opens Normal / Battle / Master; during the intro movie it only notices a press of about 5+ frames (scripts press for 8); it reads the pads every frame (USA: every other frame) and ends INTBACK with a break (IREG0 = 0xC0) after the first peripheral block. It has a "hurry" gong at one minute left in the Master Game that blinks by design (section 13); the USA version has none.
- **Japan Demo:** the same product and version as the retail Japanese disc but different data. `PROGRAM.BIN`, the music and the movies are byte for byte the retail ones; only 176 bytes of the kernel differ (0x06031E7A-0x0603202D once loaded), so it uses the retail Japanese modules and hints. The picture fades out about 50 seconds into a Normal Game stage (the demo's time limit).

The kernels and `PROGRAM.BIN` differ between USA, Europe and Japan, so each version is analysed on its own.

## 4. Architecture

```
sbpc (MIT)                                   sbpc-tcc (LGPL, separate exe)
├─ launcher (SDL)  scan → identify → Build ──┐   compiles generated C → .dll/.so
├─ analysis        SH-2 code discovery       │
├─ codegen         C per module + code table ├─> <game folder>/<module>.dll
├─ runtime         dispatcher + loader  <────┘
└─ saturn          SH-2 ×2, SCU, SMPC, CD block, VDP1, VDP2, SCSP + 68000, BIOS HLE, backup RAM
```

Pipeline: the **launcher** scans a folder and **identifies** each disc; **Build** analyses each module, generates C and compiles it with `sbpc-tcc` into one library per module, installs the game's own disc copy and writes the stamp; **Play** loads the modules into the runtime, where recompiled code runs wherever it matches memory and the SH-2 interpreter covers the rest.

**Source map** (`program/src/`):
- `core/`: paths, config, logging, files, PNG writer, `proc.c` (runs `sbpc-tcc`).
- `disc/`: cue parser, disc reader, CHD reader (`chd.c` with `inflate.c`, `lzma.c`, `flac.c`), ISO 9660, Saturn header, identification (`identify.c`), the version table (`versions.c`).
- `analysis/`: boot-image loader and SH-2 code analyser.
- `codegen/`: the C generator. `SBPC_CODEGEN_VERSION` (currently **5**) is bumped whenever analysis or generated output changes; built games then rebuild once.
- `sh2/`: decoder, interpreter, shared instruction semantics (`sh2_ops.h`) and the module ABI (`sbpc_rt.h`, `SBPC_MODULE_ABI` = 2). `sh2_state.h`, `sh2_ops.h` and `sbpc_rt.h` are copied to `tcc/sbpc/` next to `sbpc-tcc`.
- `runtime/`: `dispatch.c` (address → function, code checks), `loader.c` (`LoadLibraryW` / `dlopen`, ABI and disc-hash checks), `tcc_build.c` (the TinyCC command and prelude), `tcc_include/`.
- `m68k/`: 68000 interpreter (sound CPU).
- `saturn/`: the machine. `saturn.c` (bus, frame loop, SH-2 on-chip peripherals, report, feature survey), `saturn_bios.c` (BIOS HLE, backup library), `saturn_bup.c` (backup format), `saturn_cdb.c`, `saturn_scu.c`, `saturn_smpc.c`, `saturn_scsp.c`, `saturn_vdp1.c`, `saturn_vdp2.c`, `saturn_rec.c` (input recordings).
- `launcher/`: `launcher_gui.c` (launcher), `launcher_cli.c` (`--cli` commands, Build, wallpaper), `play.c` (game window), `overlay.c` (in-game menu logic), `input.c` (keyboard, gamepads, bindings), `scan.c`, `ui_font.c` (shared look).

## 5. Discs

**Formats:** cue/bin (MODE1/2352, MODE2/2352, AUDIO), ISO + WAV (MODE1/2048 + WAVE), and **CHD v5** (MAME/chdman CD images, read natively rather than converted). No archives.

**Identification** (`identify.c`): read the Saturn system ID in sector 0 of the data track (product, version, area, title), then hash the data track's user data and look it up. `identify_installed` is a quick variant for a game's own disc copy: it reads only the header and track count and trusts the build for the hash.

**CHD** (`chd.c`; decoders `inflate.c`, `lzma.c`, `flac.c`):
- **Container:** the v5 header, `CHT2`/`CHTR` track metadata (pregaps stored as `V*` types are part of the track data; every track is padded to a multiple of 4 frames), and the compressed hunk map: Huffman-coded compression types with run-length codes, then lengths, offsets and CRC-16s, with self-references. Parent (diff) CHDs and non-CD codecs are refused with a clear message.
- **CD codecs:** `cdlz` (LZMA lc3/lp0/pb2), `cdzl` (raw Deflate), `cdfl` (FLAC frames with no stream header, stereo, big-endian samples). Subcode is Deflate. Stripped sync/ECC is rebuilt only for verification; the game only needs the 2048 user bytes. CD audio comes out big-endian and is swapped to little-endian, like a .bin.
- **Disc layer:** a `.chd` path opens as a cue sheet built from the metadata; the launcher scans `.chd` files next to `.cue`.
- **Checks:** `--cli chdverify <file.chd>` decodes every hunk and compares its CRC-16 (the Japan dump: 0 bad of about 12k, in 16 s). `--cli tracksha <image> <track> [out.raw]` hashes or dumps an audio track as the game hears it.

**Export:** `disc_export_isowav` writes a disc as ISO + WAV (each audio track keeps its pregap); used for the game's own copy (section 9).

## 6. Analysis (`--cli analyse <image>`)

- `iso9660.c` reads the filesystem. `analysis/boot.c` loads the IP at 0x06002000 and the 1st read file at the header's 1st read address; other modules come from `versions.c` (a file path and a load address). Modules are analysed separately because they overlap in memory.
- `sh2_decode.c` is a table-driven decoder for all 142 SH-2 instructions, with flags for branches, delay slots, calls and PC-relative loads.
- `analyse.c` walks the code from the entry point, function by function:
  - It tracks simple register constants, so `mov.l @(lit),rN; jsr @rN` resolves to a call.
  - It recognises switch tables: `mova` + `mov.b`/`mov.w @(r0,rI)` + `add` + `jmp @r0` (offsets from the table); `mova` + `shll2` + `add` + `jmp @r0` (a table of code); `mov.l @(lit),rT` + `mov.l @(r0,rT),rN` + `jmp @rN` (absolute pointers, the kernel's compiler); `mova` + `mov.w @(r0,rI),r0` + `braf r0` (offsets from the `braf`). The bound comes from `cmp/hi` or `and #k`, else the table ends where the first case's code begins.
  - A call followed directly by literal data is a no-return call.
  - Functions are also found through any aligned pointer, in literal pools or tables, that lands on a prologue **or on a function boundary** (directly after a literal pool, or after explored code ending in `rts`/`rte`/`jmp`/`bra`/`braf` + delay slot) and decodes cleanly. The boundary rule catches leaf methods with no prologue called through method tables (USA kernel: the stream readers at 0x06033DF0-0x06034350, listed at 0x060B52C0-0x060B5400, most of the interpreted time during FMV).
  - Code that nothing points at comes from per-version **hints** (`game_module.hints`). Candidates: run `SBPC_IHIST=40 --cli boot <image> <frames>` and read the `ihist start` lines.
  - **0x06002100, the IP's security code, is never made a function** (`an_add_entry`): the BIOS stand-in takes over when the dispatcher reaches it (section 8), so a front end that boots through it must go through the dispatcher, not a direct call.
  - Calls through BIOS vectors (0x06000200-0x060007FF) are labelled `bios-call`.
- Reports (`summary.txt`, `functions.txt`, `listing.txt`) go to `<data>/analysis/<id>/`. They are derived from the disc, so they stay local.

## 7. Code generation and runtime

**Generated code** (`--cli generate <image>`):
- `sh2_ops.h` holds the semantics of every non-branch instruction as inline functions `sh2_x_<ID>(s, n, m, i, a)`. Both the interpreter and the generated code call them, so the two can't disagree.
- `codegen.c` turns each function into `void f_XXXXXXXX(sh2_state *s)`, opening with a `switch (s->pc)` over its resume points: the entry, return sites after calls, backward-branch targets, switch cases, and the instruction after `ldc sr` / `sleep` / `trapa`. Execution can leave generated code anywhere and re-enter later.
  - **Calls:** calls to known functions are direct C calls. Indirect calls with a guessed target (`mov.l @(lit),rN` shortly before) are guarded with `if (t == K)`. Everything else goes through `rt->call`. If `s->pc` isn't the return address after a call (a time-slice yield, an exception, a non-local return), the function returns and the dispatcher resumes at `s->pc`.
  - **Returns and jumps:** `rts` sets `s->pc = PR` and returns. Dynamic jumps (`jmp`, `braf`) set `s->pc` and go back through the entry switch, which covers resolved switch tables; unknown targets go to the dispatcher.
  - **Cycles:** every instruction subtracts its cycles; backward branches yield when `s->cycles <= 0`.
- Modules talk to the runtime only through `sbpc_rt` (`sbpc_rt.h`), so they build as separate libraries. Each exports `sbpc_module_<name>(rt)`.

**Runtime:**
- `dispatch.c` holds a sorted table of every resume point (address → function), merged across modules. It runs recompiled code where an entry exists and the interpreter everywhere else.
- **CPUs:** master and slave SH-2 take turns on one host thread in time slices, with a cycle count per block, and sync when either touches shared hardware or the communication registers. Execution is **deterministic** for given inputs.
- **Code checks:** every generated function starts with `SBPC_CHECK(i)`: once per epoch it hashes the guest bytes it was generated from and compares them with the module's code table. The epoch is bumped every frame, after SCU/SH-2 DMA and after an IP boot. On a mismatch (an overlay, another program at that address) the function returns without running, and the dispatcher tries another module's function at that address, then the interpreter. Native code adds its cycles to `total_cycles`, so the FRT/WDT timers keep time.
- **Hooks** (planned): every recompiled function is reached through the dispatch table, so C replacements and before/after hooks keyed by version and address can be added. None exist; widescreen didn't need any.

**Coverage** (interpreted SH-2 instructions, codegen 4+): USA about 3.3M over 9,000 frames (mostly the NetLink loader at 0x0028xxxx and code copied to 0x06004xxx at run time, which no module covers); Europe about 42k over 7,500 frames; Japan about 63k over 9,000 frames; battle runs about 196k (Europe) and 171k (Japan). Without recompiled code the USA intro alone is 2.88 billion. USA modules: boot 2,001 functions / 139k instructions, kernel 4,783 / 249k, fld_knl 47 / 1.3k; TinyCC compiles all three in about 1.6 s. No module needs an interpreter fallback.

## 8. The Saturn model (`program/src/saturn/`)

- **Memory map** (`saturn.c`): low (0x00200000) and high (0x06000000, with mirrors) work RAM are fast pages; VDP1/VDP2 VRAM, CRAM, sound RAM and backup RAM are buffers; SMPC, SCU, CD block and VDP registers are decoded; cartridge space reads 0xFF. The SH-2 on-chip area has a working divider (DIVU), the free-running timer, the watchdog timer and DMA; the rest is stored. Every register access is counted and the first one to each address is logged with its PC (`saturn_report`).
- **Timing:** NTSC is 263 lines at 60 Hz, 1706 cycles per line (1820 in 352-pixel mode). PAL is 313 lines at 50 Hz, 1705 / 1817 cycles per line, with TVSTAT bit 0 set. V-blank in at line 224, out at line 0, H-blank every line, then the master's slice (and the slave's, if on).
- **SH-2 on-chip peripherals:** the free-running timer (`frt_update`, once per line; FRC at clock/8, /32 or /128; output-compare A raises the interrupt, priority from IPRB, vector from VCRD, default 0x65) drives the game's tick counter and its NetLink time-out. The watchdog timer in interval mode runs Sega's CD file server (vector 0x68). The DMA controller (used by the movie player for CD sectors) is an immediate transfer when a channel is enabled, with TE and an optional interrupt.
- **BIOS** (`saturn_bios.c`, written for SBPC, no BIOS image):
  - The vector table and function slots at 0x06000200-0x060003FF point at trap addresses handled in C. Implemented: SETUINT/GETUINT (with a wrapper that saves r0-r7/MAC/PR and returns with `rte`), SETSINT/GETSINT (`SETSINT(v, 0)` restores the BIOS handler, which calls the SETUINT function: the DMA library's end callback relies on it), CHGSYSCK, TASSEM/CLRSEM, SETSCUIM (0x340) and CHGSCUIM (0x344, and-mask then or-mask). Unknown slots, interrupts with no handler and CPU exceptions are logged.
  - Hand-off like the boot ROM: VBR 0x06000000, SR imask 15, SCU mask clear, stack 0x06002000, jump to the 1st read address.
  - **IP boot** (`ip_boot`): when execution reaches 0x06002100 (the IP's security code) with an IP present, SBPC loads the 1st read file named by the BIOS boot variables (0x06000CCC = 1, 0x06000CCE = directory ID) and jumps to the AIP, as the real BIOS would. The USA loader chain depends on it.
  - **Backup library:** BUP_Init fills the device config (internal memory only) and the function table; SelPart, Format, Stat, Write, Read, Delete, Dir, Verify, GetDate and SetDate. Dir returns the match count (negative when the table is too small); the kernel calls it with table size 0 to ask whether its save exists (0x06039394).
- **Backup memory** (`saturn_bup.c`): the real 32 KB format, 512 blocks of 64 bytes. Block 0 holds "BackUpRam Format" four times; block 1 is reserved. Each data block starts with a 4-byte tag (0x80000000 = a file's first block, 0 = continuation). A file's payload (tags skipped) is name[11], language, comment[10], date (BE32, minutes since 1980), size (BE32), a BE16 block list ending in 0, then the data; n bytes take ceil((n + 30) / 58) blocks. A 64 KB image with data in the odd bytes is accepted. USA save names: `BOMBERMANSS`, `BOMBER_NRM`, `BOMBER_MST` (name table at 0x060B1E1C) and `BOMBERSS_xx`; the kernel's save routine writes with BUP_Write, then BUP_Verify (0x06039210 / 0x06039224). The game window never attaches a file (section 2); headless runs can with `SBPC_BACKUP=<file>`.
- **SCU** (`saturn_scu.c`): interrupt status and mask, delivery by priority to the master, DMA levels 0-2 (direct and indirect, immediate or on V-blank), timer 0 on a line match.
- **SMPC** (`saturn_smpc.c`):
  - INTBACK returns status (time, area code) and the peripheral report. Each port is empty (`0xF0`), one direct pad (`0xF1 02 hi lo`), or a 6Player multitap (`0x16`, then 6 connectors, each `02 hi lo` or `FF`). 12 pad slots: port 1 = 0-5, port 2 = 6-11.
  - The report can exceed the 32 OREGs (two multitaps = 38 bytes), so it goes out in 32-byte blocks: SR bit 7 = peripheral data, bit 6 = first block, bit 5 = more follows. The game asks for the next block by **toggling** IREG0 bit 7 (the USA kernel writes 0x80, then 0x00). OREG31 isn't overwritten after peripheral data. The kernel uses IREG1 = 0x08 (15-byte mode on both ports).
  - `smpc_layout()` plugs in pads for N players. Auto: direct pads for 1-2; player 1 direct plus a multitap on port 2 for 3-7; a multitap on each port for 8-10 (10 = 6 + 4). Port 2 is empty by default (a pad always present there made Battle Game's character select wait for a player 2). The game handles up to **10 players** (`max_players`); slots 9 and 10 open only on the **Wide** battle stage.
  - **Clock:** `rtc_start` is the PC's time when the machine is created; the clock then follows emulated frames, so a replayed recording sees the same date and time.
  - SSHON/SSHOFF, clock change and NetLink on/off are handled (NetLink reports no modem). After SSHON the slave is parked: its start protocol isn't modelled, and the slave has stayed off in every run.
- **CD block** (`saturn_cdb.c`): the command set the game's CD libraries use: status, TOC and session info, play and seek (CD audio with repeats, data sectors into the buffer), filters and buffer partitions, sector reads and deletes, and the file-system commands (change directory, file info, read file, abort). Details Sega's CD library (GFS) depends on:
  - The CR1-4 command registers are separate from the response registers; writes never read back (so a modem probe fails, as with no modem).
  - HIRQ DCHG must be clear after boot (otherwise GFS reports "tray open", -23). Command 06 (end transfer) reports the words read, and a transfer stays open until 06. Command 03 returns session 1's start as LBA 0. Handlers read CR as written, before the status report is copied in. File info (73) has XA file number 0 on this Mode 1 disc.
  - **Pause:** the game's own pause sends Seek `0xFFFFFF` ("pause here") and resumes with Play `0xFFFFFF`/`0xFFFFFF`. `0xFFFFFF` also has bit 23 set (the absolute-FAD flag), so it's checked first; taken as a FAD it moved the music to 0x7FFFFF and the music never came back (`test_saturn` covers it).
  - CD audio repeats follow the PLAY command (0-14 times, 15 = forever); the title theme plays once because the game asks for that.
- **Sound** (`saturn_scsp.c`, `m68k/`): the game's 68000 sound driver runs on SBPC's 68000 interpreter (started with SMPC SNDON; the kernel talks to it through a mailbox at 0x25A00730). The SCSP has registers, timers A/B/C, interrupts to both CPUs, sound DMA and 32 slots. CD audio is mixed in; output is 44.1 kHz stereo.
  - **Envelopes:** attack, decay 1 (to DL), decay 2 and release on a 10-bit level; rate = base + 2R with key rate scaling (OCT + 2 KRS + FNS bit 9), R = 0 holds; EGHOLD and LPSLNK; attacks start around -60 dB.
  - **Levels in dB:** envelope 3/32 dB per step, TL 0.375 dB, DISDL/IMXL/EFSDL 6 dB, pan 3 dB; the slot sum is halved for headroom.
  - **Effects DSP:** 128-step microcode, TEMP/MEMS/MIXS/EXTS/EFREG, 24-bit ACC, the ring buffer in sound RAM (RBP/RBL) with PACK/UNPACK. Slots send with ISEL/IMXL; EFREG 0-15 return with EFSDL/EFPAN of slots 0-15, CD audio (EXTS) with those of 16-17.
  - The USA driver uses attack/decay/KRS on every note and no LFO or FM; a 279-word reverb with every slot sending to MIXS 0 at -30/-24 dB, EFREG 0-3 returning at full level, CD audio returning hard left/right; ring buffer 32K words at 0x6A000.
  - The intro movie (Cinepak through the STM library) keeps time with the audio through the SCSP monitor register (MSLC/CA at 0x408), so slots must advance.
- **VDP1** (`saturn_vdp1.c`): command lists (normal, scaled and distorted sprites, polygons, lines, polylines, system/user clip, local coordinates, flips, every texture colour mode), two frame buffers, frame-buffer change and erase (1-cycle, manual erase, manual change). Drawn as plain replacement: no gouraud, shadow, half-luminance/half-transparency, mesh or end-code line stop (the game uses replace mode on every screen surveyed).
- **VDP2** (`saturn_vdp2.c`): `saturn_render` composites NBG0-3 (cell and bitmap, every colour depth, 1- and 2-word names, 8x8 and 16x16 characters, plane sizes, scroll), the sprite layer (types 0-7, RGB sprites), the back screen and colour offset, by priority. Also:
  - **Colour calculation** for NBG layers: ratio and add, per screen or per character (pattern-name bit 12, or the PNC supplement bit). It mixes the top dot with the one under it. (Battle explosion flames: NBG0, ratio 8.)
  - **Sprite colour calculation:** SPCTL SPCCCS conditions (priority <=, =, >= the CC number, or colour MSB) with CCRSA-D ratios by the dot's CC bits. (Normal Game: SPCTL 0x0616, CCRSA 0x1300.)
  - **Windows:** rectangle windows 0/1 and the sprite window, AND/OR logic, for NBG0-3, sprites and the colour-calculation window; area bit 0 = hidden inside, 1 = hidden outside. (Battle setup: NBG2, the bomb pattern, only inside window 0; NBG0 only outside window 1.)
  - **Special priority** (`SFPRMD`, `layer_prio`): per cell, a layer's priority LSB is the cell's SPR bit (two-word name bit 13, PNC supplement bit 9, BMPNA for bitmaps); per dot, the dot's colour code must also be in `SFCODE` (A or B by `SFSEL`). Normal Game stages set `SFPRMD = 0x0055`: that's how the front rail passes in front of the train and blocks, and how scenery (arches, lanterns, waterfalls) covers the bombers.
  - **Line colour screen** (`LNCLEN`, `LCTA`/`LCTB`, `line_color`): when the top dot's layer has its LNCLEN bit set, colour calculation mixes it with the line colour (a CRAM index from the line colour table, one entry or one per line) instead of the screen under it. The Normal Game stage select after a cleared stage sets LNCLEN 0x2F, a black line colour and ratio 23, so the stage dims evenly behind the map.
  - **Double-density interlace** (the Wide battle stage): TVMD `80C2` = 640 wide, LSMD 3 = 448 lines; VDP1 TVMR `0001` (8-bit, 1024x256 buffer), FBCR DIE = 1 with DIL toggling. A frame draws only lines of parity DIL, at row y/2. SBPC shows both fields at once: each frame buffer records its parity (`fb_field`) and the one leaving the display is kept as the other field (`vdp1_field`). In hi-res the 8-bit buffer maps 1:1 to 640 columns (the 16-bit one is shown at half resolution).
  - **Widescreen** (16:9, `wide_extra` = 53 → 426x224): VDP2 draws the background layers further out on both sides (the game's x 0 is at 53); VDP1 draws 53 pixels to the right in its 512-wide buffer, a full-width system clip widens by 53 each side, and erase covers the sides. Each frame buffer remembers the shift it was drawn with (`fb_xoff`), and the compositor reads it offset by any difference (`sprite_shift`), so switching ratio while paused doesn't move sprites. No game hooks: arenas, stages, the title and the Battle setup extend naturally; HUDs and movies stay 4:3 in the middle. Hi-res and 8-bit modes stay 4:3 (`render_extra` = 0).
- **Feature survey** (always on): each frame the machine records which VDP2 features the renderer doesn't draw are enabled (and VDP1 colour modes, shadow and window dots). `boot_report.txt` lists them under "Features not drawn yet" with first frame and count. A long scripted run shows what a screen needs. Scripted runs of the attract mode, Normal Game, Battle Game (setup and a match) and Master Game report none.

## 9. Launcher (`launcher_gui.c`)

**Look:** green `#39F855` lines and uppercase text on black, from the owner's mockups. Text is Half Eighties, an 8x8 pixel font, converted to a bitmap table (`libs/half-eighties/half_eighties.h`) by `program/scripts/font_to_c.py` and drawn as a tinted glyph strip. Canvas 800x533; the window opens at that size and resizing letterboxes it. Header, games-folder bar, list | details, key-box footer (Enter / F5 / F2 / Esc, or the pad's buttons once a pad is used). The overlay shares the look (`ui_font.c`).
- **List:** the selected row in a green bar; READY on built games, which are listed first.
- **Details:** FORMAT (DISC for a cue sheet, CHD for a `.chd`), region, product, version, tracks, then a badge: SUPPORTED, RECOMPILED / READY TO PLAY, or NOT SUPPORTED in the same badge on red `#F83939` (Enter on it says why, in the notice line). Notices (a failed build, an unreadable folder) show at the bottom of the panel.
- **Games folder:** `SBPC/games` in the repository layout; otherwise the program's own folder, or the folder holding the `.AppImage` file (`$APPIMAGE`, since the program runs from a read-only mount). F2 picks another, which is remembered.
- **Icon:** `program/res/icon.png`, the owner's own pixel art (15x15). `program/scripts/icon_to_c.py` makes `res/sbpc.ico` (16/32/48/256, whole-step nearest scaling; built into both Windows executables through `res/sbpc.rc`), `res/icon_rgba.h` (set on the launcher and game windows by `ui_set_window_icon`, every platform) and, with a third argument, a 256x256 PNG for the AppImage. Rerun it after changing the PNG.

**The window never waits on work.** Scanning and recompiling run on a worker thread (`scan_job`, `build_job`); the worker fills `job_*` fields that the main loop takes over once `job_done` is set. A rescan asked for during a job runs after it.
- **Built games first:** before the folder scan, `quick_list` lists every version whose own disc copy has a current stamp (header read only), so built games are playable at once while the full scan (about 3 s per disc, it hashes each one) runs below them with a "Scanning the games folder... n/total" line and a loader. When it finishes, the list is replaced (ready games first) and the game picked meanwhile stays picked. Playing during a scan detaches it and skips its remaining wallpaper work; recompiling waits for the scan; quitting during a build waits for the build out of sight.
- **Recompiling screen:** titled "Recompiling", with four rows over `launcher_build`'s steps (`launcher_set_build_progress`): Reading your disc; Finding the game's code; Recompiling code (translating and building the native code); Copying the recompilation to your PC (the game's disc copy and the launcher picture). Finding and recompiling run once per code file ("start-up program", "main program", "extra program") and the current row says which ("main program (2/3)"). Done rows are green with OK, the current one white with a three-square loader, the rest grey; below: "The game starts when the recompilation is ready."

**Build & Play** (`launcher_build`):
1. Analyse each module and generate C into `<game folder>/<module>/`.
2. Compile with `sbpc-tcc` into `<module>.dll` / `.so`: `-nostdinc -nostdlib -shared`, includes from `<exe>/tcc/include` (TinyCC headers plus SBPC's `stdint.h`) and `<exe>/tcc/sbpc`, plus a generated `sbpc_prelude.c` (mem* helpers; `_dllstart` on Windows). Output in `<module>.compile.log`. Shared with the `tcc` test (`tcc_build_module`).
3. Install the game's own disc copy in `<game folder>/disc/` as ISO + WAV, unless an identical one is there; the copy is re-identified after writing.
4. Write `build.stamp`, then make the wallpaper.

While the stamp matches, the button reads **Play** and the game starts at once from the installed copy (`launcher_installed_image`), whatever the original format was; the original dump can be removed (`scan_add_installed` still lists the game, marked *installed*). The game window loads the modules (`launcher_load_modules` → `rt_load_module`, ABI and disc-hash checks). If the build fails, the game still runs on the interpreter; the log says why. Quitting the game ends SBPC (the launcher doesn't reopen).

**Wallpaper:** behind a built game's details, the Battle Game menu's bomb pattern scrolls up and to the left at the game's speed (half a game pixel per frame on both axes, drawn at 2x), dimmed to about 35% so the text stays readable. The picture comes from the player's own game: `launcher_extract_art` runs the recompiled game headless along a scripted route to the Battle setup (USA/Europe: Start x5, Down, Start; Japan: Start x5 with 8-frame presses, Down, Start), checks from frame 1300 on that VDP2 NBG2 holds a 128x64 tile repeated over its 512x512 page (16 colours, 8x8 cells, two-word names), and saves the tile as `launcher_bg.bmp`. It runs after every build, and during a scan for games built before it existed (7-9 s each). If it never shows up, `launcher_bg.none` stops retries until the next build. (Only the palette is stored raw in `PROGRAM.BIN`; the cells are packed, hence the headless run.) USA and Japan give the identical tile.

## 10. Game window and overlay

**Game window** (`play.c`): the picture at 4:3 (16:9 with widescreen), SCSP output through SDL's audio stream (44.1 kHz stereo, at most about 100 ms queued), paced to 60 Hz (50 Hz for PAL). Software VDP1/VDP2 at native resolution, then GPU scaling: Sharp (nearest), Smooth (linear), Integer, Stretch, Sharp bilinear (a whole-number nearest upscale into a render target, then linear for the fractional step). Every session's pad input is recorded to `<logs>/input.rec` (section 12).

**No frame interpolation**, on purpose: a prototype redrew each VDP1 list with positions blended from the previous one, but in USA battles only 1 of ~26 matched sprites moved between frames (sprites move about a pixel at a time, arenas don't scroll), so there was nothing to blend. Real smoothness would need sprites drawn at a higher internal resolution.

**Overlay** (F1 or L3+R3; closed with the same, or B/Esc; Esc alone never opens it, because Steam Input maps controller B to Esc). The game and its audio pause while it's open. Logic in `overlay.c` (no SDL, tested by `test_overlay`); `play.c` draws it in the launcher's look at whole-number scales (window height / 400), with a footer that switches to A / B / L3+R3 once a pad is used.
- **Main:** Resume, Video, Audio, Input, Info, Quit game (ends SBPC).
- **Video:** fullscreen (F11 also toggles it), scaling, vsync, widescreen 16:9 (experimental).
- **Audio:** master (stream gain), music (CD audio), sound effects (SCSP slots), 0-100 % in steps of 10.
- **Input:** who plays which player, the port layout, rebinding and reset.
- **Info:** SBPC version, disc version, recompiled entry points, a rough recompiled share over the last second, frame time, frame number.
- Settings save to `sbpc.ini` as soon as they change (`video_fullscreen`, `video_scale`, `video_vsync`, `video_widescreen`, `audio_master`, `audio_music`, `audio_sfx`, `input_ports`, `key_*`, `pad_*`) and apply when the game starts. Gamepads plugged in later are picked up.

**Controls:**
- **Players:** player 1 = keyboard + first gamepad, player N = gamepad N (connection order, hot-plug), up to the game's limit of 10; extra pads are listed as unused. The left stick also works as the d-pad. A `gamecontrollerdb.txt` next to the executable or in the data folder is loaded if present. Multitaps are emulated automatically (section 8).
- **Gamepad:** Saturn layout. A, B, C on the bottom-row and right face buttons; X, Y, Z on the left and top face buttons and a shoulder; L and R on the triggers; Start is Start. In the overlay: d-pad, A = OK, B or Start = back.
- **Keyboard (P1):** arrows; Z, X, C = A, B, C; A, S, D = X, Y, Z; Q, E = L, R; Enter = Start; F1 overlay; F11 fullscreen. In the overlay: arrows, Enter/Z = OK, Esc/X/Backspace = back.
- **Rebinding:** overlay → Input → Rebind keyboard / gamepads asks for each Saturn button (Esc or Back keeps the current one); Reset bindings restores the defaults. Stored as `key_<button>=<SDL scancode name>` and `pad_<button>=<SDL button name, or +axis / -axis>`.

## 11. Quality, CI and releases

**Tests** (21 `ctest` entries, none need game data):
- Per-instruction SH-2 (`test_sh2_interp`, all 142 instructions plus delay slots, exceptions, interrupts) and 68000 tests with hand-written vectors; decoder, analysis, machine (`test_saturn`: SMPC blocks, CD pause), video (`test_video`: compositor, VDP1, widescreen shift, line colour, special priority), sound (`test_scsp`), backup format (`test_bup`), overlay, CHD decoders (`test_chd`, vectors from `tests/mkvec_chd.py`), disc export round trip (`test_export`), input recordings (`test_rec`), cue, ISO, header, identification, SHA-1.
- `diff` (`test_diff`): `diffgen` assembles 64 random SH-2 programs at build time (every ALU and memory form, bt/bf incl. /S, loops, bsr, jsr through a literal, switch tables, bra, `ldc sr`); each runs on the interpreter, as generated code in one slice and with 5-cycle slices, and registers, SR, MAC, PR, PC and all 1 MB of RAM must match.
- `tcc`: generated C built by the bundled `sbpc-tcc` into a real library, loaded and run against the interpreter: the Build & Play path end to end.
- `cli_help`, `cli_scan_empty`: start the real executable.
- Optional local checks against the developer's own dumps (`SBPC_LOCAL_GAMES_DIR`), skipped in CI.

**CI** (`.github/workflows/ci.yml`): Linux gcc (`linux-release`), clang and no-SDL builds, and Windows MSVC with `-DSBPC_WERROR=ON`; build and ctest, logs uploaded on failure. Linux installs SDL's documented build dependencies (`libs/SDL/docs/README-linux.md`). Linux warnings show but don't fail yet.

**Releases** (`.github/workflows/release.yml`), on a `v*` tag push, or by hand from Actions (with a tag: rebuild that tag's downloads; without: a test build that publishes nothing):
- **Windows:** MSVC Release with the C runtime linked in (`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`, so no Visual C++ runtime is needed), tests, then `program/scripts/package_windows.py` makes `SBPC-<version>-windows-x64.zip` (data in `%APPDATA%\SBPC`) and `SBPC-<version>-windows-x64-portable.zip` (adds `portable.txt`). README texts and `portable.txt` live in `program/packaging/windows` (version and TinyCC commit filled in). A start-up check runs on each zip.
- **Linux:** `SBPC-<version>-x86_64.AppImage`, built on Ubuntu 22.04 (glibc 2.35+). SDL is static and loads X11, Wayland and audio at run time, so the AppImage carries no libraries; it holds `sbpc`, `sbpc-tcc`, `tcc/`, the licences, a desktop file and the icon. Linux has no separate `sbpc-cli`: `sbpc` takes `--cli` itself. A start-up check runs on it.
- **Publish:** `SHA256SUMS.txt`, and everything attached to the release. If the release doesn't exist yet it's created: a full release for a plain version tag (`v0.1.0`), a pre-release for a tag with a suffix (`v0.2.0-rc1`).
- Every download carries the licences of the bundled parts (SDL3 zlib, TinyCC LGPL 2.1 with its exact source commit, Half Eighties OFL) and the not-affiliated notice.

**Targets:** x86-64 Windows and Linux. ARM64 Linux is best effort. No telemetry.

## 12. Developer tools

**Working loop:** on Windows, `SBPC\rebuild.bat` finds VS Build Tools with `vswhere`, configures, builds, runs the tests and starts the launcher (output in `rebuild.log`). On Linux: `cmake --preset linux-release && cmake --build --preset linux-release && ctest --preset linux-release`. Check warnings with a clean build in a separate folder (`-DSBPC_WERROR=ON`). A running `builds\sbpc*.exe` locks the file and makes the build's copy step fail, so long headless jobs should use a copy elsewhere.

**Commands** (`sbpc --cli <command>`, or `sbpc-cli.exe --cli` on Windows; `--verbose` and `--trace=cpu,vdp1,vdp2,scsp,cd,input,hooks,disc` go before `--cli`):
- `boot <image> <frames> [--screenshot] [--screenshot-every=N]`: run headless and write `boot_report.txt` (registers, CD commands, CPU states, VDP registers, feature survey) and PNGs to the logs folder. It uses the built game's modules; `--portable` or a copy outside the repository layout sees its own data folder.
- `disasm <image> <hexaddr> <count> [frames]`, `generate <image>`, `analyse <image>`, `files <image>`, `identify <image>`, `scan <folder>`, `info`.
- `art <image>`: make the launcher wallpaper by hand.
- `call <image> <func> <frames> <in> <out>`: boot, then call a game function on the interpreter once per input record (`cmd_call`); used to run the graphics unpacker over every sprite.
- `chdverify <file.chd>`, `tracksha <image> <track> [out.raw]`, `extract <file.chd> <out.cue>` (CHD to cue/bin).

**Environment variables** (mostly for `boot`):
- Input and setup: `SBPC_INPUT="[player@]first-last:mask,..."` holds buttons (hex, active high: 0800 Start, 0400 A, 0100 B, 0200 C, 1000 Up, 2000 Down, 4000 Left, 8000 Right); `SBPC_PLAYERS=n` and `SBPC_PORTS=auto|direct|tap1|tap2|both`; `SBPC_WIDE=<pixels>` (53 = 16:9); `SBPC_BACKUP=file`; `SBPC_INTERP=1` (interpreter only); `SBPC_POKE=frame:hexaddr=hexbyte,...` (write high work RAM at a frame).
- **Recordings:** the game window always records its pad input to `<logs>/input.rec` (the previous session as `input.rec.1`; about 1.5 MB per 15 minutes; format in `saturn_rec.h`: the clock start, then per frame the layout and 12 pad words). `SBPC_REPLAY=file` plays one back in `boot`, `SBPC_RECORD=file` records a scripted run. Replays are exact (same picture and CPU state), which is how to reach any screen a player saw: play there, quit, replay `input.rec`.
- Finding code: `SBPC_PCHIST=N` (top PCs over the last 30 frames); `SBPC_IHIST=N` (interpreter profile and `ihist start` hint candidates); `SBPC_WATCHPC=a,b` with `SBPC_WATCHFROM=frame`, `SBPC_WATCHRING=1` (64 previous PCs), `SBPC_WATCHREGS=1` (registers and two stack words), `SBPC_WATCHALL=1` (every hit, not just the first). Recompiled code only reports PCs reached through the dispatcher.
- `SBPC_WATCHMEM=addr`: each change of a byte at any address (VDP1 VRAM included) with the PC that made it, first 40 changes, from `SBPC_WATCHFROM`. With recompiled code the PC is the running function's entry; `SBPC_INTERP=1` gives exact PCs but different timing, so scripted routes can end elsewhere.
- Dumps and capture: `SBPC_DUMP_WRAM`, `SBPC_DUMP_WRAML`, `SBPC_DUMP_VDP1`, `SBPC_DUMP_VDP2` (VRAM 512 KB, CRAM 4 KB, registers 512 bytes) at the end of the run; `SBPC_SHOT_FROM=frame` with `--screenshot-every`; `SBPC_LAYERS=hexmask` (bits 0-3 NBG0-3, bit 4 sprites; cleared bits hide); `SBPC_WAV=file.raw` (s16le stereo 44.1 kHz).
- Game window: `SBPC_PLAY_SHOT=<frame>` (with `SBPC_PLAY_SHOT_PAGE=video|audio|input|info|none`) saves `<logs>/play_shot.png` and quits; headless with `SDL_VIDEODRIVER=offscreen SDL_AUDIO_DRIVER=dummy` and `--portable`.
- Launcher: `SBPC_LAUNCHER_SHOT=file.bmp` saves the first list frame and quits; `SBPC_LAUNCHER_SHOT_MS=ms` takes it after that delay on any screen; `SBPC_LAUNCHER_AUTOPLAY=<version id>` presses Play on that game after the scan.

**Debugging recipe:** reproduce headless with `boot` (or replay the player's `input.rec`); find a stuck CPU with `SBPC_PCHIST`, then `disasm`; find who writes a value with `SBPC_WATCHMEM`; compare recompiled and interpreted runs (`SBPC_INTERP=1`); look for slow interpreted code with `SBPC_IHIST` (hint candidates for `versions.c`); for missing graphics, read the report's "Features not drawn yet" and hide layers with `SBPC_LAYERS`.

**Scripted routes:** USA/Europe reach the title with Start pulses at frames 600-1080 (every 120). Battle Game: then Down at 1150, Start at 1250, then A every 90 frames from 1700; a match starts by about frame 3700. Japan: Start at 600-1000 (every 100, 8-frame presses), Down at 1650, Start at 1700 (Battle); a second Down for Master. The title only accepts Start once its logo animation has finished.

## 13. What the discs showed

**How the USA disc boots.** The disc is a NetLink edition, reached through a chain of loaders:
1. **`/0`** (the 1st read file, at 0x06010000) is the PlanetWeb NetLink browser. It probes for a NetLink modem (UART scratch register at `0x2589501D`) and an XBAND modem. With neither, network init returns -506; the browser writes a `"Richard"` marker at 0x06000210 and loads `/NETLINK/XS028000.EXE`.
2. **`XS028000.EXE`** loads `XBMAP.BIN` (which names the game folder `BOMSS`), `XBAND/GAMEINFO.BIN`, `SAMPLEIP.BIN` (the game's real IP) and **`FLD_KNL.BIN`** (Hudson's "FLD" loader, running from low work RAM at 0x00280000).
3. **FLD** puts the IP at 0x06002000, copies its header to 0x06000C00 / 0x060002A0, sets the BIOS boot variables (0x06000CCC = 1, 0x06000CCE = directory ID 3 = `BOMSS`, counting "." and ".." as 0 and 1) and jumps into the IP's security code at 0x06002100.
4. SBPC's `ip_boot` then loads **`/BOMSS/0KRNL.BIN`** (768 KB) to 0x06006000 and jumps to the AIP. (`/0` also copies a 140-byte routine from 0x0604B4B0 to 0x00200000, or to just below 0x00300000, whichever avoids the destination, and runs it once from there; that routine is left to the interpreter.)
5. The kernel reads `/BOMSS` through the CD block's file-system commands (70/72/73) and opens `PROGRAM.BIN` once with Sega's file library, keeping the handle in a global.

The kernel's load address comes from its own pointers: at 0x06006000, 7,685 land on function prologues; the next best base scores 1,696. Its compiler uses `bsr` heavily and absolute-pointer switch tables; `/0` always calls through `jsr` with a literal. About 150-300 indirect calls and 38 kernel jumps are object/method dispatch through tables, resolved at run time.

**`PROGRAM.BIN`** (13 MB) holds no SH-2 code; its first 64 KB is the 68000 sound driver.
- The kernel's file table (USA, offsets in `0KRNL.BIN`): 72 big-endian u16 start sectors at `0x8DECC`, 72 u16 sizes at `0x8DF5C`, 72 u32 load addresses at `0x8DFEC`; the loader is `0x06008B26` (r4 = file number, r5 = slot), reached only through `0x0600AE58`. Japan: 66 files, table at `0x8B018`. Each file names itself at +0x2C: `s_init` (the sound driver), `base1`, `base2`, `openning`, `normal1`-`5`, `battle1`, `master1`, `staff`, `xband1` (USA only), sound banks `s_open` ... `s_bat1`, and voice clips `s_vo17` ... (USA up to `s_vo106`, Japan up to `s_voha`).
- Graphics files load verbatim into low work RAM (`base*` at `0x00200000`, the others at `0x00254000`) and start with 13 pointers, the last two holding the name. The files end at sector 5277 of 6336; the last ~2 MB and three USA files (41-43, where `s_vo35`-`37` would be; Japan points those numbers at copies of other clips) hold leftover older copies of sound banks and voices.
- Sprite records are 24 bytes: +2 id, +4 flags (`0x0880`), +8 size (width/8 << 8 | height), +10/+12 signed x/y offsets, +16 pointer to the data. Data starts with u16 unpacked size and u16 type; type 5 is raw 4bpp, anything else goes through the unpacker (USA `0x0603443C`, Japan `0x0603EA08`; r4 = data+4, r5 = output, r6 = size; the game unpacks to `0x002FB000`, then copies to VDP1 VRAM through `0x0609F708`). The movie decoder (`0x060951CC`, a 2x2/4x4 codebook) is separate.
- Running the game's own unpacker over every record gives correct sprites (880 of base2's 1456 match VDP1 VRAM byte for byte). `base2` holds every character's battle animations in order: Bomberman, Miss Honey, Kotetsu, Manjimaru, Master Higgins, Bonk, Milon, Kinu, Kabuki, Manto, Yuna, and nothing after. Measured packed lengths cover every graphics file with no stretch over 140 bytes uncovered: there is no orphaned sprite data.
- The Battle setup's bomb wallpaper (section 9) is NBG2; only its palette is stored raw.

**Battle Game characters.** The character-select cursor is one byte: USA/Europe `0x060C122C`, Japan `0x060C1F64`.
- Slots 0-9, the normal roster: W-Bomberman, B-Bomberman, Miss Honey, Kotetsu, Milon, Master Higgins, Bonk, Manjimaru, Kabuki, Kinu (Right wraps from 9 to 0).
- 10 Manto and 11 Yuna: the game's own unlockables (hold L + R on "Which mode?"). They have name plates but no pedestal.
- 12 and up: no name plate; picking one leaves player 1 invisible and the battle never starts (a loader at `0x0609E440` follows a null pointer). The tables end at 11.

**Characters cut before release.** Pretty Bomber, Ziria (Jiraiya) and Challenger have no slot, no table entry and no art in any supported version, the demo included. Their battle sprites exist only in a pre-release build (sheets found online match nothing on these discs: no exact or near match among all ~18,000 decoded sprites). Only voice clips remain: `s_vo23` "Jiraiya" and `s_vo28` "Challenger" (both on USA and Japanese discs, in gaps of the announcement list, never used) and `s_vo91` "Pretty Bomber". Adding them back would mean drawing complete characters (battle animations, select figure, name plate, HUD face, results art) and extending the tables; there is no data to switch back on.

**Battle voices.** `0x060496E4` picks the voice bank for a scene (`0x06049730`): per player, the character number from `0x060C064A` indexes a u32 list of file numbers at `0x060B68C0`, or in team play (`0x060C1048 & 0xC00` != `0x400`) the team colour from `0x060C0640` / 4 indexes `0x060B68F0`.
- Characters 0-11, USA: W-Bomberman `s_vo105`, B-Bomberman `s_vo106`, Miss Honey `s_vo102`, Kotetsu `s_vo103`, Milon `s_vo20`, Master Higgins `s_vo21`, Bonk `s_vo22`, Manjimaru `s_vo24`, Kabuki `s_vo25`, Kinu `s_vo26`, Manto `s_vo29`, Yuna `s_vo27`. Japan uses `s_vo19` for both Bombermen, `s_voha` (Honey) and `s_voko` (Kotetsu).
- Teams (White, Red, Blue, Yellow, Green): `s_vo30`-`s_vo34`. Other clips: `s_vo17` "hikiwake" (draw), USA `s_vo101` "no way you can stop me", USA `s_vo19` "Bomberman".
- Clips are 8-bit signed PCM (sample count at +0x86, data from +0xA0); the playback rate isn't established.

**Normal Game.** `0x060C029C` (byte) is the world (0-5); the scene loader at `0x0600AEB0` picks its files by it (world 1: files 14 and 4). Stages use special priority on all four background layers, and the stage-select map after a cleared stage uses the line colour screen (section 8).

**Japanese Master Game hurry gong.** At one minute left a gong (`急`) slides in and is drawn on every other frame only, while the start sign (the same kind of sprite) is steady. This is the game's own logic and stays as it is: the gong is two 88-byte tasks (`0x060C5948`, `0x060C599C`, created at `0x06069BF6`) whose state words alternate every frame (`+0x48` `8000`/`0000`, `+0x4E` `0102`/`0101`, `+0x78` `0300`/`0200`), written by their own handlers from the task scheduler (`0x0600AB94`); the empty half of each pair writes no VDP1 commands. The game keeps two VDP1 command lists (`0x00020`, `0x06D00`) and alternates between them.

## 14. Known gaps and next steps

**Known gaps:**
- **Video:** RBG0/1, zoom, line and vertical-cell scroll, line windows, mosaic, shadow, per-dot/colour-MSB colour calculation, extended (three-layer) colour calculation, the line colour screen as a layer of its own; VDP1 gouraud, half-transparency and mesh. None is enabled on the screens surveyed so far.
- **Sound:** no LFO or FM (unused by the driver). Not yet compared by ear with a real Saturn.
- **Not modelled:** SCU external interrupts (A-bus, vectors 0x50-0x5F), 68000 timing details, the slave SH-2 start protocol.
- **Overlay:** no frame-rate options (dropped, see section 10), no way to assign a specific pad to a player (pads follow connection order), no module build ID or interpreter fallback addresses on the Info page. No recompile settings exist.
- **PAL:** Europe runs at its native 50 Hz; a 60 Hz mode would only be offered once verified.
- **Linux:** the AppImage passes its start-up check in CI but hasn't been played on a Linux desktop yet. Linux warnings don't fail CI yet.
- **USA "stuck before the main menu":** reported once, not reproduced in about 300 headless runs with different Start timings (every run reached the title menu). If it happens again: the F1 → Info frame number, then `input.rec` and `sbpc.log`.

**Next steps (suggested order):**
1. Play the Linux AppImage on a desktop; then turn on `-DSBPC_WERROR=ON` for Linux in CI once a run is clean.
2. **Europe and Japan in depth:** script menus and battles as for the USA, then read the feature survey and `SBPC_IHIST`.
3. **More content:** later Normal Game worlds, other battle stages, the ending, options; use the feature survey to see what each screen needs.
4. **Coverage leftovers:** hints for hot spots in new screens; optionally drop `boot` from dispatch once the kernel runs (about 1,600 cheap code-check misses per 9,000 frames).
5. **Later:** a udev rules file for Linux pads, crash reports (host frames, both SH-2 states, recent addresses, the module hash; never game memory), C hooks for game fixes, sprites at a higher internal resolution.
