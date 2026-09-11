<p align="center">
  <a href="https://github.com/OpenMS/mzpeak/search?l=c%2B%2B">
    <img alt="Language" src="https://img.shields.io/badge/lang-c%2B%2B23-red?style=for-the-badge">
  </a>

  <a href="https://github.com/OpenMS/mzpeak/actions">
    <img alt="CI" src="https://img.shields.io/github/actions/workflow/status/openms/mzpeak/test.yml?branch=trunk&style=for-the-badge">
  </a>

  <a href="https://github.com/OpenMS/mzpeak/blob/trunk/LICENSE">
    <img alt="License" src="https://img.shields.io/github/license/openms/mzpeak?style=for-the-badge&color=blue">
  </a>

  <a href="https://discord.com/channels/832282841836159006/1483925549179994332">
    <img alt="Discord" src="https://img.shields.io/discord/832282841836159006?style=for-the-badge&label=support&color=orange">
  </a>
</p>

# mzPeak

A C++ library that implements the mzPeak file format for efficiently
storing data acquired by a mass spectrometer.

mzPeak is a forthcoming standard format from HUPO-PSI.  For more
information please see:

  - https://github.com/HUPO-PSI/mzPeak

  - https://pubs.acs.org/doi/10.1021/acs.jproteome.5c00435

## About this fork

