/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

// Reproduce the reference implementation's rounding exactly: it fuses a
// multiply-add only where it writes one (`f64::mul_add` -> std::fma below),
// and a contraction the compiler adds on its own changes the result.  meson
// also passes -ffp-contract=off (or /fp:precise) for this file; this pragma
// keeps the guarantee with the code should it ever be built another way.
#pragma STDC FP_CONTRACT OFF

#include <algorithm>
#include <cmath>
#include <string>

#include "mzpeak/data/grid.h"
#include "mzpeak/data/grid_chunk.h"
#include "mzpeak/exception.h"

namespace MzPeak::Data {

namespace
{
  /// The CURIE each model is written as.
  constexpr std::string_view LINEAR = "MS:1003824";
  constexpr std::string_view SQRT_LINEAR = "MS:1003825";
  constexpr std::string_view TIMSTOF_TIMS2 = "MS:9999001";
  constexpr std::string_view TIMSTOF_MZ2 = "MS:9999002";

  /// How many parameters each model takes: [min, max].
  std::pair<std::size_t, std::size_t> arity(GridModel::Kind kind)
  {
    switch (kind) {
    case GridModel::Kind::Linear:
    case GridModel::Kind::SquareRootLinear:
      // The optional third parameter is `scale`, which defaults to 1.
      return {2, 3};
    case GridModel::Kind::TimsTofMz2:
      return {7, 7};
    case GridModel::Kind::TimsTofTims2:
      return {4, 4};
    }

    return {0, 0};
  }
} // namespace

/******************************************************************************/
GridModel::GridModel(Kind kind, const double* parameters, std::size_t count)
    : kind_(kind)
    , p_{}
{
  // `scale` defaults to 1 for the two interpolating models; every other
  // slot defaults to 0, which is what the models below expect for an
  // absent coefficient.
  if (kind == Kind::Linear || kind == Kind::SquareRootLinear) p_[2] = 1.0;

  std::copy_n(parameters, std::min(count, p_.size()), p_.begin());
}

/******************************************************************************/
GridModel GridModel::from_parameters(std::string_view grid_type,
                                     const double* parameters,
                                     std::size_t count)
{
  Kind kind{};

  if (grid_type == LINEAR) {
    kind = Kind::Linear;
  } else if (grid_type == SQRT_LINEAR) {
    kind = Kind::SquareRootLinear;
  } else if (grid_type == TIMSTOF_MZ2) {
    kind = Kind::TimsTofMz2;
  } else if (grid_type == TIMSTOF_TIMS2) {
    kind = Kind::TimsTofTims2;
  } else {
    throw InvalidFormatError("unsupported coordinate grid model: " +
                             std::string(grid_type));
  }

  const auto [lo, hi] = arity(kind);

  if (count < lo || count > hi) {
    throw InvalidFormatError(
        "coordinate grid model " + std::string(grid_type) + " takes " +
        (lo == hi ? std::to_string(lo)
                  : std::to_string(lo) + " to " + std::to_string(hi)) +
        " parameters but the file supplies " + std::to_string(count));
  }

  return GridModel(kind, parameters, count);
}

/******************************************************************************/
std::string_view GridModel::grid_type() const
{
  switch (kind_) {
  case Kind::Linear:
    return LINEAR;
  case Kind::SquareRootLinear:
    return SQRT_LINEAR;
  case Kind::TimsTofMz2:
    return TIMSTOF_MZ2;
  case Kind::TimsTofTims2:
    return TIMSTOF_TIMS2;
  }

  return {};
}

/******************************************************************************/
double GridModel::from_index(uint32_t index) const
{
  const double i = static_cast<double>(index);

  switch (kind_) {
  case Kind::Linear: {
    // LinearGrid::from_index: (index * slope + intercept) / scale
    const double intercept = p_[0], slope = p_[1], scale = p_[2];
    return (i * slope + intercept) / scale;
  }

  case Kind::SquareRootLinear: {
    // SquareRootLinearGrid::from_index: (index * slope + intercept)^2 / scale
    const double intercept = p_[0], slope = p_[1], scale = p_[2];
    const double t = i * slope + intercept;
    return (t * t) / scale;
  }

  case Kind::TimsTofTims2: {
    // TimsCalibrationModel2::convert: 1 / (c6 + c7 / (offset + slope * scan)),
    // where `offset + slope * scan` is the TIMS ramp voltage.
    const double c6 = p_[0], c7 = p_[1], offset = p_[2], slope = p_[3];
    return 1.0 / (c6 + c7 / (offset + slope * i));
  }

  case Kind::TimsTofMz2: {
    // MzCalibrationModel2::convert_f64.  The fused multiply-adds are
    // `f64::mul_add` in the reference and must stay fused to reproduce
    // its values exactly.
    const double c0 = p_[0], beta = p_[1], c2 = p_[2], c3 = p_[3], c4 = p_[4];
    const double timebase = p_[5], delay = p_[6];

    const double tof = std::fma(i, timebase, delay);
    const double s0 = (tof - c0) / beta;
    double refined;

    if (c3 != 0.0) {
      // Newton refinement of the cubic, as `rustims` writes it.
      double s = s0;
      const double c2_2 = c2 * 2.0;
      const double c3_3 = c3 * 3.0;

      for (int iter = 0; iter < 8; ++iter) {
        const double s2 = s * s;

        const double f1 = std::fma(s, beta, c0);
        const double f2 = s2 * c2;
        const double f3 = std::fma(s2 * s, c3, -tof);
        const double f = f1 + f2 + f3;

        const double df1 = std::fma(s, c2_2, beta);
        const double df2 = s2 * c3_3;
        const double deriv_f = df1 + df2;

        if (deriv_f == 0.0) break;

        const double step = f / deriv_f;
        s -= step;
        if (std::abs(step) < 1e-12) break;
      }

      refined = s;
    } else if (c2 != 0.0) {
      const double d = beta * beta - 4.0 * c2 * (c0 - tof);
      if (d < 0.0) {
        refined = s0;
      } else {
        const double q = -0.5 * (beta + std::sqrt(d));
        refined = (c0 - tof) / q;
      }
    } else {
      refined = s0;
    }

    return refined * refined - c4;
  }
  }

  return 0.0;
}

} // namespace MzPeak::Data

