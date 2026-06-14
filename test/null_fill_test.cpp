/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE NullFill
#include <boost/test/included/unit_test.hpp>

#include <vector>

#include "mzpeak/util/null_fill.h"

/******************************************************************************/
// Validate the reconstruction against the Rust reference reader's output for
// the first two profile segments of small.mzpeak spectrum 0: seven real values,
// two null-marked points, five real values.  The reference reconstructs the
// two nulls as (last_of_segment1 + delta1) and (first_of_segment2 - delta2).
BOOST_AUTO_TEST_CASE(matches_reference_reconstruction)
{
  using namespace MzPeak::Util;

  // Rust read_spectrum m/z for spectrum 0, positions 0..13 (the reconstructed
  // values at positions 7 and 8 are what we must reproduce).
  const std::vector<double> expected{
      202.60657495520474, 202.60682348271374, 202.60707201083247,
      202.607320539561,   202.60756906889915, 202.60781759884705,
      202.60806612940473, 202.60831465843808, // <- reconstructed
      204.75933490116418,                     // <- reconstructed
      204.75958873936264, 204.7598425769317,  204.76009641513016,
      204.76035025395797, 204.76060409341508};

  // The stored, null-marked array: the real values are `expected` except the
  // two null slots (7, 8) carry no value.
  std::vector<double> values(expected);
  std::vector<bool> valid(expected.size(), true);
  values[7] = 0.0;
  values[8] = 0.0;
  valid[7] = false;
  valid[8] = false;

  // spec 0 mz_delta_model (unused here — both segments have >1 value).
  const std::vector<double> beta{-2.2113927547804747e-08, 9.697546039425898e-11,
                                 6.054228626999033e-09};

  std::vector<double> out(reconstruct_null_mz(values, valid, beta));

  BOOST_TEST(out.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    BOOST_TEST(out[i] == expected[i], boost::test_tools::tolerance(1e-9));
  }
}

/******************************************************************************/
// A single-value run uses the delta MODEL (predict) rather than a local median.
// Nulls are PAIRED between runs (the spec's interior layout), so the length is
// preserved: [a0 a1 | null null | X | null null | b0 b1].
BOOST_AUTO_TEST_CASE(single_value_run_uses_model)
{
  using namespace MzPeak::Util;

  std::vector<double> values{100.0, 100.2, 0.0, 0.0, 500.0, 0.0, 0.0, 900.0, 900.3};
  std::vector<bool> valid{true, true, false, false, true, false, false, true, true};
  const std::vector<double> beta{0.25}; // constant model: predict() = 0.25

  std::vector<double> out(reconstruct_null_mz(values, valid, beta));

  const std::vector<double> expected{
      100.0,  100.2, 100.4,  // run a + its trailing delta (0.2)
      499.75, 500.0, 500.25, // singleton X +/- predict()=0.25
      899.7,  900.0, 900.3}; // run b leading delta (0.3) + values
  BOOST_TEST(out.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    BOOST_TEST(out[i] == expected[i], boost::test_tools::tolerance(1e-9));
  }
}

/******************************************************************************/
// predict evaluates the polynomial; estimate_median_delta is the median below
// the median of consecutive deltas.
BOOST_AUTO_TEST_CASE(predict_and_median_helpers)
{
  using namespace MzPeak::Util;

  BOOST_TEST(predict_delta({1.0, 2.0, 3.0}, 10.0) == 321.0,
             boost::test_tools::tolerance(1e-12)); // 1 + 20 + 300

  // deltas of {0,1,3,6,10} = {1,2,3,4}; median (upper pair) = (3+4)/2 = 3.5;
  // keep <=3.5 -> {1,2,3}; median = 2.
  std::vector<double> v{0.0, 1.0, 3.0, 6.0, 10.0};
  BOOST_TEST(estimate_median_delta(v, 0, v.size()) == 2.0,
             boost::test_tools::tolerance(1e-12));
}
