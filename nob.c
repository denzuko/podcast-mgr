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
 *   ast         Regenerate ast.json via clang AST dump | jq filter
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
#define NOB_AST_OUT  "nob_ast.json"
#define NOB_AST_RAW  "/tmp/podcast_mgr_nob_ast_raw.json"
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
 * kcgi_prefix — locate kcgi headers + libs at runtime
 *
 * Search order mirrors the conventional install hierarchy:
 *   1. $HOME/.local   (user-local: ./configure --prefix=$HOME/.local)
 *   2. /usr/local     (system-local: BSD ports, manual install)
 *   3. /usr           (distro package manager fallback)
 *
 * Returns the first prefix where <prefix>/include/kcgi.h exists, or NULL
 * if kcgi is not found under any of the candidates (system headers / default
 * search path will be tried by the compiler without explicit flags).
 * ====================================================================== */

static const char *kcgi_prefix(void) {
    static char buf[4096];

    /* candidate list: $HOME/.local first, then system prefixes */
    const char *candidates[4];
    char home_local[2048] = {0};
    int  n = 0;

    const char *home = getenv("HOME");
    if (NULL != home && '\0' != home[0]) {
        snprintf(home_local, sizeof(home_local), "%s/.local", home);
        candidates[n++] = home_local;
    }
    candidates[n++] = "/usr/local";
    candidates[n++] = "/usr";
    candidates[n]   = NULL;

    for (int i = 0; i < n; ++i) {
        snprintf(buf, sizeof(buf), "%s/include/kcgi.h", candidates[i]);
        if (nob_file_exists(buf)) {
            /* return the prefix, not the header path */
            snprintf(buf, sizeof(buf), "%s", candidates[i]);
            nob_log(NOB_INFO, "build: kcgi found at prefix %s", buf);
            return buf;
        }
    }
    nob_log(NOB_WARNING,
            "build: kcgi.h not found under $HOME/.local, /usr/local, or /usr"
            " — relying on compiler default search path");
    return NULL;
}

/* =========================================================================
 * build
 * ====================================================================== */

static bool cmd_build(void) {
    nob_log(NOB_INFO, "==> build: %s", TARGET);
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, "cc");
    nob_cmd_append(&cmd, "-Wall", "-Wextra", "-std=c2x", "-O2");
    nob_cmd_append(&cmd, "-Isrc");

    /* Inject -I<prefix>/include and -L<prefix>/lib when kcgi is installed
     * outside the compiler's default search path (e.g. $HOME/.local or
     * /usr/local on systems where only /usr is in the default path). */
    const char *pfx = kcgi_prefix();
    char inc[4096], lib[4096];
    if (NULL != pfx) {
        snprintf(inc, sizeof(inc), "-I%s/include", pfx);
        snprintf(lib, sizeof(lib), "-L%s/lib",     pfx);
        nob_cmd_append(&cmd, inc, lib);
    }

    nob_cmd_append(&cmd, "-o", TARGET, "src/main.c");
    nob_cmd_append(&cmd, "-lkhtml", "-lkcgi", "-lz");
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
    nob_cmd_append(&cc, "-Isrc", "-I.");
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
 * ast  — clang -ast-dump=json | jq -f scripts/ast_filter.jq → ast.json
 * ====================================================================== */

