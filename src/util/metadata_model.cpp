/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include <arrow/array/array_binary.h>
#include <arrow/array/array_nested.h>
#include <arrow/array/array_primitive.h>
#include <arrow/table.h>
#include <iomanip>
#include <memory>
#include <parquet/arrow/reader.h>
#include <sstream>

#include "mzpeak/exception.h"
#include "mzpeak/util/metadata_model.h"

namespace MzPeak::Util {

namespace {

/// Read the `spectrum` struct column from a metadata table.  Returns null if
/// the column is absent or not a struct.
std::shared_ptr<arrow::ChunkedArray> spectrum_column(Parquet& metadata)
{
  std::shared_ptr<arrow::Table> table;
  arrow::Status status = metadata.reader().ReadTable(&table);
  if (!status.ok()) {
    throw ParquetError("read metadata table: " + status.ToString());
  }
  return table->GetColumnByName("spectrum");
}

/// Optional integer field of any stored width (uint8/int8/uint64/...),
/// returned as the caller's integer type T.
template <typename T>
std::optional<T>
opt_int(const std::shared_ptr<arrow::StructArray>& s, const char* name, int64_t row)
{
  auto field(s->GetFieldByName(name));
  if (!field || field->IsNull(row)) return std::nullopt;
  switch (field->type_id()) {
  case arrow::Type::UINT8:
    return static_cast<T>(
        std::static_pointer_cast<arrow::UInt8Array>(field)->Value(row));
  case arrow::Type::INT8:
    return static_cast<T>(
        std::static_pointer_cast<arrow::Int8Array>(field)->Value(row));
  case arrow::Type::UINT16:
    return static_cast<T>(
        std::static_pointer_cast<arrow::UInt16Array>(field)->Value(row));
  case arrow::Type::INT16:
    return static_cast<T>(
        std::static_pointer_cast<arrow::Int16Array>(field)->Value(row));
  case arrow::Type::UINT32:
    return static_cast<T>(
        std::static_pointer_cast<arrow::UInt32Array>(field)->Value(row));
  case arrow::Type::INT32:
    return static_cast<T>(
        std::static_pointer_cast<arrow::Int32Array>(field)->Value(row));
  case arrow::Type::UINT64:
    return static_cast<T>(
        std::static_pointer_cast<arrow::UInt64Array>(field)->Value(row));
  case arrow::Type::INT64:
    return static_cast<T>(
        std::static_pointer_cast<arrow::Int64Array>(field)->Value(row));
  default:
    return std::nullopt;
  }
}

/// Optional float64 field.
std::optional<double> opt_double(const std::shared_ptr<arrow::StructArray>& s,
                                 const char* name,
                                 int64_t row)
{
  auto field(s->GetFieldByName(name));
  if (!field || field->IsNull(row)) return std::nullopt;
  if (field->type_id() != arrow::Type::DOUBLE) return std::nullopt;
  return std::static_pointer_cast<arrow::DoubleArray>(field)->Value(row);
}

/// Optional float32 field.
std::optional<float> opt_float(const std::shared_ptr<arrow::StructArray>& s,
                               const char* name,
                               int64_t row)
{
  auto field(s->GetFieldByName(name));
  if (!field || field->IsNull(row)) return std::nullopt;
  if (field->type_id() != arrow::Type::FLOAT) return std::nullopt;
  return std::static_pointer_cast<arrow::FloatArray>(field)->Value(row);
}

/// String field (large_string or string); empty string if absent/null.
std::string get_string(const std::shared_ptr<arrow::StructArray>& s,
                       const char* name,
                       int64_t row)
{
  auto field(s->GetFieldByName(name));
  if (!field || field->IsNull(row)) return {};
  if (auto large = std::dynamic_pointer_cast<arrow::LargeStringArray>(field)) {
    return large->GetString(row);
  }
  if (auto str = std::dynamic_pointer_cast<arrow::StringArray>(field)) {
    return str->GetString(row);
  }
  return {};
}

/// M2 — deterministic double-to-string conversion used for CvParam values.
/// Prints with full precision (17 significant digits), then strips trailing
/// zeros and a trailing decimal point so that 35.0 -> "35", 1.5 -> "1.5",
/// and 810.789428710938 -> "810.789428710938".
///
/// std::to_string(double) is FORBIDDEN for value-union floats — it produces
/// "35.000000" for 35.0, making string assertions fragile.
std::string format_double_canonical(double v)
{
  std::ostringstream oss;
  oss << std::defaultfloat << std::setprecision(17) << v;
  std::string s = oss.str();
  // Strip trailing zeros after a decimal point.
  if (s.find('.') != std::string::npos) {
    auto last = s.find_last_not_of('0');
    if (last != std::string::npos) {
      // If the last non-zero char is '.', strip it too.
      if (s[last] == '.') {
        s.erase(last);
      } else {
        s.erase(last + 1);
      }
    }
  }
  return s;
}

/// Extract one CvParam from list item @p k of an items StructArray.
///
/// @param items  the flat values() array of a large_list<struct<value,
///               accession, name, unit>> column.
/// @param k      absolute offset into items (i.e. after adding
///               value_offset(row)).
///
/// Reads accession (string), name (large_string), unit (string), and
/// the value union struct whose arms are named "integer" (Int64),
/// "float" (Double), "string" (LargeString), "boolean" (Bool); the first
/// non-null arm is serialised to CvParam.value as an optional<string>.
/// All-null value struct -> CvParam.value stays nullopt.
///
/// This is the SINGLE-ITEM body factored out of read_cv_params_from_list
/// so plans 01-02/01-03 can reuse it for the aux-array name struct (a lone
/// CvParam, not a list).
///
/// Security note (T-01-01): GetFieldByName returns nullptr for an unknown
/// name — every arm is null-checked before use, so a wrong child name
/// silently skips that arm rather than producing UB.
CvParam extract_one_cv_param(const arrow::StructArray& items, int64_t k)
{
  CvParam p;

  // accession: string
  if (auto f = std::dynamic_pointer_cast<arrow::StringArray>(
          items.GetFieldByName("accession"))) {
    if (!f->IsNull(k)) p.accession = f->GetString(k);
  }

  // name: large_string
  if (auto f = std::dynamic_pointer_cast<arrow::LargeStringArray>(
          items.GetFieldByName("name"))) {
    if (!f->IsNull(k)) p.name = f->GetString(k);
  }

  // unit: string
  if (auto f = std::dynamic_pointer_cast<arrow::StringArray>(
          items.GetFieldByName("unit"))) {
    if (!f->IsNull(k)) p.unit = f->GetString(k);
  }

  // value: struct<integer:int64, float:double, string:large_string, boolean:bool>
  // Exact child names are lowercase and come from the Parquet schema (Pitfall 1).
  auto val_field = items.GetFieldByName("value");
  if (val_field && !val_field->IsNull(k)) {
    auto val_struct = std::dynamic_pointer_cast<arrow::StructArray>(val_field);
    if (val_struct) {
      // "string" arm — large_string; check first (most common in fixtures).
      if (auto f = std::dynamic_pointer_cast<arrow::LargeStringArray>(
              val_struct->GetFieldByName("string"))) {
        if (!f->IsNull(k)) {
          p.value = f->GetString(k);
          return p;
        }
      }
      // "integer" arm — Int64.
      if (auto f = std::dynamic_pointer_cast<arrow::Int64Array>(
              val_struct->GetFieldByName("integer"))) {
        if (!f->IsNull(k)) {
          p.value = std::to_string(f->Value(k));
          return p;
        }
      }
      // "float" arm — Double (stored as float64 in Arrow; use canonical
      // format per M2 to avoid trailing zeros, e.g. 35.0 -> "35").
      if (auto f = std::dynamic_pointer_cast<arrow::DoubleArray>(
              val_struct->GetFieldByName("float"))) {
        if (!f->IsNull(k)) {
          p.value = format_double_canonical(f->Value(k));
          return p;
        }
      }
      // "boolean" arm — Bool.
      if (auto f = std::dynamic_pointer_cast<arrow::BooleanArray>(
              val_struct->GetFieldByName("boolean"))) {
        if (!f->IsNull(k)) {
          p.value = f->Value(k) ? std::string("true") : std::string("false");
          return p;
        }
      }
    }
  }

  return p;
}

/// Decode all CvParam items from a large_list<struct<...>> field of @p parent
/// at row @p row.  Returns an empty vector when the field is absent or the
/// row is null (matching the "empty-means-decoded" API contract on
/// SpectrumMetadata::parameters).
///
/// Mirrors the offset/length idiom already used by read_mz_delta_models.
/// Each item body is handled by extract_one_cv_param so plans 01-02/01-03
/// can reuse that helper for lone-CvParam structs (not lists).
std::vector<CvParam>
read_cv_params_from_list(const std::shared_ptr<arrow::StructArray>& parent,
                         const char* list_field_name,
                         int64_t row)
{
  std::vector<CvParam> out;

  auto list_col = parent->GetFieldByName(list_field_name);
  if (!list_col || list_col->IsNull(row)) return out;

  auto la = std::dynamic_pointer_cast<arrow::LargeListArray>(list_col);
  if (!la || la->IsNull(row)) return out;

  auto items = std::dynamic_pointer_cast<arrow::StructArray>(la->values());
  if (!items) return out;

  int64_t begin = la->value_offset(row);
  int64_t length = la->value_length(row);
  out.reserve(static_cast<std::size_t>(length));
  for (int64_t k = 0; k < length; ++k) {
    out.push_back(extract_one_cv_param(*items, begin + k));
  }
  return out;
}

} // namespace

/******************************************************************************/
std::map<uint64_t, std::vector<double>> read_mz_delta_models(Parquet& metadata)
{
  std::map<uint64_t, std::vector<double>> out;

  std::shared_ptr<arrow::ChunkedArray> col(spectrum_column(metadata));
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

      int64_t offset =
          large_list ? large_list->value_offset(r) : list->value_offset(r);
      int64_t length =
          large_list ? large_list->value_length(r) : list->value_length(r);
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

/******************************************************************************/
std::map<uint64_t, SpectrumMetadata> read_spectra_metadata(Parquet& metadata)
{
  std::map<uint64_t, SpectrumMetadata> out;

  std::shared_ptr<arrow::ChunkedArray> col(spectrum_column(metadata));
  if (!col) return out;

  for (const auto& chunk : col->chunks()) {
    if (chunk->type_id() != arrow::Type::STRUCT) continue;
    auto spectrum(std::static_pointer_cast<arrow::StructArray>(chunk));

    auto index_field(spectrum->GetFieldByName("index"));
    if (!index_field || index_field->type_id() != arrow::Type::UINT64) continue;
    auto index(std::static_pointer_cast<arrow::UInt64Array>(index_field));

    for (int64_t r = 0; r < spectrum->length(); ++r) {
      if (index->IsNull(r)) continue;

      SpectrumMetadata m;
      m.index = index->Value(r);
      m.id = get_string(spectrum, "id", r);
      m.ms_level = opt_int<int>(spectrum, "MS_1000511_ms_level", r);
      m.retention_time = opt_double(spectrum, "time", r);
      m.polarity = opt_int<int>(spectrum, "MS_1000465_scan_polarity", r);
      m.representation =
          get_string(spectrum, "MS_1000525_spectrum_representation", r);
      m.number_of_data_points =
          opt_int<uint64_t>(spectrum, "MS_1003060_number_of_data_points", r);
      m.number_of_peaks =
          opt_int<uint64_t>(spectrum, "MS_1003059_number_of_peaks", r);
      m.base_peak_mz =
          opt_double(spectrum, "MS_1000504_base_peak_mz_unit_MS_1000040", r);
      m.base_peak_intensity =
          opt_float(spectrum, "MS_1000505_base_peak_intensity_unit_MS_1000131", r);
      m.total_ion_current =
          opt_float(spectrum, "MS_1000285_total_ion_current_unit_MS_1000131", r);

      // RDR-10b scalar fields.
      m.spectrum_type = get_string(spectrum, "MS_1000559_spectrum_type", r);
      m.lowest_observed_mz =
          opt_double(spectrum, "MS_1000528_lowest_observed_mz_unit_MS_1000040", r);
      m.highest_observed_mz =
          opt_double(spectrum, "MS_1000527_highest_observed_mz_unit_MS_1000040", r);
      m.data_processing_ref = get_string(spectrum, "data_processing_ref", r);

      // RDR-10b CvParam list.
      m.parameters = read_cv_params_from_list(spectrum, "parameters", r);

      out[m.index] = std::move(m);
    }
  }

  return out;
}

} // namespace MzPeak::Util
