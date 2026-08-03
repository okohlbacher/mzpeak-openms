/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE Util
#include <boost/test/included/unit_test.hpp>

#include <arrow/io/api.h>

#include "mzpeak/util/algorithm.h"
#include "mzpeak/util/enumerable_proxy.h"

/******************************************************************************/
BOOST_AUTO_TEST_CASE(enumerable_proxy_simple)
{
  std::vector<int> v1{0, 1, 2, 3, 4, 5}, v2;
  v2.reserve(v1.size());

  MzPeak::Util::EnumerableProxy<int, int> ep(v1.size(),
                                             [v1](std::size_t n) { return v1[n]; });

  for (auto i : ep) {
    v2.push_back(i);
  }

  BOOST_TEST(v1 == v2);
}

/******************************************************************************/
// Range/batch intersection.  A page-index range is given in ABSOLUTE file rows
// and must be rebased onto each record batch it overlaps.  The bug this
// replaces subtracted the batch's absolute start position from the range's
// LENGTH, which is correct only for the first batch and negative afterwards —
// invisible to every bundled fixture, all of which fit in one batch.
BOOST_AUTO_TEST_CASE(intersect_range_rebases_onto_the_batch)
{
  using MzPeak::Util::Algorithm::intersect_range;

  // Wholly inside the first batch: offset unchanged, full length.
  BOOST_TEST((intersect_range(10, 5, 0, 100) == std::pair<int64_t, int64_t>{10, 5}));

  // The regression: a range starting beyond the first batch.  The old form
  // computed length = 100 - 65536 and went negative here.
  BOOST_TEST((intersect_range(70000, 100, 65536, 65536) ==
              std::pair<int64_t, int64_t>{4464, 100}));

  // Range straddling the batch's end: clipped to the batch.
  BOOST_TEST(
      (intersect_range(90, 50, 0, 100) == std::pair<int64_t, int64_t>{90, 10}));

  // Range straddling the batch's start: clipped, and rebased to 0.
  BOOST_TEST(
      (intersect_range(50, 100, 100, 100) == std::pair<int64_t, int64_t>{0, 50}));

  // Range covering the whole batch.
  BOOST_TEST(
      (intersect_range(0, 1000, 100, 100) == std::pair<int64_t, int64_t>{0, 100}));

  // No overlap in either direction yields an empty, non-negative result.
  BOOST_TEST(
      (intersect_range(0, 10, 100, 100) == std::pair<int64_t, int64_t>{0, 0}));
  BOOST_TEST(
      (intersect_range(500, 10, 100, 100) == std::pair<int64_t, int64_t>{0, 0}));

  // Zero-length range never selects anything.
  BOOST_TEST((intersect_range(50, 0, 0, 100) == std::pair<int64_t, int64_t>{0, 0}));
}