static bool cmd_ast(void) {
    nob_log(NOB_INFO, "==> ast: generating %s", AST_OUT);
    if (!require_tool("clang")) return false;
    if (!require_tool("jq"))    return false;

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
                   "-Isrc", "-I.");
    /* Mirror kcgi_prefix so clang can resolve kcgi.h for a complete AST */
    {
        char ast_inc[4096];
        const char *ast_pfx = kcgi_prefix();
        if (NULL != ast_pfx) {
            snprintf(ast_inc, sizeof(ast_inc), "-I%s/include", ast_pfx);
            nob_cmd_append(&dump, ast_inc);
        }
    }
    nob_cmd_append(&dump, "src/main.c");
    nob_cmd_run(&dump,
                .stdout_path = AST_RAW,
                .stderr_path = "/dev/null");
    /* clang exits 1 when system headers are absent (e.g. kcgi.h not
     * installed on the build host).  It still emits the full AST JSON
     * for the translation unit.  The ERROR above is expected. */
    if (!nob_file_exists(AST_RAW)) {
        nob_log(NOB_ERROR, "ast: clang produced no output");
        return false;
    }

    /* Step 2: filter → ast.json via jq.
     * jq reads the raw 25 MB clang AST, recursively traverses CallExpr
     * and GotoStmt nodes, and emits the compact structured summary that
     * policy/ast.rego consumes.  Depends only on libc — no Python runtime. */
    Nob_Cmd filter = {0};
    nob_cmd_append(&filter, "jq", "-f", "scripts/ast_filter.jq", AST_RAW);
    if (!nob_cmd_run(&filter, .stdout_path = AST_OUT)) {
        nob_log(NOB_ERROR, "ast: jq filter failed");
        return false;
    }
    nob_log(NOB_INFO, "==> ast: written %s", AST_OUT);
    return true;
}

/* =========================================================================
 * nob-ast  — clang AST of nob.c | jq → nob_ast.json
 * ====================================================================== */

static bool cmd_nob_ast(void) {
    nob_log(NOB_INFO, "==> nob-ast: generating %s", NOB_AST_OUT);
    if (!require_tool("clang")) return false;
    if (!require_tool("jq"))    return false;

    Nob_Cmd dump = {0};
    nob_cmd_append(&dump,
                   "clang",
                   "-Xclang", "-ast-dump=json",
                   "-fsyntax-only",
                   "-fno-color-diagnostics",
                   "-w", "-ferror-limit=0",
                   "-I.", "nob.c");
    nob_cmd_run(&dump,
                .stdout_path = NOB_AST_RAW,
                .stderr_path = "/dev/null");
    /* clang exits 1 on missing system headers — expected, AST still emitted */
    if (!nob_file_exists(NOB_AST_RAW)) {
        nob_log(NOB_ERROR, "nob-ast: clang produced no output");
        return false;
    }

    Nob_Cmd filter = {0};
    nob_cmd_append(&filter, "jq", "-f", "scripts/nob_ast_filter.jq", NOB_AST_RAW);
    if (!nob_cmd_run(&filter, .stdout_path = NOB_AST_OUT)) {
        nob_log(NOB_ERROR, "nob-ast: jq filter failed");
        return false;
    }
    nob_log(NOB_INFO, "==> nob-ast: written %s", NOB_AST_OUT);
    return true;
}

/* =========================================================================
 * cflow  — cflow static call graph → cflow.txt
 * ====================================================================== */

#define CFLOW_OUT "cflow.txt"

static bool cmd_cflow(void) {
    nob_log(NOB_INFO, "==> cflow: generating %s", CFLOW_OUT);
    if (!require_tool("cflow")) return false;
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd,
                   "cflow", "--omit-arguments", "--no-main",
                   "src/main.c");
    if (!nob_cmd_run(&cmd, .stdout_path = CFLOW_OUT)) {
        nob_log(NOB_ERROR, "cflow: failed");
        return false;
    }
    nob_log(NOB_INFO, "==> cflow: written %s", CFLOW_OUT);
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
 * sarif  — cppcheck + osv-scanner + jq → podcast_mgr.sarif
 *
 * Pipeline (no Python):
 *   1. cppcheck --template → pipe-delimited findings
 *   2. shell awk → JSON array  → $cpp argjson
 *   3. cdxgen --spec-version 1.5 → SBOM_OSV (osv-scanner compat)
 *   4. osv-scanner --sbom SBOM_OSV --format sarif → OSV_SARIF
 *   5. jq -f scripts/gen_sarif.jq merges both → SARIF_OUT
 * ====================================================================== */

#define SBOM_OSV  "/tmp/podcast_mgr_sbom_osv.json"
#define OSV_SARIF "/tmp/podcast_mgr_osv.sarif"
#define CPP_JSON  "/tmp/podcast_mgr_cppcheck.json"

