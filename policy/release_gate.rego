# policy/release_gate.rego
#
# Aggregated release gate — deny release if ANY sub-policy has violations.
# This is the single policy to evaluate before tagging a release.
#
# The gate checks:
#   - SARIF scan is PASS (podcast_mgr.sarif)
#   - SBOM is complete (sbom.json)
#   - VEX is valid and covers all known CVEs (vex.cdx.json)
#   - AST code style passes (ast.json)
#
# Each sub-policy can also be run independently; this aggregates them.
#
# Usage (run all at once with conftest):
#   sh scripts/gen_ast.sh          # regenerate ast.json from main.c
#   conftest test --policy policy/ \
#     podcast_mgr.sarif sbom.json vex.cdx.json ast.json
#
# Usage (OPA direct):
#   opa eval \
#     -d policy/ \
#     -i podcast_mgr.sarif \
#     'data.podcast_mgr.release_gate.allow'
#
package podcast_mgr.release_gate

import rego.v1
import data.podcast_mgr.sarif
import data.podcast_mgr.sbom
import data.podcast_mgr.vex
import data.podcast_mgr.ast

# ── Gate ──────────────────────────────────────────────────────────────────

allow if {
    count(sarif.violations) == 0
    count(sbom.violations)  == 0
    count(vex.violations)   == 0
    count(ast.violations)   == 0
}

# ── Aggregate violations for reporting ───────────────────────────────────

all_violations contains msg if {
    some msg in sarif.violations
}

all_violations contains msg if {
    some msg in sbom.violations
}

all_violations contains msg if {
    some msg in vex.violations
}

all_violations contains msg if {
    some msg in ast.violations
}

# ── Summary ───────────────────────────────────────────────────────────────

summary := {
    "allow":          count(all_violations) == 0,
    "total_violations": count(all_violations),
    "sarif_violations": count(sarif.violations),
    "sbom_violations":  count(sbom.violations),
    "vex_violations":   count(vex.violations),
    "ast_violations":   count(ast.violations),
}
