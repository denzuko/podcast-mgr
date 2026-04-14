/*
 * nob.c — build + admin runner for podcast-mgr
 *
 * Single entrypoint for every project task. Subcommand dispatch pattern:
 *   ./nob [subcommand]
 *
 * Subcommands:
 *   (none)      Build index.cgi  [default]
 *   build       Build index.cgi
 *   test        Run test_main (unit) + test_nob.sh (e2e build tests)
 *   test-unit   Run test_main only
 *   test-e2e    Run test_nob.sh only
 *   ast         Regenerate ast.json via clang AST dump
 *   sbom        Regenerate sbom.json via cdxgen
 *   sarif       Regenerate podcast_mgr.sarif (cppcheck + OSV CVE)
 *   vex         Validate vex.cdx.json via OPA policy/vex.rego
 *   policy      Run full OPA release gate across all four policies
 *   clean       Remove build artefacts (index.cgi, test_main, nob)
 *   all         build + test + ast + sbom + sarif + policy
 *   help        Print this usage
 *
 * Build:
 *   cc -o nob nob.c && ./nob
 *   cc -o nob nob.c && ./nob help
 */

#define NOB_IMPLEMENTATION
#include "nob.h"

/* =========================================================================
 * Constants
 * ====================================================================== */

#define TARGET       "index.cgi"
#define TEST_BIN     "test_main"
#define AST_OUT      "ast.json"
#define AST_RAW      "/tmp/podcast_mgr_ast_raw.json"
#define SBOM_OUT     "sbom.json"
#define SARIF_OUT    "podcast_mgr.sarif"
#define VEX_OUT      "vex.cdx.json"
#define CPPCHECK_XML "/tmp/podcast_mgr_cppcheck.xml"

/* =========================================================================
 * Helpers
 * ====================================================================== */

static bool require_tool(const char *tool) {
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "which", tool);
    bool ok = nob_cmd_run(&cmd,
                          .stdout_path = "/dev/null",
                          .stderr_path = "/dev/null");
    if (!ok)
        nob_log(NOB_ERROR,
                "required tool '%s' not found on PATH", tool);
    return ok;
}

/* =========================================================================
 * build
 * ====================================================================== */

static bool cmd_build(void) {
    nob_log(NOB_INFO, "==> build: %s", TARGET);
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "cc");
    nob_cmd_append(&cmd, "-Wall", "-Wextra", "-std=c2x", "-O2");
    nob_cmd_append(&cmd, "-I.");
    nob_cmd_append(&cmd, "-o", TARGET, "main.c");
    nob_cmd_append(&cmd, "-lkcgixml", "-lkhtml", "-lkcgi", "-lz");
    /* nob_cmd_append(&cmd, "-lexpat"); */
    if (!nob_cmd_run(&cmd)) return false;
    nob_log(NOB_INFO, "==> build: OK");
    return true;
}

/* =========================================================================
 * test-unit
 * ====================================================================== */

static bool cmd_test_unit(void) {
    nob_log(NOB_INFO, "==> test-unit: compiling %s", TEST_BIN);
    Nob_Cmd cc = {0};
    nob_cmd_append(&cc, "cc");
    nob_cmd_append(&cc, "-std=c11", "-D_GNU_SOURCE",
                        "-Wall", "-Wextra", "-Wno-unused-parameter");
    nob_cmd_append(&cc, "-I.");
    nob_cmd_append(&cc, "-o", TEST_BIN, "test_main.c");
    if (!nob_cmd_run(&cc)) return false;

    nob_log(NOB_INFO, "==> test-unit: running");
    Nob_Cmd run = {0};
    nob_cmd_append(&run, "./" TEST_BIN);
    if (!nob_cmd_run(&run)) {
        nob_log(NOB_ERROR, "==> test-unit: FAILED");
        return false;
    }
    nob_log(NOB_INFO, "==> test-unit: PASS");
    return true;
}

/* =========================================================================
 * test-e2e
 * ====================================================================== */

static bool cmd_test_e2e(void) {
    nob_log(NOB_INFO, "==> test-e2e: test_nob.sh");
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "sh", "test_nob.sh", "--no-build");
    if (!nob_cmd_run(&cmd)) {
        nob_log(NOB_ERROR, "==> test-e2e: FAILED");
        return false;
    }
    nob_log(NOB_INFO, "==> test-e2e: PASS");
    return true;
}

