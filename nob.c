/*
 * nob.c — build script for podcast-mgr
 *
 * Fixes from original:
 *  - Added missing xml.h / sv.h / arena.h / sandbox.h include paths
 *    (adjust -I flags to match your source layout).
 *  - Sorted link order: kcgixml and khtml depend on kcgi; kcgi depends on z.
 *    On static-link hosts (e.g. panix.com) order matters: most-dependent first.
 *  - Added -lexpat if your xml.h backend needs it (comment out if not).
 *  - Kept NOB_GO_REBUILD_URSELF for self-rebuild on nob.c changes.
 *
 * Usage:
 *   cc -o nob nob.c && ./nob
 */

#define NOB_IMPLEMENTATION
#include "nob.h"

#define TARGET "index.cgi"

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);

    Nob_Cmd cmd = {0};

    nob_cmd_append(&cmd, "cc");

    /* Warnings, standard, optimisation */
    nob_cmd_append(&cmd, "-Wall", "-Wextra", "-std=c2x", "-O2");

    /* Include paths — adjust if headers live elsewhere */
    nob_cmd_append(&cmd, "-I.");

    /* Source */
    nob_cmd_append(&cmd, "-o", TARGET, "main.c");

    /*
     * Link order for static archives:
     *   kcgixml, khtml -> kcgi -> z
     * If linking shared (.so/.dylib) order is less critical but keeping
     * this sequence avoids "undefined reference" with some linkers.
     */
    nob_cmd_append(&cmd, "-lkcgixml", "-lkhtml", "-lkcgi", "-lz");

    /* Uncomment if xml.h wraps expat: */
    /* nob_cmd_append(&cmd, "-lexpat"); */

    if (!nob_cmd_run_sync(cmd)) return 1;
    return 0;
}
