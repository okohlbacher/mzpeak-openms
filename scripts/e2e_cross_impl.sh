#!/usr/bin/env bash
#
# T2 cross-implementation check: write a .mzpeak with the C++ writer and read
# it back with the Rust reference reader (mzpeak_prototyping::MzPeakReader via
# the read_spectrum example).
#
# Expects:
#   - this repo built at ./build (meson)
#   - the Rust reference checked out at ../hupo-mzpeak with a Rust toolchain
#
# Currently EXPECTED TO FAIL with "Spectrum metadata entry not found" until the
# C++ writer emits spectra_metadata.parquet (Phase 1b). See docs/e2e-testing.md.

set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
rust="$here/../hupo-mzpeak"
out="${TMPDIR:-/tmp}/mzpeak_cross_impl.mzpeak"

export PATH="/opt/homebrew/bin:$PATH"
boost_inc="$(brew --prefix boost)/include"

echo "== building C++ archive writer driver =="
cat > "${TMPDIR:-/tmp}/_xi_write.cpp" <<'CPP'
#include <mzpeak/writer.h>
int main(int argc, char** argv) {
  MzPeak::write_spectra_archive(argv[1], {
    {{100.0, 200.5, 300.25}, {10.0f, 20.0f, 30.0f}},
    {{150.0, 250.75}, {40.0f, 50.0f}},
  });
  return 0;
}
CPP
c++ -std=c++23 -I "$here/include" -I "$boost_inc" "${TMPDIR:-/tmp}/_xi_write.cpp" \
    -o "${TMPDIR:-/tmp}/_xi_write" \
    $(pkg-config --cflags --libs arrow parquet libzip) \
    -L "$here/build" -lmzpeak -Wl,-rpath,"$here/build"

echo "== C++ writes $out =="
rm -f "$out"
DYLD_LIBRARY_PATH="$here/build" "${TMPDIR:-/tmp}/_xi_write" "$out"
ls -l "$out"

echo "== building Rust read_spectrum oracle =="
( cd "$rust" && cargo build --release --example read_spectrum >/dev/null 2>&1 )
rs="$rust/target/release/examples/read_spectrum"

echo "== Rust reads spectrum 0 (expect mz 100,200.5,300.25 / int 10,20,30) =="
if RUST_LOG=error "$rs" "$out" 0; then
  echo "T2 PASS: Rust reference read the C++-written archive."
else
  echo "T2 FAIL (expected until Phase 1b writes spectra_metadata.parquet)."
  exit 1
fi
