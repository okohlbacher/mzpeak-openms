/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/chromatogram.h"

#include <vector>

#include "mzpeak/exception.h"
#include "mzpeak/schema/psi/array_type.h"
#include "mzpeak/util/delta_estimator.h"

namespace MzPeak {

/******************************************************************************/
Chromatogram::Chromatogram(
    uint64_t index,
    std::shared_ptr<Data::Signals> data,
    const std::vector<Data::ArrayIndex::Dimension>& dims,
    std::unique_ptr<Util::Slice> slice,
    std::shared_ptr<const std::map<uint64_t, ChromatogramMetadata>> md_map)
    : index_(index)
    , decoder_(std::move(data), std::move(slice), Util::DeltaEstimator<double>({}))
    , time_()
    , intensity_()
    , md_map_(std::move(md_map))
{
  for (auto& dim : dims) {
    if (dim.array_type == Schema::PSI::ArrayType::RelativeTimeOffset) {
      decoder_.decimal(dim, time_);

      // Convert to seconds using the unit the file declares, rather than
      // assuming minutes.  Minutes is what every observed file uses and all the
      // specification asks for, so an unrecognised unit means the file is
      // saying something this reader has not been taught -- and silently
      // treating it as minutes would scale every value by 60.
      const std::string& unit = decoder_.unit_of(dim);
      if (unit == "UO:0000031" || unit.empty()) {
        // UO:0000031 is minutes.  An absent unit is treated as minutes because
        // that is the specification's recommendation and the only thing any
        // writer has emitted; the conversion is recorded here so a file that
        // starts omitting units does not silently change meaning.
        for (auto& t : time_)
          t *= 60.0;
      } else if (unit == "UO:0000010") {
        // Already seconds.
      } else {
        throw ParquetError("chromatogram time is in unit '" + unit +
                           "', which this reader cannot convert to seconds");
      }
    } else if (dim.array_type == Schema::PSI::ArrayType::Intensity) {
      decoder_.decimal(dim, intensity_);
      intensity_unit_ = decoder_.unit_of(dim);
    }
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
const std::string& Chromatogram::intensity_unit() const { return intensity_unit_; }

/******************************************************************************/
const ChromatogramMetadata& Chromatogram::metadata() const
{
  static const ChromatogramMetadata empty{};
  if (!md_map_) return empty;
  auto it = md_map_->find(index_);
  return it == md_map_->end() ? empty : it->second;
}

} // namespace MzPeak
