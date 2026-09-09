/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/util/metadata_model.h"

#include <algorithm>
#include <arrow/array/array_binary.h>
#include <arrow/array/array_nested.h>
#include <arrow/array/array_primitive.h>
#include <arrow/table.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <memory>
#include <parquet/api/reader.h>
#include <parquet/arrow/reader.h>
#include <ranges>
#include <span>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include "mzpeak/exception.h"
#include "mzpeak/util/arrow.h"

namespace MzPeak::Util {

namespace {

/// Read the full metadata table from @p metadata.  Throws ParquetError on
/// failure.  The caller is responsible for caching: this function is NOT
/// idempotent (each call re-reads the Parquet file).
/// Read a metadata table, optionally leaving some TOP-LEVEL columns undecoded.
///
/// `ReadTable`'s indices are Parquet LEAF columns, not top-level fields.  The
/// two coincide only while every field is a scalar, which is why passing field
/// positions appeared to work on a precursor table whose first three columns
/// are scalars and then silently dropped `scan_windows` -- leaf 11 of the scan
/// table lies inside `parameters`, so the index selected the wrong column and
/// the wanted one was never requested.  Leaves are therefore mapped back to the
/// field that owns them via `GetColumnRoot`, and the skip list is matched on
/// that field's name.
///
/// A SKIP LIST, not a whitelist.  Naming what to keep would silently drop any
/// column a future writer adds -- and this reader resolves several fields by CV
/// accession rather than by name, so the keep set is not even statically known.
/// Naming what to drop can only ever discard what the caller already said it
/// did not want.
///
/// The older NESTED layout puts every field inside one `spectrum` struct
/// column, where none of these names appear at the top level.  Nothing matches,
/// nothing is skipped, and the read is exactly as it was -- Lean degrades to
/// Full on those files rather than misreading them.
std::shared_ptr<arrow::Table>
read_metadata_table(Parquet& metadata, std::span<const std::string_view> skip = {})
{
  arrow::Result<std::shared_ptr<arrow::Table>> result = [&] {
    if (skip.empty()) return read_table(metadata.reader());

    const parquet::SchemaDescriptor* schema =
        metadata.reader().parquet_reader()->metadata()->schema();
    if (!schema) return read_table(metadata.reader());

    const int leaves = schema->num_columns();
    std::vector<int> keep;
    keep.reserve(static_cast<std::size_t>(leaves));
    for (int i = 0; i < leaves; ++i) {
      const parquet::schema::Node* root = schema->GetColumnRoot(i);
      if (!root) { keep.push_back(i); continue; }
      if (std::ranges::find(skip, std::string_view(root->name())) == skip.end())
        keep.push_back(i);
    }
    if (keep.size() == static_cast<std::size_t>(leaves))
      return read_table(metadata.reader());

    return read_table(metadata.reader(), &keep);
  }();

  if (!result.ok()) {
    throw ParquetError("read metadata table: " + result.status().ToString());
  }
  return std::move(result).ValueOrDie();
}

/// Braced-list convenience: `{"a", "b"}` does not deduce to a span.
std::shared_ptr<arrow::Table>
read_metadata_table(Parquet& metadata, std::initializer_list<std::string_view> skip)
{
  return read_metadata_table(metadata,
                             std::span<const std::string_view>(skip.begin(), skip.size()));
}

/// Read the `spectrum` struct column from an already-read table.  Returns null
/// if the column is absent or not a struct.
std::shared_ptr<arrow::ChunkedArray>
spectrum_column_from_table(const std::shared_ptr<arrow::Table>& table)
{
  return table->GetColumnByName("spectrum");
}

/// A facet's rows together with the index entry that describes its columns.
///
/// Carried as one value so that every accessor can consult `column_mapping`
/// without threading a second argument through seventy-odd call sites.  The
/// pointer indirections make it a drop-in for the StructArray it wraps.
struct Facet {
  std::shared_ptr<arrow::StructArray> array;

  /// The index's entry for the file this facet came from, or null when the
  /// caller has none.  Only used to resolve a column by CV accession.
  const Schema::File* file = nullptr;

  /// Columns already resolved for this facet, by the name the caller asked for.
  ///
  /// WHY: resolve_field() ran per ROW per FIELD, and a column does not change
  /// between rows -- loop-invariant work done once per spectrum instead of
  /// once per file.  Each call built a std::string from the literal (24-char
  /// names are past SSO, so a malloc and free every time), hashed it into the
  /// struct type, and copied a shared_ptr (two atomics); a name needing
  /// normalisation did that up to four times.  Measured at 3.77 us per
  /// spectrum across two archives 44x apart in size -- about 12,000 cycles to
  /// fill one struct.
  ///
  /// Heterogeneous lookup, so a hit costs one hash of the caller's
  /// string_view and no allocation.
  struct Hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept
    {
      return std::hash<std::string_view>{}(s);
    }
  };
  struct Eq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
  };
  mutable std::unordered_map<std::string, std::shared_ptr<arrow::Array>, Hash, Eq> resolved;

  const arrow::StructArray* operator->() const { return array.get(); }
  const arrow::StructArray& operator*() const { return *array; }
  explicit operator bool() const { return array != nullptr; }
};