static bool cmd_sarif(void) {
    nob_log(NOB_INFO, "==> sarif: cppcheck + osv-scanner → %s", SARIF_OUT);
    if (!require_tool("cppcheck"))    return false;
    if (!require_tool("cdxgen"))      return false;
    if (!require_tool("osv-scanner")) return false;
    if (!require_tool("jq"))          return false;

    /* Step 1+2: cppcheck --template → awk → JSON array */
    /* Pipe: cppcheck ... | awk '{...}' > CPP_JSON */
    Nob_Cmd cpp = {0};
    nob_cmd_append(&cpp, "sh", "-c",
        "cppcheck --enable=warning,style --std=c11"
        " --suppress=missingInclude --suppress=missingIncludeSystem"
        " --suppress=unusedFunction --suppress=knownConditionTrueFalse"
        " --file-filter='*src/main.c'"
        " --template='{id}|{severity}|{cwe}|{message}|{file}|{line}|{column}'"
        " src/main.c 2>&1 | grep -v '^Checking'"
        " | awk -F'|'"
        " 'BEGIN{print \"[\"} {gsub(/\"/,\"\\\\\\\"\",$4);"
        " if(NR>1)printf\",\"; printf\"{\\\"id\\\":\\\"%s\\\","
        "\\\"severity\\\":\\\"%s\\\",\\\"cwe\\\":\\\"%s\\\","
        "\\\"msg\\\":\\\"%s\\\",\\\"file\\\":\\\"%s\\\","
        "\\\"line\\\":\\\"%s\\\",\\\"col\\\":\\\"%s\\\"}\","
        "$1,$2,$3,$4,$5,$6,$7} END{print \"]\"}'");
    if (!nob_cmd_run(&cpp, .stdout_path = CPP_JSON)) {
        nob_log(NOB_ERROR, "sarif: cppcheck pipeline failed");
        return false;
    }

    /* Step 3: cdxgen CycloneDX 1.5 for osv-scanner */
    Nob_Cmd cdx = {0};
    nob_cmd_append(&cdx, "cdxgen", "-t", "c", ".",
                        "--spec-version", "1.5", "-o", SBOM_OSV);
    if (!nob_cmd_run(&cdx,
                     .stdout_path = "/dev/null",
                     .stderr_path = "/dev/null")) {
        nob_log(NOB_ERROR, "sarif: cdxgen (spec 1.5) failed");
        return false;
    }

    /* Step 4: osv-scanner → SARIF (exits 1 on vulns found — that's ok) */
    Nob_Cmd osv = {0};
    nob_cmd_append(&osv, "osv-scanner",
                        "--sbom", SBOM_OSV,
                        "--format", "sarif");
    nob_cmd_run(&osv, .stdout_path = OSV_SARIF, .stderr_path = "/dev/null");
    if (!nob_file_exists(OSV_SARIF)) {
        nob_log(NOB_ERROR, "sarif: osv-scanner produced no output");
        return false;
    }

    /* Step 5: jq merge → SARIF_OUT */
    Nob_Cmd merge = {0};
    nob_cmd_append(&merge, "sh", "-c",
        "jq -f scripts/gen_sarif.jq"
        " --argjson cpp \"$(cat " CPP_JSON ")\""
        " --arg serial \"$(jq -r '.serialNumber // \"urn:uuid:unknown\"' " SBOM_OUT ")\""
        " --arg ts \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\""
        " " OSV_SARIF " > " SARIF_OUT);
    if (!nob_cmd_run(&merge)) {
        nob_log(NOB_ERROR, "sarif: jq merge failed");
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

    /* Deps (ast, nob-ast, sbom, sarif) are guaranteed to have run before
     * this function when invoked via the DAG (dag_run("policy")).
     * Direct invocation (./nob policy) also triggers dep resolution. */

    static const PolicyCheck checks[] = {
        { "sarif",   "policy/sarif.rego",
          "data.podcast_mgr.sarif.allow",
          SARIF_OUT,   true  },
        { "sbom",    "policy/sbom.rego",
          "data.podcast_mgr.sbom.violations[_]",
          SBOM_OUT,    false },
        { "vex",     "policy/vex.rego",
          "data.podcast_mgr.vex.violations[_]",
          VEX_OUT,     false },
        { "ast",     "policy/ast.rego",
          "data.podcast_mgr.ast.violations[_]",
          AST_OUT,     false },
        { "nob-ast", "policy/nob_ast.rego",
          "data.podcast_mgr.nob_ast.violations[_]",
          NOB_AST_OUT, false },
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

/* =========================================================================
 * install
 *
 * Linux:  deploys index.cgi + user systemd units for the calling user.
 *         Wires socket ACL so system haproxy can reach the user socket.
 *         Requires: systemctl, loginctl, setfacl (acl package).
 *         Does NOT require root for the unit install itself — only the
 *         loginctl enable-linger and setfacl steps need sudo.
 *
 * BSD:    prints the pkg install command and exits.  Package management
 *         is handled natively; we don't second-guess it.
 * ====================================================================== */

#ifdef __linux__

#include <linux/limits.h>  /* PATH_MAX */

/* Resolve the calling user's XDG_RUNTIME_DIR without relying on the
 * env var being set (it may not be in a nob session).  Fallback:
 * /run/user/<uid>  which is the systemd-logind convention. */
static void runtime_dir(char *buf, size_t n) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (NULL != xdg && '\0' != xdg[0]) {
        snprintf(buf, n, "%s", xdg);
    } else {
        snprintf(buf, n, "/run/user/%u", (unsigned)getuid());
    }
}

static bool cmd_install(void) {
    nob_log(NOB_INFO, "==> install (Linux user-unit deployment)");

    /* ── Paths ── */
    char rt[PATH_MAX];
    runtime_dir(rt, sizeof(rt));

    char sock_dir[PATH_MAX], unit_dir[PATH_MAX], cgi_dest[PATH_MAX];
    /* The suffix strings are short; PATH_MAX is a safe upper bound.
     * Suppress -Wformat-truncation: paths will never approach PATH_MAX. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(sock_dir,  sizeof(sock_dir),  "%s/podcast-mgr",       rt);
    snprintf(unit_dir,  sizeof(unit_dir),  "%s/.config/systemd/user",
             getenv("HOME") ? getenv("HOME") : ".");
    snprintf(cgi_dest,  sizeof(cgi_dest),  "%s/podcast-mgr/%s",
             getenv("HOME") ? getenv("HOME") : ".", TARGET);
#pragma GCC diagnostic pop

    /* ── 1. Build index.cgi if not already built ── */
    if (!nob_file_exists(TARGET)) {
        nob_log(NOB_INFO, "install: " TARGET " not found — building first");
        if (!cmd_build()) return false;
    }

    /* ── 2. Install index.cgi to ~/podcast-mgr/ ── */
    nob_log(NOB_INFO, "install: deploying %s -> %s", TARGET, cgi_dest);
    {
        char dest_dir[PATH_MAX];
        snprintf(dest_dir, sizeof(dest_dir), "%s/podcast-mgr",
                 getenv("HOME") ? getenv("HOME") : ".");
        Nob_Cmd mk = {0};
        nob_cmd_append(&mk, "mkdir", "-p", dest_dir);
        if (!nob_cmd_run(&mk)) return false;

        Nob_Cmd cp = {0};
        nob_cmd_append(&cp, "cp", TARGET, cgi_dest);
        if (!nob_cmd_run(&cp)) return false;

        Nob_Cmd ch = {0};
        nob_cmd_append(&ch, "chmod", "0755", cgi_dest);
        if (!nob_cmd_run(&ch)) return false;
    }

    /* ── 3. Install user unit files ── */
    nob_log(NOB_INFO, "install: unit dir %s", unit_dir);
    {
        Nob_Cmd mk = {0};
        nob_cmd_append(&mk, "mkdir", "-p", unit_dir);
        if (!nob_cmd_run(&mk)) return false;
    }

    const char *units[] = {
        "examples/podcast-mgr.socket",
        "examples/podcast-mgr.service",
    };
    for (size_t i = 0; i < sizeof(units)/sizeof(units[0]); ++i) {
        if (!nob_file_exists(units[i])) {
            nob_log(NOB_ERROR, "install: %s not found", units[i]);
            return false;
        }
        Nob_Cmd cp = {0};
        nob_cmd_append(&cp, "cp", units[i], unit_dir);
        if (!nob_cmd_run(&cp)) return false;
        nob_log(NOB_INFO, "install: installed %s -> %s", units[i], unit_dir);
    }

    /* Patch ExecStart in the installed service to point at the built cgi */
    {
        char svc_path[PATH_MAX];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(svc_path, sizeof(svc_path),
                 "%s/podcast-mgr.service", unit_dir);
#pragma GCC diagnostic pop
        char sed_expr[PATH_MAX * 2];
        snprintf(sed_expr, sizeof(sed_expr),
                 "s|/usr/local/libexec/podcast-mgr/index.cgi|%s|g",
                 cgi_dest);
        Nob_Cmd sed = {0};
        nob_cmd_append(&sed, "sed", "-i", sed_expr, svc_path);
        if (!nob_cmd_run(&sed)) return false;
    }

    /* ── 4. Reload user daemon and enable socket ── */
    nob_log(NOB_INFO, "install: systemctl --user daemon-reload");
    {
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, "systemctl", "--user", "daemon-reload");
        if (!nob_cmd_run(&cmd)) return false;
    }
    {
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd,
                       "systemctl", "--user", "enable", "--now",
                       "podcast-mgr.socket");
        if (!nob_cmd_run(&cmd)) return false;
    }

    /* ── 5. Create socket dir and apply ACL for system haproxy ── */
    nob_log(NOB_INFO, "install: socket dir %s", sock_dir);
    {
        Nob_Cmd mk = {0};
        nob_cmd_append(&mk, "mkdir", "-p", sock_dir);
        nob_cmd_run(&mk);   /* may already exist — ignore error */
    }

    /* setfacl: grant haproxy user rx on runtime dir and socket dir.
     * This requires the acl package and the calling user to have
     * permission to set ACLs on their own files.
     * Failures here are non-fatal — print guidance and continue. */
    nob_log(NOB_INFO,
            "install: setting ACL so haproxy can reach socket");
    nob_log(NOB_INFO,
            "  (requires 'acl' package; errors here are non-fatal)");
    {
        /* ACL on XDG_RUNTIME_DIR itself */
        Nob_Cmd acl = {0};
        nob_cmd_append(&acl, "setfacl", "-m", "u:haproxy:rx", rt);
        if (!nob_cmd_run(&acl))
            nob_log(NOB_WARNING,
                    "setfacl on %s failed — run manually or use "
                    "'sudo usermod -aG %s haproxy'",
                    rt, getenv("USER") ? getenv("USER") : "nuci3");

        /* ACL on socket subdir */
        Nob_Cmd acl2 = {0};
        nob_cmd_append(&acl2, "setfacl", "-m", "u:haproxy:rx", sock_dir);
        nob_cmd_run(&acl2);

        /* Default ACL so the socket itself inherits rw for haproxy */
        Nob_Cmd acl3 = {0};
        nob_cmd_append(&acl3, "setfacl", "-dm", "u:haproxy:rw", sock_dir);
        nob_cmd_run(&acl3);
    }

    nob_log(NOB_INFO, "==> install: done");
    nob_log(NOB_INFO,
            "  Socket will appear at: %s/fcgi.sock", sock_dir);
    nob_log(NOB_INFO,
            "  To enable boot-time start (one-time, requires sudo):");
    nob_log(NOB_INFO,
            "    sudo loginctl enable-linger %s",
            getenv("USER") ? getenv("USER") : "$(whoami)");
    nob_log(NOB_INFO,
            "  To verify: systemctl --user status podcast-mgr.socket");
    return true;
}

