## Summary

What does this PR do? Reference any related issue with `Fixes #N`.

## Test results

```
# test_main output
$ cc -std=c11 -D_GNU_SOURCE -I. -o test_main test_main.c && ./test_main


# test_nob output
$ sh test_nob.sh --no-build

```

## cppcheck

```
$ cppcheck --enable=all --std=c11 \
    --suppress=missingInclude --suppress=missingIncludeSystem main.c
```

## Checklist

- [ ] Both test suites pass with zero failures
- [ ] cppcheck reports zero findings
- [ ] `CHANGELOG.md` updated under `[Unreleased]`
- [ ] If schema changed: `feeds.xsd` updated
- [ ] If new field added: `FIELDS[]`, `PodcastAttr`, `feeds.xsd`,
      and a test in `test_main.c` suite 4
