/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#pragma once

#include "mzpeak/spectrum.h"
#include "mzpeak/util/enumerable_proxy.h"

// Forward declarations:
namespace MzPeak::Data {
class Signals;
} // namespace MzPeak::Data

namespace MzPeak::Metadata {
class Table;
}

namespace MzPeak {

/// RDR-17: one sample of an extracted-ion chromatogram (EIC) — the summed
/// intensity within an m/z window for a single spectrum, tagged with that
/// spectrum's retention time and index.  An EIC is the ascending-time sequence
/// of these points across the RT-selected scans.
struct EicPoint final {
  double time;                ///< the spectrum's retention time.
  double intensity;           ///< summed intensity of points in the m/z window.
  std::size_t spectrum_index; ///< the source spectrum's index.
};

/**
 * Access all spectra in a MzPeak file.
 */
class Spectra final : public Util::EnumerableProxy<Spectrum> {
public:
  /// Low-level constructor from a Parquet file.
  explicit Spectra(std::unique_ptr<Data::Signals>, std::unique_ptr<Metadata::Table>);

  /// RDR-17: extracted-ion chromatogram.  For every spectrum whose
  /// retention_time lies in [rt_low, rt_high] (via indices_in_time_range) and,
  /// if @p ms_level is set, whose metadata ms_level equals it, decode the
  /// spectrum and sum the intensities of all points with m/z in
  /// [mz_low, mz_high] (both inclusive).  Emits one EicPoint per matching
  /// spectrum in ascending time order; a spectrum with no points in the window
  /// still emits a point with intensity 0 (an EIC is a dense trace over the
  /// selected scans).  Mirrors the Rust `extract_signal` (reader.rs:608) time x
  /// m/z x ms-level selection, simplified to a single-threaded summing pass
  /// (Rust streams raw filtered points and skips empty scans; we emit dense
  /// zeros instead).
  std::vector<EicPoint> extract_ion_chromatogram(
      double mz_low, double mz_high, double rt_low, double rt_high,
      std::optional<int> ms_level = std::nullopt) const;

  /// RDR-18: read several spectra by index.  A copy of @p indices is sorted
  /// ascending so the underlying reads proceed in file / row-group order, but
  /// results are returned in the SAME order as @p indices (positional
  /// correspondence).  An out-of-range index yields an empty Spectrum (matching
  /// how fetch() returns empty for an absent index).  Mirrors the Rust
  /// `get_spectra_batch` (reader.rs:1311) sort-then-read.  Future optimization
  /// (RDR-21): share row-group reads across indices via an LRU cache.
  std::vector<Spectrum>
  get_spectra_batch(const std::vector<std::size_t>& indices) const;

private:
  // Internal data access.
  std::shared_ptr<Data::Signals> data_;
  std::shared_ptr<Metadata::Table> meta_;

  // Function to fetch a specific spectrum.
  Spectrum fetch(uint64_t);
};

} // namespace MzPeak