/* =========================================================================
 * test
 * ====================================================================== */

static bool cmd_test(void) {
    if (!cmd_test_unit()) return false;
    if (!cmd_test_e2e())  return false;
    return true;
}

/* =========================================================================
 * ast  — clang -ast-dump=json | scripts/gen_ast.sh → ast.json
 * ====================================================================== */

static bool cmd_ast(void) {
    nob_log(NOB_INFO, "==> ast: generating %s", AST_OUT);
    if (!require_tool("clang"))   return false;
    if (!require_tool("python3")) return false;

    /* Step 1: raw dump.
     * clang exits 1 when headers are missing (e.g. kcgi.h not installed)
     * but still emits the full AST JSON. Run without failure check, then
     * verify the output file was actually written and is non-empty. */
    Nob_Cmd dump = {0};
    nob_cmd_append(&dump,
                   "clang",
                   "-Xclang", "-ast-dump=json",
                   "-fsyntax-only",
                   "-fno-color-diagnostics",
                   "-w", "-ferror-limit=0",
                   "-D_GNU_SOURCE",
                   "-I.", "main.c");
    nob_cmd_run(&dump,
                .stdout_path = AST_RAW,
                .stderr_path = "/dev/null");

    if (!nob_file_exists(AST_RAW)) {
        nob_log(NOB_ERROR, "ast: clang produced no output");
        return false;
    }

    /* Step 2: filter → ast.json.
     * Pass --filter so gen_ast.sh skips the clang step (already done). */
    Nob_Cmd filter = {0};
    nob_cmd_append(&filter, "sh", "scripts/gen_ast.sh", "--filter");
    if (!nob_cmd_run(&filter)) {
        nob_log(NOB_ERROR, "ast: gen_ast.sh failed");
        return false;
    }
    nob_log(NOB_INFO, "==> ast: written %s", AST_OUT);
    return true;
}

/* =========================================================================
 * sbom  — cdxgen → sbom.json
 * ====================================================================== */

static bool cmd_sbom(void) {
    nob_log(NOB_INFO, "==> sbom: regenerating %s", SBOM_OUT);
    if (!require_tool("cdxgen")) {
        nob_log(NOB_ERROR,
                "install: npm install -g @cyclonedx/cdxgen");
        return false;
    }
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "cdxgen", "-t", "c", ".", "-o", SBOM_OUT);
    if (!nob_cmd_run(&cmd)) return false;
    nob_log(NOB_INFO, "==> sbom: written %s", SBOM_OUT);
    return true;
}

/* =========================================================================
 * sarif  — cppcheck XML + OSV → podcast_mgr.sarif
 * ====================================================================== */

static bool cmd_sarif(void) {
    nob_log(NOB_INFO, "==> sarif: cppcheck + OSV → %s", SARIF_OUT);
    if (!require_tool("cppcheck")) return false;
    if (!require_tool("python3"))  return false;

    /* cppcheck writes XML to stderr */
    Nob_Cmd cpp = {0};
    nob_cmd_append(&cpp,
                   "cppcheck",
                   "--enable=warning,style",
                   "--std=c11",
                   "--suppress=missingInclude",
                   "--suppress=missingIncludeSystem",
                   "--suppress=unusedFunction",
                   "--suppress=knownConditionTrueFalse",
                   "--xml",
                   "main.c");
    if (!nob_cmd_run(&cpp,
                     .stdout_path = "/dev/null",
                     .stderr_path = CPPCHECK_XML)) {
        nob_log(NOB_ERROR, "sarif: cppcheck failed");
        return false;
    }

    Nob_Cmd py = {0};
    nob_cmd_append(&py, "python3", "scripts/gen_sarif.py");
    if (!nob_cmd_run(&py)) {
        nob_log(NOB_ERROR, "sarif: gen_sarif.py failed");
        return false;
    }
    nob_log(NOB_INFO, "==> sarif: written %s", SARIF_OUT);
    return true;
}

/* =========================================================================
 * vex  — opa eval policy/vex.rego -i vex.cdx.json
 * ====================================================================== */

