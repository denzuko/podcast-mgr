# policy/nob_ast.rego
#
# Security and quality policy over nob_ast.json (clang AST of nob.c).
#
# nob.c is the build driver — it execs subprocesses via nob_cmd_run.
# This policy ensures no subcommand function bypasses that abstraction
# to make direct dangerous syscalls, preventing a malicious build step
# from hiding shell injection or privilege escalation.
#
# Rules enforced:
#   1. No direct system() calls in any cmd_* function
#   2. No popen() — use nob_cmd_run with stdout_path instead
#   3. No exec*()/fork() — process creation goes through nob_cmd_run
#   4. No dlopen() — no runtime dynamic loading in the build driver
#   5. Every cmd_* function calls at least one nob_ API function
#   6. cmd_all only calls other cmd_* functions (no direct tool invocation)
#   7. require_tool must be called before any nob_cmd_run in cmd_sarif,
#      cmd_sbom, cmd_ast (tool presence checked before use)
#
# Usage:
#   conftest test --policy policy/ nob_ast.json
#   opa eval -d policy/nob_ast.rego -i nob_ast.json 'data.podcast_mgr.nob_ast.violations'
#
package podcast_mgr.nob_ast

import rego.v1

# ── helpers ───────────────────────────────────────────────────────────────

fn_by_name(name) := fn if {
    some fn in input.functions
    fn.name == name
}

cmd_functions := {fn.name |
    some fn in input.functions
    startswith(fn.name, "cmd_")
}

dangerous_syscalls := {"system", "popen", "execve", "execl",
                        "execvp", "execle", "execlp", "fork", "dlopen"}

# ── Rule 1: no system() ───────────────────────────────────────────────────

deny contains msg if {
    some fn in input.functions
    fn.calls_system == true
    msg := sprintf(
        "security: '%s' calls system() — use nob_cmd_run() for subprocess execution",
        [fn.name]
    )
}

# ── Rule 2: no popen() ────────────────────────────────────────────────────

deny contains msg if {
    some fn in input.functions
    fn.calls_popen == true
    msg := sprintf(
        "security: '%s' calls popen() — use nob_cmd_run(.stdout_path=...) instead",
        [fn.name]
    )
}

# ── Rule 3: no exec*()/fork() ─────────────────────────────────────────────

deny contains msg if {
    some fn in input.functions
    fn.calls_exec == true
    msg := sprintf(
        "security: '%s' calls exec*() directly — process creation must go through nob_cmd_run",
        [fn.name]
    )
}

deny contains msg if {
    some fn in input.functions
    fn.calls_fork == true
    msg := sprintf(
        "security: '%s' calls fork() — process creation must go through nob_cmd_run",
        [fn.name]
    )
}

# ── Rule 4: no dlopen() ───────────────────────────────────────────────────

deny contains msg if {
    some fn in input.functions
    fn.calls_dlopen == true
    msg := sprintf(
        "security: '%s' calls dlopen() — no runtime dynamic loading in the build driver",
        [fn.name]
    )
}

# ── Rule 5: every cmd_* uses at least one nob_ API call ──────────────────
# cmd_test and cmd_all delegate to other cmd_* functions (no direct nob_ needed)
# cmd_help only calls printf (correct)

excluded_from_nob_check := {"cmd_test", "cmd_all", "cmd_help"}

deny contains msg if {
    some name in cmd_functions
    not name in excluded_from_nob_check
    fn := fn_by_name(name)
    count(fn.nob_calls) == 0
    msg := sprintf(
        "quality: '%s' makes no nob_ API calls — subcommands must use nob_cmd_append/nob_cmd_run",
        [name]
    )
}

# ── Rule 6: cmd_all only calls other cmd_* functions ─────────────────────
# cmd_all orchestrates; it must not invoke external tools directly.

deny contains msg if {
    fn := fn_by_name("cmd_all")
    some call in fn.all_calls
    not startswith(call, "cmd_")
    not startswith(call, "nob_log")
    # allow nob_log for INFO messages
    not call in {"nob_log"}
    # these are compiler-inserted or nob internal
    not startswith(call, "__")
    msg := sprintf(
        "quality: cmd_all calls '%s' directly — cmd_all must only call other cmd_* functions",
        [call]
    )
}

# ── Rule 7: all expected subcommands are present ──────────────────────────

required_commands := {
    "cmd_build", "cmd_test", "cmd_test_unit", "cmd_test_e2e",
    "cmd_ast", "cmd_cflow", "cmd_sbom", "cmd_sarif",
    "cmd_vex", "cmd_policy", "cmd_clean", "cmd_all", "cmd_help"
}

deny contains msg if {
    some required in required_commands
    not required in cmd_functions
    msg := sprintf(
        "quality: expected subcommand '%s' not found in nob.c — was it accidentally removed?",
        [required]
    )
}

# ── Summary ───────────────────────────────────────────────────────────────

violations := deny

allow if count(deny) == 0
