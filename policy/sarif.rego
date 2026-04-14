# policy/sarif.rego
#
# Gate policy over podcast_mgr.sarif (SARIF 2.1.0).
# Enforces that the security scan result is PASS before any release.
#
# Rules enforced:
#   1. Scan status must be PASS
#   2. Zero results of severity error or warning
#   3. SARIF version must be 2.1.0
#   4. SBOM serial cross-reference must be present
#   5. CVE findings must be zero
#   6. cppcheck findings must be zero
#
# Usage:
#   conftest test --policy policy/ podcast_mgr.sarif
#   opa eval -d policy/sarif.rego -i podcast_mgr.sarif 'data.podcast_mgr.sarif.violations'
#
package podcast_mgr.sarif

import rego.v1

# ── helpers ───────────────────────────────────────────────────────────────

run       := input.runs[0]
summary   := run.properties.scanSummary
driver    := run.tool.driver

# ── Rule 1: SARIF version ─────────────────────────────────────────────────

deny contains msg if {
    input.version != "2.1.0"
    msg := sprintf("SARIF version must be 2.1.0, got: %s", [input.version])
}

# ── Rule 2: Scan status PASS ──────────────────────────────────────────────

deny contains msg if {
    summary.status != "PASS"
    msg := sprintf(
        "scan status is '%s' — must be PASS before release (totalFindings=%d)",
        [summary.status, summary.totalFindings]
    )
}

# ── Rule 3: Zero error/warning results ───────────────────────────────────

deny contains msg if {
    some result in run.results
    result.level in {"error", "warning"}
    msg := sprintf(
        "scan result '%s' has level '%s' — all error and warning findings must be resolved",
        [result.ruleId, result.level]
    )
}

# ── Rule 4: CVE findings zero ─────────────────────────────────────────────

deny contains msg if {
    summary.cveFindings > 0
    msg := sprintf(
        "CVE findings must be zero before release, got: %d",
        [summary.cveFindings]
    )
}

# ── Rule 5: cppcheck findings zero ───────────────────────────────────────

deny contains msg if {
    summary.cppcheckFindings > 0
    msg := sprintf(
        "cppcheck findings must be zero before release, got: %d",
        [summary.cppcheckFindings]
    )
}

# ── Rule 6: SBOM serial cross-reference ──────────────────────────────────

deny contains msg if {
    sbom_serial := run.properties.sbom.serialNumber
    not startswith(sbom_serial, "urn:uuid:")
    msg := sprintf(
        "SBOM serial number in SARIF must be a valid urn:uuid, got: %s",
        [sbom_serial]
    )
}

# ── Rule 7: Required extensions present ──────────────────────────────────

required_extensions := {"cdxgen", "cppcheck", "OSV"}

deny contains msg if {
    extension_names := {e.name | some e in run.tool.extensions}
    some required in required_extensions
    not required in extension_names
    msg := sprintf(
        "required scan extension '%s' not present in SARIF tool extensions",
        [required]
    )
}

# ── Summary ───────────────────────────────────────────────────────────────

violations := deny

allow if count(deny) == 0