#else /* BSD — defer to native package management */

static bool cmd_install(void) {
    nob_log(NOB_INFO, "==> install (BSD)");
    nob_log(NOB_INFO,
            "BSD package management handles service installation natively.");
    nob_log(NOB_INFO,
            "Install the port/package, then enable via rc.conf:");
    nob_log(NOB_INFO, "  FreeBSD:  pkg install podcast-mgr");
    nob_log(NOB_INFO, "            sysrc podcast_mgr_enable=YES");
    nob_log(NOB_INFO, "            service podcast_mgr start");
    nob_log(NOB_INFO, "  OpenBSD:  pkg_add podcast-mgr");
    nob_log(NOB_INFO, "            rcctl enable podcast_mgr");
    nob_log(NOB_INFO, "            rcctl start podcast_mgr");
    nob_log(NOB_INFO,
            "See examples/fcgiwrap.conf for rc.d unit templates.");
    return true;
}

#endif /* __linux__ */

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
 * DAG — dependency graph, topological executor, and cmd_all
 *
 * Each Node has a name, a function pointer, and a list of dependency names.
 * dag_run(name) performs a DFS: runs all deps before the node itself, and
 * visits each node exactly once regardless of how many chains reference it.
 *
 * Adding a subcommand: add a Node row. Deps are resolved by name at runtime.
 * ====================================================================== */

