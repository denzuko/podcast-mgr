# scripts/gen_sarif.jq
#
# Merge cppcheck pipe-delimited findings + osv-scanner SARIF
# into a unified SARIF 2.1.0 document.
#
# Input:  osv-scanner SARIF JSON (via -i osv.sarif)
# Arg:    $cpp  — cppcheck findings as a JSON array (via --argjson cpp)
# Arg:    $serial — SBOM serial number string (via --arg serial)
# Arg:    $ts     — ISO timestamp string (via --arg ts)
#
# Usage (see nob.c cmd_sarif):
#   jq -f scripts/gen_sarif.jq \
#      --argjson cpp "$(cppcheck ... | scripts/cpp2json.sh)" \
#      --arg serial "$(jq -r .serialNumber sbom.json)" \
#      --arg ts "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
#      osv.sarif

# Level mapping for cppcheck severity
def cpp_level:
  if . == "error"   then "error"
  elif . == "warning" then "warning"
  else "note"
  end;

# Bring in osv-scanner run (may have zero results)
. as $osv |

# Build unified tool extensions
($osv.runs[0].tool.driver |
  { name: "osv-scanner",
    version: (.version // "unknown"),
    informationUri: (.informationUri // "https://github.com/google/osv-scanner") }
) as $osv_ext |

# cppcheck rules — deduplicated by id
([$cpp[] | .id] | unique |
  map({
    id: ("cppcheck/" + .),
    shortDescription: { text: ("cppcheck/" + .) },
    helpUri: "https://cppcheck.sourceforge.io/",
    defaultConfiguration: { level: "note" }
  })
) as $cpp_rules |

# OSV rules from osv-scanner run
($osv.runs[0].tool.driver.rules // []) as $osv_rules |

# cppcheck results
[$cpp[]
  | select(.id != "toomanyconfigs" and .severity != "information")
  | select(.file != "nofile")
  | {
      ruleId: ("cppcheck/" + .id),
      level:  (.severity | cpp_level),
      message: { text: (.msg + if .cwe != "" then " [CWE-" + .cwe + "]" else "" end) },
      locations: [{
        physicalLocation: {
          artifactLocation: { uri: .file, uriBaseId: "%SRCROOT%" },
          region: { startLine: (.line | tonumber), startColumn: (.col | tonumber) }
        }
      }]
    }
] as $cpp_results |

# OSV results from osv-scanner run
($osv.runs[0].results // []) as $osv_results |

{
  "$schema": "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/master/Schemata/sarif-schema-2.1.0.json",
  version: "2.1.0",
  runs: [{
    tool: {
      driver: {
        name:    "podcast-mgr security scan",
        version: "1.0.0",
        rules:   ($cpp_rules + $osv_rules)
      },
      extensions: [
        { name: "cppcheck",    version: "2.13.0",
          informationUri: "https://cppcheck.sourceforge.io/" },
        $osv_ext,
        { name: "cdxgen",      version: "12.1.5",
          informationUri: "https://cyclonedx.github.io/cdxgen/" }
      ]
    },
    invocations: [{ executionSuccessful: true, endTimeUtc: $ts }],
    results: ($cpp_results + $osv_results),
    columnKind: "utf16CodeUnits",
    properties: {
      sbom: { serialNumber: $serial },
      scanSummary: {
        cppcheckFindings: ($cpp_results | length),
        cveFindings:      ($osv_results | length),
        totalFindings:    (($cpp_results | length) + ($osv_results | length)),
        status:           (if (($cpp_results | length) + ($osv_results | length)) == 0
                           then "PASS" else "FAIL" end)
      }
    }
  }]
}
