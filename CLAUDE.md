# CLAUDE.md

This file provides context for AI coding assistants working in this repository.

## Project overview

`podcast-mgr` is a two-component system:

- **`index.cgi`** — FastCGI SPA (C, BCHS stack) that manages `feeds.xml`
  via a browser UI. Add, edit, delete podcast subscriptions.
- **`podcast.sh`** — tcsh cron script that reads `feeds.xml` and downloads
  episodes. Runs independently at midnight; not invoked by the CGI.

The components are intentionally decoupled. The CGI owns the write path;
the cron job owns the read/download path.

## Build

```sh
cc -o nob nob.c && ./nob     # produces index.cgi
```

Post-clone hook does this automatically if activated:
```sh
git config core.hooksPath .githooks
```

## Test

```sh
# Unit tests (pure-C, no kcgi required)
cc -std=c11 -D_GNU_SOURCE -I. -o test_main test_main.c && ./test_main

# End-to-end build tests (requires cc; kcgi optional for full run)
sh test_nob.sh

# Skip kcgi-dependent suites (CI without kcgi)
sh test_nob.sh --no-build
```

52 xUnit test cases in `test_main.c`. 22 end-to-end tests in `test_nob.sh`.
All must pass before committing.

## Stack

| Layer | Library |
|-------|---------|
| FastCGI framing + field parsing | kcgi |
| SPA root document | khtml (one function: `render_shell`) |
| All htmx partial responses | kxml |
| XML parse (startup only) | mrvladus/xml.h |
| String views | tsoding/sv |
| Memory | arena.h (tsoding/arena, zero-value init) |
| Sandbox | sandbox.h (seccomp / Capsicum / pledge) |
| Build | tsoding/nob |

## Key design rules

**Data-driven, not scattered.** `FIELDS[FIELD_COUNT]` is the single source
of truth for XML attribute names, form labels, input kinds, length caps,
and enum option lists. Renderers, validators, and the serialiser all iterate
this table. Adding a field = one row in `FIELDS[]` + one entry in `PodcastAttr`.

**khtml only in `render_shell`, with caveats.** `render_shell` uses khtml
for `<head>` where all attributes are standard `KATTR_*` enum values.
`<body>` and its descendants are emitted via raw `khttp_puts` because
htmx (`hx-*`) and Alpine.js (`x-*`) attributes are not in the `kattr`
enum and cannot be expressed through `khtml_attr` or `khtml_attrx`.
`khtml_attrx` takes `KATTR_*` enum keys with typed values (`KATTRX_STRING`,
`KATTRX_INT`, `KATTRX_DOUBLE`) — it does not accept arbitrary string
attribute names. Every other output function uses kxml.

**Arena owns all strings for the worker lifetime.** `kp->val` from kcgi is
freed by `khttp_free` at the end of each request. Any string that needs to
outlive the request (POST field values stored in `PodcastArray`) must be
`arena_alloc` + `memcpy` before `khttp_free` is called.

**Atomic writes only.** `write_feeds_xml` writes to `feeds.xml.tmp` and
renames. If anything fails, the original is untouched. The in-memory `db`
is snapshot/restored on write failure so it stays consistent with disk.

**`xml_node_serialize` is broken.** mrvladus/xml.h emits `</tag attr="v">`
for self-closing nodes. Use `XMLString` + `xml_string_append` directly.

**xml.h does not entity-decode attributes.** `xml_node_attr` returns the
raw escaped form (`&amp;` not `&`). This is documented in `test_main.c`
suite 6 and in the BUGS section of `index.cgi.8`.

## Routes

```
GET  /podcast/index.cgi/index   SPA shell (khtml)
GET  /podcast/index.cgi/list    card list partial (kxml)
GET  /podcast/index.cgi/add     blank form partial (kxml)
GET  /podcast/index.cgi/edit    pre-filled form (?id=N) (kxml)
POST /podcast/index.cgi/save    upsert → write → list partial
POST /podcast/index.cgi/delete  soft-delete → write → list partial (?id=N)
```

