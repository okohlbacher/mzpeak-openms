/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include <functional>
#include <memory>

#include "mzpeak/chromatogram.h"
#include "mzpeak/chromatograms.h"
#include "mzpeak/exception.h"
#include "mzpeak/query.h"
#include "mzpeak/util/enumerable_proxy.h"

namespace MzPeak {

/******************************************************************************/
Chromatograms::Chromatograms() {}

/******************************************************************************/
Chromatograms::Chromatograms(std::unique_ptr<Util::Parquet> data,
                             std::optional<std::size_t> count)
    : EnumerableProxy(
          0,
          std::bind(std::mem_fn(&Chromatograms::fetch), this, std::placeholders::_1))
    , data_(std::make_shared<Data::Arrays>(std::move(data)))
{
  // Chromatograms currently only support the "point" layout.  The chunked
  // layout uses a different top-level node ("chunk") which would require a
  // separate decode path.
  if (data_->array_index().prefix() != "point") {
    throw ParquetError("chunked chromatograms not yet supported");
  }

  // The chromatogram count lives in the metadata table's `chromatogram_count`
  // key; the data table does not carry it, so prefer the supplied count and
  // fall back to the data table's record count otherwise.
  resize(count.value_or(data_->record_count()));
}

/******************************************************************************/
Chromatogram Chromatograms::fetch(std::size_t index)
{
  auto array_index(data_->array_index());
  auto fields = data_->columns_to_fields(array_index.columns());

  // RDR-4a: the index column is unsigned 64-bit.
  auto dest = data_->field("chromatogram_index");
  if (!dest.has_value()) return Chromatogram(array_index, nullptr);

  Query query = Query::Builder(*dest).eq<uint64_t>(static_cast<uint64_t>(index));
  auto map = data_->read_arrays(query, fields);

  return Chromatogram(array_index, std::move(map));
}

} // namespace MzPeak
