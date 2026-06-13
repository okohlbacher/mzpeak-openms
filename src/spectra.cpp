/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#include <memory>

#include "mzpeak/data/signals.h"
#include "mzpeak/metadata/table.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"
#include "mzpeak/util/enumerable_proxy.h"

namespace MzPeak {

/******************************************************************************/
Spectra::Spectra(std::unique_ptr<Data::Signals> data,
                 std::unique_ptr<Metadata::Table> meta)
    : EnumerableProxy(
          0, std::bind(std::mem_fn(&Spectra::fetch), this, std::placeholders::_1))
    , data_(std::move(data))
    , meta_(std::move(meta))
{
  // Update the record count.
  resize(data_->record_count());
}

/******************************************************************************/
Spectrum Spectra::fetch(uint64_t index)
{
  // These are the dimensions we'll project by default.
  std::vector<Data::ArrayIndex::Dimension> dims =
      data_->array_index()->dimensions() | std::views::filter([](auto& d) {
        return d.array_type == Schema::PSI::ArrayType::Mz ||
               d.array_type == Schema::PSI::ArrayType::Intensity;
      }) |
      std::ranges::to<std::vector<Data::ArrayIndex::Dimension>>();

  std::unique_ptr<Util::Slice> slice = data_->select(dims, data_->index().eq(index));
  return Spectrum(index, data_, dims, std::move(slice), meta_);
}

/******************************************************************************/
// RDR-17: extracted-ion chromatogram over time x m/z (x ms-level).
std::vector<EicPoint> Spectra::extract_ion_chromatogram(
    double mz_low, double mz_high, double rt_low, double rt_high,
    std::optional<int> ms_level) const
{
  std::vector<EicPoint> result;

  // RT selection drives the scan list (ascending time order, inclusive range).
  auto indices = indices_in_time_range(rt_low, rt_high);
  result.reserve(indices.size());

  for (auto index : indices) {
    // Optional ms-level filter, decided from metadata_ without decoding arrays.
    if (ms_level.has_value()) {
      auto it = metadata_.find(static_cast<uint64_t>(index));
      if (it == metadata_.end() || it->second.ms_level != ms_level) continue;
    }

    // fetch() is non-const (reads via the cached tables); the const contract is
    // logical (no observable state change), so cast away to reuse it — same
    // pattern as by_id() above.
    Spectrum spectrum = const_cast<Spectra*>(this)->fetch(index);

    // The metadata time is authoritative for the EIC sample's time; fall back
    // to the decoded spectrum's time only if metadata lacked one.
    double time = spectrum.retention_time().value_or(0.0);

    // Sum intensities of points with m/z in [mz_low, mz_high].  m/z arrays are
    // ascending in this format, so a linear scan with an early break past the
    // window is sufficient and simple (binary-search bounds would also work).
    const auto& mz = spectrum.mz();
    const auto& intensity = spectrum.intensity();
    const std::size_t n = std::min(mz.size(), intensity.size());
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      if (mz[i] > mz_high) break;       // past the window (ascending m/z).
      if (mz[i] >= mz_low) sum += static_cast<double>(intensity[i]);
    }

    // Dense trace: emit a point even when the window is empty (sum == 0).
    result.push_back(EicPoint{time, sum, index});
  }

  return result; // ascending time order (indices already sorted by time).
}

/******************************************************************************/
// RDR-18: batch read — sort indices for in-order reads, return input order.
std::vector<Spectrum>
Spectra::get_spectra_batch(const std::vector<std::size_t>& indices) const
{
  // Read in ascending index order so reads proceed in file / row-group order;
  // a future optimization (RDR-21) would share row-group reads via an LRU
  // cache.  We key results by index to restore the caller's order afterwards.
  std::vector<std::size_t> ordered(indices);
  std::sort(ordered.begin(), ordered.end());
  ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());

  const std::size_t count = data_->record_count();
  std::map<std::size_t, Spectrum> by_index;
  for (auto index : ordered) {
    // Out-of-range indices yield an empty Spectrum (matching fetch()'s
    // empty-for-absent behavior); skip the read entirely for them.
    if (index >= count) continue;
    by_index.emplace(index, const_cast<Spectra*>(this)->fetch(index));
  }

  // Return in the SAME order as the input indices (positional correspondence);
  // an out-of-range (or otherwise unread) index maps to an empty Spectrum.
  std::vector<Spectrum> result;
  result.reserve(indices.size());
  for (auto index : indices) {
    if (auto it = by_index.find(index); it != by_index.end()) {
      result.push_back(it->second);
    } else {
      result.emplace_back(Spectrum(Schema::ArrayIndex{},
                                   std::make_unique<Util::array_map_type>()));
    }
  }
  return result;
}

} // namespace MzPeak
