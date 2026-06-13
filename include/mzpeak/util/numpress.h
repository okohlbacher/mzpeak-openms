/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <cstdint>
#include <vector>

namespace MzPeak::Util {

/**
 * Decode a MS-Numpress *linear* encoded byte buffer (typically an m/z array)
 * into the original doubles.  Thin wrapper over
 * `ms::numpress::MSNumpress::decodeLinear`.
 *
 * The linear codec stores a fixed-point scaling factor in the header and is
 * lossy to roughly `1 / (2 * fixedPoint)` absolute on each value.
 *
 * Throws `const char*` (propagated from the reference implementation) if the
 * buffer is malformed/truncated.
 */
std::vector<double> numpress_decode_linear(const std::vector<uint8_t>& bytes);

/**
 * Decode a MS-Numpress *slof* (short logged float) encoded byte buffer
 * (typically an intensity array) into floats.  The reference codec decodes to
 * double; the result is narrowed to `float`.  Thin wrapper over
 * `ms::numpress::MSNumpress::decodeSlof`.
 *
 * SLOF is lossy: each value is stored as a 2-byte log, giving roughly 0.05%
 * relative error.
 *
 * Throws `const char*` (propagated from the reference implementation) if the
 * buffer is malformed/truncated.
 */
std::vector<float> numpress_decode_slof(const std::vector<uint8_t>& bytes);

} // namespace MzPeak::Util
