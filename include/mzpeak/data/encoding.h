/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#pragma once

#include <algorithm>
#include <arrow/array.h>
#include <arrow/builder.h>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "mzpeak/data/array_index.h"
#include "mzpeak/data/null_marking.h"
#include "mzpeak/data/signals.h"
#include "mzpeak/exception.h"
#include "mzpeak/schema/buffer_format.h"
#include "mzpeak/schema/group.h"
#include "mzpeak/schema/psi/data_type.h"
#include "mzpeak/schema/psi/transform.h"
#include "mzpeak/util/numpress.h"
#include "mzpeak/util/slice.h"
#include "mzpeak/util/types.h"

namespace MzPeak::Data::Encoding {

/**
 * Decode mzPeak signal data encoding (point and chunk).
 *
 * The template type T should match the type for the main axis.
 */
template <typename T> class Decoder {
public:
  /// Constructor.
  Decoder(std::shared_ptr<Signals> signals,
          std::shared_ptr<Util::Slice> slice,
          const Util::DeltaEstimator<T>& estimator)
      : signals_(std::move(signals))
      , slice_(std::move(slice))
      , delta_estimator_(estimator)
  {
  }

  /**
   * Decode a float or double.
   */
  template <typename V>
  void decimal(const ArrayIndex::Dimension&, std::vector<V>&) const;

  /**
   * Decode a 32- or 64-bit integer.
   */
  template <typename V>
  void integer(const ArrayIndex::Dimension&, std::vector<V>&) const;

  /**
   * The unit CURIE of the column that actually supplied a dimension's values,
   * as a CURIE (e.g. "UO:0000031", "MS:1000131"), or empty when unknown.
   *
   * This is not decoration.  One logical array can be spread over several
   * physical columns holding the SAME quantity in DIFFERENT units -- the
   * bundled `has_uv` file stores one chromatogram's intensities in detector
   * counts and another's in absorbance units, in two columns, each null where
   * the other has values.  The decoded numbers are correct either way, and
   * without this the caller cannot tell which of the two they were handed.
   *
   * Only meaningful after the dimension has been decoded.
   */
  const std::string& unit_of(const ArrayIndex::Dimension& dim) const
  {
    static const std::string none;
    auto it = units_.find(dim.name);
    return it == units_.end() ? none : it->second;
  }

private:
  /// Record which entry supplied this dimension's values.  A dimension whose
  /// rows came from columns in two different units has no single answer, so it
  /// records none rather than whichever was seen last.
  void note_unit(const ArrayIndex::Dimension& dim, const std::string& unit) const
  {
    auto [it, inserted] = units_.try_emplace(dim.name, unit);
    if (!inserted && it->second != unit) it->second.clear();
  }

  template <typename V>
  void decode(const ArrayIndex::Dimension&, std::vector<V>&) const;

  template <typename V>
  void chunked(const ArrayIndex::Dimension&, std::vector<V>&) const;

  /// Merge several point columns describing one logical array (see the
  /// definition for the precedence rule).
  template <typename V>
  void coalesced_point(const ArrayIndex::Dimension&, std::vector<V>&) const;

  /// Reconstruct the main axis of one chunk from its delta-encoded values.
  ///
  /// Mirrors the reference implementation's null_delta_decode: the chunk's
  /// `start` is the first value and every later entry is a delta from the
  /// previous one — EXCEPT that a value following a null is stored absolutely,
  /// because there is no previous value to delta against.  A chunk that opens
  /// with two nulls had a singleton peak on the boundary, so `start` is
  /// re-emitted; a chunk that opens with exactly one null does not carry
  /// `start` as a value at all.
  template <typename V, typename ValuesArray>
  static void delta_decode_chunk(const ValuesArray& values,
                                 int64_t begin,
                                 int64_t length,
                                 double start,
                                 std::vector<V>& out,
                                 std::vector<bool>& valid);

  /// The Arrow builder that produces an array of V.
  template <typename V>
  using builder_for = arrow::NumericBuilder<
      typename Util::type_traits<Util::enum_type_v<V>>::array_type::TypeClass>;

  /// Finish @p builder and run the result through the shared null handling.
  /// Every path that assembles an array in memory ends here, so chunked,
  /// coalesced and point layouts all reconstruct nulls identically.
  template <typename V>
  void finish(builder_for<V>& builder,
              const ArrayIndex::Dimension& dim,
              std::vector<V>& out) const;

  /// Run @p src through the shared scalar decoder so that chunked and point
  /// layouts reconstruct nulls with exactly one implementation.
  template <typename V>
  void reconstruct(const std::shared_ptr<arrow::Array>& src,
                   bool needs_delta_model,
                   std::vector<V>& out) const;

  /// Concatenate every Arrow chunk of one point column, then reconstruct nulls
  /// once over the whole entity (see the call site for why).
  template <typename V>
  void concatenated(const Schema::Column&,
                    const ArrayIndex::Dimension&,
                    std::vector<V>&) const;

  template <typename N, typename V>
  void point(const Schema::Column&, const N& null_decoder, std::vector<V>&) const;

  template <Util::Type From, typename V>
  void remap(const ArrayIndex::Dimension& dim, std::vector<V>& v) const;

  std::shared_ptr<Signals> signals_;
  std::shared_ptr<Util::Slice> slice_;
  Util::DeltaEstimator<T> delta_estimator_;

  /// Dimension name -> unit CURIE of the column that supplied its values.
  /// Mutable because decoding is const and this is a record of what it did.
  mutable std::map<std::string, std::string> units_;
};

/******************************************************************************/
template <typename T>
template <typename V>
void Decoder<T>::decimal(const ArrayIndex::Dimension& dim, std::vector<V>& v) const
{
  Util::Type type = dim.type_or_throw();

  if (type == Util::Type::Float32) {
    remap<Util::Type::Float32>(dim, v);
  } else if (type == Util::Type::Float64) {
    remap<Util::Type::Float64>(dim, v);
  } else {
    Util::lift_type(type, []<Util::Type X> {
      std::string msg("Expected float or double but got: ");
      msg += Util::type_traits<X>::name;
      throw(TypeError(msg));
    });
  }
}

/******************************************************************************/
template <typename T>
template <typename V>
void Decoder<T>::integer(const ArrayIndex::Dimension& dim, std::vector<V>& v) const
{
  Util::Type type = dim.type_or_throw();

  if (type == Util::Type::Int32) {
    remap<Util::Type::Int32>(dim, v);
  } else if (type == Util::Type::Int64) {
    remap<Util::Type::Int64>(dim, v);
  } else {
    Util::lift_type(type, []<Util::Type X> {
      std::string msg("Expected int32 or int64 but got: ");
      msg += Util::type_traits<X>::name;
      throw(TypeError(msg));
    });
  }
}

/******************************************************************************/
template <typename T>
template <Util::Type From, typename V>
void Decoder<T>::remap(const ArrayIndex::Dimension& dim, std::vector<V>& v) const
{
  using F = Util::type_traits<From>::value_type;

  if constexpr (std::is_same_v<F, V>) {
    decode<V>(dim, v);
  } else {
    std::vector<F> tmp;
    decode<F>(dim, tmp);
    v.reserve(tmp.size());
    v.insert(v.end(), tmp.begin(), tmp.end());
  }
}

/******************************************************************************/
template <typename T>
template <typename V>
void Decoder<T>::decode(const ArrayIndex::Dimension& dim, std::vector<V>& v) const
{
  const auto& entries = dim.entries;

  if (entries.empty()) {
    std::string msg("unable to decode dimension, wrong encoding: ");
    throw ParquetError(msg + dim.name);
  } else if (entries.size() == 1 &&
             entries[0].buffer_format == Schema::BufferFormat::Point) {

    auto field =
        signals_->array_index()->entry_column(*signals_->groups(), entries[0]);

    if (!field.has_value()) {
      throw ParquetError("unable to decode dimension, not in schema: " + dim.name);
    }

    // Assemble the whole entity BEFORE reconstructing nulls.
    //
    // Feeding the null decoder one physical Arrow chunk at a time makes
    // reconstruction depend on record-batch boundaries, which are a storage
    // artifact the writer chooses freely.  When a batch boundary falls between
    // a run and its flanking null, that null is anchored to the run on the
    // WRONG side — subtracting a delta from the following run instead of adding
    // one to the preceding run.  The result stays monotonic and plausible, so
    // nothing downstream notices.  The spec is explicit that an entry must be
    // buffered before null filling.
    note_unit(dim, entries[0].unit);
    concatenated<V>(field.value(), dim, v);
  } else if (std::ranges::all_of(entries, [](const auto& e) {
               return e.buffer_format == Schema::BufferFormat::Point;
             })) {
    // Several point columns describing ONE logical array — e.g. has_uv stores
    // a counts intensity and an absorbance intensity as separate columns, and
    // each chromatogram populates exactly one, leaving the other all-null.
    // This is still the point layout, not a chunked one.
    coalesced_point<V>(dim, v);
  } else {
    // Prefer the entry the file marks primary, but fall back to the first --
    // buffer_priority is OPTIONAL, and a chunked array whose entries omit it
    // would otherwise decode with no unit at all.  An absent unit is not
    // harmless: the caller cannot then tell seconds from minutes.
    const ArrayIndex::Entry* chosen = &entries.front();
    for (const auto& e : entries) {
      if (e.buffer_priority) {
        chosen = &e;
        break;
      }
    }
    note_unit(dim, chosen->unit);
    chunked<V>(dim, v);
  }
}

/******************************************************************************/
/*
 * Merge several point columns of the same logical array into one.
 *
 * For each row the first column carrying a value wins, with `primary` columns
 * consulted first, so the conventional column takes precedence over an
 * alternate-unit sibling.  A row that is null everywhere stays null and is left
 * to the shared null handling below.
 */
template <typename T>
template <typename V>
void Decoder<T>::coalesced_point(const ArrayIndex::Dimension& dim,
                                 std::vector<V>& v) const
{
  using array_type = Util::type_traits<Util::enum_type_v<V>>::array_type;

  // Primary columns first; otherwise keep the array index's own order.
  std::vector<const ArrayIndex::Entry*> ordered;
  ordered.reserve(dim.entries.size());
  for (const auto& e : dim.entries) {
    if (e.buffer_priority) ordered.push_back(&e);
  }
  for (const auto& e : dim.entries) {
    if (!e.buffer_priority) ordered.push_back(&e);
  }

  // Units travel alongside the columns, not in a parallel index into
  // `ordered`: an entry absent from the schema is skipped here, so the two
  // vectors would not line up.
  std::vector<std::shared_ptr<Util::Slice::Raw>> columns;
  std::vector<std::string> column_units;
  for (const auto* e : ordered) {
    auto col = signals_->array_index()->entry_column(*signals_->groups(), *e);
    if (!col.has_value()) continue;
    if (auto raw = slice_->raw(col.value())) {
      columns.push_back(std::move(raw));
      column_units.push_back(e->unit);
    }
  }

  if (columns.empty()) {
    throw ParquetError("unable to decode dimension, not in schema: " + dim.name);
  }

  builder_for<V> builder;

  const std::size_t n_chunks = columns.front()->size();
  for (std::size_t c = 0; c < n_chunks; ++c) {
    auto first = std::static_pointer_cast<array_type>((*columns.front())[c]);
    for (int64_t r = 0; r < first->length(); ++r) {
      // Take the first column carrying a value, primary first — but do NOT
      // pass over a second one silently.  These sibling columns can hold the
      // same quantity in DIFFERENT units (has_uv stores detector counts and
      // absorbance), so two values on one row means the file is telling us two
      // incompatible things and picking either would be a guess presented as
      // fact.  In every bundled file they are strictly disjoint.
      bool written = false;
      for (std::size_t ci = 0; ci < columns.size(); ++ci) {
        const auto& column = columns[ci];
        if (c >= column->size()) continue;
        auto arr = std::static_pointer_cast<array_type>((*column)[c]);
        if (r >= arr->length() || arr->IsNull(r)) continue;
        if (written) {
          throw ParquetError(
              "two columns of dimension '" + dim.name +
              "' carry a value on the same row; they may be in different units "
              "and there is no basis for preferring one");
        }
        (void)builder.Append(arr->Value(r));
        note_unit(dim, column_units[ci]);
        written = true;
      }
      if (!written) (void)builder.AppendNull();
    }
  }

  finish<V>(builder, dim, v);
}

/******************************************************************************/
template <typename T>
template <typename V, typename ValuesArray>
void Decoder<T>::delta_decode_chunk(const ValuesArray& values,
                                    int64_t begin,
                                    int64_t length,
                                    double start,
                                    std::vector<V>& out,
                                    std::vector<bool>& valid)
{
  auto emit = [&out, &valid](double x, bool ok) {
    out.push_back(static_cast<V>(x));
    valid.push_back(ok);
  };

  bool have_last = true;
  double last = start;

  if (length > 0 && values.IsNull(begin)) {
    // Two leading nulls: a singleton peak sat on the chunk boundary, so the
    // start value is a real point in its own right and must be re-emitted.
    if (length > 1 && values.IsNull(begin + 1)) emit(start, true);
    have_last = false;
  } else {
    emit(start, true);
  }

  for (int64_t k = 0; k < length; ++k) {
    const int64_t at = begin + k;
    if (values.IsNull(at)) {
      emit(0.0, false);
      have_last = false;
    } else if (!have_last) {
      // First real value after a null is absolute, not a delta.
      last = values.Value(at);
      have_last = true;
      emit(last, true);
    } else {
      last += values.Value(at);
      emit(last, true);
    }
  }
}

/******************************************************************************/
template <typename T>
template <typename V>
void Decoder<T>::finish(builder_for<V>& builder,
                        const ArrayIndex::Dimension& dim,
                        std::vector<V>& out) const
{
  std::shared_ptr<arrow::Array> assembled;
  if (!builder.Finish(&assembled).ok()) {
    throw ParquetError("failed to assemble array for dimension: " + dim.name);
  }
  reconstruct<V>(assembled, dim.needs_delta_model(), out);
}

/******************************************************************************/
template <typename T>
template <typename V>
void Decoder<T>::reconstruct(const std::shared_ptr<arrow::Array>& src,
                             bool needs_delta_model,
                             std::vector<V>& out) const
{
  if (needs_delta_model) {
    using N = NullMarking::Decoder<V, T>;
    Util::Decoders::Scalar<V, std::vector<V>, N> decoder{N{delta_estimator_}};
    decoder.decode(src, out);
  } else {
    using N = Util::Decoders::NullToZero<V>;
    Util::Decoders::Scalar<V, std::vector<V>, N> decoder{N{}};
    decoder.decode(src, out);
  }
}

/******************************************************************************/
/*
 * Decode one dimension of the chunked layout.
 *
 * Each row of the slice is one chunk.  The main axis is rebuilt per chunk from
 * (chunk_start, chunk_values) under chunk_encoding and concatenated; a
 * secondary array is simply its per-chunk lists concatenated.  Null marking is
 * then applied ONCE to the assembled array — never per component column, which
 * matters because chunk_start/end/values/encoding all carry sorting_rank 0 and
 * would otherwise each look like a main axis needing reconstruction.
 */
template <typename T>
template <typename V>
void Decoder<T>::chunked(const ArrayIndex::Dimension& dim, std::vector<V>& v) const
{
  using Schema::BufferFormat;

  const ArrayIndex::Entry* start_entry = nullptr;
  const ArrayIndex::Entry* values_entry = nullptr;
  const ArrayIndex::Entry* encoding_entry = nullptr;
  const ArrayIndex::Entry* end_entry = nullptr;
  const ArrayIndex::Entry* secondary_entry = nullptr;
  const ArrayIndex::Entry* transform_entry = nullptr;

  for (const auto& e : dim.entries) {
    switch (e.buffer_format) {
    case BufferFormat::ChunkStart:
      start_entry = &e;
      break;
    case BufferFormat::ChunkValues:
      values_entry = &e;
      break;
    case BufferFormat::ChunkEncoding:
      encoding_entry = &e;
      break;
    case BufferFormat::ChunkSecondary:
      // Several secondary columns of one dimension is the chunked equivalent
      // of the coalesced point layout -- the same logical array split across
      // columns that may differ in UNIT, each null where the others have
      // values.  The point path merges those; this one has only ever kept the
      // last, which returns one entity's values under another's unit.
      //
      // Refused rather than merged: no bundled fixture has the shape, so a
      // merge here would be an unexercised guess, and the failure it replaces
      // is silent.  The point layout handles the case today.
      if (secondary_entry != nullptr && secondary_entry->unit != e.unit) {
        throw ParquetError(
            "dimension '" + dim.name +
            "' has chunked secondary columns in more than one unit ('" +
            secondary_entry->unit + "' and '" + e.unit +
            "'); this reader cannot merge them");
      }
      secondary_entry = &e;
      break;
    case BufferFormat::ChunkTransform:
      transform_entry = &e;
      break;
    case BufferFormat::ChunkEnd:
      end_entry = &e;
      break;
    case BufferFormat::Point:
      break;
    }
  }

  auto raw_of =
      [this](const ArrayIndex::Entry* e) -> std::shared_ptr<Util::Slice::Raw> {
    if (e == nullptr) return nullptr;
    auto col = signals_->array_index()->entry_column(*signals_->groups(), *e);
    if (!col.has_value()) return nullptr;
    return slice_->raw(col.value());
  };

  // MS-Numpress keeps the values as opaque bytes in their own column; the
  // plain values column is null on those rows.  Decode per chunk and
  // concatenate.  Numpress output is dense — the compressor has no notion of a
  // null — so there is nothing for null marking to reconstruct afterwards.
  if (transform_entry != nullptr) {
    auto ends_for_transform = raw_of(end_entry);
    auto byte_lists = raw_of(transform_entry);
    if (byte_lists == nullptr) {
      throw ParquetError("unable to decode dimension, not in schema: " + dim.name);
    }

    const auto& transform = transform_entry->transform;
    if (!transform.has_value()) {
      throw ParquetError("chunk transform column declares no transform: " +
                         dim.name);
    }
    const Schema::CV cv = transform->to_cv();
    const bool linear = (cv.accession() == "1002312");
    const bool slof = (cv.accession() == "1002314");
    if (!linear && !slof) {
      throw ParquetError("unsupported chunk transform 'MS:" + cv.accession() +
                         "' for dimension: " + dim.name);
    }

    for (const auto& chunk : *byte_lists) {
      auto list = std::dynamic_pointer_cast<arrow::LargeListArray>(chunk);
      if (!list) {
        throw ParquetError("chunk transform column is not a large_list: " +
                           dim.name);
      }
      auto bytes = std::static_pointer_cast<arrow::UInt8Array>(list->values());

      std::shared_ptr<arrow::DoubleArray> ends;
      if (ends_for_transform != nullptr) {
        const std::size_t which =
            static_cast<std::size_t>(&chunk - &(*byte_lists)[0]);
        if (which < ends_for_transform->size()) {
          ends = std::dynamic_pointer_cast<arrow::DoubleArray>(
              (*ends_for_transform)[which]);
        }
      }

      for (int64_t r = 0; r < list->length(); ++r) {
        if (list->IsNull(r)) continue;
        const int64_t begin = list->value_offset(r);
        const int64_t length = list->value_length(r);

        std::vector<uint8_t> raw;
        raw.reserve(static_cast<std::size_t>(length));
        for (int64_t k = 0; k < length; ++k) {
          raw.push_back(bytes->Value(begin + k));
        }

        const std::size_t before = v.size();

        if (linear) {
          for (double x : Util::numpress_decode_linear(raw)) {
            v.push_back(static_cast<V>(x));
          }
        } else {
          for (float x : Util::numpress_decode_slof(raw)) {
            v.push_back(static_cast<V>(x));
          }
        }

        // NOT bounded against chunk_end, deliberately.
        //
        // chunk_end marks the last REAL point of the chunk, but a null-marked
        // array continues past it with flanking zero-intensity points.  In the
        // delta path those are nulls, so walking back to the last valid entry
        // lands exactly on chunk_end; under Numpress the compressor has no
        // notion of a null, so they decode as dense zeros and the final value
        // legitimately overshoots the bound — measured 3.9e-4 past it on
        // small.numpress.mzpeak, against a 1.6e-8 round-trip error at the
        // declared end itself.  Distinguishing the two needs the intensity
        // array, which this function does not have: it decodes one dimension.
        //
        // Loosening the tolerance until it passed would have hidden that rather
        // than checked anything.
        (void)before;
      }
    }
    return;
  }

  // A secondary array is just its lists concatenated, nulls preserved.
  if (values_entry == nullptr && secondary_entry != nullptr) {
    auto lists = raw_of(secondary_entry);
    if (lists == nullptr) {
      throw ParquetError("unable to decode dimension, not in schema: " + dim.name);
    }

    using array_type = Util::type_traits<Util::enum_type_v<V>>::array_type;
    builder_for<V> builder;

    for (const auto& chunk : *lists) {
      auto list = std::dynamic_pointer_cast<arrow::LargeListArray>(chunk);
      if (!list) {
        throw ParquetError("chunk secondary column is not a large_list: " +
                           dim.name);
      }
      auto items = std::static_pointer_cast<array_type>(list->values());
      for (int64_t r = 0; r < list->length(); ++r) {
        if (list->IsNull(r)) continue;
        const int64_t begin = list->value_offset(r);
        const int64_t length = list->value_length(r);
        for (int64_t k = 0; k < length; ++k) {
          if (items->IsNull(begin + k)) {
            (void)builder.AppendNull();
          } else {
            (void)builder.Append(items->Value(begin + k));
          }
        }
      }
    }

    finish<V>(builder, dim, v);
    return;
  }

  if (values_entry == nullptr || start_entry == nullptr) {
    throw ParquetError("chunked dimension is missing chunk_start/chunk_values: " +
                       dim.name);
  }

  auto starts_raw = raw_of(start_entry);
  auto values_raw = raw_of(values_entry);
  auto ends_raw = raw_of(end_entry);
  auto encodings_raw = raw_of(encoding_entry);
  if (starts_raw == nullptr || values_raw == nullptr) {
    throw ParquetError("unable to decode dimension, not in schema: " + dim.name);
  }
  if (starts_raw->size() != values_raw->size()) {
    throw ParquetError("chunk_start and chunk_values disagree on chunk count: " +
                       dim.name);
  }

  std::vector<V> assembled_values;
  std::vector<bool> assembled_valid;

  // End of the previous chunk, for the ordering check below.
  std::optional<double> previous_end;

  for (std::size_t c = 0; c < values_raw->size(); ++c) {
    auto starts = std::dynamic_pointer_cast<arrow::DoubleArray>((*starts_raw)[c]);
    auto lists = std::dynamic_pointer_cast<arrow::LargeListArray>((*values_raw)[c]);
    if (!starts || !lists) {
      throw ParquetError("unexpected chunk column types for dimension: " + dim.name);
    }
    // The spec permits float32 coordinates, and static_pointer_cast has no
    // runtime check — casting a float32 child to DoubleArray would read eight
    // bytes per four-byte value and yield plausible coordinates assembled from
    // adjacent memory.  Dispatch on the actual type instead.
    auto items = std::dynamic_pointer_cast<arrow::DoubleArray>(lists->values());
    auto items32 = std::dynamic_pointer_cast<arrow::FloatArray>(lists->values());
    if (!items && !items32) {
      throw ParquetError("chunk_values is neither float32 nor float64: " + dim.name);
    }

    std::shared_ptr<arrow::DoubleArray> ends;
    if (ends_raw != nullptr && c < ends_raw->size()) {
      ends = std::dynamic_pointer_cast<arrow::DoubleArray>((*ends_raw)[c]);
    }

    // Only the delta encoding is understood; anything else would silently
    // produce plausible-but-wrong coordinates, so refuse it explicitly.
    // String and LargeString must be treated equivalently.  Accepting only
    // StringArray meant a LargeString tag failed the cast, the encoding check
    // was skipped entirely, and a basic-encoded chunk was delta-decoded — e.g.
    // [100, 101, 102] coming back as [100, 201, 303].
    std::shared_ptr<arrow::StringArray> encodings;
    std::shared_ptr<arrow::LargeStringArray> encodings_large;
    if (encodings_raw != nullptr && c < encodings_raw->size()) {
      encodings = std::dynamic_pointer_cast<arrow::StringArray>((*encodings_raw)[c]);
      encodings_large =
          std::dynamic_pointer_cast<arrow::LargeStringArray>((*encodings_raw)[c]);
    }

    for (int64_t r = 0; r < lists->length(); ++r) {
      std::optional<std::string> enc;
      if (encodings && !encodings->IsNull(r)) enc = encodings->GetString(r);
      if (encodings_large && !encodings_large->IsNull(r)) {
        enc = encodings_large->GetString(r);
      }
      // An absent tag is NOT an implicit delta encoding.  Guessing would decode
      // a basic-encoded chunk as deltas and yield a plausible, wrong axis.
      if (!enc.has_value()) {
        throw ParquetError("chunk carries no chunk_encoding tag: " + dim.name);
      }
      if (*enc != "MS:1003089") {
        throw ParquetError("unsupported chunk encoding '" + *enc +
                           "' for dimension: " + dim.name);
      }
      if (lists->IsNull(r) || starts->IsNull(r)) continue;

      // An empty or all-null chunk is written with start == end == 0 (the
      // reference writer does this for empty spectra).  The reference reader
      // drops such a row before decoding; decoding it here would emit `start`
      // as a real coordinate, adding a spurious m/z 0.0 point and desynchronising
      // the m/z and intensity arrays for the rest of the spectrum.
      if (ends != nullptr && !ends->IsNull(r) && starts->Value(r) == 0.0 &&
          ends->Value(r) == 0.0) {
        continue;
      }

      const std::size_t before = assembled_values.size();
      if (items) {
        delta_decode_chunk<V>(*items, lists->value_offset(r), lists->value_length(r),
                              starts->Value(r), assembled_values, assembled_valid);
      } else {
        delta_decode_chunk<V>(*items32, lists->value_offset(r),
                              lists->value_length(r), starts->Value(r),
                              assembled_values, assembled_valid);
      }

      // chunk_end states the chunk's last coordinate.  Checking the decode
      // against it turns a corrupt or misread chunk_values list into an error
      // instead of a plausible axis that is quietly wrong.  Tolerance is
      // relative: the value is reached by accumulating deltas, so exact
      // equality is not guaranteed in general even though every bundled chunk
      // matches to the bit.
      if (ends != nullptr && !ends->IsNull(r)) {
        // A chunk must not run backwards, and chunks must not overlap or go
        // backwards relative to each other: either means the rows were joined
        // wrongly, which is otherwise undetectable.
        if (!(starts->Value(r) <= ends->Value(r))) {
          throw ParquetError("chunk_start exceeds chunk_end (" + dim.name + ")");
        }
        if (previous_end.has_value() && starts->Value(r) < *previous_end) {
          throw ParquetError("chunks overlap or are out of order (" + dim.name +
                             ")");
        }
        previous_end = ends->Value(r);

        for (std::size_t k = assembled_values.size(); k > before; --k) {
          if (!assembled_valid[k - 1]) continue;
          const double last = static_cast<double>(assembled_values[k - 1]);
          const double expected = ends->Value(r);
          // Tight: the bound exists to catch a mis-joined or corrupt values
          // list, and every chunk of every bundled file reproduces its
          // chunk_end to the bit.  1e-6 relative would tolerate ~0.001 Da at
          // m/z 1000 — far more than any real accumulation error, and enough to
          // let genuine corruption through.
          const double scale = std::max(std::abs(expected), 1.0);
          if (std::abs(last - expected) > 1e-9 * scale) {
            throw ParquetError("chunk does not end at its declared chunk_end (" +
                               dim.name + ")");
          }
          break;
        }
      }
    }
  }

  // Hand the assembled axis to the same null-marking decoder the point layout
  // uses, so both layouts reconstruct identically.
  builder_for<V> builder;
  (void)builder.Reserve(static_cast<int64_t>(assembled_values.size()));
  for (std::size_t i = 0; i < assembled_values.size(); ++i) {
    if (assembled_valid[i]) {
      (void)builder.Append(assembled_values[i]);
    } else {
      (void)builder.AppendNull();
    }
  }

  finish<V>(builder, dim, v);
}

/******************************************************************************/
template <typename T>
template <typename V>
void Decoder<T>::concatenated(const Schema::Column& col,
                              const ArrayIndex::Dimension& dim,
                              std::vector<V>& v) const
{
  using array_type = Util::type_traits<Util::enum_type_v<V>>::array_type;

  auto chunks = slice_->raw(col);
  if (chunks == nullptr) return;

  builder_for<V> builder;
  for (const auto& chunk : *chunks) {
    auto typed = std::static_pointer_cast<array_type>(chunk);
    for (int64_t r = 0; r < typed->length(); ++r) {
      if (typed->IsNull(r)) {
        (void)builder.AppendNull();
      } else {
        (void)builder.Append(typed->Value(r));
      }
    }
  }

  finish<V>(builder, dim, v);
}

/******************************************************************************/
template <typename T>
template <typename N, typename V>
void Decoder<T>::point(const Schema::Column& col,
                       const N& null_decoder,
                       std::vector<V>& v) const
{
  slice_->array(col, v, Util::Decoders::Scalar<V, std::vector<V>, N>(null_decoder));
}

} // namespace MzPeak::Data::Encoding
