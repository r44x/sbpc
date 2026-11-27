/* sbpc: launcher (default), command-line tools (--cli) and the game window (--play). */
#include "core/log.h"
#include "core/paths.h"
#include "core/fs.h"
#include "launcher/launcher.h"
#ifdef SBPC_HAVE_SDL
#include "launcher/play.h"
#endif

#include <stdio.h>
#include <string.h>

#ifdef SBPC_HAVE_SDL
#include <SDL3/SDL_main.h>
#endif

#if defined(_WIN32) && !defined(SBPC_CONSOLE)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/* GUI builds use the WINDOWS subsystem; reattach to the parent console for CLI output.
 * (sbpc-cli.exe is a console program and doesn't need this.) */
static void attach_console(void)
{
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
}
#else
static void attach_console(void) {}
#endif

static void usage(void)
{
    printf("sbpc %s - Saturn Bomberman PC recompilation\n\n"
           "usage: sbpc [options]                 open the launcher\n"
           "       sbpc [options] --cli <cmd>     command-line tools (sbpc --cli help)\n"
           "       sbpc [options] --play <id>     play a game in a window (id: usa/jpn/eur, or a .cue/.chd path)\n\n"
           "options:\n"
           "  --portable          keep settings/logs in ./data next to the executable\n"
           "  --trace=<list>      detailed logs: core,disc,launcher,cpu,vdp1,vdp2,scsp,cd,input,hooks,all\n"
           "  --verbose           debug-level logging\n",
           SBPC_VERSION_STRING);
}

int main(int argc, char **argv)
{
    bool portable = false, cli = false, verbose = false;
    const char *traces = NULL, *play = NULL;
    int cli_index = argc, rc;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--portable") == 0) portable = true;
        else if (strcmp(argv[i], "--verbose") == 0) verbose = true;
        else if (strncmp(argv[i], "--trace=", 8) == 0) traces = argv[i] + 8;
        else if (strcmp(argv[i], "--cli") == 0) { cli = true; cli_index = i + 1; break; }
        else if (strcmp(argv[i], "--play") == 0 && i + 1 < argc) play = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            attach_console();
            usage();
            return 0;
        } else {
            attach_console();
            fprintf(stderr, "unknown option: %s\n\n", argv[i]);
            usage();
            return 2;
        }
    }

    if (cli || play)
        attach_console();

    if (!paths_init(portable)) {
        fprintf(stderr, "could not create the data folder\n");
        return 1;
    }
    if (!log_open(paths_log_dir()))
        fprintf(stderr, "warning: could not open log files in %s\n", paths_log_dir());
    if (verbose)
        log_set_level(LOG_DEBUG);
    if (traces && !log_enable_traces(traces))
        LOGW(SUB_CORE, "unknown subsystem in --trace=%s", traces);
    LOGI(SUB_CORE, "sbpc %s starting (data=%s%s)", SBPC_VERSION_STRING, paths_data_dir(),
         paths_is_portable() ? ", portable" : "");

    if (cli) {
        rc = launcher_cli(argc - cli_index, argv + cli_index);
    } else if (play) {
#ifdef SBPC_HAVE_SDL
        rc = play_run(play);
#else
        fprintf(stderr, "--play needs a build with SDL3\n");
        rc = 2;
#endif
    } else {
#ifdef SBPC_HAVE_SDL
        /* launcher -> Build & Play -> game window. Quitting the game (F1 -> Quit game, or closing the
         * window) ends SBPC; it doesn't reopen the launcher. */
        char cue[SBPC_PATH_MAX] = "";
        rc = launcher_gui(cue, sizeof cue);
        if (rc == 0 && cue[0]) {
            LOGI(SUB_CORE, "launching %s", cue);
            rc = play_run(cue);
        }
#else
        printf("This build has no GUI (built without SDL3). Use: sbpc --cli help\n");
        rc = 2;
#endif
    }

    LOGI(SUB_CORE, "exit %d", rc);
    log_close();
    return rc;
}
