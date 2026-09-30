/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace MzPeak::Data {

/**
 * A coordinate grid model: the map from an integer grid index back to
 * the coordinate it stands for.
 *
 * PSI-MS "coordinate grid encoding" (MS:1003826) stores a chunk's main
 * axis as integer indices into a grid rather than as the coordinates
 * themselves.  The grid itself is described by a `grid_type` CURIE
 * naming one of the models below plus a list of parameters.
 *
 * The arithmetic here reproduces the reference implementation
 * (`mzpeak_prototyping::grid`, and for the two Bruker models mzdata
 * 0.67's `io::tdf::calibration`) operation for operation, including its
 * uses of a fused multiply-add, so that a grid-encoded archive decodes
 * to bit-identical values.  Do not "simplify" the expressions.
 */
class GridModel final {
public:
  /// The known grid models.
  enum class Kind {
    /// MS:1003824 -- linear grid interpolation.
    ///
    /// Parameters `[intercept, slope]`, with an optional third
    /// `scale` that defaults to 1.
    Linear,

    /// MS:1003825 -- square root grid interpolation.
    ///
    /// Parameters `[intercept, slope]`, with an optional third
    /// `scale` that defaults to 1.  The reference writer emits only
    /// the first two, so `scale` is almost always the default.
    SquareRootLinear,

    /// MS:9999002 -- the Bruker timsTOF m/z calibration, model type 2.
    ///
    /// Parameters `[c0, beta, c2, c3, c4, digitizer_timebase,
    /// digitizer_delay]` (exactly seven).
    TimsTofMz2,

    /// MS:9999001 -- the Bruker timsTOF mobility calibration, model
    /// type 2.
    ///
    /// Parameters `[c6, c7, offset, slope]` (exactly four), where
    /// `offset + slope * scan` is the TIMS ramp voltage.
    ///
    /// Archives written before the reference implementation's
    /// `eb08ba0` carry that last pair the other way round, as
    /// `[c6, c7, slope, offset]`.  Both spellings are read: the ramp
    /// offset is the larger of the two in magnitude by a factor of
    /// the mobility scan count, so the roles are not in doubt.
    TimsTofTims2,
  };

  /**
   * Build the model named by @p grid_type from @p parameters.
   *
   * Throws InvalidFormatError naming the grid type when it is not one
   * of the models above, and when the parameter count does not match
   * the model.
   */
  static GridModel from_parameters(std::string_view grid_type,
                                   const double* parameters,
                                   std::size_t count);

  /**
   * The coordinate that @p index stands for.
   */
  double from_index(uint32_t index) const;

  /**
   * Which model this is.
   */
  Kind kind() const { return kind_; }

  /**
   * The CURIE naming this model.
   */
  std::string_view grid_type() const;

private:
  GridModel(Kind kind, const double* parameters, std::size_t count);

  Kind kind_;
  std::array<double, 7> p_;
};

} // namespace MzPeak::Data
