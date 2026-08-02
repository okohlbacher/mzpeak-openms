/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/util/metadata_model.h"

#include <arrow/array/array_binary.h>
#include <arrow/array/array_nested.h>
#include <arrow/array/array_primitive.h>
#include <arrow/table.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <memory>
#include <parquet/arrow/reader.h>
#include <sstream>
#include <unordered_map>

#include "mzpeak/exception.h"

namespace MzPeak::Util {

namespace {

/// Read the full metadata table from @p metadata.  Throws ParquetError on
/// failure.  The caller is responsible for caching: this function is NOT
/// idempotent (each call re-reads the Parquet file).
std::shared_ptr<arrow::Table> read_metadata_table(Parquet& metadata)
{
  std::shared_ptr<arrow::Table> table;
  arrow::Status status = metadata.reader().ReadTable(&table);
  if (!status.ok()) {
    throw ParquetError("read metadata table: " + status.ToString());
  }
  return table;
}

/// Read the `spectrum` struct column from an already-read table.  Returns null
/// if the column is absent or not a struct.
std::shared_ptr<arrow::ChunkedArray>
spectrum_column_from_table(const std::shared_ptr<arrow::Table>& table)
{
  return table->GetColumnByName("spectrum");
}

/// Resolve a child field by name, tolerating the newer flat column naming.
///
/// Older writers encode the CV term in the column name
/// (`MS_1000016_scan_start_time_unit_UO_0000031`); newer ones name the column
/// plainly (`scan_start_time`) and declare the term in the index's
/// `column_mapping`.  Rather than thread a second name through ~40 call sites,
/// resolve here: exact match first, then the mechanically de-prefixed and
/// de-suffixed form, then the handful of genuine renames that transformation
/// cannot reach.
std::shared_ptr<arrow::Array> resolve_field(const arrow::StructArray& s,
                                            std::string_view name)
{
  if (auto f = s.GetFieldByName(std::string(name))) return f;

  std::string plain(name);

  // Strip a leading CV prefix: "MS_1000511_" / "UO_0000031_".
  if (plain.size() > 11 && plain[2] == '_' &&
      (plain.compare(0, 3, "MS_") == 0 || plain.compare(0, 3, "UO_") == 0)) {
    std::size_t after = plain.find('_', 3);
    if (after != std::string::npos) plain.erase(0, after + 1);
  }

  // Strip a trailing unit suffix: "_unit_MS_1000040".
  if (auto u = plain.rfind("_unit_"); u != std::string::npos) plain.erase(u);

  if (plain != name) {
    if (auto f = s.GetFieldByName(plain)) return f;
  }

  // Renames the transformation above cannot derive.
  static const std::pair<std::string_view, std::string_view> kAliases[] = {
      // Selected-ion intensity became peak_intensity.
      {"intensity", "peak_intensity"},
      // The isolation-window target lost its _mz suffix.
      {"isolation_window_target_mz", "isolation_window_target"},
      // data_processing_ref became data_processing_id.
      {"data_processing_ref", "data_processing_id"},
  };
  for (const auto& [from, to] : kAliases) {
    if (plain == from) {
      if (auto f = s.GetFieldByName(std::string(to))) return f;
    }
  }

  return nullptr;
}

/// Build a StructArray whose children are the flat top-level columns of @p
/// table, so that a "one flat table per facet" file reads through exactly the
/// same code as a nested struct column.  Chunks are combined first so every
/// child is a single contiguous array and a row index is absolute.
std::shared_ptr<arrow::StructArray>
struct_from_table(const std::shared_ptr<arrow::Table>& table)
{
  if (!table || table->num_columns() == 0) return nullptr;

  auto combined = table->CombineChunks();
  if (!combined.ok()) return nullptr;
  auto flat = *combined;

  std::vector<std::shared_ptr<arrow::Array>> children;
  std::vector<std::string> names;
  children.reserve(static_cast<std::size_t>(flat->num_columns()));
  names.reserve(static_cast<std::size_t>(flat->num_columns()));

  for (int i = 0; i < flat->num_columns(); ++i) {
    auto col = flat->column(i);
    // CombineChunks yields one chunk; a zero-row table yields none.
    children.push_back(col->num_chunks() == 1
                           ? col->chunk(0)
                           : arrow::MakeArrayOfNull(col->type(), 0).ValueOrDie());
    names.push_back(flat->schema()->field(i)->name());
  }

  auto st = arrow::StructArray::Make(children, names);
  if (!st.ok()) return nullptr;
  return *st;
}

/// Optional integer field of any stored width (uint8/int8/uint64/...),
/// returned as the caller's integer type T.
template <typename T>
std::optional<T>
opt_int(const std::shared_ptr<arrow::StructArray>& s, const char* name, int64_t row)
{
  auto field(resolve_field(*s, name));
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
  auto field(resolve_field(*s, name));
  if (!field || field->IsNull(row)) return std::nullopt;
  if (field->type_id() != arrow::Type::DOUBLE) return std::nullopt;
  return std::static_pointer_cast<arrow::DoubleArray>(field)->Value(row);
}

/// Optional float32 field.
std::optional<float> opt_float(const std::shared_ptr<arrow::StructArray>& s,
                               const char* name,
                               int64_t row)
{
  auto field(resolve_field(*s, name));
  if (!field || field->IsNull(row)) return std::nullopt;
  if (field->type_id() != arrow::Type::FLOAT) return std::nullopt;
  return std::static_pointer_cast<arrow::FloatArray>(field)->Value(row);
}

/// Optional string field (large_string or string); nullopt if absent/null.
/// Distinct from get_string (which flattens null to empty) because ion-mobility
/// type must preserve the null-vs-empty distinction.
std::optional<std::string> opt_string(const std::shared_ptr<arrow::StructArray>& s,
                                      const char* name,
                                      int64_t row)
{
  auto field(resolve_field(*s, name));
  if (!field || field->IsNull(row)) return std::nullopt;
  if (auto large = std::dynamic_pointer_cast<arrow::LargeStringArray>(field)) {
    return large->GetString(row);
  }
  if (auto str = std::dynamic_pointer_cast<arrow::StringArray>(field)) {
    return str->GetString(row);
  }
  return std::nullopt;
}

/// String field (large_string or string); empty string if absent/null.
std::string get_string(const std::shared_ptr<arrow::StructArray>& s,
                       const char* name,
                       int64_t row)
{
  auto field(resolve_field(*s, name));
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
          resolve_field(items, "accession"))) {
    if (!f->IsNull(k)) p.accession = f->GetString(k);
  }

