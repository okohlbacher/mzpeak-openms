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
Spectrum::Spectrum()
    : index_(0)
    , peaks_(std::make_shared<Peaks>())
{
}

/******************************************************************************/
Spectrum::Spectrum(
    uint64_t index,
    std::shared_ptr<Data::Signals> data,
    std::vector<Data::ArrayIndex::Dimension> dims,
    std::shared_ptr<Metadata::Table> metadata,
    std::shared_ptr<const std::map<uint64_t, SpectrumMetadata>> md_map,
    ImsCalibration ims)
    : index_(index)
    , md_table_(std::move(metadata))
    , md_map_(std::move(md_map))
    , signals_(std::move(data))
    , dims_(std::move(dims))
    , ims_(ims)
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
    // A default-constructed Spectrum has no backing file; it decodes to empty
    // rather than dereferencing a null signal reader.
    if (!signals_) return;

    // The signal-file read for this spectrum happens here (not at
    // construction) so a metadata-only pass never touches the peak data.
    std::shared_ptr<Util::Slice> slice =
        signals_->select(dims_, signals_->index().eq(index_));

    // The delta model comes from the already-cached metadata map.  It used to be
    // re-queried per spectrum through Metadata::Spectrum, which needs a
    // `spectrum` struct GROUP in the Parquet schema — the flat layout has none,
    // so that query silently produced a garbage slice and a corrupt array.
    decoder_type decoder(signals_, std::move(slice),
                         Util::DeltaEstimator<double>(metadata().mz_delta_model));

    // The ims-compact layout stores no m/z array: a non-standard `tof` column
    // stands in for it and m/z is reconstructed from the index calibration.
    std::vector<double> tof;

    for (const auto& dim : dims_) {
      if (dim.array_type == Schema::PSI::ArrayType::Mz) {
        decoder.decimal(dim, peaks_->mz);
      } else if (dim.array_type == Schema::PSI::ArrayType::Intensity) {
        // Bruker TDF stores intensities as INT32 and decimal() throws on
        // integer types.  Dispatch on the declared type rather than widening
        // decimal(), which would instantiate the delta estimator for 8-bit
        // types and fail to compile.
        const Util::Type type = dim.type_or_throw();
        if (type == Util::Type::Int32 || type == Util::Type::Int64) {
          decoder.integer(dim, peaks_->intensity);
        } else {
          decoder.decimal(dim, peaks_->intensity);
        }
      } else if (dim.name.find("tof") != std::string::npos) {
        decoder.integer(dim, tof);
      } else if (Schema::PSI::is_ion_mobility(dim.array_type) ||
                 dim.name.find("mobility") != std::string::npos) {
        // The name check is a deliberate fallback, not redundancy: converters
        // emit mobility terms this library may not model yet, and such a column
        // arrives typed NonStandard.  Matching only the modelled terms would
        // silently yield an empty mobility array rather than an error.
        decoder.decimal(dim, peaks_->mobility);
      }
    }

    // Reconstruct m/z from TOF only when the file really carried no m/z array
    // AND the archive declared a calibration.  Without the calibration the
    // coefficients are meaningless, so leaving the array empty is honest;
    // converting anyway would produce confident nonsense.
    if (peaks_->mz.empty() && !tof.empty() && ims_.valid) {
      peaks_->mz.reserve(tof.size());
      for (double t : tof)
        peaks_->mz.push_back(ims_.mz(t));
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
const std::vector<double>& Spectrum::ion_mobility_array() const
{
  decode_();
  return peaks_->mobility;
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