typedef bool (*CmdFn)(void);

typedef struct {
    const char  *name;
    CmdFn        fn;
    const char **deps;
    size_t       ndeps;
} Node;

/* Visited flags — index matches node table order.
 * Static so the DFS state persists across recursive calls in one run. */
#define MAX_NODES 32
static bool dag_visited[MAX_NODES];
static bool dag_failed[MAX_NODES];

/* Convenience macro: list deps as a compound literal */
#define DEPS(...) ((const char *[]){__VA_ARGS__}), \
                  (sizeof((const char *[]){__VA_ARGS__}) / sizeof(const char *))
#define NO_DEPS   NULL, 0

/* Forward declarations for functions referenced in dag_nodes.
 * Only needed for cmd_policy which references cmd_ast etc. defined later —
 * but all cmd_* are defined BEFORE this section, so none are needed.
 * Kept as documentation of the full node set. */

static const Node dag_nodes[] = {
    /* name         fn             deps                          */
    { "build",   cmd_build,   NO_DEPS                          },
    { "test",    cmd_test,    DEPS("build")                    },
    { "ast",     cmd_ast,     NO_DEPS                          },
    { "nob-ast", cmd_nob_ast, NO_DEPS                          },
    { "cflow",   cmd_cflow,   NO_DEPS                          },
    { "sbom",    cmd_sbom,    NO_DEPS                          },
    { "sarif",   cmd_sarif,   DEPS("sbom")                     },
    { "vex",     cmd_vex,     NO_DEPS                          },
    { "policy",  cmd_policy,  DEPS("ast","nob-ast","sbom","sarif") },
    { "install", cmd_install, DEPS("build")                    },
    { "clean",   cmd_clean,   NO_DEPS                          },
    { "all",     NULL,        DEPS("build","test","ast","nob-ast",
                                   "cflow","sbom","sarif","policy") },
};
#define DAG_LEN (sizeof(dag_nodes) / sizeof(dag_nodes[0]))

