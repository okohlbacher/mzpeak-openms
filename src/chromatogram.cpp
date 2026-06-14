/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include <string>
#include <vector>

#include "mzpeak/chromatogram.h"
#include "mzpeak/data/encoding.h"
#include "mzpeak/exception.h"
#include "mzpeak/schema/psi/data_type.h"

namespace MzPeak {

/******************************************************************************/
inline std::vector<Chromatogram::time_type>
decode_time(const Schema::ArrayIndex& index, Data::array_map_type& map)
{
  // The chromatogram time array is the PSI "time array" (MS:1000595), which
  // the ArrayType enum models as RelativeTimeOffset.  It is stored as
  // 64-bit floating point.
  Data::Encoding<Schema::PSI::DataType::Float64> enc(map, index);
  return enc.decode_array(Schema::PSI::ArrayType::RelativeTimeOffset);
}

/******************************************************************************/
inline std::vector<Chromatogram::intensity_type>
decode_intensity(const Schema::ArrayIndex& index, Data::array_map_type& map)
{
  Data::Encoding<Schema::PSI::DataType::Float32> enc(map, index);
  return enc.decode_array(Schema::PSI::ArrayType::Intensity);
}

/******************************************************************************/
Chromatogram::Chromatogram(const Schema::ArrayIndex& idx,
                           std::unique_ptr<Data::array_map_type> map)
    : array_index_(idx)
    , map_(std::move(map))
    , time_(decode_time(array_index_, *map_))
    , intensity_(decode_intensity(array_index_, *map_))
{
  // time and intensity are paired sample arrays; a length mismatch means the
  // decode dropped or duplicated points and the chromatogram is corrupt.
  // Surface it rather than returning mismatched arrays (mirrors Spectrum).
  // Either array may legitimately be empty (an absent index yields two empty
  // arrays; some layouts carry only one of the two).
  if (!time_.empty() && !intensity_.empty() && time_.size() != intensity_.size()) {
    throw ParquetError("chromatogram time and intensity length mismatch: " +
                       std::to_string(time_.size()) + " vs " +
                       std::to_string(intensity_.size()));
  }
}

/******************************************************************************/
const std::vector<Chromatogram::time_type>& Chromatogram::time() const
{
  return time_;
}

/******************************************************************************/
const std::vector<Chromatogram::intensity_type>& Chromatogram::intensity() const
{
  return intensity_;
}

/******************************************************************************/
const Data::array_map_type& Chromatogram::raw_encoded_arrays() const
{
  return *map_;
}

/******************************************************************************/
const Schema::ArrayIndex& Chromatogram::array_index() const { return array_index_; }

} // namespace MzPeak