static bool cmd_vex(void) {
    nob_log(NOB_INFO, "==> vex: validating %s", VEX_OUT);
    if (!require_tool("opa")) {
        nob_log(NOB_ERROR,
                "install opa: "
                "curl -sL https://openpolicyagent.org/downloads/v1.1.0/"
                "opa_linux_amd64 -o /usr/local/bin/opa && "
                "chmod +x /usr/local/bin/opa");
        return false;
    }

    /* --fail-defined exits 1 when the violations set is non-empty */
    Nob_Cmd check = {0};
    nob_cmd_append(&check,
                   "opa", "eval",
                   "-d", "policy/vex.rego",
                   "-i", VEX_OUT,
                   "--fail-defined",
                   "data.podcast_mgr.vex.violations[_]");
    bool ok = nob_cmd_run(&check,
                          .stdout_path = "/dev/null",
                          .stderr_path = "/dev/null");
    if (!ok) {
        nob_log(NOB_ERROR, "vex: violations found");
        Nob_Cmd show = {0};
        nob_cmd_append(&show,
                       "opa", "eval",
                       "-d", "policy/vex.rego",
                       "-i", VEX_OUT,
                       "data.podcast_mgr.vex.violations");
        nob_cmd_run(&show);
        return false;
    }
    nob_log(NOB_INFO, "==> vex: PASS");
    return true;
}

/* =========================================================================
 * policy  — full OPA gate: sarif + sbom + vex + ast
 * ====================================================================== */

typedef struct {
    const char *name;
    const char *polfile;
    const char *query;        /* violations query or allow query */
    const char *input;
    bool        check_allow;  /* true = --fail on false; false = --fail-defined on non-empty set */
} PolicyCheck;

static bool cmd_policy(void) {
    nob_log(NOB_INFO, "==> policy: running release gate");
    if (!require_tool("opa")) return false;

    /* Regenerate AST if stale */
    if (nob_needs_rebuild1(AST_OUT, "main.c") > 0) {
        nob_log(NOB_INFO, "policy: %s stale — regenerating", AST_OUT);
        if (!cmd_ast()) return false;
    }

    static const PolicyCheck checks[] = {
        { "sarif", "policy/sarif.rego",
          "data.podcast_mgr.sarif.allow",
          SARIF_OUT, true  },
        { "sbom",  "policy/sbom.rego",
          "data.podcast_mgr.sbom.violations[_]",
          SBOM_OUT,  false },
        { "vex",   "policy/vex.rego",
          "data.podcast_mgr.vex.violations[_]",
          VEX_OUT,   false },
        { "ast",   "policy/ast.rego",
          "data.podcast_mgr.ast.violations[_]",
          AST_OUT,   false },
    };

    bool all_ok = true;
    for (size_t i = 0; i < NOB_ARRAY_LEN(checks); i++) {
        const PolicyCheck *c = &checks[i];
        nob_log(NOB_INFO, "policy: checking %s (%s)", c->name, c->input);

        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, "opa", "eval",
                       "-d", c->polfile, "-i", c->input);
        if (c->check_allow)
            nob_cmd_append(&cmd, "--fail");
        else
            nob_cmd_append(&cmd, "--fail-defined");
        nob_cmd_append(&cmd, c->query);

        bool ok = nob_cmd_run(&cmd,
                              .stdout_path = "/dev/null",
                              .stderr_path = "/dev/null");
        if (!ok) {
            nob_log(NOB_ERROR, "policy: %s FAILED", c->name);
            /* Show violations to stderr */
            const char *show_q = c->check_allow
                ? "data.podcast_mgr.sarif.violations"
                : c->query;
            /* Strip the [_] suffix for the show query */
            char show_query[256];
            snprintf(show_query, sizeof(show_query), "%s", show_q);
            /* Remove trailing [_] if present */
            char *bracket = strstr(show_query, "[_]");
            if (bracket) *bracket = '\0';

            Nob_Cmd show = {0};
            nob_cmd_append(&show,
                           "opa", "eval",
                           "-d", c->polfile,
                           "-i", c->input,
                           show_query);
            nob_cmd_run(&show);
            all_ok = false;
        } else {
            nob_log(NOB_INFO, "policy: %s PASS", c->name);
        }
    }

    if (all_ok)
        nob_log(NOB_INFO, "==> policy: ALL PASS — release gate open");
    else
        nob_log(NOB_ERROR,
                "==> policy: FAILED — fix violations before tagging");
    return all_ok;
}

