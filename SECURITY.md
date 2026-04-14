# Security Policy

## Supported Versions

| Version | Supported |
|---------|-----------|
| 1.0     | ✅ Yes    |

Only the latest tagged release receives security fixes.

## Threat Model

`podcast-mgr` is a **single-user, local-network tool**. It is not
designed to be exposed to the internet or to serve multiple users.

Mitigations in place:

- **Sandbox**: the worker drops privileges after startup using
  `seccomp` BPF (Linux), Capsicum `cap_enter` (FreeBSD), or
  `pledge` (OpenBSD/NetBSD). Only the syscalls required for
  FastCGI I/O and file write are permitted after sandbox entry.
- **Atomic writes**: `feeds.xml` is written via rename(2) — the
  original is never partially overwritten.
- **Input validation**: all POST fields are validated against
  `FIELDS[]` for presence, length caps, and enum allowlists before
  any write occurs.
- **Symlink rejection**: `feeds.xml` is rejected if it is a symbolic
  link or not a regular file.
- **Size cap**: `feeds.xml` is rejected if it exceeds 512 KB.
- **No authentication**: by design — single-user, local access only.
  Do not expose the FastCGI socket or the HTTP server to untrusted
  networks.

## Reporting a Vulnerability

**Do not open a public GitHub Issue for security vulnerabilities.**

Report security issues by email:

**denzuko@dapla.net**

Please include:

- A description of the vulnerability and its potential impact
- Steps to reproduce or a proof-of-concept (if safe to share)
- Your assessment of severity

You will receive an acknowledgement within **72 hours** and a
resolution timeline within **7 days** for confirmed issues.

Fixes will be released as a new patch tag (e.g. `v1.1`) and the
issue will be disclosed in `CHANGELOG.md` under a `### Security`
heading once the fix is available.

If you prefer coordinated disclosure with a CVE assignment, mention
that in your initial report.

## Out of Scope

The following are **not** considered security vulnerabilities for
this project:

- Attacks requiring local filesystem write access to `feeds.xml` or
  the binary (you already have equivalent access)
- Attacks requiring control of `XDG_CONFIG_HOME` or `HOME`
  environment variables
- Denial-of-service via malformed RSS feeds (handled by `podcast.sh`,
  not by this CGI)
- Browser-side issues in the htmx or Tailwind CDN scripts

## Security Artefacts

Each release includes:

- `sbom.json` — CycloneDX 1.6 Software Bill of Materials
- `podcast_mgr.sarif` — SARIF 2.1.0 report (cppcheck static
  analysis + OSV CVE scan)

Both are regenerated before each release tag.
- `vex.cdx.json` — CycloneDX 1.6 VEX document with exploitability statements
  for all CVEs matched against build-host dependencies by NVD CPE query.
