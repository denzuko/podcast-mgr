# podcast-mgr

A single-user FastCGI SPA for managing a podcast subscription list
stored as `feeds.xml`. Built on the BCHS stack (kcgi, khtml, kcgixml)
with htmx for partial page updates.

`podcast.sh` is a companion tcsh cron script that reads `feeds.xml`
and downloads episodes according to the schedule defined therein.
The two components are intentionally decoupled: this CGI manages the
subscription database; the cron job consumes it.

## Components

| File | Purpose |
|------|---------|
| `main.c` | FastCGI worker — feed manager SPA |
| `nob.c` | Build script (tsoding/nob) |
| `sandbox.h` | Portable privilege-drop (seccomp / Capsicum / pledge) |
| `podcast.sh` | tcsh cron script — downloads episodes from feeds.xml |
| `feeds.xsd` | XML Schema for feeds.xml validation |
| `index.cgi.8` | mdoc(7) manpage |
| `sbom.json` | CycloneDX 1.6 Software Bill of Materials |
| `podcast_mgr.sarif` | SARIF 2.1.0 — cppcheck + OSV CVE scan results |

## Dependencies

- [kcgi](https://kristaps.bsd.lv/kcgi/) — FastCGI + HTML/XML generation
- [mrvladus/xml.h](https://github.com/mrvladus/xml.h) — header-only XML parser
- [tsoding/sv](https://github.com/tsoding/sv) — string view library
- [tsoding/nob.h](https://github.com/tsoding/nob) — build system
- `arena.h` — region allocator (bring your own)

## Build

```sh
cc -o nob nob.c && ./nob
```

The post-clone git hook handles this automatically.

## Configuration

`feeds.xml` is read from `$XDG_CONFIG_HOME/podcasts/feeds.xml`
(fallback: `$HOME/.config/podcasts/feeds.xml`).

See `index.cgi.8` for full documentation.

## Cron setup

```
0 0 * * * /path/to/podcast.sh --playlist
```

## License

BSD 2-Clause. Copyright (C) 2026 Dwight Spencer <denzuko@dapla.net>
