/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#pragma once

#include <arrow/array.h>
#include <arrow/builder.h>
#include <memory>
#include <vector>

#include "mzpeak/data/array_index.h"
#include "mzpeak/data/null_marking.h"
#include "mzpeak/data/signals.h"
#include "mzpeak/exception.h"
#include "mzpeak/schema/buffer_format.h"
#include "mzpeak/schema/group.h"
#include "mzpeak/schema/psi/data_type.h"
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

private:
  template <typename V>
  void decode(const ArrayIndex::Dimension&, std::vector<V>&) const;

  template <typename V>
  void chunked(const ArrayIndex::Dimension&, std::vector<V>&) const;

  /// Reconstruct the main axis of one chunk from its delta-encoded values.
  ///
  /// Mirrors the reference implementation's null_delta_decode: the chunk's
  /// `start` is the first value and every later entry is a delta from the
  /// previous one — EXCEPT that a value following a null is stored absolutely,
  /// because there is no previous value to delta against.  A chunk that opens
  /// with two nulls had a singleton peak on the boundary, so `start` is
  /// re-emitted; a chunk that opens with exactly one null does not carry
  /// `start` as a value at all.
  template <typename V>
  static void delta_decode_chunk(const arrow::DoubleArray& values,
                                 int64_t begin,
                                 int64_t length,
                                 double start,
                                 std::vector<V>& out,
                                 std::vector<bool>& valid);

  /// Run @p src through the shared scalar decoder so that chunked and point
  /// layouts reconstruct nulls with exactly one implementation.
  template <typename V>
  void reconstruct(const std::shared_ptr<arrow::Array>& src,
                   bool needs_delta_model,
                   std::vector<V>& out) const;

  template <typename N, typename V>
  void point(const Schema::Column&, const N& null_decoder, std::vector<V>&) const;

  template <Util::Type From, typename V>
  void remap(const ArrayIndex::Dimension& dim, std::vector<V>& v) const;

  std::shared_ptr<Signals> signals_;
  std::shared_ptr<Util::Slice> slice_;
  Util::DeltaEstimator<T> delta_estimator_;
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

    if (dim.needs_delta_model()) {
      using N = NullMarking::Decoder<V, T>;
      point<N, V>(field.value(), N{delta_estimator_}, v);
    } else {
      using N = Util::Decoders::NullToZero<V>;
      point<N, V>(field.value(), N{}, v);
    }
  } else {
    chunked<V>(dim, v);
  }
}

/******************************************************************************/
template <typename T>
template <typename V>
void Decoder<T>::delta_decode_chunk(const arrow::DoubleArray& values,
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
      secondary_entry = &e;
      break;
    case BufferFormat::ChunkTransform:
      transform_entry = &e;
      break;
    case BufferFormat::ChunkEnd:
      break; // bounds only; not needed to decode
    case BufferFormat::Point:
      break;
    }
  }

  if (transform_entry != nullptr) {
    // MS-Numpress (and friends) store opaque bytes in their own column.
    throw ParquetError(
        "chunked array uses an opaque transform, which is not supported yet (" +
        Schema::PSI::array_type_to_string(dim.array_type) + ")");
  }

  auto raw_of =
      [this](const ArrayIndex::Entry* e) -> std::shared_ptr<Util::Slice::Raw> {
    if (e == nullptr) return nullptr;
    auto col = signals_->array_index()->entry_column(*signals_->groups(), *e);
    if (!col.has_value()) return nullptr;
    return slice_->raw(col.value());
  };

  // A secondary array is just its lists concatenated, nulls preserved.
  if (values_entry == nullptr && secondary_entry != nullptr) {
    auto lists = raw_of(secondary_entry);
    if (lists == nullptr) {
      throw ParquetError("unable to decode dimension, not in schema: " + dim.name);
    }

    using array_type = Util::type_traits<Util::enum_type_v<V>>::array_type;
    using builder_type = arrow::NumericBuilder<typename array_type::TypeClass>;
    builder_type builder;

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

    std::shared_ptr<arrow::Array> assembled;
    if (!builder.Finish(&assembled).ok()) {
      throw ParquetError("failed to assemble chunked array: " + dim.name);
    }
    reconstruct<V>(assembled, dim.needs_delta_model(), v);
    return;
  }

  if (values_entry == nullptr || start_entry == nullptr) {
    throw ParquetError("chunked dimension is missing chunk_start/chunk_values: " +
                       dim.name);
  }

  auto starts_raw = raw_of(start_entry);
  auto values_raw = raw_of(values_entry);
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

  for (std::size_t c = 0; c < values_raw->size(); ++c) {
    auto starts = std::dynamic_pointer_cast<arrow::DoubleArray>((*starts_raw)[c]);
    auto lists = std::dynamic_pointer_cast<arrow::LargeListArray>((*values_raw)[c]);
    if (!starts || !lists) {
      throw ParquetError("unexpected chunk column types for dimension: " + dim.name);
    }
    auto items = std::static_pointer_cast<arrow::DoubleArray>(lists->values());

    // Only the delta encoding is understood; anything else would silently
    // produce plausible-but-wrong coordinates, so refuse it explicitly.
    std::shared_ptr<arrow::StringArray> encodings;
    if (encodings_raw != nullptr && c < encodings_raw->size()) {
      encodings = std::dynamic_pointer_cast<arrow::StringArray>((*encodings_raw)[c]);
    }

    for (int64_t r = 0; r < lists->length(); ++r) {
      if (encodings && !encodings->IsNull(r)) {
        const std::string enc(encodings->GetString(r));
        if (enc != "MS:1003089") {
          throw ParquetError("unsupported chunk encoding '" + enc +
                             "' for dimension: " + dim.name);
        }
      }
      if (lists->IsNull(r) || starts->IsNull(r)) continue;

      delta_decode_chunk<V>(*items, lists->value_offset(r), lists->value_length(r),
                            starts->Value(r), assembled_values, assembled_valid);
    }
  }

  // Hand the assembled axis to the same null-marking decoder the point layout
  // uses, so both layouts reconstruct identically.
  using array_type = Util::type_traits<Util::enum_type_v<V>>::array_type;
  using builder_type = arrow::NumericBuilder<typename array_type::TypeClass>;
  builder_type builder;
  (void)builder.Reserve(static_cast<int64_t>(assembled_values.size()));
  for (std::size_t i = 0; i < assembled_values.size(); ++i) {
    if (assembled_valid[i]) {
      (void)builder.Append(assembled_values[i]);
    } else {
      (void)builder.AppendNull();
    }
  }

  std::shared_ptr<arrow::Array> assembled;
  if (!builder.Finish(&assembled).ok()) {
    throw ParquetError("failed to assemble chunked array: " + dim.name);
  }
  reconstruct<V>(assembled, dim.needs_delta_model(), v);
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
