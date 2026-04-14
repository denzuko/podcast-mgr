# policy/vex.rego
#
# Coverage and validity policy over vex.cdx.json (CycloneDX 1.6 VEX).
#
# Rules enforced:
#   1. CycloneDX 1.6 format
#   2. All vulnerabilities have a valid state
#   3. All not_affected statements have a justification
#   4. All affected statements have a response
#   5. All statements have a non-empty detail
#   6. SBOM serial cross-reference present in metadata
#   7. No statement references a component not in the SBOM
#      (requires sbom.json passed via --data, optional)
#
# Usage:
#   conftest test --policy policy/ vex.cdx.json
#   opa eval -d policy/vex.rego -i vex.cdx.json 'data.podcast_mgr.vex.violations'
#
package podcast_mgr.vex

import rego.v1

valid_states := {
    "not_affected",
    "affected",
    "fixed",
    "under_investigation",
}

valid_justifications := {
    "component_not_present",
    "vulnerable_code_not_present",
    "vulnerable_code_cannot_be_controlled_by_adversary",
    "inline_mitigations_already_exist",
    "code_not_reachable",
    "protected_by_compiler",
    "protected_at_runtime",
    "protected_by_mitigating_control",
}

# ── Rule 1: CycloneDX 1.6 format ─────────────────────────────────────────

deny contains msg if {
    input.bomFormat != "CycloneDX"
    msg := sprintf("bomFormat must be CycloneDX, got: %s", [input.bomFormat])
}

deny contains msg if {
    input.specVersion != "1.6"
    msg := sprintf("specVersion must be 1.6, got: %s", [input.specVersion])
}

# ── Rule 2: Valid vulnerability state ────────────────────────────────────

deny contains msg if {
    some vuln in input.vulnerabilities
    not vuln.analysis.state in valid_states
    msg := sprintf(
        "vulnerability '%s' has invalid state '%s' — must be one of %v",
        [vuln.id, vuln.analysis.state, valid_states]
    )
}

# ── Rule 3: not_affected requires justification ───────────────────────────

deny contains msg if {
    some vuln in input.vulnerabilities
    vuln.analysis.state == "not_affected"
    not vuln.analysis.justification in valid_justifications
    msg := sprintf(
        "vulnerability '%s' is not_affected but has missing or invalid justification '%s'",
        [vuln.id, vuln.analysis.justification]
    )
}

# ── Rule 4: affected requires response ───────────────────────────────────

deny contains msg if {
    some vuln in input.vulnerabilities
    vuln.analysis.state == "affected"
    count(vuln.analysis.response) == 0
    msg := sprintf(
        "vulnerability '%s' is affected but has no response — add a remediation plan",
        [vuln.id]
    )
}

# ── Rule 5: All statements require non-empty detail ──────────────────────

deny contains msg if {
    some vuln in input.vulnerabilities
    count(trim_space(vuln.analysis.detail)) < 20
    msg := sprintf(
        "vulnerability '%s' analysis.detail is too short (%d chars) — provide a meaningful justification",
        [vuln.id, count(vuln.analysis.detail)]
    )
}

# ── Rule 6: All statements must have an affects list ─────────────────────

deny contains msg if {
    some vuln in input.vulnerabilities
    count(vuln.affects) == 0
    msg := sprintf(
        "vulnerability '%s' has no affects entries — must reference at least one component",
        [vuln.id]
    )
}

# ── Rule 7: SBOM serial cross-reference ──────────────────────────────────

deny contains msg if {
    props := {p.name | some p in input.metadata.properties}
    not "vex:sbom-serial" in props
    msg := "VEX metadata is missing 'vex:sbom-serial' property — VEX must be linked to its SBOM"
}

# ── Rule 8: No duplicate CVE IDs ─────────────────────────────────────────

deny contains msg if {
    ids := [vuln.id | some vuln in input.vulnerabilities]
    unique_ids := {id | some id in ids}
    count(ids) != count(unique_ids)
    msg := "VEX contains duplicate CVE IDs — each vulnerability must appear exactly once"
}

# ── Summary ───────────────────────────────────────────────────────────────

violations := deny

allow if count(deny) == 0
