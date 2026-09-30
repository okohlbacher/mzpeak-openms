/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <arrow/array.h>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mzpeak/data/array_index.h"
#include "mzpeak/exception.h"
#include "mzpeak/schema/cv.h"
#include "mzpeak/util/slice.h"

// The model arithmetic below mirrors the reference implementation operation
// for operation.  Clang contracts `a*b + c` into a fused multiply-add by
// default, which would fuse where the reference does not and unfuse nothing;
// the contract here is bit-for-bit agreement, so contraction is switched off
// for this header and every fusion is written out explicitly as std::fma.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace MzPeak::Data::Transformer::Grid {

/**
 * Coordinate grid encoding (`MS:1003826`).
 *
 * A grid-encoded chunk stores no coordinate values.  Its `chunk_values` list
 * is null and the coordinates are integer INDICES into a model whose
 * parameters ride in the same row, in a struct column
 * `<array>_grid { grid_type, parameters, indices }` that the array index
 * registers with `buffer_format = chunk_transform` and
 * `transform = MS:1003826`.  The reader evaluates the model at each index.
 *
 * On the MAIN axis the index list is delta-coded -- `[first, deltas...]`, the
 * start point included -- because a sorted axis compresses far better that
 * way.  A secondary axis (ion mobility) stores its indices as they are.
 *
 * Models, with the parameter order and the arithmetic of the reference
 * (mzpeak_prototyping `grid.rs`, mzdata `io::tdf::calibration.rs`), so that
 * a bound written by one implementation equals the value decoded by the
 * other bit for bit:
 *
 *   MS:1003824  linear       [intercept, slope, scale=1]   (i*slope + intercept) /
 * scale MS:1003825  square root  [intercept, slope, scale=1]   (i*slope +
 * intercept)^2 / scale MS:9999002  timsTOF m/z  [C0, beta, C2, C3, C4, timebase,
 * delay] t = fma(i, timebase, delay); solve t = C0 + beta*u + C2*u^2 (+ C3*u^3); m/z
 * = u^2 - C4 MS:9999001  TIMS 1/K0    [C6, C7, offset, slope]       1 / (C6 + C7 /
 * (offset + slope*i))
 *
 * The two `MS:9999xxx` accessions are the reference's PLACEHOLDERS for the
 * Bruker models until PSI assigns terms; they are matched literally.
 */

/// The chunk encoding this whole header exists for.
inline const Schema::CV& encoding_cv()
{
  static const Schema::CV cv("MS", "1003826");
  return cv;
}

/// Is @p entry the `<array>_grid` column of a grid-encoded dimension?
inline bool is_grid_encoded(const ArrayIndex::Entry& entry)
{
  return entry.buffer_format == Schema::BufferFormat::ChunkTransform &&
         entry.transform.has_value() && entry.transform->to_cv() == encoding_cv();
}

/// Bruker timsTOF m/z at digitizer index @p i: mzdata's
/// `MzCalibrationModel2::convert_f64`, operation for operation.
inline double timstof_mz(const std::vector<double>& p, double i)
{
  const double c0 = p[0], beta = p[1], c2 = p[2], c3 = p[3], c4 = p[4];
  const double timebase = p[5], delay = p[6];

  const double tof = std::fma(i, timebase, delay);
  const double s0 = (tof - c0) / beta;

  double refined = s0;
  if (c3 != 0.0) {
    // Cubic: Newton from the linear estimate, at most eight steps.
    double s = s0;
    const double c2_2 = c2 * 2.0, c3_3 = c3 * 3.0;
    for (int step = 0; step < 8; ++step) {
      const double s2 = s * s;
      const double f = std::fma(s, beta, c0) + s2 * c2 + std::fma(s2 * s, c3, -tof);
      const double deriv = std::fma(s, c2_2, beta) + s2 * c3_3;
      if (deriv == 0.0) break;
      const double delta = f / deriv;
      s -= delta;
      if (std::fabs(delta) < 1e-12) break;
    }
    refined = s;
  } else if (c2 != 0.0) {
    // Quadratic, closed form, the numerically stable root.
    const double d = beta * beta - 4.0 * c2 * (c0 - tof);
    refined = (d < 0.0) ? s0 : (c0 - tof) / (-0.5 * (beta + std::sqrt(d)));
  }
  return refined * refined - c4;
}

