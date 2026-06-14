#!/usr/bin/env bash
#
# Full end-to-end validation driver for the mzPeak C++ reader/writer.
#
# Runs the whole validation matrix from docs/e2e-testing.md and prints a
# PASS / FAIL / SKIP summary.  Stages that need an external toolchain (the Rust
# reference at ../hupo-mzpeak) are skipped gracefully when it is absent, so the
# core always runs anywhere the C++ project builds.
#
#   T1  forward intra     C++ write  -> C++ read            (meson suite)
#   T3  reverse intra     C++ read   -> C++ write -> read   (meson suite)
#   E2E integration       whole reader API over every fixture + round trip
#   T2  forward cross     C++ write  -> Rust read           (needs cargo)
#   T5  full pipeline      mzML -> Rust convert -> C++ read  (needs cargo)
#
# Usage:  scripts/e2e.sh            # run everything available
#         FAST=1 scripts/e2e.sh     # skip the slow `e2e` meson suite
#
# Exit status is non-zero if any stage that actually ran FAILED (SKIP is ok).

set -uo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
rust="$here/../hupo-mzpeak"
build="$here/build"
tmp="${TMPDIR:-/tmp}"

export PATH="/opt/homebrew/bin:$PATH"
export CPLUS_INCLUDE_PATH="$(brew --prefix boost 2>/dev/null)/include:${CPLUS_INCLUDE_PATH:-}"

# Stage results, filled in as we go.
declare -A RESULT
order=()
record() { order+=("$1"); RESULT["$1"]="$2"; }

hr() { printf '%s\n' "------------------------------------------------------------"; }
section() { hr; printf '== %s\n' "$1"; hr; }

# --------------------------------------------------------------------------
section "Build"
if ! meson compile -C "$build" 2>&1 | tail -3; then
  echo "BUILD FAILED — cannot continue."
  exit 1
fi

# --------------------------------------------------------------------------
# T1 + T3 + unit tests (everything except the slow e2e suite).
section "T1/T3 + unit suite (meson)"
if meson test -C "$build" --no-suite e2e --print-errorlogs; then
  record "T1/T3 + unit (intra)" PASS
else
  record "T1/T3 + unit (intra)" FAIL
fi

# --------------------------------------------------------------------------
# Full integration test: whole reader API over every fixture + reverse round
# trip.  Slow (decodes real profile fixtures), so it can be skipped with FAST=1.
section "E2E integration (meson :e2e)"
if [[ "${FAST:-0}" == "1" ]]; then
  record "E2E integration" SKIP
  echo "FAST=1 set — skipping the e2e suite."
elif meson test -C "$build" --suite e2e --print-errorlogs; then
  record "E2E integration" PASS
else
  record "E2E integration" FAIL
fi

# --------------------------------------------------------------------------
# Cross-implementation stages need the Rust reference + a toolchain.
have_rust=0
if command -v cargo >/dev/null 2>&1 && [[ -d "$rust" ]]; then
  have_rust=1
fi

# T2 — C++ writes an archive, the Rust reference reads it back.
section "T2 forward cross (C++ write -> Rust read)"
if [[ "$have_rust" == "1" ]]; then
  if "$here/scripts/e2e_cross_impl.sh"; then
    record "T2 forward cross" PASS
  else
    record "T2 forward cross" FAIL
  fi
else
  record "T2 forward cross" SKIP
  echo "cargo and/or $rust not found — skipping."
fi

# T5 — mzML -> Rust convert -> C++ read.  Validates the C++ reader on a file the
# Rust writer produced fresh from real mzML (point layout, the convert default).
section "T5 full pipeline (mzML -> Rust convert -> C++ read)"
mzml="$here/test/files/small.mzML"
if [[ "$have_rust" == "1" && -f "$mzml" ]]; then
  out="$tmp/mzpeak_t5.mzpeak"
  rm -f "$out"
  echo "== building Rust convert example =="
  if ( cd "$rust" && cargo build --release --example convert >/dev/null 2>&1 ); then
    echo "== Rust converts $(basename "$mzml") -> $out =="
    if RUST_LOG=error "$rust/target/release/examples/convert" "$mzml" -o "$out" >/dev/null 2>&1 && [[ -f "$out" ]]; then
      # Inline-compile a tiny C++ reader that counts the spectra.
      drv="$tmp/_t5_read.cpp"
      cat > "$drv" <<'CPP'
#include <mzpeak/open.h>
#include <mzpeak/spectra.h>
#include <cstdio>
int main(int argc, char** argv) {
  auto idx = MzPeak::open(argv[1]);
  auto spectra = idx.spectra();
  std::size_t n = spectra.size();
  // Touch the first spectrum to prove the arrays decode.
  std::size_t pts = 0;
  if (n > 0) { auto s = spectra[0]; pts = s.mz().size(); }
  std::printf("%zu %zu\n", n, pts);
  return n > 0 && pts > 0 ? 0 : 2;
}
CPP
      if c++ -std=c++23 -I "$here/include" $(brew --prefix boost >/dev/null 2>&1 && echo "-I $(brew --prefix boost)/include") \
            "$drv" -o "$tmp/_t5_read" \
            $(pkg-config --cflags --libs arrow parquet libzip) \
            -L "$build" -lmzpeak -Wl,-rpath,"$build" 2>/dev/null; then
        if counts=$(DYLD_LIBRARY_PATH="$build" "$tmp/_t5_read" "$out"); then
          echo "C++ reader: spectra=$(echo "$counts" | cut -d' ' -f1) first-spectrum-points=$(echo "$counts" | cut -d' ' -f2)"
          record "T5 full pipeline" PASS
        else
          record "T5 full pipeline" FAIL
        fi
      else
        echo "could not build the C++ reader driver."
        record "T5 full pipeline" FAIL
      fi
    else
      echo "Rust convert did not produce $out."
      record "T5 full pipeline" FAIL
    fi
  else
    echo "Rust convert example failed to build."
    record "T5 full pipeline" FAIL
  fi
else
  record "T5 full pipeline" SKIP
  echo "cargo, $rust, and/or $mzml not found — skipping."
fi

# --------------------------------------------------------------------------
section "Summary"
fail=0
for k in "${order[@]}"; do
  printf '  %-26s %s\n' "$k" "${RESULT[$k]}"
  [[ "${RESULT[$k]}" == "FAIL" ]] && fail=1
done
hr
if [[ "$fail" == "1" ]]; then
  echo "RESULT: FAIL"
  exit 1
fi
echo "RESULT: PASS (skipped stages are not failures)"