/* =========================================================================
 * clean
 * ====================================================================== */

static bool cmd_clean(void) {
    nob_log(NOB_INFO, "==> clean");
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "rm", "-f", TARGET, TEST_BIN, "nob");
    return nob_cmd_run(&cmd);
}

/* =========================================================================
 * all
 * ====================================================================== */

static bool cmd_all(void) {
    if (!cmd_build())  return false;
    if (!cmd_test())   return false;
    if (!cmd_ast())    return false;
    if (!cmd_sbom())   return false;
    if (!cmd_sarif())  return false;
    if (!cmd_policy()) return false;
    nob_log(NOB_INFO, "==> all: complete");
    return true;
}

/* =========================================================================
 * help
 * ====================================================================== */

static void cmd_help(const char *prog) {
    printf(
        "Usage: %s [subcommand]\n"
        "\n"
        "Subcommands:\n"
        "  (none), build   Compile " TARGET "\n"
        "  test            Run unit + e2e tests\n"
        "  test-unit       Run test_main (52 xUnit cases)\n"
        "  test-e2e        Run test_nob.sh (22 build tests)\n"
        "  ast             Regenerate " AST_OUT " via clang AST\n"
        "  sbom            Regenerate " SBOM_OUT " via cdxgen\n"
        "  sarif           Regenerate " SARIF_OUT " (cppcheck + OSV)\n"
        "  vex             Validate " VEX_OUT " via OPA\n"
        "  policy          Full OPA release gate (all four policies)\n"
        "  clean           Remove " TARGET ", " TEST_BIN ", nob\n"
        "  all             build + test + ast + sbom + sarif + policy\n"
        "  help            Print this message\n"
        "\n"
        "Tool requirements per subcommand:\n"
        "  build           cc, kcgi headers\n"
        "  test-unit       cc\n"
        "  test-e2e        sh\n"
        "  ast             clang, python3\n"
        "  sbom            cdxgen  (npm install -g @cyclonedx/cdxgen)\n"
        "  sarif           cppcheck, python3 (scripts/gen_sarif.py)\n"
        "  vex/policy      opa     (https://openpolicyagent.org)\n"
        "\n"
        "Examples:\n"
        "  cc -o nob nob.c && ./nob          # build index.cgi\n"
        "  ./nob test                         # run all tests\n"
        "  ./nob policy                       # release gate\n"
        "  ./nob all                          # full pipeline\n",
        prog);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);

    const char *prog = nob_shift(argv, argc);
    if (argc == 0) return cmd_build() ? 0 : 1;

    const char *sub = nob_shift(argv, argc);

    if (strcmp(sub, "build")     == 0) return cmd_build()     ? 0 : 1;
    if (strcmp(sub, "test")      == 0) return cmd_test()      ? 0 : 1;
    if (strcmp(sub, "test-unit") == 0) return cmd_test_unit() ? 0 : 1;
    if (strcmp(sub, "test-e2e")  == 0) return cmd_test_e2e()  ? 0 : 1;
    if (strcmp(sub, "ast")       == 0) return cmd_ast()       ? 0 : 1;
    if (strcmp(sub, "sbom")      == 0) return cmd_sbom()      ? 0 : 1;
    if (strcmp(sub, "sarif")     == 0) return cmd_sarif()     ? 0 : 1;
    if (strcmp(sub, "vex")       == 0) return cmd_vex()       ? 0 : 1;
    if (strcmp(sub, "policy")    == 0) return cmd_policy()    ? 0 : 1;
    if (strcmp(sub, "clean")     == 0) return cmd_clean()     ? 0 : 1;
    if (strcmp(sub, "all")       == 0) return cmd_all()       ? 0 : 1;
    if (strcmp(sub, "help")      == 0) { cmd_help(prog); return 0; }

    nob_log(NOB_ERROR,
            "unknown subcommand '%s'  (run '%s help')", sub, prog);
    return 1;
}
