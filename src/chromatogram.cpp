/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#include <vector>

#include "mzpeak/chromatogram.h"
#include "mzpeak/schema/psi/data_type.h"
#include "mzpeak/util/encoding.h"

namespace MzPeak {

/******************************************************************************/
inline std::vector<Chromatogram::time_type>
decode_time(const Schema::ArrayIndex& index, Util::array_map_type& map)
{
  // The chromatogram time array is the PSI "time array" (MS:1000595), which
  // the ArrayType enum models as RelativeTimeOffset.  It is stored as
  // 64-bit floating point.
  Util::Encoding<Schema::PSI::DataType::Float64> enc(map, index);
  return enc.decode_array(Schema::PSI::ArrayType::RelativeTimeOffset);
}

/******************************************************************************/
inline std::vector<Chromatogram::intensity_type>
decode_intensity(const Schema::ArrayIndex& index, Util::array_map_type& map)
{
  Util::Encoding<Schema::PSI::DataType::Float32> enc(map, index);
  return enc.decode_array(Schema::PSI::ArrayType::Intensity);
}

/******************************************************************************/
Chromatogram::Chromatogram(const Schema::ArrayIndex& idx,
                           std::unique_ptr<Util::array_map_type> map)
    : array_index_(idx)
    , map_(std::move(map))
    , time_(decode_time(array_index_, *map_))
    , intensity_(decode_intensity(array_index_, *map_))
{
}

/******************************************************************************/
const std::vector<Chromatogram::time_type>& Chromatogram::time() const
{
  return time_;
}

/******************************************************************************/
const std::vector<Chromatogram::intensity_type>&
Chromatogram::intensity() const
{
  return intensity_;
}

/******************************************************************************/
const Util::array_map_type& Chromatogram::raw_encoded_arrays() const
{
  return *map_;
}

/******************************************************************************/
const Schema::ArrayIndex& Chromatogram::array_index() const
{
  return array_index_;
}

} // namespace MzPeak
