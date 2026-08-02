/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#include "mzpeak/spectra.h"

#include <memory>

#include "mzpeak/data/signals.h"
#include "mzpeak/metadata/table.h"
#include "mzpeak/schema/group.h"
#include "mzpeak/spectrum.h"
#include "mzpeak/util/decoders.h"
#include "mzpeak/util/enumerable_proxy.h"
#include "mzpeak/util/projection.h"

namespace MzPeak {

/******************************************************************************/
Spectra::Spectra(std::unique_ptr<Data::Signals> data,
                 std::unique_ptr<Metadata::Table> meta)
    : EnumerableProxy(
          0, std::bind(std::mem_fn(&Spectra::fetch), this, std::placeholders::_1))
    , data_(std::move(data))
    , peaks_()
    , meta_(std::move(meta))
{
  resize(data_->record_count());
  load_metadata_();
}

/******************************************************************************/
Spectra::Spectra(std::unique_ptr<Data::Signals> data,
                 std::unique_ptr<Data::Signals> peaks,
                 std::unique_ptr<Metadata::Table> meta)
    : EnumerableProxy(
          0, std::bind(std::mem_fn(&Spectra::fetch), this, std::placeholders::_1))
    , data_(std::move(data))
    , peaks_(std::move(peaks))
    , meta_(std::move(meta))
{
  // Both files share the same spectrum_count KV; read it from data_.
  resize(data_->record_count());
  load_metadata_();
}

/******************************************************************************/
void Spectra::load_metadata_()
{
  // Built here rather than lazily on first fetch(): the descriptive metadata is
  // small (~1 MB for 32k spectra), almost every access path needs it, and a
  // lazily-published cache was a data race between concurrent fetch() calls.
  // Peak decode stays lazy — that is the expensive part.
  if (!meta_) return;
  md_map_ = std::make_shared<const std::map<uint64_t, SpectrumMetadata>>(
      meta_->read_spectrum_metadata());
}

/******************************************************************************/
Spectrum Spectra::fetch(uint64_t index)
{
  // Dispatch to the peaks file when metadata says this is a centroid spectrum.
  std::shared_ptr<Data::Signals> signals = data_;
  if (peaks_ && meta_) {
    auto grp = meta_->group("spectrum");
    if (grp) {
      Util::Projection proj;
      auto peaks_col = proj.project(grp, Schema::Group::CVType("MS", "1003059"));
      if (peaks_col.has_value()) {
        auto slice = meta_->indexed(index, grp, proj);
        if (slice) {
          std::optional<uint64_t> n_peaks;
          slice->singleton<Util::Decoders::Scalar<uint64_t, uint64_t>>(*peaks_col,
                                                                       n_peaks);
          if (n_peaks.has_value() && *n_peaks > 0) signals = peaks_;
        }
      }
    }
  }

  std::vector<Data::ArrayIndex::Dimension> dims =
      signals->array_index()->dimensions() | std::views::filter([](auto& d) {
        return d.array_type == Schema::PSI::ArrayType::Mz ||
               d.array_type == Schema::PSI::ArrayType::Intensity;
      }) |
      std::ranges::to<std::vector<Data::ArrayIndex::Dimension>>();

  return Spectrum(index, signals, std::move(dims), meta_, md_map_);
}

} // namespace MzPeak