/******************************************************************************/
namespace MzPeak::Data {

namespace
{
  /// The element array of one row of a (large) list column, or nullptr
  /// when that row is null.
  std::shared_ptr<arrow::Array>
  list_value_slice(const std::shared_ptr<arrow::Array>& array,
                   int64_t row,
                   std::string_view member,
                   std::string_view dim_name)
  {
    if (array->IsNull(row)) return nullptr;

    switch (array->type_id()) {
    case arrow::Type::LARGE_LIST:
      return static_cast<const arrow::LargeListArray&>(*array).value_slice(row);
    case arrow::Type::LIST:
      return static_cast<const arrow::ListArray&>(*array).value_slice(row);
    default:
      throw InvalidFormatError("coordinate grid member '" + std::string(member) +
                               "' of " + std::string(dim_name) +
                               " must be a list but is: " +
                               array->type()->ToString());
    }
  }

  /// A named member of the grid struct, or a clear error.
  std::shared_ptr<arrow::Array> member_of(const arrow::StructArray& row,
                                          const char* name,
                                          std::string_view dim_name)
  {
    std::shared_ptr<arrow::Array> field = row.GetFieldByName(name);

    if (field == nullptr) {
      throw InvalidFormatError("coordinate grid column for " +
                               std::string(dim_name) + " has no '" +
                               std::string(name) + "' member");
    }

    return field;
  }
} // namespace

/******************************************************************************/
GridChunk grid_chunk_at(const std::shared_ptr<arrow::Array>& row,
                        std::string_view dim_name)
{
  if (row == nullptr || row->type_id() != arrow::Type::STRUCT) {
    throw InvalidFormatError(
        "coordinate grid column for " + std::string(dim_name) +
        " must be a struct of {grid_type, parameters, indices} but is: " +
        (row == nullptr ? std::string("absent") : row->type()->ToString()));
  }

  const auto& in = static_cast<const arrow::StructArray&>(*row);

  std::shared_ptr<arrow::Array> grid_type = member_of(in, "grid_type", dim_name);
  std::shared_ptr<arrow::Array> parameters = member_of(in, "parameters", dim_name);
  std::shared_ptr<arrow::Array> indices = member_of(in, "indices", dim_name);

  std::string type_name;

  switch (grid_type->type_id()) {
  case arrow::Type::LARGE_STRING:
    type_name = static_cast<const arrow::LargeStringArray&>(*grid_type).GetString(0);
    break;
  case arrow::Type::STRING:
    type_name = static_cast<const arrow::StringArray&>(*grid_type).GetString(0);
    break;
  default:
    throw InvalidFormatError("coordinate grid 'grid_type' of " +
                             std::string(dim_name) +
                             " must be a string but is: " +
                             grid_type->type()->ToString());
  }

  std::shared_ptr<arrow::Array> params =
      list_value_slice(parameters, 0, "parameters", dim_name);

  if (params == nullptr || params->type_id() != arrow::Type::DOUBLE) {
    throw InvalidFormatError("coordinate grid 'parameters' of " +
                             std::string(dim_name) +
                             " must be a list of float64");
  }

  const auto& values = static_cast<const arrow::DoubleArray&>(*params);

  return GridChunk{
      GridModel::from_parameters(type_name, values.raw_values(),
                                 static_cast<std::size_t>(values.length())),
      list_value_slice(indices, 0, "indices", dim_name)};
}

} // namespace MzPeak::Data