This is a fork of [OpenMS/mzpeak](https://github.com/OpenMS/mzpeak) carrying
reader and writer work driven by a demanding consumer: a DIA search engine that
streams whole runs and must never materialise them. It is up to date with
upstream `trunk` and adds the following.

**Reader**

- Both metadata layouts: the nested single-table form and the newer per-facet
  split form (`spectra_metadata` + `_scans` / `_precursors` / `_selected_ions`),
  joined by `source_index` value.
- Both signal layouts: point, and chunked with delta (`MS:1003089`) and
  MS-Numpress (`MS:1002312` / `MS:1002314`) encodings.
- Bruker TDF *ims-compact*, which stores no m/z array at all — m/z is
  reconstructed from a `tof` column as `(a + b*tof)^2`.
- Per-peak ion mobility (`Spectrum::ion_mobility_array()`) and per-window
  mobility limits, which diaPASEF needs: a frame is one spectrum carrying
  several isolation windows over disjoint mobility ranges.
- Selection and batch access: `by_id`, `indices_in_time_range`,
  `extract_ion_chromatogram`, `get_spectra_batch`.
- Streaming is preserved throughout: metadata is readable without decoding
  peaks, and peak decode is lazy, `std::call_once`-guarded and shared across
  copies of a spectrum.

**Writer** — emits the split-metadata layout; the reference Rust reader reads
its output.

**Retention time is exposed in SECONDS.** The format stores minutes
(`UO:0000031`); the conversion happens once, at the boundary.

### Validation

`scripts/e2e.sh` runs the whole matrix. All of it passes: every unit suite
registered in `meson.build` (34 at the time of writing; `meson test` reports the
current number, and `test/check_registered.py` fails the build if a test file is
not registered), the e2e integration suite, and the cross-implementation checks against the Rust
reference — C++ writes / Rust reads, and Rust writes / C++ reads. Decoded
values match the reference exactly at stored points and to <=8.7e-07 Da at
null-reconstructed ones.  That last figure is a one-off measurement from the
cross-implementation comparison (`docs/roadmap.md` records 8.5e-07 Da for the
same run); no test in the suite enforces it as a bound, so treat it as evidence,
not as a guarantee.

### What is NOT validated

- **Ion mobility** — no bundled fixture carries a mobility array, so that code
  is pinned only by its CV mapping and its absent-data behaviour.
- **Bruker TDF** — exercised end to end by a hand-built fixture
  (`test/files/ims_compact.dir`), not by a real vendor archive.

Both need real files before their numbers are trusted. Remaining known issues
are listed in [docs/roadmap.md](docs/roadmap.md).

## Building

Three configurations are used here, and all three are expected green:

```sh
meson setup build                                          # debug
meson setup build-release --buildtype=release
meson setup build-tsan --buildtype=debug -Db_sanitize=thread

meson compile -C build && meson test -C build
```

`scripts/e2e.sh` additionally rebuilds the Rust reference from `../hupo-mzpeak`
and runs the cross-implementation stages; a stage whose dependencies are absent
is skipped rather than failed.

### When a build that used to work stops linking

Suspect the toolchain before the tree. Homebrew keeps several versions of a
dependency installed at once, and a configured build directory caches the
*absolute* path of the library it found. After a `brew upgrade` the headers
under `/opt/homebrew/include` point at the new version while the build directory
still links the old one. That surfaces as an undefined symbol in a third-party
namespace, naming neither the version skew nor the fix:

```
Undefined symbols for architecture arm64:
  "boost::program_options::detail::arg"
```

`meson setup --reconfigure <dir>` re-runs dependency detection and keeps the
options the directory was created with. Observed with Boost 1.90 -> 1.92, where
both versions remained in `/opt/homebrew/Cellar/boost`.

## About

The mzPeak C++ library provides both high- and low-level interfaces.

Most users will appreciate the high-level interface that abstracts
away most of the underlying format details.  This allows quick and
easy access to the stored data without sacrificing efficiency.

Those who want to read or write proprietary or encrypted tables can
use the low-level interface.  If necessary, direct access to the
Parquet reader and writer objects is provided.

**NOTE**: This is a *work in progress*, no stability is guaranteed at
this point.  Our current goal is to stabilize the API by the end of
the summer (2026).

## Features

- [X] Supports Linux, macOS, and Windows
- [X] Memory efficient, random access to stored data
- [X] Read local mzPeak files (zip archives or directories)
- [ ] Transparently read mzPeak files from the cloud (summer 2026)
- [X] Automatic detection and decoding of data tables
- [X] Point and chunked signal layouts (delta and MS-Numpress)
- [X] Nested and split metadata layouts
- [ ] Streaming writer interface (summer 2026)

## Example Spectra Reader

The following example, taken from the `examples/read_spectra.cpp`
file, reports some m/z values from the first few spectra in an mzPeak
file:

```c++
#include <mzpeak.h>
#include <print>
#include <ranges>

int main(int argc, char* argv[])
{
  if (argc < 2) {
    std::println(stderr, "Usage: {} file", std::string_view{argv[0]});
    return 1;
  }

  MzPeak::Index index = MzPeak::open(argv[1]);
  MzPeak::Spectra spectra = index.spectra();

  std::size_t to_review = std::min(5ul, spectra.size());

  std::println("There are {} spectra in this file.", spectra.size());
  std::println("Reviewing the first {} spectra.", to_review);

  std::println();
  std::println("| Index | First m/z | Last m/z |");
  std::println("|-------|-----------|----------|");

  for (auto spec_index = 0;
       const auto& spectrum : spectra | std::views::take(to_review)) {
    const auto& mz = spectrum.mz();
    std::print("| {:5d} | ", spec_index);
    if (mz.empty()) {
      std::print("{:>9} | {:>8} |", "n/a", "n/a");
    } else {
      std::print("{:9.2f} | {:8.2f} |", mz.front(), mz.back());
    }
    std::println();
    ++spec_index; // std::views::enumerate isn't available on macOS :-(
  }

  return 0;
}
```

Running the above with the `test/files/small.mzpeak` produces the
following output:

```
There are 48 spectra in this file.
Reviewing the first 5 spectra.

| Index | First m/z | Last m/z |
|-------|-----------|----------|
|     0 |    202.61 |  1999.84 |
|     1 |    200.09 |  1999.82 |
|     2 |    231.39 |  1560.72 |
|     3 |    236.05 |  1636.43 |
|     4 |    203.22 |  1412.57 |
```
