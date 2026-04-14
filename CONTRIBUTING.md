# Contributing to podcast-mgr

Thanks for taking the time. This is a small, focused project — contributions
are welcome but the scope is intentionally narrow.

## Ways to contribute

- Bug reports via [GitHub Issues](https://github.com/denzuko/podcast-mgr/issues)
- Bug fixes via pull request
- Documentation corrections
- Security issues — see [SECURITY.md](SECURITY.md) before opening a ticket

Feature requests are welcome but this project will not grow into a general
podcast client. The scope is: manage `feeds.xml` in a browser; let
`podcast.sh` do the rest.

## Before you open a PR

1. **Run policy checks before PR:**

   ```sh
   sh scripts/gen_ast.sh
   opa eval -d policy/sarif.rego -i podcast_mgr.sarif 'data.podcast_mgr.sarif.allow'
   conftest test --policy policy/ --namespace podcast_mgr.sbom sbom.json \
     --namespace podcast_mgr.vex vex.cdx.json --namespace podcast_mgr.ast ast.json
   ```


1. **Run both test suites and confirm they pass:**

   ```sh
   cc -std=c11 -D_GNU_SOURCE -I. -o test_main test_main.c && ./test_main
   sh test_nob.sh --no-build
   ```

2. **Run cppcheck — zero findings expected:**

   ```sh
   cppcheck --enable=all --std=c11 \
     --suppress=missingInclude --suppress=missingIncludeSystem main.c
   ```

3. **If you changed `feeds.xml` structure, regenerate the SBOM and SARIF:**

   ```sh
   cdxgen -t c . -o sbom.json
   ```

4. **If you added a new feed attribute:**
   - Add one row to `FIELDS[FIELD_COUNT]` in `main.c` and increment the
     `FIELD_COUNT` define.
   - Add the corresponding entry to the `PodcastAttr` enum.
   - Update `feeds.xsd` to include the new attribute with its constraints.
   - Add a test case to `test_main.c` suite 4 (FIELDS table integrity).

5. **Update `CHANGELOG.md`** under an `[Unreleased]` header.

## Code style

This project follows the conventions already in `main.c`:

- C11 (`-std=c2x` for the build, `c11` for the test suite which avoids c2x-isms)
- `const`-correct: if a pointer is not written through, it is `const`
- `NULL` comparisons: `if (NULL == p)` not `if (!p)`
- Section headers: `/* === §N NAME === */`
- Single `done:` exit label in `main()`; NULL-guarded cleanup
- No `malloc` in the hot path — use the root arena
- khtml only in `render_shell`; kxml everywhere else
- No `xml_node_serialize` — see BUGS in `index.cgi.8`

## Commit messages

```
type: short summary (72 chars max)

Longer explanation if needed. Wrap at 72 chars.
```

Types: `feat`, `fix`, `test`, `docs`, `chore`, `refactor`, `security`.

## Branch and review

- Branch off `main`, name it `fix/description` or `feat/description`
- One logical change per PR
- All paths are owned by `@denzuko` (see `CODEOWNERS`) — review is required
- Linear history enforced; rebase before requesting review

## Getting help

- GitHub Issues: https://github.com/denzuko/podcast-mgr/issues
- Email: denzuko@dapla.net

## Code of Conduct

See [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
