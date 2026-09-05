/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#pragma once

#include <arrow/array.h>
#include <arrow/builder.h>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

#include "mzpeak/data/array_index.h"
#include "mzpeak/data/null_marking.h"
#include "mzpeak/data/signals.h"
#include "mzpeak/data/transformer/primary.h"
#include "mzpeak/data/transformer/secondary.h"
#include "mzpeak/exception.h"
#include "mzpeak/schema/psi/data_type.h"
#include "mzpeak/util/decoders.h"
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

  /// The unit the values of @p dim were decoded in, or empty when the dimension
  /// declared none or drew rows from columns in two different units.  Load
  /// bearing: a time array in minutes vs seconds is a 60x error, and has_uv
  /// stores intensities in counts vs absorbance.  Only valid after decode.
  const std::string& unit_of(const ArrayIndex::Dimension& dim) const
  {
    static const std::string none;
    auto it = units_.find(dim.name);
    return it == units_.end() ? none : it->second;
  }

private:
  template <typename V>
  void decode(const ArrayIndex::Dimension&, std::vector<V>&) const;

  template <typename N, typename V>
  void decode_with_nulls(const ArrayIndex::Dimension&,
                         const N& null_decoder,
                         std::vector<V>&) const;

  template <Util::Type From, typename V>
  void remap(const ArrayIndex::Dimension& dim, std::vector<V>& v) const;

  void note_unit(const ArrayIndex::Dimension& dim, const std::string& unit) const
  {
    auto [it, inserted] = units_.try_emplace(dim.name, unit);
    if (!inserted && it->second != unit) it->second.clear();
  }

  template <typename V>
  using builder_for = arrow::NumericBuilder<
      typename Util::type_traits<Util::enum_type_v<V>>::array_type::TypeClass>;

  template <typename V>
  void finish(builder_for<V>& builder,
              const ArrayIndex::Dimension& dim,
              std::vector<V>& out) const;

  template <typename V>
  void reconstruct(const std::shared_ptr<arrow::Array>& src,
                   bool needs_delta_model,
                   std::vector<V>& out) const;

  /// One logical array spread across several complementary point columns
  /// (has_uv).  Upstream's single values_entry() path cannot express this.
  template <typename V>
  void coalesced_point(const ArrayIndex::Dimension&, std::vector<V>&) const;

  std::shared_ptr<Signals> signals_;
  std::shared_ptr<Util::Slice> slice_;
  Util::DeltaEstimator<T> delta_estimator_;
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
    v.insert(v.end(), tmp.begin(), tmp.end());
  }
}

/******************************************************************************/
template <typename T>
template <typename V>
void Decoder<T>::decode(const ArrayIndex::Dimension& dim, std::vector<V>& v) const
{
  if (signals_->array_index()->layout() == ArrayIndex::Layout::Unknown) {
    throw UnknownLayoutError("cannot decode dimension, unknown layout: " + dim.name);
  }

  // Coalesced point: one logical array over several complementary point columns
  // (has_uv).  Upstream's dispatch decodes a single values_entry() and cannot
  // express this, so it is handled before the ordinary single-entry cases.
  const auto& entries = dim.entries;
  if (entries.size() > 1 && std::ranges::all_of(entries, [](const auto& e) {
        return e.buffer_format == Schema::BufferFormat::Point;
      })) {
    coalesced_point<V>(dim, v);
    return;
  }

  note_unit(dim, dim.values_entry().unit);

  switch (signals_->array_index()->layout()) {
  case ArrayIndex::Layout::Point:
  case ArrayIndex::Layout::Chunked:
    if (dim.needs_delta_model()) {
      using N = NullMarking::Decoder<V, T>;
      decode_with_nulls<N, V>(dim, N{delta_estimator_}, v);
    } else {
      using N = Util::Decoders::NullToZero<V>;
      decode_with_nulls<N, V>(dim, N{}, v);
    }
    break;

  case ArrayIndex::Layout::Unknown:
    throw UnknownLayoutError("cannot decode dimension: " + dim.name);
  }
}

/******************************************************************************/
template <typename T>
template <typename N, typename V>
void Decoder<T>::decode_with_nulls(const ArrayIndex::Dimension& dim,
                                   const N& null_decoder,
                                   std::vector<V>& v) const
{
  const auto& primary_entry = dim.values_entry();
  auto col = signals_->column(primary_entry);

  if (!col.has_value()) {
    throw InvalidFormatError("unable to decode dimension, not in schema: " +
                             dim.name);
  } else if (!slice_->has_column(col.value())) {
    return; // No data to decode so we can exit early.
  }

  auto go = [&](auto&& decoder) -> void { slice_->array(col.value(), v, decoder); };

  if (primary_entry.buffer_format == Schema::BufferFormat::Point) {
    auto decoder = Util::Decoders::Scalar<V, std::vector<V>, N>(null_decoder);
    go(decoder);
  } else {
    if (dim.is_main_axis()) {
      using Transformer = Transformer::Primary::Decoder<V>;
      Transformer transformer(signals_, slice_, dim);
      auto decoder = Util::Decoders::Flattened<V, std::vector<V>, N, Transformer>(
          null_decoder, std::move(transformer));
      go(decoder);
    } else {
      using Transformer = Transformer::Secondary::Decoder<V>;
      Transformer transformer(dim);
      auto decoder = Util::Decoders::Flattened<V, std::vector<V>, N, Transformer>(
          null_decoder, std::move(transformer));
      go(decoder);
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
template <typename T>
template <typename V>
void Decoder<T>::coalesced_point(const ArrayIndex::Dimension& dim,
                                 std::vector<V>& v) const
{
  using array_type = Util::type_traits<Util::enum_type_v<V>>::array_type;

  std::vector<const ArrayIndex::Entry*> ordered;
  ordered.reserve(dim.entries.size());
  for (const auto& e : dim.entries) {
    if (e.buffer_priority) ordered.push_back(&e);
  }
  for (const auto& e : dim.entries) {
    if (!e.buffer_priority) ordered.push_back(&e);
  }

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

} // namespace MzPeak::Data::Encoding