static_assert(DAG_LEN <= MAX_NODES, "dag_nodes exceeds MAX_NODES — increase it");

static int dag_find(const char *name) {
    for (int i = 0; i < (int)DAG_LEN; i++)
        if (strcmp(dag_nodes[i].name, name) == 0) return i;
    return -1;
}

/* DFS topological executor. Returns false on first failure. */
static bool dag_run(const char *name) {
    int idx = dag_find(name);
    if (idx < 0) {
        nob_log(NOB_ERROR, "dag: unknown node '%s'", name);
        return false;
    }
    if (dag_failed[idx])  return false;   /* propagate earlier failure */
    if (dag_visited[idx]) return true;    /* already ran successfully  */

    const Node *n = &dag_nodes[idx];

    /* Run deps first */
    for (size_t d = 0; d < n->ndeps; d++) {
        if (!dag_run(n->deps[d])) {
            dag_failed[idx] = true;
            return false;
        }
    }

    /* Run this node (NULL fn = virtual aggregate node — deps only) */
    if (n->fn) {
        nob_log(NOB_INFO, "dag: running '%s'", name);
        if (!n->fn()) {
            nob_log(NOB_ERROR, "dag: '%s' failed", name);
            dag_failed[idx] = true;
            return false;
        }
    }

    dag_visited[idx] = true;
    return true;
}

