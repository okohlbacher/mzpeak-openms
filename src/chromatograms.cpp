/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "mzpeak/chromatogram.h"
#include "mzpeak/chromatograms.h"
#include "mzpeak/exception.h"
#include "mzpeak/query.h"
#include "mzpeak/util/enumerable_proxy.h"

namespace MzPeak {

/******************************************************************************/
Chromatograms::Chromatograms() {}

/******************************************************************************/
Chromatograms::Chromatograms(
    std::unique_ptr<Util::Parquet> data,
    std::optional<std::size_t> count,
    std::unordered_map<std::string, std::size_t> id_to_index)
    : EnumerableProxy(
          0,
          std::bind(std::mem_fn(&Chromatograms::fetch), this, std::placeholders::_1))
    , data_(std::make_shared<Data::Arrays>(std::move(data)))
    , id_to_index_(std::move(id_to_index))
{
  // RDR-28a: chunked chromatograms are now supported.  Their data table uses
  // the "chunk" top-level node, but the time axis (MS:1000595,
  // RelativeTimeOffset, Float64) and intensity axis route through the same
  // chunked decoder as chunked spectra: the time chunk is delta- or
  // numpress-linear-encoded (chunk_start + chunk_values), and the intensity
  // chunk is a plain secondary list or a numpress-SLOF transform.  See
  // Encoding<T>::decode_array / decode_chunked.  Chunked wavelength spectra
  // remain deferred (RDR-28b).

  // The chromatogram count lives in the metadata table's `chromatogram_count`
  // key; the data table does not carry it, so prefer the supplied count and
  // fall back to the data table's record count otherwise.
  resize(count.value_or(data_->record_count()));
}

/******************************************************************************/
Chromatogram Chromatograms::by_id(const std::string& id) const
{
  auto it = id_to_index_.find(id);
  if (it == id_to_index_.end()) {
    throw ParquetError("no chromatogram with id '" + id + "'");
  }
  return fetch(it->second);
}

/******************************************************************************/
Chromatogram Chromatograms::fetch(std::size_t index) const
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
