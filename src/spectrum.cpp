/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#include "mzpeak/spectrum.h"

#include <memory>
#include <utility>
#include <vector>

namespace MzPeak {

/******************************************************************************/
Spectrum::Spectrum(
    uint64_t index,
    std::shared_ptr<Data::Signals> data,
    std::vector<Data::ArrayIndex::Dimension> dims,
    std::shared_ptr<Metadata::Table> metadata,
    std::shared_ptr<const std::map<uint64_t, SpectrumMetadata>> md_map)
    : index_(index)
    , md_table_(std::move(metadata))
    , md_map_(std::move(md_map))
    , signals_(std::move(data))
    , dims_(std::move(dims))
    , peaks_(std::make_shared<Peaks>())
{
}

/******************************************************************************/
void Spectrum::decode_() const
{
  // call_once gives us both halves of the contract: the decode runs exactly
  // once across every copy of this Spectrum, and concurrent callers block until
  // it has finished rather than racing on the vectors.
  std::call_once(peaks_->once, [this] {
    // The signal-file read for this spectrum happens here (not at
    // construction) so a metadata-only pass never touches the peak data.
    std::shared_ptr<Util::Slice> slice =
        signals_->select(dims_, signals_->index().eq(index_));

    // The delta model (for null-marking reconstruction) lives in the metadata
    // table and is read per-spectrum here — also deferred to first peak access.
    Metadata::Spectrum md_spec(md_table_, index_);
    decoder_type decoder(signals_, std::move(slice),
                         Util::DeltaEstimator<double>(md_spec.delta_model()));

    for (const auto& dim : dims_) {
      if (dim.array_type == Schema::PSI::ArrayType::Mz) {
        decoder.decimal(dim, peaks_->mz);
      } else if (dim.array_type == Schema::PSI::ArrayType::Intensity) {
        decoder.decimal(dim, peaks_->intensity);
      }
    }
  });
}

/******************************************************************************/
const std::vector<double>& Spectrum::mz() const
{
  decode_();
  return peaks_->mz;
}

/******************************************************************************/
const std::vector<float>& Spectrum::intensity() const
{
  decode_();
  return peaks_->intensity;
}

/******************************************************************************/
const SpectrumMetadata& Spectrum::metadata() const
{
  static const SpectrumMetadata empty{};
  if (!md_map_) return empty;
  auto it = md_map_->find(index_);
  return it == md_map_->end() ? empty : it->second;
}

/******************************************************************************/
uint8_t Spectrum::ms_level() const
{
  return static_cast<uint8_t>(metadata().ms_level.value_or(0));
}

} // namespace MzPeak
