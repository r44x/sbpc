/* Runs a game in an SDL window. `what` is a .cue path or a version id (usa/jpn/eur) looked up in
 * the saved games folder. Returns a process exit code. Only available when built with SDL3. */
#ifndef SBPC_LAUNCHER_PLAY_H
#define SBPC_LAUNCHER_PLAY_H

int play_run(const char *what);

#endif
