/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include <arrow/array/array_nested.h>
#include <arrow/array/array_primitive.h>
#include <arrow/table.h>
#include <memory>
#include <parquet/arrow/reader.h>

#include "mzpeak/exception.h"
#include "mzpeak/util/metadata_model.h"

namespace MzPeak::Util {

/******************************************************************************/
std::map<uint64_t, std::vector<double>> read_mz_delta_models(Parquet& metadata)
{
  std::map<uint64_t, std::vector<double>> out;

  std::shared_ptr<arrow::Table> table;
  arrow::Status status = metadata.reader().ReadTable(&table);
  if (!status.ok()) {
    throw ParquetError("read metadata table: " + status.ToString());
  }

  std::shared_ptr<arrow::ChunkedArray> col(table->GetColumnByName("spectrum"));
  if (!col) return out;

  for (const auto& chunk : col->chunks()) {
    if (chunk->type_id() != arrow::Type::STRUCT) continue;
    auto spectrum(std::static_pointer_cast<arrow::StructArray>(chunk));

    auto index_field(spectrum->GetFieldByName("index"));
    auto model_field(spectrum->GetFieldByName("mz_delta_model"));
    if (!index_field || !model_field) continue;
    if (index_field->type_id() != arrow::Type::UINT64) continue;

    auto index(std::static_pointer_cast<arrow::UInt64Array>(index_field));

    // mz_delta_model is a (large) list of float64.
    auto large_list(std::dynamic_pointer_cast<arrow::LargeListArray>(model_field));
    auto list(std::dynamic_pointer_cast<arrow::ListArray>(model_field));
    if (!large_list && !list) continue;

    auto values(std::static_pointer_cast<arrow::DoubleArray>(
        large_list ? large_list->values() : list->values()));
    if (!values) continue;

    for (int64_t r = 0; r < spectrum->length(); ++r) {
      if (index->IsNull(r)) continue;
      bool null_model = large_list ? large_list->IsNull(r) : list->IsNull(r);
      if (null_model) continue;

      int64_t offset = large_list ? large_list->value_offset(r)
                                  : list->value_offset(r);
      int64_t length = large_list ? large_list->value_length(r)
                                  : list->value_length(r);
      if (length == 0) continue;

      std::vector<double> betas;
      betas.reserve(static_cast<std::size_t>(length));
      for (int64_t k = 0; k < length; ++k) {
        betas.push_back(values->Value(offset + k));
      }
      out[index->Value(r)] = std::move(betas);
    }
  }

  return out;
}

} // namespace MzPeak::Util
