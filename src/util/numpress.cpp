/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/util/numpress.h"

#include "mzpeak/util/vendor/MSNumpress.hpp"

namespace MzPeak::Util {

namespace {

// The vendored reference API works with `std::vector<unsigned char>` for the
// encoded buffer.  Our public API uses `std::vector<uint8_t>`; on every
// supported platform these are the same type, but build a view-compatible
// vector explicitly to stay strictly correct.
std::vector<unsigned char> as_uchar(const std::vector<uint8_t>& bytes)
{
  return std::vector<unsigned char>(bytes.begin(), bytes.end());
}

} // namespace

/******************************************************************************/
std::vector<double> numpress_decode_linear(const std::vector<uint8_t>& bytes)
{
  std::vector<unsigned char> data = as_uchar(bytes);
  std::vector<double> result;
  ms::numpress::MSNumpress::decodeLinear(data, result);
  return result;
}

/******************************************************************************/
std::vector<float> numpress_decode_slof(const std::vector<uint8_t>& bytes)
{
  std::vector<unsigned char> data = as_uchar(bytes);
  std::vector<double> decoded;
  ms::numpress::MSNumpress::decodeSlof(data, decoded);

  std::vector<float> result;
  result.reserve(decoded.size());
  for (double v : decoded)
    result.push_back(static_cast<float>(v));
  return result;
}

} // namespace MzPeak::Util