/// Bruker TIMS 1/K0 at scan @p i: mzdata's `TimsCalibrationModel2::convert`.
inline double timstof_mobility(const std::vector<double>& p, double i)
{
  const double c6 = p[0], c7 = p[1], offset = p[2], slope = p[3];
  return 1.0 / (c6 + c7 / (offset + slope * i));
}

/// The value of grid index @p idx under @p grid_type, or nullopt for a model
/// this reader does not know or a parameter list of the wrong length.
inline std::optional<double>
value_at(std::string_view grid_type, const std::vector<double>& p, std::uint32_t idx)
{
  const double i = static_cast<double>(idx);
  const std::size_t n = p.size();

  if (grid_type == "MS:1003824" && n >= 2 && n <= 3) {
    return (i * p[1] + p[0]) / (n == 3 ? p[2] : 1.0);
  }
  if (grid_type == "MS:1003825" && n >= 2 && n <= 3) {
    const double u = i * p[1] + p[0];
    return (u * u) / (n == 3 ? p[2] : 1.0);
  }
  if (grid_type == "MS:9999002" && n == 7) return timstof_mz(p, i);
  if (grid_type == "MS:9999001" && n == 4) return timstof_mobility(p, i);
  return std::nullopt;
}

namespace detail {

/// One string, tolerating both Arrow widths.
inline std::optional<std::string> string_at(const arrow::Array& a, int64_t i)
{
  if (a.IsNull(i)) return std::nullopt;
  if (a.type_id() == arrow::Type::LARGE_STRING) {
    return static_cast<const arrow::LargeStringArray&>(a).GetString(i);
  }
  if (a.type_id() == arrow::Type::STRING) {
    return static_cast<const arrow::StringArray&>(a).GetString(i);
  }
  return std::nullopt;
}

/// The values of row @p i of a list column, tolerating both offset widths.
inline std::shared_ptr<arrow::Array> list_row(const arrow::Array& a, int64_t i)
{
  if (a.IsNull(i)) return nullptr;
  if (a.type_id() == arrow::Type::LARGE_LIST) {
    return static_cast<const arrow::LargeListArray&>(a).value_slice(i);
  }
  if (a.type_id() == arrow::Type::LIST) {
    return static_cast<const arrow::ListArray&>(a).value_slice(i);
  }
  return nullptr;
}

/// Read a numeric Arrow array as doubles (parameters are float64 by
/// specification; any float width is accepted rather than refused).
inline bool doubles_of(const arrow::Array& a, std::vector<double>& out)
{
  out.clear();
  out.reserve(static_cast<std::size_t>(a.length()));
  switch (a.type_id()) {
  case arrow::Type::DOUBLE:
    for (int64_t k = 0; k < a.length(); ++k) {
      out.push_back(static_cast<const arrow::DoubleArray&>(a).Value(k));
    }
    return true;
  case arrow::Type::FLOAT:
    for (int64_t k = 0; k < a.length(); ++k) {
      out.push_back(static_cast<const arrow::FloatArray&>(a).Value(k));
    }
    return true;
  default:
    return false;
  }
}

/// Read an integer Arrow array as grid indices.  The specification says
/// uint32; the narrower unsigned widths and int32 are accepted as the
/// reference accepts them.
inline bool indices_of(const arrow::Array& a, std::vector<std::uint32_t>& out)
{
  out.clear();
  out.reserve(static_cast<std::size_t>(a.length()));
  auto take = [&](const auto& typed) {
    for (int64_t k = 0; k < typed.length(); ++k) {
      out.push_back(static_cast<std::uint32_t>(typed.Value(k)));
    }
  };
  switch (a.type_id()) {
  case arrow::Type::UINT32:
    take(static_cast<const arrow::UInt32Array&>(a));
    return true;
  case arrow::Type::UINT16:
    take(static_cast<const arrow::UInt16Array&>(a));
    return true;
  case arrow::Type::UINT8:
    take(static_cast<const arrow::UInt8Array&>(a));
    return true;
  case arrow::Type::INT32:
    take(static_cast<const arrow::Int32Array&>(a));
    return true;
  default:
    return false;
  }
}

/// Flatten a per-row double column across record batches; null stays null.
inline std::vector<std::optional<double>> flatten(const Util::Slice::Raw& raw)
{
  std::vector<std::optional<double>> out;
  for (const auto& chunk : raw) {
    if (chunk->type_id() != arrow::Type::DOUBLE) {
      throw InvalidFormatError("grid chunk bound column is not float64");
    }
    const auto& a = static_cast<const arrow::DoubleArray&>(*chunk);
    for (int64_t i = 0; i < a.length(); ++i) {
      out.push_back(a.IsNull(i) ? std::nullopt : std::optional<double>(a.Value(i)));
    }
  }
  return out;
}

} // namespace detail

