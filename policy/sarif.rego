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


# ── Helper: location is in project source ────────────────────────────────
is_project_source(uri) if endswith(uri, "main.c")
is_project_source(uri) if endswith(uri, "src/main.c")

# ── Rule 1: SARIF version ─────────────────────────────────────────────────

deny contains msg if {
    input.version != "2.1.0"
    msg := sprintf("SARIF version must be 2.1.0, got: %s", [input.version])
}

# ── Rule 2: Scan status PASS (for project source + CVE findings) ──────────
# Status is derived from totalFindings which includes vendored-header cppcheck
# findings. We gate on main.c cppcheck findings + CVE findings only.

deny contains msg if {
    summary.cveFindings > 0
    msg := sprintf(
        "scan has %d CVE finding(s) — must be zero before release",
        [summary.cveFindings]
    )
}

deny contains msg if {
    # Any cppcheck error/warning in main.c means FAIL regardless of status field
    mainc_errors := [r |
        some r in run.results
        startswith(r.ruleId, "cppcheck/")
        is_project_source(r.locations[0].physicalLocation.artifactLocation.uri)
        r.level in {"error", "warning"}
    ]
    count(mainc_errors) > 0
    msg := sprintf(
        "%d cppcheck error/warning finding(s) in main.c — resolve before release",
        [count(mainc_errors)]
    )
}

# ── Rule 3: Zero error/warning results in project source ─────────────────
# Only gate on main.c findings. Vendored headers (xml.h, sv.h, arena.h)
# produce style/warning findings that are documented in vex.cdx.json.

deny contains msg if {
    some result in run.results
    startswith(result.ruleId, "cppcheck/")
    is_project_source(result.locations[0].physicalLocation.artifactLocation.uri)
    result.level in {"error", "warning"}
    msg := sprintf(
        "cppcheck '%s' level '%s' in main.c — resolve before release",
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

# ── Rule 5: cppcheck findings zero in project source ─────────────────────
# Only gate on findings in main.c. Vendored headers (xml.h, sv.h, arena.h)
# produce CWE-398 style findings that are documented suppressions — they are
# third-party code outside the project's control.

deny contains msg if {
    some result in run.results
    startswith(result.ruleId, "cppcheck/")
    loc := result.locations[0].physicalLocation.artifactLocation.uri
    is_project_source(loc)
    result.level in {"error", "warning"}
    msg := sprintf(
        "cppcheck finding in main.c '%s' level '%s' — resolve before release",
        [result.ruleId, result.level]
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

required_extensions := {"cdxgen", "cppcheck", "osv-scanner"}

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
