#!/usr/bin/env python3
"""Fail if a test/*_test.cpp is not registered in meson.build.

Meson does not glob source files, so a test file is only ever built because
someone listed it.  That is a silent failure mode: `5f45ac2` dropped eight
entries from `test_names` while adapting to an API change, and the suite went
on reporting "28/28 ok" for four months while ~1,100 lines of tests for shipped
features were never compiled, let alone run.  Two of those files had rotted
into asserting the OPPOSITE of the current contract (retention time in minutes,
after the API had been corrected to seconds).

This check makes that impossible to repeat without noticing.  A test file that
is deliberately not built must be named in SKIP below, WITH a reason, so the
decision is explicit and reviewable rather than an omission.
"""

import re
import sys
from pathlib import Path

# Deliberately unregistered, with the reason.  Anything here is a known gap,
# not an accident.
SKIP = {
    "detail_level_test.cpp": "needs DetailLevel, removed in 5f45ac2; capability "
                             "now met by lazy peak decode",
    "unsigned_index_test.cpp": "targets removed internals (parquet_array_cast, "
                               "DataType::UInt64); behaviour covered by the "
                               "uint32/uint64 index fixtures",
    "zip_buffer_test.cpp": "needs MzPeak::open_buffer / an in-memory IO::Zip "
                           "source, which no longer exists",
}


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    meson = (root / "meson.build").read_text()
    present = {p.name for p in (root / "test").glob("*_test.cpp")}

    # Every quoted bare name in meson.build; a test is registered as 'name'
    # and built as 'name_test.cpp'.
    quoted = set(re.findall(r"'([A-Za-z0-9_]+)'", meson))
    registered = {n + "_test.cpp" for n in quoted}
    # e2e is registered by its own test() call rather than via test_names.
    registered |= {"e2e_test.cpp"}

    missing = sorted(present - registered - set(SKIP))
    stale_skips = sorted(set(SKIP) - present)

    for name in missing:
        print(f"ERROR: test/{name} exists but is not registered in meson.build "
              f"-- it is never built and never run.", file=sys.stderr)
    for name in stale_skips:
        print(f"ERROR: {name} is listed in SKIP but no longer exists; "
              f"remove the entry.", file=sys.stderr)

    if missing or stale_skips:
        print(f"\n{len(missing) + len(stale_skips)} problem(s).  Add the file to "
              f"test_names in meson.build, or to SKIP in {Path(__file__).name} "
              f"with a reason.", file=sys.stderr)
        return 1

    skipped = len(SKIP)
    print(f"ok: {len(present) - skipped} test files registered, "
          f"{skipped} deliberately skipped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
