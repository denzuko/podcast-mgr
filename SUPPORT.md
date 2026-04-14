# Support

## How to get help

### GitHub Issues

For bug reports and feature requests, open an issue:

https://github.com/denzuko/podcast-mgr/issues

Please include:

- Your OS and version
- Your HTTP server (nginx, lighttpd, Apache, etc.) and version
- The output of `./index.cgi` if it exits unexpectedly
- The contents of your `feeds.xml` (redact any private URLs)
- Steps to reproduce the problem

### Email

For questions that are not bugs or feature requests — deployment
help, integration questions, general usage — email:

**denzuko@dapla.net**

Response time is best-effort; typically within a few days.

### Security Issues

Do **not** use GitHub Issues or email lists for security
vulnerabilities. See [SECURITY.md](SECURITY.md).

## What is not supported

- Configurations where `index.cgi` is exposed to the public internet
- Multi-user deployments
- Platforms other than Linux, FreeBSD, OpenBSD, and NetBSD
- HTTP servers that do not support FastCGI

## Useful references

- `index.cgi.8` — system manager manpage (`man ./index.cgi.8`)
- `podcast-mgr.iso12207.7` — architecture and requirements
- `CONTRIBUTING.md` — how to submit a fix yourself