/// Bounds the file records for each chunk row, used to check the decode.
struct Bounds {
  std::vector<std::optional<double>> start;
  std::vector<std::optional<double>> end;
};

/**
 * Decode every row of a `<array>_grid` struct column into @p out.
 *
 * @param raw          the struct column's record batches, in row order
 * @param delta_coded  true on the main axis (indices are `[first, deltas...]`)
 * @param bounds       the row's `chunk_start`/`chunk_end` when the dimension
 *                     has them; each decoded chunk's first and last value is
 *                     checked against them.  A model ported with one wrong
 *                     operation still produces plausible numbers, and the file
 *                     hands us the means to catch that on every single row --
 *                     so it is checked rather than trusted.
 */
template <typename V>
void decode(const Util::Slice::Raw& raw,
            bool delta_coded,
            const Bounds* bounds,
            std::string_view dim_name,
            std::vector<V>& out)
{
  // Relative slack for the bound check: the model is evaluated with the same
  // operations as the writer, so agreement is normally exact; a few ulp of
  // headroom keeps a platform without hardware fma from being refused.
  constexpr double kRelativeTolerance = 1e-9;

  std::vector<double> parameters;
  std::vector<std::uint32_t> indices;
  std::size_t flat = 0;

  for (const auto& chunk : raw) {
    if (chunk->type_id() != arrow::Type::STRUCT) {
      throw InvalidFormatError("grid column of " + std::string(dim_name) +
                               " is not a struct");
    }
    const auto& rows = static_cast<const arrow::StructArray&>(*chunk);
    auto grid_type = rows.GetFieldByName("grid_type");
    auto params = rows.GetFieldByName("parameters");
    auto idx = rows.GetFieldByName("indices");
    if (!grid_type || !params || !idx) {
      throw InvalidFormatError("grid column of " + std::string(dim_name) +
                               " lacks grid_type, parameters or indices");
    }

    for (int64_t r = 0; r < rows.length(); ++r, ++flat) {
      if (rows.IsNull(r)) continue; // no data for this chunk

      const std::optional<std::string> type = detail::string_at(*grid_type, r);
      const std::shared_ptr<arrow::Array> p_row = detail::list_row(*params, r);
      const std::shared_ptr<arrow::Array> i_row = detail::list_row(*idx, r);
      if (!type || !p_row || !i_row || !detail::doubles_of(*p_row, parameters) ||
          !detail::indices_of(*i_row, indices)) {
        throw InvalidFormatError("malformed grid row in " + std::string(dim_name));
      }
      if (indices.empty()) continue;

      if (delta_coded) {
        std::uint32_t acc = 0;
        for (auto& v : indices) {
          acc += v; // wrapping, as the reference does
          v = acc;
        }
      }

      const std::size_t first = out.size();
      for (const std::uint32_t k : indices) {
        const std::optional<double> value = value_at(*type, parameters, k);
        if (!value) {
          throw InvalidFormatError("unknown grid model " + *type + " with " +
                                   std::to_string(parameters.size()) +
                                   " parameters in " + std::string(dim_name));
        }
        out.push_back(static_cast<V>(*value));
      }

      if (bounds == nullptr || flat >= bounds->start.size()) continue;
      const auto check = [&](const std::optional<double>& expected, double got,
                             const char* which) {
        if (!expected) return;
        const double scale = std::max(std::fabs(*expected), 1.0);
        if (std::fabs(got - *expected) > kRelativeTolerance * scale) {
          throw InvalidFormatError(
              "grid decode of " + std::string(dim_name) + " disagrees with " +
              which + " on chunk " + std::to_string(flat) + ": model gives " +
              std::to_string(got) + ", file records " + std::to_string(*expected));
        }
      };
      check(bounds->start[flat], static_cast<double>(out[first]), "chunk_start");
      check(bounds->end[flat], static_cast<double>(out.back()), "chunk_end");
    }
  }
}

} // namespace MzPeak::Data::Transformer::Grid
