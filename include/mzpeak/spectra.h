/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "mzpeak/ims_calibration.h"
#include "mzpeak/spectrum.h"
#include "mzpeak/spectrum_metadata.h"
#include "mzpeak/util/enumerable_proxy.h"

// Forward declarations:
namespace MzPeak::Data {
class Signals;
} // namespace MzPeak::Data

namespace MzPeak::Metadata {
class Table;
}

namespace MzPeak {

/**
 * One sample of an extracted-ion chromatogram: the summed intensity within an
 * m/z window for a single spectrum, tagged with that spectrum's retention time
 * and index.  An EIC is the ascending-time sequence of these points.
 */
struct EicPoint final {
  /// The spectrum's retention time, in SECONDS.
  double time = 0.0;

  /// Summed intensity of the points falling inside the m/z window.
  double intensity = 0.0;

  /// Index of the spectrum this sample came from.
  std::size_t spectrum_index = 0;
};

/**
 * Access all spectra in a MzPeak file.
 *
 * Iteration is streaming: spectra are fetched on demand and their peak arrays
 * decode lazily, so a run is traversed in bounded memory.  The selection
 * helpers below resolve from the cached descriptive metadata and therefore do
 * NOT decode peaks — only extract_ion_chromatogram() does, and only for the
 * spectra it actually selects.
 */
class Spectra final : public Util::EnumerableProxy<Spectrum> {
public:
  /// Default constructor: empty (zero spectra).
  Spectra() = default;

  /// Neither copyable nor movable.
  ///
  /// The base class stores a fetch callback that binds `this`.  With implicit
  /// copy/move that callback keeps pointing at the ORIGINAL object, so after
  /// `auto b = a;` every `b[i]` dispatches through `a` — and dangles once `a`
  /// dies, or silently reads a different run after `a` is reassigned.  Nothing
  /// about the copy looks wrong at the call site.
  ///
  /// Deleting them costs nothing in practice: `Spectra s = index.spectra();`
  /// initialises directly from a prvalue and is elided, which is how every
  /// caller obtains one.  It also makes the "use one Spectra per thread" rule
  /// enforceable rather than advisory, since a Spectra can no longer be copied
  /// into a thread.
  Spectra(const Spectra&) = delete;
  Spectra& operator=(const Spectra&) = delete;
  Spectra(Spectra&&) = delete;
  Spectra& operator=(Spectra&&) = delete;

  /// Constructor for profile-only or centroid-only data.
  explicit Spectra(std::unique_ptr<Data::Signals>,
                   std::unique_ptr<Metadata::Table>,
                   ImsCalibration ims = {});

  /// Constructor for mixed profile+centroid data (data = profile file, peaks =
  /// centroid file).
  explicit Spectra(std::unique_ptr<Data::Signals> data,
                   std::unique_ptr<Data::Signals> peaks,
                   std::unique_ptr<Metadata::Table>,
                   ImsCalibration ims = {});

  /**
   * Resolve a native spectrum id (e.g. "controllerType=0 controllerNumber=1
   * scan=1") to its index, or nullopt when no spectrum carries that id.
   */
  std::optional<std::size_t> index_for_id(const std::string& id) const;

  /**
   * Fetch the spectrum with the given native id.
   * @throws ParquetError when the id is unknown.
   */
  Spectrum by_id(const std::string& id) const;

  /**
   * Indices of every spectrum whose retention time lies in the INCLUSIVE range
   * [rt_low, rt_high], in ascending time order.  Spectra with no retention time
   * are skipped.
   *
   * @note Times are SECONDS, matching SpectrumMetadata::retention_time — the
   * file stores minutes, and the reader converts.  Passing minutes here is a
   * 60x error that silently selects the wrong scans.
   *
   * @note An inverted range (`rt_low > rt_high`) is NORMALISED rather than
   * rejected: the bounds are swapped, so the call returns the same window as
   * the correctly ordered one.
   */
  std::vector<std::size_t> indices_in_time_range(double rt_low,
                                                 double rt_high) const;

  /**
   * Extracted-ion chromatogram over an m/z window and an RT window (both
   * inclusive, RT in SECONDS), optionally restricted to one MS level.
   *
   * Emits one point per selected spectrum in ascending time order.  A spectrum
   * with no signal in the m/z window still emits a point with intensity 0 — an
   * EIC is a dense trace over the selected scans, so gaps must stay visible
   * rather than being dropped.
   *
   * This is the only selection helper that decodes peaks, and it decodes only
   * the spectra that survive the RT and ms-level filters.
   *
   * @note An inverted range is NORMALISED by swapping rather than rejected —
   * for m/z exactly as indices_in_time_range() does for time.
   */
  std::vector<EicPoint>
  extract_ion_chromatogram(double mz_low,
                           double mz_high,
                           double rt_low,
                           double rt_high,
                           std::optional<int> ms_level = std::nullopt) const;

  /**
   * Read several spectra by index.  Reads proceed in ascending index order so
   * the underlying file access is sequential, but results are returned in the
   * SAME order as @p indices.  An out-of-range index yields a default Spectrum.
   */
  std::vector<Spectrum>
  get_spectra_batch(const std::vector<std::size_t>& indices) const;

private:
  // Internal data access.
  std::shared_ptr<Data::Signals> data_;
  std::shared_ptr<Data::Signals> peaks_; // centroid-only file (optional)
  std::shared_ptr<Metadata::Table> meta_;

  // TOF -> m/z calibration, passed to each Spectrum (ims-compact layout only).
  ImsCalibration ims_;

  // Cached per-spectrum descriptive metadata (RT, precursors, ion mobility, …),
  // read once at construction and shared into every Spectrum.  Stays null when
  // there is no metadata table.
  std::shared_ptr<const std::map<uint64_t, SpectrumMetadata>> md_map_;

  // Native id -> index, built alongside md_map_ so by_id() needs no file read.
  // Ids are not guaranteed unique; the FIRST spectrum carrying an id wins.
  std::map<std::string, std::size_t> id_to_index_;

  // Populate md_map_ and id_to_index_ from meta_.  Called from the constructors
  // so that fetch() never publishes them lazily (that was a data race).
  void load_metadata_();

  // Function to fetch a specific spectrum.  Const because it mutates no Spectra
  // state — the metadata cache is built in the constructor — which lets the
  // selection helpers above be const without casting.
  Spectrum fetch_(uint64_t) const;

  /// EnumerableProxy's fetch is a non-const pure virtual; ours is const so the
  /// const public API (by_id, range queries) still works.  Forward.
  Spectrum fetch(uint64_t n) override { return fetch_(n); }
};

} // namespace MzPeak