/// Recover the CV accession a caller encoded in a field name, e.g.
/// "MS_1000016_scan_start_time_unit_UO_0000031" -> "MS:1000016".
std::optional<std::string> accession_of(std::string_view name)
{
  if (name.size() < 4) return std::nullopt;
  if (name.compare(0, 3, "MS_") != 0 && name.compare(0, 3, "UO_") != 0) {
    return std::nullopt;
  }
  const std::size_t end = name.find('_', 3);
  if (end == std::string_view::npos) return std::nullopt;

  std::string accession(name.substr(0, end));
  accession[2] = ':';
  return accession;
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
std::shared_ptr<arrow::Array> resolve_field_uncached(const Facet& facet,
                                                    std::string_view name)
{
  const arrow::StructArray& s = *facet;
  if (auto f = s.GetFieldByName(std::string(name))) return f;

  std::string plain(name);

  // Strip a leading CV prefix: "MS_1000511_" / "UO_0000031_".
  if (plain.size() > 11 && plain[2] == '_' &&
      (plain.compare(0, 3, "MS_") == 0 || plain.compare(0, 3, "UO_") == 0)) {
    std::size_t after = plain.find('_', 3);
    if (after != std::string::npos) plain.erase(0, after + 1);
  }

  // One writer emitted a single column with a literal colon in its unit suffix
  // -- "MS_1003812_lambda_max_unit_UO:0000018" -- where every other column uses
  // an underscore.  Try that spelling before stripping the suffix, so a caller
  // can name the field the ordinary way and still find it.
  if (auto u = name.rfind("_unit_"); u != std::string_view::npos) {
    std::string colon(name);
    if (auto sep = colon.find('_', u + 6); sep != std::string::npos) {
      colon[sep] = ':';
      if (auto f = s.GetFieldByName(colon)) return f;
    }
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

  // Last resort: ask the index which column carries this CV term.
  //
  // Everything above guesses at the column NAME.  A writer is free to call the
  // column anything it likes and declare the binding in `column_mapping`
  // instead -- that is what the mapping is for, and it is the only way to find
  // such a column.  The reference writer's plain names happen to fall out of
  // the transformation above, which is why this was never reached and never
  // missed.
  if (facet.file != nullptr) {
    if (auto accession = accession_of(name)) {
      if (auto path = facet.file->path_for(*accession)) {
        // A mapping path may be dotted (`isolation_window.target`); the facet
        // is already the struct that owns the leaf, so take the last component.
        std::string_view leaf(*path);
        if (auto dot = leaf.rfind('.'); dot != std::string_view::npos) {
          leaf.remove_prefix(dot + 1);
        }
        if (auto f = s.GetFieldByName(std::string(leaf))) return f;
      }
    }
  }

  return nullptr;
}

/// The column @p name of @p facet, resolved ONCE per facet.
///
/// Returns a reference into the facet's own cache, so a hit costs a hash and
/// nothing else -- no string built, no shared_ptr copied.  A miss (including a
/// name this archive genuinely does not carry) is stored too, so a missing
/// column is looked for once rather than on every row.
///
/// The result depends only on (facet.array, facet.file, name), all fixed for
/// the life of a facet, so caching it cannot change what any caller sees.
const std::shared_ptr<arrow::Array>& resolve_field(const Facet& facet,
                                                  std::string_view name)
{
  if (auto it = facet.resolved.find(name); it != facet.resolved.end()) {
    return it->second;
  }
  auto [it, _] = facet.resolved.emplace(std::string(name),
                                        resolve_field_uncached(facet, name));
  return it->second;
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

/// A list column's element array and one row's [begin, begin+length) slice,
/// resolving `list` and `large_list` identically.
///
/// The specification requires a reader to treat `list` == `large_list` (32-bit
/// vs 64-bit offsets are a physical detail).  The reference writer emits
/// `large_list`, so casting only to LargeListArray reads every plain-`list`
/// column -- parameters, scan windows, auxiliary arrays -- as empty from a
/// perfectly conformant third-party archive.
struct ListSlice {
  std::shared_ptr<arrow::Array> values;
  int64_t begin = 0;
  int64_t length = 0;
};

std::optional<ListSlice> list_slice(const std::shared_ptr<arrow::Array>& field,
                                    int64_t row)
{
  if (!field || field->IsNull(row)) return std::nullopt;
  if (auto la = std::dynamic_pointer_cast<arrow::LargeListArray>(field)) {
    return ListSlice{la->values(), la->value_offset(row), la->value_length(row)};
  }
  if (auto l = std::dynamic_pointer_cast<arrow::ListArray>(field)) {
    return ListSlice{l->values(), l->value_offset(row), l->value_length(row)};
  }
  return std::nullopt;
}

/// Optional integer field of any stored width (uint8/int8/uint64/...),
/// returned as the caller's integer type T.
template <typename T>
std::optional<T> opt_int(const Facet& s, const char* name, int64_t row)
{
  const auto& field = resolve_field(s, name);
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
/// Optional real-valued field, whatever width the writer chose.
///
/// Accepting only the exact Arrow type the reference writer happens to emit
/// makes a perfectly valid file read as null: the specification permits any
/// promoted numeric type, so a producer writing an isolation-window target as
/// float64 where the reference writes float32 loses it silently.  Integers are
/// accepted too -- a whole-numbered quantity is legitimately stored that way.
std::optional<double> opt_double(const Facet& s, const char* name, int64_t row)
{
  const auto& field = resolve_field(s, name);
  if (!field || field->IsNull(row)) return std::nullopt;

  switch (field->type_id()) {
  case arrow::Type::DOUBLE:
    return std::static_pointer_cast<arrow::DoubleArray>(field)->Value(row);
  case arrow::Type::FLOAT:
    return static_cast<double>(
        std::static_pointer_cast<arrow::FloatArray>(field)->Value(row));
  case arrow::Type::HALF_FLOAT:
    return std::nullopt; // no lossless path, and nothing emits it
  default:
    break;
  }
  // Fall back to the integer widths, which opt_int already understands.
  if (auto whole = opt_int<int64_t>(s, name, row)) {
    return static_cast<double>(*whole);
  }
  return std::nullopt;
}

/// Optional float32 field.
/// Optional real-valued field narrowed to float.  See @ref opt_double for why
/// the stored width is not assumed.
std::optional<float> opt_float(const Facet& s, const char* name, int64_t row)
{
  auto value = opt_double(s, name, row);
  if (!value) return std::nullopt;
  return static_cast<float>(*value);
}

/// Optional string field (large_string or string); nullopt if absent/null.
/// Distinct from get_string (which flattens null to empty) because ion-mobility
/// type must preserve the null-vs-empty distinction.
std::optional<std::string> opt_string(const Facet& s, const char* name, int64_t row)
{
  const auto& field = resolve_field(s, name);
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
std::string get_string(const Facet& s, const char* name, int64_t row)
{
  const auto& field = resolve_field(s, name);
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

/// An entity-index column of any unsigned/integer width, read as uint64.
///
/// The spec recommends unsigned 32- or 64-bit for index columns.  Requiring
/// exactly UINT64 silently dropped every row of a facet whose index was, say,
/// uint32 -- the whole metadata map came back empty with no error.
struct IndexColumn {
  std::shared_ptr<arrow::Array> array;

  static std::optional<IndexColumn> of(const std::shared_ptr<arrow::Array>& a)
  {
    if (!a) return std::nullopt;
    switch (a->type_id()) {
    case arrow::Type::UINT64:
    case arrow::Type::UINT32:
    case arrow::Type::INT64:
    case arrow::Type::INT32:
      return IndexColumn{a};
    default:
      return std::nullopt;
    }
  }

  int64_t length() const { return array->length(); }
  bool IsNull(int64_t r) const { return array->IsNull(r); }
  uint64_t Value(int64_t r) const
  {
    switch (array->type_id()) {
    case arrow::Type::UINT64:
      return std::static_pointer_cast<arrow::UInt64Array>(array)->Value(r);
    case arrow::Type::UINT32:
      return std::static_pointer_cast<arrow::UInt32Array>(array)->Value(r);
    case arrow::Type::INT64:
      return static_cast<uint64_t>(
          std::static_pointer_cast<arrow::Int64Array>(array)->Value(r));
    case arrow::Type::INT32:
      return static_cast<uint64_t>(
          std::static_pointer_cast<arrow::Int32Array>(array)->Value(r));
    default:
      return 0;
    }
  }
};

/// Read a `string` or `large_string` child @p name of @p items at row @p k,
/// treating the two widths identically (R3).  Empty when absent or null.
std::optional<std::string>
string_at(const arrow::StructArray& items, const char* name, int64_t k)
{
  auto field = items.GetFieldByName(name);
  if (!field || field->IsNull(k)) return std::nullopt;
  if (auto f = std::dynamic_pointer_cast<arrow::LargeStringArray>(field)) {
    return f->GetString(k);
  }
  if (auto f = std::dynamic_pointer_cast<arrow::StringArray>(field)) {
    return f->GetString(k);
  }
  return std::nullopt;
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
CvParam extract_one_cv_param(const Facet& items, int64_t k)
{
  CvParam p;

  // accession / name / unit: `string` and `large_string` are equivalent (R3).
  // A per-field single-width cast silently emptied whichever field the writer
  // happened to store in the other width -- and since CV parameters resolve by
  // accession, an empty accession makes the whole term unmatchable.
  if (auto a = string_at(*items, "accession", k)) p.accession = *a;
  if (auto n = string_at(*items, "name", k)) p.name = *n;
  if (auto u = string_at(*items, "unit", k)) p.unit = *u;

  // value: struct<integer:int64, float:double, string:large_string, boolean:bool>
  // Exact child names are lowercase and come from the Parquet schema (Pitfall 1).
  auto val_field = resolve_field(items, "value");
  if (val_field && !val_field->IsNull(k)) {
    auto val_struct = std::dynamic_pointer_cast<arrow::StructArray>(val_field);
    if (val_struct) {
      const Facet value{val_struct, items.file};
      // "string" arm — either width (R3); check first (most common in fixtures).
      if (auto str = string_at(*val_struct, "string", k)) {
        p.value = *str;
        return p;
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
std::vector<CvParam> read_cv_params_from_list(const Facet& parent,
                                              const char* list_field_name,
                                              int64_t row)
{
  std::vector<CvParam> out;

  auto slice = list_slice(resolve_field(parent, list_field_name), row);
  if (!slice) return out;

  auto items = std::dynamic_pointer_cast<arrow::StructArray>(slice->values);
  if (!items) return out;

  const int64_t begin = slice->begin;
  const int64_t length = slice->length;
  out.reserve(static_cast<std::size_t>(length));
  for (int64_t k = 0; k < length; ++k) {
    out.push_back(extract_one_cv_param(Facet{items, parent.file}, begin + k));
  }
  return out;
}

/// Normalise a facet to a list of StructArrays, whichever layout it uses.
///
/// Older writers nest each facet as a struct column of the primary table; newer
/// ones give it a flat file of its own.  Both reduce to "a list of structs with
/// these child names", so every pass below is written once.
std::vector<Facet> facets_of(const std::shared_ptr<arrow::Table>& table,
                             const char* nested,
                             const std::shared_ptr<arrow::Table>& separate,
                             const Schema::File* nested_file = nullptr,
                             const Schema::File* separate_file = nullptr)
{
  std::vector<Facet> facets;
  if (table) {
    if (auto col = table->GetColumnByName(nested)) {
      for (const auto& chunk : col->chunks()) {
        if (chunk->type_id() != arrow::Type::STRUCT) continue;
        facets.push_back(
            Facet{std::static_pointer_cast<arrow::StructArray>(chunk), nested_file});
      }
      return facets;
    }
  }
  if (separate) {
    if (auto st = struct_from_table(separate))
      facets.push_back(Facet{st, separate_file});
  }
  return facets;
}

/// Attach precursor rows to entities, joined by `source_index` VALUE.
///
/// Templated on the entity because spectra and chromatograms carry the very
/// same precursor structure and join it the very same way; the alternative was
/// a second copy of this and of @ref attach_selected_ions, which is exactly
/// where the two would drift apart.
///
/// H1: join by source_index VALUE, NEVER by row position — the facet has its
/// own row count and ordering, and chunk boundaries differ per column.
template <typename Map>
void attach_precursors(Map& out,
                       const std::vector<Facet>& facets,
                       MetadataDetail detail = MetadataDetail::Full)
{
  for (const auto& prec : facets) {
    for (int64_t r = 0; r < prec->length(); ++r) {
      // F7: outer struct null => row carries no precursor data, skip.
      if (prec->IsNull(r)) continue;
      // source_index NULL => MS1 row, skip (Pitfall 2).
      auto si = opt_int<uint64_t>(prec, "source_index", r);
      if (!si) continue;

      auto it = out.find(*si);
      if (it == out.end()) {
        // T-02-01 mitigation: log unmatched source_index, never drop silently.
        // Using stderr — the library has no logger; callers may redirect.
        std::fprintf(stderr,
                     "mzpeak: precursor source_index %llu has no matching "
                     "entity — skipped\n",
                     static_cast<unsigned long long>(*si));
        continue;
      }

      PrecursorInfo pi;
      pi.precursor_index = opt_int<uint64_t>(prec, "precursor_index", r);
      pi.precursor_id = get_string(prec, "precursor_id", r);

      // Isolation window: nested struct field.
      auto iw_field = resolve_field(prec, "isolation_window");
      if (iw_field && !iw_field->IsNull(r)) {
        auto iw_struct = std::dynamic_pointer_cast<arrow::StructArray>(iw_field);
        if (iw_struct) {
          const Facet iw{iw_struct, prec.file};
          pi.isolation_window.target_mz =
              opt_float(iw, "MS_1000827_isolation_window_target_mz", r);
          pi.isolation_window.lower_offset =
              opt_float(iw, "MS_1000828_isolation_window_lower_offset", r);
          pi.isolation_window.upper_offset =
              opt_float(iw, "MS_1000829_isolation_window_upper_offset", r);
          if (detail == MetadataDetail::Full) {
            pi.isolation_window.parameters =
                read_cv_params_from_list(iw, "parameters", r);
          }
        }
      }

      // Activation: nested struct carrying a parameters list.  Nothing but
      // the list is in there, so Lean skips resolving the struct at all.
      auto act_field = detail == MetadataDetail::Full ? resolve_field(prec, "activation")
                                                      : nullptr;
      if (act_field && !act_field->IsNull(r)) {
        auto act_struct = std::dynamic_pointer_cast<arrow::StructArray>(act_field);
        if (act_struct) {
          const Facet act{act_struct, prec.file};
          pi.activation_parameters = read_cv_params_from_list(act, "parameters", r);
        }
      }

      it->second.precursors.push_back(std::move(pi));
    }
  }
}

/// Attach selected-ion rows to their owning precursor.
///
/// H2: match on (source_index, precursor_index).  Run AFTER
/// @ref attach_precursors so there are precursors to attach to.  An ion whose
/// precursor has not been seen gets a holder of its own rather than being
/// attached to an arbitrary precursor — with several precursors per entity,
/// `precursors.back()` silently mis-assigns the transition.
template <typename Map>
void attach_selected_ions(Map& out,
                          const std::vector<Facet>& facets,
                          MetadataDetail detail = MetadataDetail::Full)
{
  for (const auto& si : facets) {
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
                     "entity — skipped\n",
                     static_cast<unsigned long long>(*src_idx));
        continue;
      }

      auto pi_idx = opt_int<uint64_t>(si, "precursor_index", r);

      SelectedIonInfo ion;
      ion.selected_ion_mz =
          opt_double(si, "MS_1000744_selected_ion_mz_unit_MS_1000040", r);
      // A stored 0 (or any non-finite value) means the writer had no selected
      // ion m/z, not that the ion sits at m/z 0.
      //
      // mzpeak-convert materialises an absent MS:1000744 as 0.0 rather than
      // null -- its SelectedIon model holds a bare f64 -- and Bruker diaTracer
      // mzML routinely omits that term, carrying only charge and peak
      // intensity.  Measured on a 3,086,644-spectrum diaPASEF archive: every
      // selected_ion_mz is a non-null 0.0.  A consumer that prefers a PRESENT
      // ion m/z over the isolation-window target (OpenMS does, and so does
      // this format's own precedence) then works from precursor m/z 0 and
      // finds nothing, silently: 0 tags where the same run as mzML gives
      // 62 million.  Reporting the value as absent puts such a file back on
      // the isolation window, which is what the mzML reader would have used.
      //
      // Deliberately NOT generalised to the other numeric fields: intensity 0
      // is a real measurement, retention time 0 is a real acquisition time,
      // and charge 0 already means "unknown" to consumers that read it.  Only
      // an m/z has no meaningful zero.
      if (ion.selected_ion_mz &&
          !(std::isfinite(*ion.selected_ion_mz) && *ion.selected_ion_mz > 0.0)) {
        ion.selected_ion_mz.reset();
      }
      ion.charge_state = opt_int<int>(si, "MS_1000041_charge_state", r);
      ion.intensity = opt_float(si, "MS_1000042_intensity_unit_MS_1000131", r);
      // Ion mobility: null in all bundled fixtures, so this reads as nullopt
      // everywhere today — the decode is null-safe and value-level correctness
      // is fixture-gated on a real IM run (handoff P1).
      ion.ion_mobility_value = opt_double(si, "ion_mobility_value", r);
      ion.ion_mobility_type = opt_string(si, "ion_mobility_type", r);
      // The mobility BAND, when the writer records it.  See SelectedIonInfo:
      // for diaPASEF the value above is only the midpoint, and the band is what
      // separates one isolation window from the next.
      ion.ion_mobility_lower_limit = opt_double(si, "ion_mobility_lower_limit", r);
      ion.ion_mobility_upper_limit = opt_double(si, "ion_mobility_upper_limit", r);
      if (detail == MetadataDetail::Full)
        ion.parameters = read_cv_params_from_list(si, "parameters", r);

      auto& precs = it->second.precursors;
      PrecursorInfo* target = nullptr;
      for (auto& p : precs) {
        if (p.precursor_index == pi_idx) {
          target = &p;
          break;
        }
      }
      if (!target) {
        // No precursor with this precursor_index yet — create a holder (ion
        // seen before/without its precursor row, or an ion-only entry).
        PrecursorInfo holder;
        holder.precursor_index = pi_idx;
        precs.push_back(std::move(holder));
        target = &precs.back();
      }
      target->selected_ions.push_back(std::move(ion));
    }
  }
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
    const Facet spectrum{std::static_pointer_cast<arrow::StructArray>(chunk)};

    auto model_field(resolve_field(spectrum, "mz_delta_model"));
    auto index_col = IndexColumn::of(resolve_field(spectrum, "index"));
    if (!index_col || !model_field) continue;
    const IndexColumn& index = *index_col;

    // mz_delta_model is a (large) list of float64.
    auto large_list(std::dynamic_pointer_cast<arrow::LargeListArray>(model_field));
    auto list(std::dynamic_pointer_cast<arrow::ListArray>(model_field));
    if (!large_list && !list) continue;

    auto values(std::static_pointer_cast<arrow::DoubleArray>(
        large_list ? large_list->values() : list->values()));
    if (!values) continue;

    for (int64_t r = 0; r < spectrum->length(); ++r) {
      if (index.IsNull(r)) continue;
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
      out[index.Value(r)] = std::move(betas);
    }
  }

  return out;
}

/******************************************************************************/
IndexMap<SpectrumMetadata> read_spectra_metadata(Parquet& metadata)
{
  SpectraMetadataFiles files;
  files.primary = &metadata;
  return read_spectra_metadata(files);
}

/******************************************************************************/
IndexMap<SpectrumMetadata>
read_spectra_metadata(const SpectraMetadataFiles& files)
{
  return read_spectra_metadata(files, MetadataDetail::Full);
}

/******************************************************************************/
IndexMap<SpectrumMetadata>
read_spectra_metadata(const SpectraMetadataFiles& files, MetadataDetail detail)
{
  IndexMap<SpectrumMetadata> out;
  if (!files.primary) return out;

  // Each facet resolves its columns through its OWN index entry: the mapping
  // that names a column lives with the file that holds it.
  const Schema::File* primary_file = &files.primary->index_file();

  // Read the primary table ONCE; when the facets are nested struct columns they
  // all come from it.  A split-layout file additionally reads one small table
  // per facet, each joined by `source_index` VALUE (never by row position — the
  // facet files have their own row counts and orderings).
  // Lean does not merely discard these after decoding them -- it never asks
  // Parquet for the columns, so the decode itself is skipped.  On this corpus
  // that is 1.9 MB of 4.7 MB decoded, `activation` alone being 1.0 MB of a
  // 1.5 MB precursor table for a CV-parameter list Lean does not keep.
  const bool lean = detail == MetadataDetail::Lean;

  // Columns this reader NEVER extracts, in either mode.  Skipping them changes
  // nothing observable -- no field of SpectrumMetadata is sourced from any of
  // them -- and `filter_string` alone is 0.43 MB of a 1.51 MB scan table.
  //
  // Safe against the accession fallback because that fallback resolves a
  // REQUESTED name (e.g. "MS_1000016_scan_start_time_unit_UO_0000031") through
  // the file's column mapping, and this reader requests only source_index,
  // scan start time, ion mobility value/type, parameters and scan_windows from
  // this facet -- none of which any archive maps onto the six below.  Verified
  // by dumping every extracted field of five archives before and after: no
  // difference.
  //
  // They are named here rather than wired into SpectrumMetadata because
  // deciding to EXPOSE a field is a separate call from deciding to stop paying
  // for one nobody reads.  Adding any of them means deleting it from this list.
  std::vector<std::string_view> skip_scan = {
      "scan_index", "preset_scan_configuration", "filter_string",
      "ion_injection_time", "instrument_configuration_id", "spectrum_reference"};
  // Named once: adding one of the above to SpectrumMetadata means deleting it
  // from this list, and Lean's extras are appended rather than re-typed.
  if (lean) skip_scan.insert(skip_scan.end(), {"parameters", "scan_windows"});

  auto table = lean ? read_metadata_table(*files.primary,
                                          {"parameters", "auxiliary_arrays"})
                    : read_metadata_table(*files.primary);
  auto scans_table =
      files.scans ? read_metadata_table(*files.scans, skip_scan) : nullptr;
  auto precursors_table =
      files.precursors
          ? (lean ? read_metadata_table(*files.precursors, {"activation"})
                  : read_metadata_table(*files.precursors))
          : nullptr;
  auto selected_ions_table =
      files.selected_ions ? (lean ? read_metadata_table(*files.selected_ions,
                                                        {"parameters"})
                                  : read_metadata_table(*files.selected_ions))
                          : nullptr;

  // -------------------------------------------------------------------
  // PASS 1: spectrum column — build `out` keyed by spectrum.index VALUE.
  // -------------------------------------------------------------------
  // Each facet is either a struct column of this one table (older writers) or a
  // flat table of its own (newer writers).  Normalise both to a list of
  // StructArrays so the passes below are identical for either layout.
  auto file_of = [](Parquet* p) -> const Schema::File* {
    return p ? &p->index_file() : nullptr;
  };
  auto facets_for = [&](const char* nested,
                        const std::shared_ptr<arrow::Table>& separate,
                        Parquet* separate_source) {
    return facets_of(table, nested, separate, primary_file,
                     file_of(separate_source));
  };

  std::vector<Facet> spectrum_facets;
  if (auto col = spectrum_column_from_table(table)) {
    for (const auto& chunk : col->chunks()) {
      if (chunk->type_id() != arrow::Type::STRUCT) continue;
      spectrum_facets.push_back(
          Facet{std::static_pointer_cast<arrow::StructArray>(chunk), primary_file});
    }
  } else if (table->GetColumnByName("index")) {
    // Flat layout: the primary table IS the spectrum facet.
    if (auto st = struct_from_table(table))
      spectrum_facets.push_back(Facet{st, primary_file});
  } else {
    return out; // not a spectrum metadata table
  }

  // One allocation for the whole map instead of a doubling sequence: the
  // entries are 432 bytes each, so growing into 7,534 of them copies ~3 MB
  // through a peak nearly twice the final size for no reason -- the row count
  // is known before the first row is read.
  {
    std::size_t rows = 0;
    for (const auto& spectrum : spectrum_facets)
      rows += static_cast<std::size_t>(spectrum->length());
    out.reserve(rows);
  }

  for (const auto& spectrum : spectrum_facets) {
    auto index_col = IndexColumn::of(resolve_field(spectrum, "index"));
    if (!index_col) continue;
    const IndexColumn& index = *index_col;

    for (int64_t r = 0; r < spectrum->length(); ++r) {
      // F7: outer struct null => children are unreliable, skip the row.
      if (spectrum->IsNull(r)) continue;
      if (index.IsNull(r)) continue;

      SpectrumMetadata m;
      m.index = index.Value(r);
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
      if (detail == MetadataDetail::Full)
        m.parameters = read_cv_params_from_list(spectrum, "parameters", r);

      // Delta model for null-marking reconstruction (large_list<double>).
      if (auto dm = resolve_field(spectrum, "mz_delta_model")) {
        if (!dm->IsNull(r)) {
          auto ll = std::dynamic_pointer_cast<arrow::LargeListArray>(dm);
          auto sl = std::dynamic_pointer_cast<arrow::ListArray>(dm);
          if (ll || sl) {
            auto vals = std::static_pointer_cast<arrow::DoubleArray>(
                ll ? ll->values() : sl->values());
            int64_t off = ll ? ll->value_offset(r) : sl->value_offset(r);
            int64_t len = ll ? ll->value_length(r) : sl->value_length(r);
            m.mz_delta_model.reserve(static_cast<std::size_t>(len));
            for (int64_t k = 0; k < len; ++k)
              m.mz_delta_model.push_back(vals->Value(off + k));
          }
        }
      }

      // -------------------------------------------------------------------
      // RDR-9b: auxiliary_arrays (large_list<struct>) — PART A (structural,
      // VALIDATED) + PART B (raw-byte VALUE decode, FIXTURE-GATED FOLLOW-UP).
      //
      // PART A: schema parse + empty-list handling + count-consistency assert.
      // Every bundled fixture has number_of_auxiliary_arrays==0 and an empty
      // auxiliary_arrays list, so the item body never executes — but the field
      // access + empty-list path IS exercised and validated structurally.
      // -------------------------------------------------------------------
      // Lean skips the whole block, and with it the count-consistency check
      // below: that check is a CONFORMANCE assertion about the file, not
      // something any decode depends on, so a caller that opted out of
      // auxiliary arrays is not owed it.  Full still performs it.
      if (detail == MetadataDetail::Full) {
        // Read number_of_auxiliary_arrays (uint32) for the count-consistency
        // assert (T-03-02 mitigation).  Absent/null counts as 0.
        uint64_t declared_count = 0;
        if (auto cnt =
                opt_int<uint64_t>(spectrum, "number_of_auxiliary_arrays", r)) {
          declared_count = *cnt;
        }

        auto aux_slice = list_slice(resolve_field(spectrum, "auxiliary_arrays"), r);
        if (aux_slice) {
          {
            auto aux_items =
                std::dynamic_pointer_cast<arrow::StructArray>(aux_slice->values);
            if (aux_items) {
              int64_t begin = aux_slice->begin;
              int64_t length = aux_slice->length;
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
                    aa.name = extract_one_cv_param(Facet{name_struct, spectrum.file},
                                                   begin + k);
                  }
                }

                // Scalar string fields.
                const Facet aux{aux_items, spectrum.file};
                aa.data_type = get_string(aux, "data_type", begin + k);
                aa.compression = get_string(aux, "compression", begin + k);
                aa.unit = get_string(aux, "unit", begin + k);
                aa.data_processing_ref =
                    get_string(aux, "data_processing_ref", begin + k);

                // parameters: large_list<CvParam>.
                aa.parameters =
                    read_cv_params_from_list(aux, "parameters", begin + k);

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
                auto data_slice = list_slice(resolve_field(aux, "data"), begin + k);
                if (data_slice) {
                  {
                    auto data_values = std::dynamic_pointer_cast<arrow::UInt8Array>(
                        data_slice->values);
                    if (data_values) {
                      int64_t d_begin = data_slice->begin;
                      int64_t d_length = data_slice->length;

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

      // append + one sort, rather than an insert per row: the rows arrive in
      // file order (ascending in practice, but the sort does not rely on it)
      // and nothing looks anything up until PASS 2.
      const uint64_t key = m.index;
      out.append(key, std::move(m));
    }
  }
  out.sort();

  // -------------------------------------------------------------------
  // PASS 2 and 3: precursor, then selected_ion.  Both are shared with the
  // chromatogram reader -- see attach_precursors / attach_selected_ions.
  // Order matters: ions attach to the precursors pass 2 created.
  // -------------------------------------------------------------------
  attach_precursors(out,
                    facets_for("precursor", precursors_table, files.precursors),
                    detail);
  attach_selected_ions(
      out, facets_for("selected_ion", selected_ions_table, files.selected_ions),
      detail);

  // -------------------------------------------------------------------
  // PASS 4: scan column — scan_parameters + scan_windows (accepted add-on).
  // Same H1 source_index VALUE join.  scan ion_mobility is NULL in all
  // fixtures and is DEFERRED (same as selected_ion IM above).
  // -------------------------------------------------------------------
  {
    // Earliest scan seen per spectrum, so a multi-scan spectrum reports the
    // representative (earliest) scan rather than the last row read.
    std::map<uint64_t, std::optional<float>> earliest_scan;

    for (const auto& scan : facets_for("scan", scans_table, files.scans)) {
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

          // A spectrum may own SEVERAL scans (summed/averaged acquisitions and
          // ion-mobility frames).  Its scalar scan fields must then come from
          // ONE representative scan — the earliest — rather than from whichever
          // row happened to be read last.  The spec requires the minimum scan
          // start time for a multi-scan spectrum; taking the last row's instead
          // reports a spectrum later than it is, so an RT-range query silently
          // misses it.  Scan windows still accumulate across every scan.
          auto sst =
              opt_float(scan, "MS_1000016_scan_start_time_unit_UO_0000031", r);

          bool representative = true;
          if (auto seen = earliest_scan.find(*src_idx);
              seen != earliest_scan.end()) {
            // Already have a scan for this spectrum: only replace when this one
            // is genuinely earlier.  A scan with no time never displaces one
            // that has a time.
            representative =
                sst.has_value() && seen->second.has_value() && *sst < *seen->second;
          }

          if (representative) {
            earliest_scan[*src_idx] = sst;

            if (detail == MetadataDetail::Full) {
              it->second.scan_parameters =
                  read_cv_params_from_list(scan, "parameters", r);
            }

            // RT (seconds): scan.MS_1000016_scan_start_time is the authoritative,
            // unit-annotated field (UO_0000031 = minutes — normative MUST, see
            // docs/schemas/spectra.md).  ×60 -> seconds (a silent
            // minutes/seconds mismatch is a 60x error).
            //
            // scan_start_time is float32 while spectrum.time is float64 and
            // carries the same quantity, so when the two agree to within float32
            // precision keep the PASS 1 value rather than losing digits at an
            // exact range boundary.
            if (sst) {
              const double from_scan = static_cast<double>(*sst) * 60.0;
              const double previous = it->second.retention_time.value_or(from_scan);
              const double scale = std::max(std::abs(from_scan), 1.0);
              if (std::abs(previous - from_scan) > 1e-6 * scale) {
                it->second.retention_time = from_scan;
              }
            }

            // Scan-level ion mobility (handoff P1): null in all bundled
            // fixtures, so null-safe today; value-level correctness is
            // fixture-gated.
            it->second.ion_mobility = opt_double(scan, "ion_mobility_value", r);
            it->second.ion_mobility_type = opt_string(scan, "ion_mobility_type", r);
          }

          // scan_windows: large_list<struct<lower_limit, upper_limit, parameters>>
          auto sw_slice = detail == MetadataDetail::Full
                              ? list_slice(resolve_field(scan, "scan_windows"), r)
                              : std::nullopt;
          if (sw_slice) {
            {
              auto sw_items =
                  std::dynamic_pointer_cast<arrow::StructArray>(sw_slice->values);
              if (sw_items) {
                int64_t begin = sw_slice->begin;
                int64_t length = sw_slice->length;
                it->second.scan_windows.reserve(static_cast<std::size_t>(length));
                const Facet window{sw_items, scan.file};
                for (int64_t k = 0; k < length; ++k) {
                  ScanWindow sw;
                  sw.lower_limit = opt_float(
                      window, "MS_1000501_scan_window_lower_limit_unit_MS_1000040",
                      begin + k);
                  sw.upper_limit = opt_float(
                      window, "MS_1000500_scan_window_upper_limit_unit_MS_1000040",
                      begin + k);
                  sw.parameters =
                      read_cv_params_from_list(window, "parameters", begin + k);
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
std::map<uint64_t, ChromatogramMetadata>
read_chromatogram_metadata(const ChromatogramMetadataFiles& files)
{
  std::map<uint64_t, ChromatogramMetadata> out;
  if (!files.primary) return out;

  // Each facet resolves its columns through its OWN index entry: the mapping
  // that names a column lives with the file that holds it.
  const Schema::File* primary_file = &files.primary->index_file();

  auto table = read_metadata_table(*files.primary);
  auto precursors_table =
      files.precursors ? read_metadata_table(*files.precursors) : nullptr;
  auto selected_ions_table =
      files.selected_ions ? read_metadata_table(*files.selected_ions) : nullptr;

  // The primary facet is a `chromatogram` struct column (nested layout) or the
  // table's own top-level columns (split layout).
  std::vector<Facet> primary;
  if (table->GetColumnByName("chromatogram")) {
    primary = facets_of(table, "chromatogram", nullptr, primary_file);
  } else if (table->GetColumnByName("index")) {
    if (auto st = struct_from_table(table))
      primary.push_back(Facet{st, primary_file});
  } else {
    return out; // not a chromatogram metadata table
  }

  for (const auto& chrom : primary) {
    auto index_col = IndexColumn::of(resolve_field(chrom, "index"));
    if (!index_col) continue;
    const IndexColumn& index = *index_col;

    for (int64_t r = 0; r < chrom->length(); ++r) {
      // F7: outer struct null => children are unreliable, skip the row.
      if (chrom->IsNull(r)) continue;
      if (index.IsNull(r)) continue;

      ChromatogramMetadata m;
      m.index = index.Value(r);
      m.id = get_string(chrom, "id", r);
      m.chromatogram_type = get_string(chrom, "MS_1000626_chromatogram_type", r);
      m.polarity = opt_int<int>(chrom, "MS_1000465_scan_polarity", r);
      m.number_of_data_points =
          opt_int<uint64_t>(chrom, "MS_1003060_number_of_data_points", r);
      m.data_processing_ref = get_string(chrom, "data_processing_ref", r);
      m.parameters = read_cv_params_from_list(chrom, "parameters", r);

      // Flag the types whose Q3 selection the format cannot deliver.  ONLY
      // SRM/MRM has a product: selected ion monitoring names a Q1 and nothing
      // else, so flagging it would claim a lost product that never existed.
      // Both spellings are accepted: mzdata's to_curie() emits MS:1000473 for
      // SRM while its own reader expects MS:1001473.
      static constexpr std::string_view kProductBearing[] = {"MS:1001473",
                                                             "MS:1000473"};
      for (std::string_view type : kProductBearing) {
        if (m.chromatogram_type == type) {
          m.has_unreadable_product = true;
          break;
        }
      }

      out[m.index] = std::move(m);
    }
  }

  attach_precursors(
      out, facets_of(table, "precursor", precursors_table, primary_file,
                     files.precursors ? &files.precursors->index_file() : nullptr));
  attach_selected_ions(
      out,
      facets_of(table, "selected_ion", selected_ions_table, primary_file,
                files.selected_ions ? &files.selected_ions->index_file() : nullptr));

  return out;
}

/******************************************************************************/
std::map<uint64_t, WavelengthSpectrumMetadata>
read_wavelength_spectrum_metadata(const WavelengthMetadataFiles& files)
{
  std::map<uint64_t, WavelengthSpectrumMetadata> out;
  if (!files.primary) return out;

  // Each facet resolves its columns through its OWN index entry: the mapping
  // that names a column lives with the file that holds it.
  const Schema::File* primary_file = &files.primary->index_file();

  auto table = read_metadata_table(*files.primary);
  auto scans_table = files.scans ? read_metadata_table(*files.scans) : nullptr;

  // The primary struct column is named `spectrum`, exactly as for mass spectra
  // — the entity type is carried by the file's role in the index, not by the
  // column name.
  std::vector<Facet> primary;
  if (table->GetColumnByName("spectrum")) {
    primary = facets_of(table, "spectrum", nullptr, primary_file);
  } else if (table->GetColumnByName("index")) {
    if (auto st = struct_from_table(table))
      primary.push_back(Facet{st, primary_file});
  } else {
    return out;
  }

  for (const auto& spectrum : primary) {
    auto index_col = IndexColumn::of(resolve_field(spectrum, "index"));
    if (!index_col) continue;
    const IndexColumn& index = *index_col;

    for (int64_t r = 0; r < spectrum->length(); ++r) {
      if (spectrum->IsNull(r)) continue;
      if (index.IsNull(r)) continue;

      WavelengthSpectrumMetadata m;
      m.index = index.Value(r);
      m.id = get_string(spectrum, "id", r);
      // Minutes on disk (UO:0000031) -> seconds here, matching
      // SpectrumMetadata::retention_time.  The scan facet overrides below.
      if (auto t = opt_double(spectrum, "time", r)) m.time = *t * 60.0;
      m.spectrum_type = get_string(spectrum, "MS_1000559_spectrum_type", r);
      m.representation =
          get_string(spectrum, "MS_1000525_spectrum_representation", r);
      m.lowest_observed_wavelength = opt_double(
          spectrum, "MS_1000619_lowest_observed_wavelength_unit_UO_0000018", r);
      m.highest_observed_wavelength = opt_double(
          spectrum, "MS_1000618_highest_observed_wavelength_unit_UO_0000018", r);
      // The legacy writer emitted this one column with a literal colon in the
      // unit suffix ("..._unit_UO:0000018") where every other column uses an
      // underscore.  resolve_field strips the CV prefix and everything from
      // "_unit_" onward, so the plain name matches either spelling.
      m.lambda_max =
          opt_double(spectrum, "MS_1003812_lambda_max_unit_UO_0000018", r);
      m.number_of_data_points =
          opt_int<uint64_t>(spectrum, "MS_1003060_number_of_data_points", r);
      m.base_peak_intensity =
          opt_float(spectrum, "MS_1000505_base_peak_intensity_unit_MS_1000131", r);
      m.total_ion_current =
          opt_float(spectrum, "MS_1000285_total_ion_current_unit_MS_1000131", r);
      m.data_processing_ref = get_string(spectrum, "data_processing_ref", r);
      m.parameters = read_cv_params_from_list(spectrum, "parameters", r);

      out[m.index] = std::move(m);
    }
  }

  // Scan facet: acquisition time and scan parameters, joined by source_index
  // VALUE.  Only the earliest scan of a multi-scan spectrum is representative,
  // for the reason given in the spectra reader: taking whichever row was read
  // last reports the spectrum later than it is.
  //
  // scan_windows are deliberately NOT read here.  The reference writer maps
  // their limits to m/z units even for wavelength spectra, so a nanometre bound
  // arrives labelled as m/z; reading it would launder that error into our API.
  std::map<uint64_t, std::optional<float>> earliest_scan;
  for (const auto& scan :
       facets_of(table, "scan", scans_table, primary_file,
                 files.scans ? &files.scans->index_file() : nullptr)) {
    for (int64_t r = 0; r < scan->length(); ++r) {
      if (scan->IsNull(r)) continue;
      auto src_idx = opt_int<uint64_t>(scan, "source_index", r);
      if (!src_idx) continue;

      auto it = out.find(*src_idx);
      if (it == out.end()) {
        std::fprintf(stderr,
                     "mzpeak: scan source_index %llu has no matching wavelength "
                     "spectrum — skipped\n",
                     static_cast<unsigned long long>(*src_idx));
        continue;
      }

      auto sst = opt_float(scan, "MS_1000016_scan_start_time_unit_UO_0000031", r);

      bool representative = true;
      if (auto seen = earliest_scan.find(*src_idx); seen != earliest_scan.end()) {
        representative =
            sst.has_value() && seen->second.has_value() && *sst < *seen->second;
      }
      if (!representative) continue;
      earliest_scan[*src_idx] = sst;

      it->second.scan_parameters = read_cv_params_from_list(scan, "parameters", r);

      // scan_start_time is float32 while spectrum.time is float64 and carries
      // the same quantity, so keep the more precise value when the two agree to
      // within float32 rounding.
      if (sst) {
        const double from_scan = static_cast<double>(*sst) * 60.0;
        const double previous = it->second.time.value_or(from_scan);
        const double scale = std::max(std::abs(from_scan), 1.0);
        if (std::abs(previous - from_scan) > 1e-6 * scale) {
          it->second.time = from_scan;
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
    const Facet arr{std::static_pointer_cast<arrow::StructArray>(chunk),
                    &metadata.index_file()};

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
