/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#include <vector>

#include "mzpeak/schema/psi/data_type.h"
#include "mzpeak/util/encoding.h"
#include "mzpeak/wavelength_spectrum.h"

namespace MzPeak {

/******************************************************************************/
inline std::vector<WavelengthSpectrum::wavelength_type>
decode_wavelength(const Schema::ArrayIndex& index, Util::array_map_type& map)
{
  // The wavelength axis is the PSI "wavelength array" (MS:1000617), which the
  // ArrayType enum models as ElectromagneticRadiation.  In this file it is
  // stored as 32-bit floating point (data_type MS:1000521), so it must be
  // decoded with the Float32 encoding (decoding it as Float64 would misread
  // the underlying Arrow FloatArray and yield garbage).  The public API
  // exposes wavelengths as double, so widen the decoded float32 values.
  Util::Encoding<Schema::PSI::DataType::Float32> enc(map, index);
  auto raw(enc.decode_array(Schema::PSI::ArrayType::ElectromagneticRadiation));

  std::vector<WavelengthSpectrum::wavelength_type> res;
  res.reserve(raw.size());
  for (float v : raw) res.push_back(static_cast<double>(v));
  return res;
}

/******************************************************************************/
inline std::vector<WavelengthSpectrum::intensity_type>
decode_intensity(const Schema::ArrayIndex& index, Util::array_map_type& map)
{
  Util::Encoding<Schema::PSI::DataType::Float32> enc(map, index);
  return enc.decode_array(Schema::PSI::ArrayType::Intensity);
}

/******************************************************************************/
WavelengthSpectrum::WavelengthSpectrum(const Schema::ArrayIndex& idx,
                                       std::unique_ptr<Util::array_map_type> map)
    : array_index_(idx)
    , map_(std::move(map))
    , wavelength_(decode_wavelength(array_index_, *map_))
    , intensity_(decode_intensity(array_index_, *map_))
{
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
const Util::array_map_type& WavelengthSpectrum::raw_encoded_arrays() const
{
  return *map_;
}

/******************************************************************************/
const Schema::ArrayIndex& WavelengthSpectrum::array_index() const
{
  return array_index_;
}

} // namespace MzPeak