Method enforcement is table-driven via `ROUTES[PAGE__MAX]`.

## feeds.xml schema

```xml
<subscriptions>
  <podcast title="NAME"
           url="https://..."
           scope="all|latest|none"
           day="Daily|Mon|Tue|Wed|Thu|Fri|Sat|Sun"
           pull_time="00..23" />
</subscriptions>
```

Validated on write by `validate_fields` (presence, length, enum allowlist).
Validated on use by `podcast.sh` via `xmlstarlet val -s feeds.xsd`.

## File layout

```
src/main.c              FastCGI worker
nob.c                   Build driver
src/sandbox.h           Platform privilege-drop
podcast.sh              tcsh cron script
feeds.xsd               XML Schema
arena.h — tsoding/arena (vendored, ARENA_IMPLEMENTATION required)
src/xml.h               mrvladus/xml.h (vendored)
src/sv.h                tsoding/sv (vendored)
test_main.c             xUnit test suite
DIAGRAM.md              Mermaid flowcharts + call graph
cflow.txt               ./nob cflow (not tracked in git)
man/index.cgi.8          mdoc(7) manpage (section 8)
man/podcast-mgr.iso12207.7  ISO 12207 lifecycle record (section 7)
man/podcast-mgr.iso10007.7  ISO 10007 CM plan (section 7)
man/podcast-mgr.ieee829.7   IEEE 829 test documentation (section 7)
CHANGELOG.md            Keep a Changelog 1.1
sbom.json               ./nob sbom (not tracked in git)
podcast_mgr.sarif       ./nob sarif (not tracked in git)
CODEOWNERS              All paths @denzuko
.githooks/post-checkout Auto-build on clone
```

## Common tasks

**Add a subscription field**
1. Add a `const char *const NEWFIELD_OPTS[]` array if constrained.
2. Add a row to `FIELDS[FIELD_COUNT]` and increment `FIELD_COUNT`.
3. Add the corresponding entry to `PodcastAttr` enum.
4. Update `feeds.xsd` to include the new attribute.
5. Run `test_main` — the `FIELDS table integrity` suite will catch mismatches.

**Run static analysis**
```sh
cppcheck --enable=all --std=c11 \
  --suppress=missingInclude --suppress=missingIncludeSystem main.c
```
Expected: 0 findings.

**Regenerate SBOM + SARIF before release**
```sh
cdxgen -t c . -o sbom.json
# then run cppcheck + OSV scan and rebuild podcast_mgr.sarif
```

## What this project is NOT

- Not a podcast player or download manager (that is `podcast.sh` + cron).
- Not multi-user. No authentication, no sessions, no CSRF tokens.
- Not internet-facing. Local network only.
- Not a general-purpose feed reader. It manages one XML file.

## Policy as Code

Policy files live in `policy/`. Each enforces machine-checkable properties
over structured artifacts. Run before every release tag.

```sh
# Regenerate AST from main.c (requires clang)
sh scripts/gen_ast.sh

# Run all policies
opa eval -d policy/sarif.rego -i podcast_mgr.sarif \
  'data.podcast_mgr.sarif.allow'
conftest test --policy policy/ \
  --namespace podcast_mgr.sbom sbom.json \
  --namespace podcast_mgr.vex vex.cdx.json \
  --namespace podcast_mgr.ast ast.json
```

| Policy | Input | Rules |
|--------|-------|-------|
| `policy/ast.rego` | `ast.json` | khtml/kxml split, malloc isolation, renderer coverage, khttp_free placement, goto discipline |
| `policy/sarif.rego` | `podcast_mgr.sarif` | PASS status, zero findings, SBOM cross-ref, extensions |
| `policy/sbom.rego` | `sbom.json` | format, purls, vendored components, minimum count |
| `policy/vex.rego` | `vex.cdx.json` | state validity, justification, detail length, no dupes |
| `policy/release_gate.rego` | all | aggregates above — deny release if any violation |

`ast.json` is generated by `scripts/gen_ast.sh` (clang AST → filtered
function summary). Do not edit by hand — regenerate when `main.c` changes.
