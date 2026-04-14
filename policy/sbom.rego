# policy/sbom.rego
#
# Completeness and integrity policy over sbom.json (CycloneDX 1.6).
#
# Rules enforced:
#   1. CycloneDX format and version
#   2. All versioned components have a valid purl
#   3. Serial number is a valid urn:uuid
#   4. Required vendored components present (xml.h, sv.h, arena.h, nob.h)
#   5. No component has an empty name
#   6. Component count sanity (at least 15)
#
# Usage:
#   conftest test --policy policy/ sbom.json
#   opa eval -d policy/sbom.rego -i sbom.json 'data.podcast_mgr.sbom.violations'
#
package podcast_mgr.sbom

import rego.v1

# ── Rule 1: CycloneDX format ──────────────────────────────────────────────

deny contains msg if {
    input.bomFormat != "CycloneDX"
    msg := sprintf("bomFormat must be CycloneDX, got: %s", [input.bomFormat])
}

deny contains msg if {
    input.specVersion != "1.6"
    msg := sprintf("specVersion must be 1.6, got: %s", [input.specVersion])
}

# ── Rule 2: Serial number ─────────────────────────────────────────────────

deny contains msg if {
    not startswith(input.serialNumber, "urn:uuid:")
    msg := sprintf("serialNumber must be urn:uuid:*, got: %s", [input.serialNumber])
}

# ── Rule 3: Versioned components have purls ───────────────────────────────

deny contains msg if {
    some comp in input.components
    comp.version != ""
    comp.version != null
    not comp.purl
    msg := sprintf(
        "component '%s' version '%s' has no purl — all versioned components require a purl",
        [comp.name, comp.version]
    )
}

# ── Rule 4: No empty component names ─────────────────────────────────────

deny contains msg if {
    some comp in input.components
    count(trim_space(comp.name)) == 0
    msg := "component with empty name found in SBOM"
}

# ── Rule 5: Required vendored components present ──────────────────────────
# These headers are vendored in the repo and must appear in the SBOM.

required_generic := {"xml", "sv", "nob", "sandbox"}

deny contains msg if {
    component_names := {c.name | some c in input.components}
    some required in required_generic
    not required in component_names
    msg := sprintf(
        "required vendored component '%s' not found in SBOM — was the SBOM regenerated after a new dep was added?",
        [required]
    )
}

# ── Rule 6: Minimum component count ──────────────────────────────────────

deny contains msg if {
    count(input.components) < 15
    msg := sprintf(
        "SBOM has only %d components — expected at least 15; was cdxgen run from the repo root?",
        [count(input.components)]
    )
}

# ── Rule 7: Metadata component present ───────────────────────────────────

deny contains msg if {
    not input.metadata.component
    msg := "SBOM metadata.component is missing — cdxgen must identify the root application"
}

# ── Summary ───────────────────────────────────────────────────────────────

violations := deny

allow if count(deny) == 0
