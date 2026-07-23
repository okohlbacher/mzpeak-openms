/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include <functional>
#include <memory>
#include <ranges>

#include "mzpeak/chromatogram.h"
#include "mzpeak/chromatograms.h"
#include "mzpeak/data/signals.h"
#include "mzpeak/schema/psi/array_type.h"
#include "mzpeak/util/enumerable_proxy.h"

namespace MzPeak {

/******************************************************************************/
Chromatograms::Chromatograms()
    : EnumerableProxy(
          0,
          std::bind(std::mem_fn(&Chromatograms::fetch), this, std::placeholders::_1))
{
}

/******************************************************************************/
Chromatograms::Chromatograms(std::unique_ptr<Data::Signals> data,
                              std::optional<std::size_t> count)
    : EnumerableProxy(
          0,
          std::bind(std::mem_fn(&Chromatograms::fetch), this, std::placeholders::_1))
    , data_(std::move(data))
{
  resize(count.value_or(data_->record_count()));
}

/******************************************************************************/
Chromatogram Chromatograms::fetch(uint64_t index)
{
  using enum Schema::PSI::ArrayType;
  std::vector<Data::ArrayIndex::Dimension> dims =
      data_->array_index()->dimensions() |
      std::views::filter([](auto& d) {
        return d.array_type == RelativeTimeOffset || d.array_type == Intensity;
      }) |
      std::ranges::to<std::vector<Data::ArrayIndex::Dimension>>();

  std::unique_ptr<Util::Slice> slice = data_->select(dims, data_->index().eq(index));
  return Chromatogram(index, data_, dims, std::move(slice));
}

} // namespace MzPeak
