/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/chromatograms.h"

#include <functional>
#include <memory>
#include <ranges>

#include "mzpeak/chromatogram.h"
#include "mzpeak/data/signals.h"
#include "mzpeak/exception.h"
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
                             std::optional<std::size_t> count,
                             std::map<uint64_t, ChromatogramMetadata> metadata)
    : EnumerableProxy(
          0,
          std::bind(std::mem_fn(&Chromatograms::fetch), this, std::placeholders::_1))
    , data_(std::move(data))
{
  // A declared count of zero is not evidence of an empty run.  The reference
  // writer emits zero for several count keys on files that plainly have rows,
  // and taking it at face value reports no chromatograms at all -- silently,
  // with nothing to distinguish it from a run that really has none.
  const std::size_t derived = data_->record_count();
  resize(count.value_or(derived) == 0 ? derived : *count);

  md_map_ = std::make_shared<const std::map<uint64_t, ChromatogramMetadata>>(
      std::move(metadata));
  for (const auto& [index, md] : *md_map_) {
    if (md.id.empty()) continue;
    id_to_index_.emplace(md.id, static_cast<std::size_t>(index));
  }
}

/******************************************************************************/
std::optional<std::size_t> Chromatograms::index_for_id(const std::string& id) const
{
  auto it = id_to_index_.find(id);
  if (it == id_to_index_.end()) return std::nullopt;
  return it->second;
}

/******************************************************************************/
Chromatogram Chromatograms::by_id(const std::string& id) const
{
  auto index = index_for_id(id);
  if (!index) throw ParquetError("no chromatogram with id '" + id + "'");
  return const_cast<Chromatograms*>(this)->fetch(static_cast<uint64_t>(*index));
}

/******************************************************************************/
Chromatogram Chromatograms::fetch(uint64_t index)
{
  using enum Schema::PSI::ArrayType;
  std::vector<Data::ArrayIndex::Dimension> dims =
      data_->array_index()->dimensions() | std::views::filter([](auto& d) {
        return d.array_type == RelativeTimeOffset || d.array_type == Intensity;
      }) |
      std::ranges::to<std::vector<Data::ArrayIndex::Dimension>>();

  std::unique_ptr<Util::Slice> slice = data_->select(dims, data_->index().eq(index));
  return Chromatogram(index, data_, dims, std::move(slice), md_map_);
}

} // namespace MzPeak
