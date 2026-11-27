# CLAUDE.md: working on SBPC

Read `program/docs/DESIGN.md` first: it is the single reference (decisions, how each part works, supported versions, what the discs showed, known gaps and next steps).

Layout: `SBPC/` is the repository. `program/` is SBPC itself (CMake, src, tests, scripts, docs), `libs/` the external submodules (SDL3, TinyCC), `games/` the user's dumps, `builds/` the built executables, `recompiled/` the built games and user data. `games/` (except its README), `builds/` and `recompiled/` are git-ignored: executables ship as release artifacts.

## Rules from the user

- **Don't commit or push** unless the user asks. The remote is github.com/r44x/sbpc (`origin`, branch `main`); the first full commit was pushed on 2026-11-27.
- **No game data in the repo**, ever: no disc files, no extracted assets, no generated C. Addresses and hashes are fine.
- **Dependencies:** SDL3 and TinyCC (submodules) only. Don't add libraries.
- TinyCC stays a **separate executable** (`sbpc-tcc`): never link libtcc into `sbpc` (MIT vs LGPL).
- Windows (MSVC) and Linux must both build. Keep C11 and warning-free under `-Wall -Wextra -Wpedantic` and MSVC `/W4`. Incremental builds only show warnings for recompiled files: check with a clean build (`-DSBPC_WERROR=ON` in a separate build folder).
- **Version table:** whenever work touches a specific version of the game, show the user the table of all versions with their support percentage (checklist and table in `program/docs/DESIGN.md`, "Supported versions"), updated with what the work changed.
- The user tests with `SBPC\rebuild.bat` and reads results in the running game. After changes, make sure `rebuild.bat` still works: it locates VS Build Tools with `vswhere`.

## Conventions

- Code style: 4-space indent, braces on the same line, `snake_case`. Short comments say *why* (hardware facts, game quirks).
- Log through `core/log.h` (`LOGI/LOGW/LOGE/LOGD/LOGT` with a subsystem). Don't use printf in library code.
- Bump `SBPC_CODEGEN_VERSION` (`src/codegen/codegen.h`) whenever analysis or generated C changes. Bump `SBPC_MODULE_ABI` (`src/sh2/sbpc_rt.h`) when the module interface changes.
- Hardware behaviour discovered from the game goes into `program/docs/DESIGN.md`, with the address or command that showed it.
- New tests go in `tests/` and are registered in the `foreach(t ...)` list in `program/CMakeLists.txt`. Tests never use game data.
- Things for the user to check in the game window are asked in the chat (the user dropped `TO_TEST.md`, 2026-11-27).

## Debugging recipe

1. Reproduce headless with `builds\sbpc-cli.exe --cli boot <image> <frames> --screenshot-every=N` (it uses the recompiled modules in `recompiled\`; `--portable` or the build-folder copy runs on the interpreter). Read `boot_report.txt` (registers, CD commands, CPU states, VDP registers, feature survey) and the PNGs in `recompiled\logs`. Script input with `SBPC_INPUT`.
2. Find where a CPU is stuck: `SBPC_PCHIST=40`, then `--cli disasm <cue> <addr> <n> <frame>`.
3. Find who writes a value: `SBPC_WATCHMEM=<addr>`. For how a PC is reached: `SBPC_WATCHPC=<a,b>` with `SBPC_WATCHFROM=<frame>`, `SBPC_WATCHRING=1` and `SBPC_WATCHREGS=1`.
4. Compare recompiled vs interpreted: run with and without `SBPC_INTERP=1`.
5. Slow or interpreted code: `SBPC_IHIST=40` lists hot interpreted pages and `ihist start` points (candidates for per-version hints in `versions.c`).
6. Missing graphics: the report's "Features not drawn yet" says which VDP features a screen turned on; `SBPC_LAYERS=hexmask` hides layers.
