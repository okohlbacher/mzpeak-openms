/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/wavelength_spectrum.h"

#include <vector>

#include "mzpeak/schema/psi/array_type.h"
#include "mzpeak/util/delta_estimator.h"

namespace MzPeak {

/******************************************************************************/
WavelengthSpectrum::WavelengthSpectrum(
    uint64_t index,
    std::shared_ptr<Data::Signals> data,
    const std::vector<Data::ArrayIndex::Dimension>& dims,
    std::unique_ptr<Util::Slice> slice,
    std::shared_ptr<const std::map<uint64_t, WavelengthSpectrumMetadata>> md_map)
    : index_(index)
    , decoder_(std::move(data), std::move(slice), Util::DeltaEstimator<double>({}))
    , wavelength_()
    , intensity_()
    , md_map_(std::move(md_map))
{
  for (auto& dim : dims) {
    if (dim.array_type == Schema::PSI::ArrayType::ElectromagneticRadiation) {
      decoder_.decimal(dim, wavelength_);
    } else if (dim.array_type == Schema::PSI::ArrayType::Intensity) {
      decoder_.decimal(dim, intensity_);
    }
  }
}

/******************************************************************************/
const std::vector<WavelengthSpectrum::wavelength_type>&
WavelengthSpectrum::wavelength() const
{
  return wavelength_;
}

/******************************************************************************/
const std::vector<WavelengthSpectrum::intensity_type>&
WavelengthSpectrum::intensity() const
{
  return intensity_;
}

/******************************************************************************/
const WavelengthSpectrumMetadata& WavelengthSpectrum::metadata() const
{
  static const WavelengthSpectrumMetadata empty{};
  if (!md_map_) return empty;
  auto it = md_map_->find(index_);
  return it == md_map_->end() ? empty : it->second;
}

} // namespace MzPeak