  // name: large_string
  if (auto f = std::dynamic_pointer_cast<arrow::LargeStringArray>(
          resolve_field(items, "name"))) {
    if (!f->IsNull(k)) p.name = f->GetString(k);
  }

  // unit: string
  if (auto f = std::dynamic_pointer_cast<arrow::StringArray>(
          resolve_field(items, "unit"))) {
    if (!f->IsNull(k)) p.unit = f->GetString(k);
  }

  // value: struct<integer:int64, float:double, string:large_string, boolean:bool>
  // Exact child names are lowercase and come from the Parquet schema (Pitfall 1).
  auto val_field = resolve_field(items, "value");
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

  auto list_col = resolve_field(*parent, list_field_name);
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

  auto table = read_metadata_table(metadata);
  std::shared_ptr<arrow::ChunkedArray> col(spectrum_column_from_table(table));
  if (!col) return out;

  for (const auto& chunk : col->chunks()) {
    if (chunk->type_id() != arrow::Type::STRUCT) continue;
    auto spectrum(std::static_pointer_cast<arrow::StructArray>(chunk));

    auto index_field(resolve_field(*spectrum, "index"));
    auto model_field(resolve_field(*spectrum, "mz_delta_model"));
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
  SpectraMetadataFiles files;
  files.primary = &metadata;
  return read_spectra_metadata(files);
}

/******************************************************************************/
std::map<uint64_t, SpectrumMetadata>
read_spectra_metadata(const SpectraMetadataFiles& files)
{
  std::map<uint64_t, SpectrumMetadata> out;
  if (!files.primary) return out;

  // Read the primary table ONCE; when the facets are nested struct columns they
  // all come from it.  A split-layout file additionally reads one small table
  // per facet, each joined by `source_index` VALUE (never by row position — the
  // facet files have their own row counts and orderings).
  auto table = read_metadata_table(*files.primary);
  auto scans_table = files.scans ? read_metadata_table(*files.scans) : nullptr;
  auto precursors_table =
      files.precursors ? read_metadata_table(*files.precursors) : nullptr;
  auto selected_ions_table =
      files.selected_ions ? read_metadata_table(*files.selected_ions) : nullptr;

  // -------------------------------------------------------------------
  // PASS 1: spectrum column — build `out` keyed by spectrum.index VALUE.
  // -------------------------------------------------------------------
  // Each facet is either a struct column of this one table (older writers) or a
  // flat table of its own (newer writers).  Normalise both to a list of
  // StructArrays so the passes below are identical for either layout.
  auto facets_for = [&table](const char* nested,
                             const std::shared_ptr<arrow::Table>& separate) {
    std::vector<std::shared_ptr<arrow::StructArray>> facets;
    if (auto col = table->GetColumnByName(nested)) {
      for (const auto& chunk : col->chunks()) {
        if (chunk->type_id() != arrow::Type::STRUCT) continue;
        facets.push_back(std::static_pointer_cast<arrow::StructArray>(chunk));
      }
    } else if (separate) {
      if (auto st = struct_from_table(separate)) facets.push_back(st);
    }
    return facets;
  };

  std::vector<std::shared_ptr<arrow::StructArray>> spectrum_facets;
  if (auto col = spectrum_column_from_table(table)) {
    for (const auto& chunk : col->chunks()) {
      if (chunk->type_id() != arrow::Type::STRUCT) continue;
      spectrum_facets.push_back(std::static_pointer_cast<arrow::StructArray>(chunk));
    }
  } else if (table->GetColumnByName("index")) {
    // Flat layout: the primary table IS the spectrum facet.
    if (auto st = struct_from_table(table)) spectrum_facets.push_back(st);
  } else {
    return out; // not a spectrum metadata table
  }

  for (const auto& spectrum : spectrum_facets) {
    auto index_field(resolve_field(*spectrum, "index"));
    if (!index_field || index_field->type_id() != arrow::Type::UINT64) continue;
    auto index(std::static_pointer_cast<arrow::UInt64Array>(index_field));

    for (int64_t r = 0; r < spectrum->length(); ++r) {
      // F7: outer struct null => children are unreliable, skip the row.
      if (spectrum->IsNull(r)) continue;
      if (index->IsNull(r)) continue;

      SpectrumMetadata m;
      m.index = index->Value(r);
      m.id = get_string(spectrum, "id", r);
      m.ms_level = opt_int<int>(spectrum, "MS_1000511_ms_level", r);
      // RT: `spectrum.time` is in minutes — the spec makes this normative
      // ("The time unit MUST be minutes", UO_0000031; docs/schemas/spectra.md),
      // and it holds the same value as scan.MS_1000016_scan_start_time.
      // Convert to seconds here as a fallback; PASS 4 overrides from the
      // unit-annotated scan field when present.
      // ponytail: single ×60 site per pass, no unit-conversion class.
      if (auto t = opt_double(spectrum, "time", r)) m.retention_time = *t * 60.0;
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

      // -------------------------------------------------------------------
      // RDR-9b: auxiliary_arrays (large_list<struct>) — PART A (structural,
      // VALIDATED) + PART B (raw-byte VALUE decode, FIXTURE-GATED FOLLOW-UP).
      //
      // PART A: schema parse + empty-list handling + count-consistency assert.
      // Every bundled fixture has number_of_auxiliary_arrays==0 and an empty
      // auxiliary_arrays list, so the item body never executes — but the field
      // access + empty-list path IS exercised and validated structurally.
      // -------------------------------------------------------------------
      {
        // Read number_of_auxiliary_arrays (uint32) for the count-consistency
        // assert (T-03-02 mitigation).  Absent/null counts as 0.
        uint64_t declared_count = 0;
        if (auto cnt =
                opt_int<uint64_t>(spectrum, "number_of_auxiliary_arrays", r)) {
          declared_count = *cnt;
        }

        auto aux_field = resolve_field(*spectrum, "auxiliary_arrays");
        if (aux_field && !aux_field->IsNull(r)) {
          auto aux_list =
              std::dynamic_pointer_cast<arrow::LargeListArray>(aux_field);
          if (aux_list && !aux_list->IsNull(r)) {
            auto aux_items =
                std::dynamic_pointer_cast<arrow::StructArray>(aux_list->values());
            if (aux_items) {
              int64_t begin = aux_list->value_offset(r);
              int64_t length = aux_list->value_length(r);
              m.auxiliary_arrays.reserve(static_cast<std::size_t>(length));

              for (int64_t k = 0; k < length; ++k) {
                AuxiliaryArray aa;

                // name: lone CvParam struct — reuse extract_one_cv_param
                // (plan-01 helper; do NOT re-inline the value-union decode).
                auto name_field = aux_items->GetFieldByName("name");
                if (name_field && !name_field->IsNull(begin + k)) {
                  auto name_struct =
                      std::dynamic_pointer_cast<arrow::StructArray>(name_field);
                  if (name_struct) {
                    aa.name = extract_one_cv_param(*name_struct, begin + k);
                  }
                }

                // Scalar string fields.
                aa.data_type = get_string(aux_items, "data_type", begin + k);
                aa.compression = get_string(aux_items, "compression", begin + k);
                aa.unit = get_string(aux_items, "unit", begin + k);
                aa.data_processing_ref =
                    get_string(aux_items, "data_processing_ref", begin + k);

                // parameters: large_list<CvParam>.
                aa.parameters =
                    read_cv_params_from_list(aux_items, "parameters", begin + k);

                // ---------------------------------------------------------
                // PART B — RAW-BYTE VALUE DECODE (FIXTURE-GATED FOLLOW-UP).
                //
                // FIXTURE-GATED FOLLOW-UP — no bundled fixture carries aux
                // bytes (number_of_auxiliary_arrays==0 everywhere), so this
                // VALUE decode is UNVERIFIED against ground truth.  It is
                // byte-length-guarded and flagged via values_decoded; value-
                // level correctness awaits a fixture with populated aux data
                // (honors CONTEXT.md "do not ship unvalidated decode paths"
                // — this path is gated, not asserted as validated).
                //
                // Treat data_type as an opaque lowercase Arrow dtype string
                // (Pitfall 5) — do NOT route through any PSI::DataType enum.
                // ---------------------------------------------------------
                auto data_field = resolve_field(*aux_items, "data");
                if (data_field && !data_field->IsNull(begin + k)) {
                  auto data_list =
                      std::dynamic_pointer_cast<arrow::LargeListArray>(data_field);
                  if (data_list && !data_list->IsNull(begin + k)) {
                    auto data_values = std::dynamic_pointer_cast<arrow::UInt8Array>(
                        data_list->values());
                    if (data_values) {
                      int64_t d_begin = data_list->value_offset(begin + k);
                      int64_t d_length = data_list->value_length(begin + k);

                      if (d_length == 0) {
                        // Legitimately empty decoded array.
                        aa.values.clear();
                        aa.values_decoded = true;
                      } else {
                        // Determine element size from opaque data_type string.
                        std::size_t element_size = 0;
                        if (aa.data_type == "float32" || aa.data_type == "int32") {
                          element_size = 4;
                        } else if (aa.data_type == "float64") {
                          element_size = 8;
                        }
                        // else: unknown data_type — decode deferred.

                        if (element_size == 0) {
                          std::fprintf(
                              stderr,
                              "AuxiliaryArray: unknown data_type '%s' at spectrum "
                              "%zu; values_decoded=false\n",
                              aa.data_type.c_str(), static_cast<std::size_t>(r));
                          aa.values_decoded = false;
                        } else {
                          // BYTE-LENGTH GUARD (T-03-01 mitigation): require
                          // data.size() % element_size == 0 before any
                          // reinterpret; a truncated/misaligned buffer MUST
                          // NOT be reinterpreted.
                          auto byte_count = static_cast<std::size_t>(d_length);
                          if (byte_count % element_size != 0) {
                            // Misaligned buffer — leave values empty, undecoded.
                            aa.values_decoded = false;
                          } else {
                            std::size_t n_elems = byte_count / element_size;
                            aa.values.reserve(n_elems);
                            for (std::size_t ei = 0; ei < n_elems; ++ei) {
                              if (aa.data_type == "float32") {
                                float fv = 0.0f;
                                std::uint8_t buf[4];
                                for (std::size_t bi = 0; bi < 4; ++bi) {
                                  buf[bi] =
                                      static_cast<std::uint8_t>(data_values->Value(
                                          d_begin +
                                          static_cast<int64_t>(ei * 4 + bi)));
                                }
                                std::memcpy(&fv, buf, 4);
                                aa.values.push_back(fv);
                              } else if (aa.data_type == "int32") {
                                std::int32_t iv = 0;
                                std::uint8_t buf[4];
                                for (std::size_t bi = 0; bi < 4; ++bi) {
                                  buf[bi] =
                                      static_cast<std::uint8_t>(data_values->Value(
                                          d_begin +
                                          static_cast<int64_t>(ei * 4 + bi)));
                                }
                                std::memcpy(&iv, buf, 4);
                                aa.values.push_back(static_cast<float>(iv));
                              } else if (aa.data_type == "float64") {
                                double dv = 0.0;
                                std::uint8_t buf[8];
                                for (std::size_t bi = 0; bi < 8; ++bi) {
                                  buf[bi] =
                                      static_cast<std::uint8_t>(data_values->Value(
                                          d_begin +
                                          static_cast<int64_t>(ei * 8 + bi)));
                                }
                                std::memcpy(&dv, buf, 8);
                                aa.values.push_back(static_cast<float>(dv));
                              }
                            }
                            aa.values_decoded = true;
                          }
                        }
                      }
                    }
                  }
                }
                // End Part B.

                m.auxiliary_arrays.push_back(std::move(aa));
              }
            }
          }
        }

        // COUNT-CONSISTENCY ASSERT (T-03-02 / Codex review): the file's
        // self-declared number_of_auxiliary_arrays must equal the number of
        // items materialised from the list.  A mismatch is a corruption signal.
        if (declared_count != m.auxiliary_arrays.size()) {
          throw ParquetError(
              "auxiliary_arrays count mismatch: number_of_auxiliary_arrays=" +
              std::to_string(declared_count) + " but list has " +
              std::to_string(m.auxiliary_arrays.size()) + " items (spectrum index " +
              std::to_string(m.index) + ")");
        }
      }

      out[m.index] = std::move(m);
    }
  }

  // -------------------------------------------------------------------
  // PASS 2: precursor column — H1 join by source_index VALUE.
  // For each facet row r, si = source_index VALUE (not r), then attach to
  // out[si].  NEVER index out positionally (chunk boundaries differ per
  // column).  MS1 rows have NULL source_index and are skipped.
  // -------------------------------------------------------------------
  {
    for (const auto& prec : facets_for("precursor", precursors_table)) {
      {
        for (int64_t r = 0; r < prec->length(); ++r) {
          // F7: outer struct null => row carries no precursor data, skip.
          if (prec->IsNull(r)) continue;
          // source_index NULL => MS1 row, skip (Pitfall 2).
          auto si = opt_int<uint64_t>(prec, "source_index", r);
          if (!si) continue;

          // H1: join by source_index VALUE into the map keyed by spectrum.index.
          auto it = out.find(*si);
          if (it == out.end()) {
            // T-02-01 mitigation: log unmatched source_index, never drop silently.
            // Using stderr — the library has no logger; callers may redirect.
            std::fprintf(stderr,
                         "mzpeak: precursor source_index %llu has no matching "
                         "spectrum — skipped\n",
                         static_cast<unsigned long long>(*si));
            continue;
          }

          PrecursorInfo pi;
          pi.precursor_index = opt_int<uint64_t>(prec, "precursor_index", r);
          pi.precursor_id = get_string(prec, "precursor_id", r);

          // Isolation window: nested struct field.
          auto iw_field = resolve_field(*prec, "isolation_window");
          if (iw_field && !iw_field->IsNull(r)) {
            auto iw_struct = std::dynamic_pointer_cast<arrow::StructArray>(iw_field);
            if (iw_struct) {
              pi.isolation_window.target_mz =
                  opt_float(iw_struct, "MS_1000827_isolation_window_target_mz", r);
              pi.isolation_window.lower_offset = opt_float(
                  iw_struct, "MS_1000828_isolation_window_lower_offset", r);
              pi.isolation_window.upper_offset = opt_float(
                  iw_struct, "MS_1000829_isolation_window_upper_offset", r);
              pi.isolation_window.parameters =
                  read_cv_params_from_list(iw_struct, "parameters", r);
            }
          }

          // Activation: nested struct carrying a parameters list.
          auto act_field = resolve_field(*prec, "activation");
          if (act_field && !act_field->IsNull(r)) {
            auto act_struct =
                std::dynamic_pointer_cast<arrow::StructArray>(act_field);
            if (act_struct) {
              pi.activation_parameters =
                  read_cv_params_from_list(act_struct, "parameters", r);
            }
          }

          it->second.precursors.push_back(std::move(pi));
        }
      }
    }
  }

  // -------------------------------------------------------------------
  // PASS 3: selected_ion column — H2 attach by (source_index, precursor_index).
  // Run AFTER the precursor pass so precursors exist to attach to.
  // Attach ion to the PrecursorInfo whose precursor_index matches the ion's
  // precursor_index.  If no such precursor exists, create one to hold the ion
  // (do NOT attach to an arbitrary precursor, do NOT use precursors.back()).
  // -------------------------------------------------------------------
  {
    for (const auto& si : facets_for("selected_ion", selected_ions_table)) {
      {
        for (int64_t r = 0; r < si->length(); ++r) {
          // F7: outer struct null => row carries no selected-ion data, skip.
          if (si->IsNull(r)) continue;
          // source_index NULL => MS1 row, skip.
          auto src_idx = opt_int<uint64_t>(si, "source_index", r);
          if (!src_idx) continue;

          auto it = out.find(*src_idx);
          if (it == out.end()) {
            std::fprintf(stderr,
                         "mzpeak: selected_ion source_index %llu has no matching "
                         "spectrum — skipped\n",
                         static_cast<unsigned long long>(*src_idx));
            continue;
          }

          // Read precursor_index for H2 matching.
          auto pi_idx = opt_int<uint64_t>(si, "precursor_index", r);

          SelectedIonInfo ion;
          ion.selected_ion_mz =
              opt_double(si, "MS_1000744_selected_ion_mz_unit_MS_1000040", r);
          ion.charge_state = opt_int<int>(si, "MS_1000041_charge_state", r);
          ion.intensity = opt_float(si, "MS_1000042_intensity_unit_MS_1000131", r);
          // Ion mobility: null in all bundled fixtures, so this reads as nullopt
          // everywhere today — the decode is null-safe and value-level
          // correctness is fixture-gated on a real IM run (handoff P1).
          ion.ion_mobility_value = opt_double(si, "ion_mobility_value", r);
          ion.ion_mobility_type = opt_string(si, "ion_mobility_type", r);
          ion.parameters = read_cv_params_from_list(si, "parameters", r);

          // H2: attach by (source_index, precursor_index) — NOT precursors.back().
          auto& precs = it->second.precursors;
          PrecursorInfo* target = nullptr;
          for (auto& p : precs) {
            if (p.precursor_index == pi_idx) {
              target = &p;
              break;
            }
          }
          if (!target) {
            // No precursor with this precursor_index yet — create a holder
            // (ion seen before/without its precursor row, or ion-only entry).
            PrecursorInfo holder;
            holder.precursor_index = pi_idx;
            precs.push_back(std::move(holder));
            target = &precs.back();
          }
          target->selected_ions.push_back(std::move(ion));
        }
      }
    }
  }

  // -------------------------------------------------------------------
  // PASS 4: scan column — scan_parameters + scan_windows (accepted add-on).
  // Same H1 source_index VALUE join.  scan ion_mobility is NULL in all
  // fixtures and is DEFERRED (same as selected_ion IM above).
  // -------------------------------------------------------------------
  {
    for (const auto& scan : facets_for("scan", scans_table)) {
      {
        for (int64_t r = 0; r < scan->length(); ++r) {
          // F7: outer struct null => row carries no scan data, skip.
          if (scan->IsNull(r)) continue;
          auto src_idx = opt_int<uint64_t>(scan, "source_index", r);
          if (!src_idx) continue;

          auto it = out.find(*src_idx);
          if (it == out.end()) {
            std::fprintf(stderr,
                         "mzpeak: scan source_index %llu has no matching spectrum — "
                         "skipped\n",
                         static_cast<unsigned long long>(*src_idx));
            continue;
          }

          it->second.scan_parameters =
              read_cv_params_from_list(scan, "parameters", r);

          // RT (seconds): scan.MS_1000016_scan_start_time is the authoritative,
          // unit-annotated field (UO_0000031 = minutes — normative MUST, see
          // docs/schemas/spectra.md).  Override the PASS 1 spectrum.time
          // fallback when present.  ×60 -> seconds (handoff P0; a silent
          // minutes/seconds mismatch is a 60x error).
          if (auto sst =
                  opt_float(scan, "MS_1000016_scan_start_time_unit_UO_0000031", r)) {
            it->second.retention_time = static_cast<double>(*sst) * 60.0;
          }

          // Scan-level ion mobility (handoff P1): null in all bundled fixtures,
          // so null-safe today; value-level correctness is fixture-gated.
          it->second.ion_mobility = opt_double(scan, "ion_mobility_value", r);
          it->second.ion_mobility_type = opt_string(scan, "ion_mobility_type", r);

          // scan_windows: large_list<struct<lower_limit, upper_limit, parameters>>
          auto sw_field = resolve_field(*scan, "scan_windows");
          if (sw_field && !sw_field->IsNull(r)) {
            auto sw_list =
                std::dynamic_pointer_cast<arrow::LargeListArray>(sw_field);
            if (sw_list && !sw_list->IsNull(r)) {
              auto sw_items =
                  std::dynamic_pointer_cast<arrow::StructArray>(sw_list->values());
              if (sw_items) {
                int64_t begin = sw_list->value_offset(r);
                int64_t length = sw_list->value_length(r);
                it->second.scan_windows.reserve(static_cast<std::size_t>(length));
                for (int64_t k = 0; k < length; ++k) {
                  ScanWindow sw;
                  sw.lower_limit = opt_float(
                      sw_items, "MS_1000501_scan_window_lower_limit_unit_MS_1000040",
                      begin + k);
                  sw.upper_limit = opt_float(
                      sw_items, "MS_1000500_scan_window_upper_limit_unit_MS_1000040",
                      begin + k);
                  sw.parameters =
                      read_cv_params_from_list(sw_items, "parameters", begin + k);
                  it->second.scan_windows.push_back(std::move(sw));
                }
              }
            }
          }
        }
      }
    }
  }

  return out;
}

/******************************************************************************/
std::unordered_map<std::string, std::size_t>
read_entity_id_map(Parquet& metadata, const std::string& col_name)
{
  std::unordered_map<std::string, std::size_t> result;
  auto table = read_metadata_table(metadata);
  auto col = table->GetColumnByName(col_name);
  if (!col) return result;

  for (const auto& chunk : col->chunks()) {
    if (chunk->type_id() != arrow::Type::STRUCT) continue;
    auto arr(std::static_pointer_cast<arrow::StructArray>(chunk));

    for (int64_t r = 0; r < arr->length(); ++r) {
      if (arr->IsNull(r)) continue;
      auto idx = opt_int<uint64_t>(arr, "index", r);
      if (!idx) continue;
      std::string id = get_string(arr, "id", r);
      if (!id.empty()) {
        result.emplace(std::move(id), static_cast<std::size_t>(*idx));
      }
    }
  }

  return result;
}

} // namespace MzPeak::Util
