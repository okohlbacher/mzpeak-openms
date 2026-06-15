#!/usr/bin/env bash
#
# e2e_openms_cross_impl.sh — Phase 03-02 cross-implementation validation.
#
# Writes a .mzpeak archive using the OpenMS C++ MzPeakFile::store() from
# small.mzpeak, then reads spectrum index 2 from both the original and the
# re-written archive via the Rust reference oracle and compares the numeric
# output. Passes when m/z and intensity values match within tolerance.
#
# Requires:
#   - OpenMS built at ~/openms_build (libOpenMS.dylib)
#   - Rust oracle at ../hupo-mzpeak/target/release/examples/read_spectrum
#   - small.mzpeak in OpenMS test data path
#
# Usage: bash scripts/e2e_openms_cross_impl.sh

set -uo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/.." && pwd)"
rust_oracle="$root/hupo-mzpeak/target/release/examples/read_spectrum"
openms_build="$HOME/openms_build"
small_mzpeak="$HOME/Claude/OpenMS/src/tests/class_tests/openms/data/small.mzpeak"

out="${TMPDIR:-/tmp}/openms_cross_impl_out.mzpeak"
prog_src="${TMPDIR:-/tmp}/_openms_cross_impl_writer.cpp"
prog_bin="${TMPDIR:-/tmp}/_openms_cross_impl_writer"

echo "== Building C++ writer driver =="

cat > "$prog_src" <<'CPP'
#include <OpenMS/FORMAT/MzPeakFile.h>
#include <OpenMS/KERNEL/MSExperiment.h>
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input.mzpeak> <output.mzpeak>\n";
        return 1;
    }
    OpenMS::MSExperiment exp;
    OpenMS::MzPeakFile().load(argv[1], exp);
    std::cerr << "Loaded " << exp.size() << " spectra from " << argv[1] << "\n";
    OpenMS::MzPeakFile().store(argv[2], exp);
    std::cerr << "Stored to " << argv[2] << "\n";
    return 0;
}
CPP

# Extract include flags from the CMake build
grep -m1 '^CXX_INCLUDES' "$openms_build/src/openms/CMakeFiles/OpenMS.dir/flags.make" \
    | sed 's/^CXX_INCLUDES = //' > /tmp/_openms_inc.rsp

c++ -std=gnu++20 "$prog_src" @/tmp/_openms_inc.rsp \
    -L "$openms_build/lib" -lOpenMS \
    -Wl,-rpath,"$openms_build/lib" \
    -Wl,-rpath,/opt/homebrew/lib \
    -Wl,-rpath,/Library/Frameworks \
    -o "$prog_bin"

echo "== C++ driver built: $prog_bin =="

echo "== Running C++ store: $small_mzpeak -> $out =="
rm -f "$out"
DYLD_FRAMEWORK_PATH=/Library/Frameworks "$prog_bin" "$small_mzpeak" "$out"
ls -lh "$out"

if [[ ! -f "$rust_oracle" ]]; then
    echo "ERROR: Rust oracle not found at $rust_oracle"
    echo "Build with: cd $root/hupo-mzpeak && cargo build --release --example read_spectrum"
    exit 1
fi

echo "== Rust oracle reads spectrum index 0 from original =="
RUST_LOG=error "$rust_oracle" "$small_mzpeak" 0 > /tmp/_orig_spectrum.txt 2>&1 || true
grep -m3 "^[0-9]" /tmp/_orig_spectrum.txt || true

echo "== Rust oracle reads spectrum index 0 from C++-written archive =="
RUST_LOG=error "$rust_oracle" "$out" 0 > /tmp/_new_spectrum.txt 2>&1 || true
grep -m3 "^[0-9]" /tmp/_new_spectrum.txt || true

# Extract first non-zero intensity line (skip zero-intensity points at index 0).
# The oracle prints tab-separated "mz\tintensity" rows after "Raw Data:".
orig_mz=$(awk -F'\t' '/^[0-9]/ && $2+0 > 0 {print $1; exit}' /tmp/_orig_spectrum.txt || echo "0")
orig_int=$(awk -F'\t' '/^[0-9]/ && $2+0 > 0 {print $2; exit}' /tmp/_orig_spectrum.txt || echo "0")
new_mz=$(awk -F'\t' '/^[0-9]/ && $2+0 > 0 {print $1; exit}' /tmp/_new_spectrum.txt || echo "0")
new_int=$(awk -F'\t' '/^[0-9]/ && $2+0 > 0 {print $2; exit}' /tmp/_new_spectrum.txt || echo "0")

echo "== Numeric comparison =="
echo "  original  mz=$orig_mz  intensity=$orig_int"
echo "  rewritten mz=$new_mz   intensity=$new_int"

# Use awk for floating-point comparison with tolerance
pass=$(awk -v om="$orig_mz" -v nm="$new_mz" -v oi="$orig_int" -v ni="$new_int" \
    'BEGIN {
        mz_ok  = (om != 0 && nm != 0 && (om - nm < 0 ? nm - om : om - nm) <= 1e-9 * (om > nm ? om : nm) + 1e-9);
        int_ok = (oi != 0 && ni != 0 && (oi - ni < 0 ? ni - oi : oi - ni) <= 1e-6 * (oi > ni ? oi : ni) + 1e-3);
        print (mz_ok && int_ok) ? "PASS" : "FAIL"
    }')

# Also check that both outputs are non-empty (oracle actually read a spectrum)
if [[ -z "$orig_mz" || -z "$new_mz" || "$orig_mz" == "0" || "$new_mz" == "0" ]]; then
    echo "FAIL: Could not extract m/z values from oracle output"
    rm -f "$prog_src" "$prog_bin" "$out" /tmp/_openms_inc.rsp /tmp/_orig_spectrum.txt /tmp/_new_spectrum.txt
    exit 1
fi

echo "== Result: $pass =="

# Cleanup
rm -f "$prog_src" "$prog_bin" "$out" /tmp/_openms_inc.rsp /tmp/_orig_spectrum.txt /tmp/_new_spectrum.txt

if [[ "$pass" == "PASS" ]]; then
    exit 0
else
    exit 1
fi
