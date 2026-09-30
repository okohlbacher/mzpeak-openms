/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <arrow/array.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mzpeak/data/grid.h"
#include "mzpeak/exception.h"

namespace MzPeak::Data {

/**
 * One row of a coordinate grid `chunk_transform` column, unpacked: the
 * model the row declares and the row's integer grid indices.
 */
struct GridChunk {
  /// The model that maps an index back to a coordinate.
  GridModel model;

  /// The row's indices.  An integer array of `uint8`, `uint16` or
  /// `uint32`; held by shared_ptr so it outlives the call.
  std::shared_ptr<arrow::Array> indices;
};

/**
 * Unpack row 0 of @p row, which must be a one-row window onto a
 * coordinate grid struct column (`{grid_type, parameters, indices}`).
 *
 * @p dim_name only names the dimension in error messages.
 *
 * Throws InvalidFormatError when the column is not such a struct, and
 * when the grid model is one this reader does not implement.
 */
GridChunk grid_chunk_at(const std::shared_ptr<arrow::Array>& row,
                        std::string_view dim_name);

/**
 * Decode the indices of @p chunk through its model, appending the
 * coordinates to @p out.
 *
 * When @p delta_indices the stored indices are deltas after the first
 * (the encoding the reference writer uses for a sorted main axis);
 * otherwise they are absolute.  Delta accumulation wraps, as it does in
 * the reference.
 */
template <typename V>
void grid_decode(const GridChunk& chunk, bool delta_indices, std::vector<V>& out);

/******************************************************************************/
namespace Detail
{
  /// Append the decoded coordinates of one integer index array.
  template <typename ArrayType, typename V>
  void grid_decode_typed(const GridModel& model,
                         const ArrayType& indices,
                         bool delta_indices,
                         std::vector<V>& out)
  {
    using raw_type = typename ArrayType::value_type;

    const int64_t n = indices.length();
    out.reserve(out.size() + static_cast<std::size_t>(n));

    raw_type last = 0;

    for (int64_t i = 0; i < n; ++i) {
      // The reference reads the values buffer straight through; the
      // field is declared non-null, and a null would read as 0 there
      // too.
      const raw_type v = indices.Value(i);

      // Unsigned accumulation wraps in both languages, and the writer
      // relies on it for a main axis that is not strictly ascending.
      last = (delta_indices && i > 0) ? static_cast<raw_type>(last + v) : v;

      out.push_back(static_cast<V>(model.from_index(static_cast<uint32_t>(last))));
    }
  }
} // namespace Detail

/******************************************************************************/
template <typename V>
void grid_decode(const GridChunk& chunk, bool delta_indices, std::vector<V>& out)
{
  // A null `indices` list is an empty chunk: nothing to append.
  if (chunk.indices == nullptr) return;

  const arrow::Array& indices = *chunk.indices;

  switch (indices.type_id()) {
  case arrow::Type::UINT32:
    Detail::grid_decode_typed(
        chunk.model, static_cast<const arrow::UInt32Array&>(indices), delta_indices,
        out);
    return;
  case arrow::Type::UINT16:
    Detail::grid_decode_typed(
        chunk.model, static_cast<const arrow::UInt16Array&>(indices), delta_indices,
        out);
    return;
  case arrow::Type::UINT8:
    Detail::grid_decode_typed(
        chunk.model, static_cast<const arrow::UInt8Array&>(indices), delta_indices,
        out);
    return;
  default:
    throw InvalidFormatError("coordinate grid indices must be an unsigned "
                             "integer array but are: " +
                             indices.type()->ToString());
  }
}

} // namespace MzPeak::Data
