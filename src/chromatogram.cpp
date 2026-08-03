/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/chromatogram.h"

#include <vector>

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
    } else if (dim.array_type == Schema::PSI::ArrayType::Intensity) {
      decoder_.decimal(dim, intensity_);
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
const ChromatogramMetadata& Chromatogram::metadata() const
{
  static const ChromatogramMetadata empty{};
  if (!md_map_) return empty;
  auto it = md_map_->find(index_);
  return it == md_map_->end() ? empty : it->second;
}

} // namespace MzPeak
