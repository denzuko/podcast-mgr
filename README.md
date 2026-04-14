# podcast-mgr

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-denzuko-FFDD00?style=flat&logo=buy-me-a-coffee&logoColor=black)](https://buymeacoffee.com/denzuko)
[![Twitch](https://img.shields.io/badge/Twitch-zekodun-9146FF?style=flat&logo=twitch&logoColor=white)](https://twitch.tv/zekodun)
[![License](https://img.shields.io/badge/License-BSD%202--Clause-blue?style=flat)](LICENSE.txt)
[![Release](https://img.shields.io/github/v/release/denzuko/podcast-mgr?style=flat&color=green)](https://github.com/denzuko/podcast-mgr/releases/latest)
[![Issues](https://img.shields.io/github/issues/denzuko/podcast-mgr?style=flat)](https://github.com/denzuko/podcast-mgr/issues)
[![SBOM](https://img.shields.io/badge/SBOM-CycloneDX%201.6-blueviolet?style=flat)](sbom.json)
[![SARIF](https://img.shields.io/badge/SARIF-2.1.0%20clean-brightgreen?style=flat)](podcast_mgr.sarif)
[![C](https://img.shields.io/badge/language-C11%2FC2x-informational?style=flat&logo=c)](main.c)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20FreeBSD%20%7C%20OpenBSD%20%7C%20NetBSD-lightgrey?style=flat)](SECURITY.md)

A single-user FastCGI SPA for managing a podcast subscription list
stored as `feeds.xml`. Built on the BCHS stack (kcgi, khtml, kcgixml)
with htmx for partial page updates.

`podcast.sh` is a companion tcsh cron script that reads `feeds.xml`
and downloads episodes according to the schedule defined therein.
The two components are intentionally decoupled: this CGI manages the
subscription database; the cron job consumes it.

## Code Flow

See [DIAGRAM.md](DIAGRAM.md) for Mermaid flowcharts of the worker
lifecycle, request dispatch, `load_feeds_xml`, `write_feeds_xml`,
and the khtml/kxml renderer split.

The raw `cflow` static call graph is in [cflow.txt](cflow.txt).

Interactive repository diagram: https://gitdiagram.com/denzuko/podcast-mgr

## Components

| File | Purpose |
|------|---------|
| `main.c` | FastCGI worker — feed manager SPA |
| `nob.c` | Build script (tsoding/nob) |
| `sandbox.h` | Portable privilege-drop (seccomp / Capsicum / pledge) |
| `podcast.sh` | tcsh cron script — downloads episodes from feeds.xml |
| `feeds.xsd` | XML Schema for feeds.xml validation |
| `index.cgi.8` | mdoc(7) manpage |
| `test_main.c` | xUnit test suite (52 tests, no external deps) |
| `DIAGRAM.md` | Mermaid code flow / UML diagrams |
| `cflow.txt` | cflow static call graph |
| `sbom.json` | CycloneDX 1.6 Software Bill of Materials |
| `podcast_mgr.sarif` | SARIF 2.1.0 — cppcheck + OSV CVE scan results |

## Documentation

| File | Standard |
|------|----------|
| `podcast-mgr.iso12207.7` | ISO/IEC/IEEE 12207:2017 — Software lifecycle |
| `podcast-mgr.iso10007.7` | ISO 10007:2003 — Configuration management |
| `podcast-mgr.ieee829.7` | IEEE 829-2008 — Test documentation |

## Dependencies

- [kcgi](https://kristaps.bsd.lv/kcgi/) — FastCGI + HTML/XML generation
- [mrvladus/xml.h](https://github.com/mrvladus/xml.h) — header-only XML parser
- [tsoding/sv](https://github.com/tsoding/sv) — string view library
- [tsoding/nob.h](https://github.com/tsoding/nob) — build system
- `arena.h` — [tsoding/arena](https://github.com/tsoding/arena) region allocator

## Build

```sh
cc -o nob nob.c && ./nob
```

The post-clone git hook handles this automatically:

```sh
git clone https://github.com/denzuko/podcast-mgr
cd podcast-mgr
git config core.hooksPath .githooks
```

## Tests

```sh
# Requires xml.h and sv.h in the working directory
cc -std=c11 -D_GNU_SOURCE -I. -o test_main test_main.c && ./test_main
```

52 test cases across 6 suites: `validate_fields`, `parse_id`,
`xml_str_escape`, `FIELDS table integrity`, `sv_is_blank`,
`xml round-trip`.

## Configuration

`feeds.xml` is read from `$XDG_CONFIG_HOME/podcasts/feeds.xml`
(fallback: `$HOME/.config/podcasts/feeds.xml`).

See `index.cgi.8` for full documentation.

## Cron setup

```
0 0 * * * /path/to/podcast.sh --playlist
```

## Community

- [Contributing](CONTRIBUTING.md) — how to submit fixes and PRs
- [Code of Conduct](CODE_OF_CONDUCT.md) — Contributor Covenant 2.1
- [Security Policy](SECURITY.md) — how to report vulnerabilities
- [Support](SUPPORT.md) — how to get help
- [Bug reports](https://github.com/denzuko/podcast-mgr/issues/new?template=bug_report.md)
- [Feature requests](https://github.com/denzuko/podcast-mgr/issues/new?template=feature_request.md)
- **Email:** denzuko@dapla.net

## License

BSD 2-Clause. Copyright (C) 2026 Dwight Spencer <denzuko@dapla.net>

---

<a href="https://buymeacoffee.com/denzuko" target="_blank">
  <img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png"
       alt="Buy Me A Coffee" height="41" width="174">
</a>
