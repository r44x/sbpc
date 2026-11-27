# Third-party code

Only these two code dependencies are allowed (see program/docs/DESIGN.md). Both are git submodules, added by `program/scripts/bootstrap.*`. The launcher's font is an asset, kept here because it's external.

| Path            | Project       | Licence  | Used for                                                                 |
|-----------------|---------------|----------|--------------------------------------------------------------------------|
| `SDL/`          | SDL3          | zlib     | window, input, gamepads, audio, file dialogs                             |
| `tinycc/`       | TinyCC        | LGPL     | compiling generated code on the user's PC (built as `sbpc-tcc`)          |
| `half-eighties/`| Half Eighties | OFL 1.1  | the launcher's 8x8 pixel font                                            |

TinyCC comes from the GitHub mirror `github.com/TinyCC/tinycc`, because its main host (repo.or.cz) returns HTTP 451 to UK connections.

TinyCC is built as a separate program, `sbpc-tcc`, that the launcher runs; it is never linked into SBPC, so the LGPL stays separate from SBPC's MIT code.

Half Eighties is by Jayvee Enaguas (HarvettFox96), under the SIL Open Font License 1.1 (`half-eighties/OFL.txt`; it is not public domain). `half_eighties.h` is the font converted to an 8x8 bitmap by `program/scripts/font_to_c.py` from `HalfEighties.ttf`; it stays under the OFL, and its header comment carries the copyright notice.