/* Reset DAG state — call before each top-level dag_run invocation */
static void dag_reset(void) {
    memset(dag_visited, 0, sizeof(dag_visited));
    memset(dag_failed,  0, sizeof(dag_failed));
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
        "  test            Run unit + e2e tests (dep: build)\n"
        "  test-unit       Run test_main (52 xUnit cases)\n"
        "  test-e2e        Run test_nob.sh build tests\n"
        "  install         Deploy index.cgi + user units (Linux) or print\n"
        "                  pkg install instructions (BSD)  (dep: build)\n"
        "  ast             Regenerate " AST_OUT " via clang + jq\n"
        "  nob-ast         Regenerate " NOB_AST_OUT " (build driver AST)\n"
        "  cflow           Generate " CFLOW_OUT " static call graph\n"
        "  sbom            Regenerate " SBOM_OUT " via cdxgen\n"
        "  sarif           Regenerate " SARIF_OUT " (dep: sbom)\n"
        "  vex             Validate " VEX_OUT " via OPA\n"
        "  policy          Full OPA release gate (dep: ast,nob-ast,sbom,sarif)\n"
        "  clean           Remove " TARGET ", " TEST_BIN ", nob\n"
        "  all             Full pipeline via DAG\n"
        "  help            Print this message\n"
        "\n"
        "DAG dependency edges:\n"
        "  test    → build\n"
        "  install → build\n"
        "  sarif   → sbom\n"
        "  policy  → ast, nob-ast, sbom, sarif\n"
        "  all     → build, test, ast, nob-ast, cflow, sbom, sarif, policy\n"
        "\n"
        "Tool requirements per subcommand:\n"
        "  build           cc, kcgi headers\n"
        "  test-unit       cc\n"
        "  test-e2e        sh\n"
        "  ast             clang, jq\n"
        "  nob-ast         clang, jq\n"
        "  cflow           cflow\n"
        "  sbom            cdxgen  (npm install -g @cyclonedx/cdxgen)\n"
        "  sarif           cppcheck, cdxgen, osv-scanner, jq\n"
        "  vex/policy      opa     (https://openpolicyagent.org)\n"
        "\n"
        "Examples:\n"
        "  cc -o nob nob.c && ./nob          # build index.cgi\n"
        "  ./nob test                         # build then test\n"
        "  ./nob policy                       # full release gate\n"
        "  ./nob all                          # complete DAG pipeline\n",
        prog);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);

    const char *prog = nob_shift(argv, argc);
    if (argc == 0) { dag_reset(); return dag_run("build") ? 0 : 1; }

    const char *sub = nob_shift(argv, argc);

    /* Leaf subcommands that bypass the DAG (no deps, run directly) */
    if (strcmp(sub, "test-unit") == 0) return cmd_test_unit() ? 0 : 1;
    if (strcmp(sub, "test-e2e")  == 0) return cmd_test_e2e()  ? 0 : 1;
    if (strcmp(sub, "help")      == 0) { cmd_help(prog); return 0; }

    /* Everything else routes through the DAG */
    if (dag_find(sub) >= 0) {
        dag_reset();
        return dag_run(sub) ? 0 : 1;
    }

    nob_log(NOB_ERROR,
            "unknown subcommand '%s'  (run '%s help')", sub, prog);
    return 1;
}
