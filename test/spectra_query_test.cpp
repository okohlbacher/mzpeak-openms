/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE SpectraQuery
#include <boost/test/included/unit_test.hpp>

#include "mzpeak/exception.h"
#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"

/******************************************************************************/
// RDR-15: random access by native spectrum id.  Ground truth from
// spectra_metadata.parquet via pyarrow — spectrum 0 has id
// "controllerType=0 controllerNumber=1 scan=1" and 13589 m/z points.
BOOST_AUTO_TEST_CASE(by_id_resolves_and_reads)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  const std::string id0 = "controllerType=0 controllerNumber=1 scan=1";
  auto idx = spectra.index_for_id(id0);
  BOOST_TEST(idx.has_value());
  BOOST_TEST(idx.value() == 0u);

  auto s0 = spectra.by_id(id0);
  BOOST_TEST(s0.mz().size() == 13589);

  // A different known id maps to its own index.
  auto idx3 = spectra.index_for_id("controllerType=0 controllerNumber=1 scan=4");
  BOOST_TEST(idx3.has_value());
  BOOST_TEST(idx3.value() == 3u);
}

/******************************************************************************/
// RDR-15: unknown ids return nullopt from index_for_id and throw from by_id.
BOOST_AUTO_TEST_CASE(by_id_unknown_id)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  BOOST_TEST(!spectra.index_for_id("no such spectrum").has_value());
  BOOST_CHECK_THROW(spectra.by_id("no such spectrum"), ParquetError);
}

/******************************************************************************/
// RDR-16: retention-time range query.  Ground truth (time, index) from
// pyarrow; times are strictly increasing with index in small.mzpeak.
//
// The file stores MINUTES (UO:0000031) and the API returns SECONDS, so every
// bound here is the stored value x 60 -- index 2 is 0.011218333333 min =
// 0.6731 s, index 5 is 0.04862 min = 2.9172 s.  Writing these in minutes is
// what a missing conversion looks like, so the numbers are deliberately the
// converted ones.
BOOST_AUTO_TEST_CASE(indices_in_time_range)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  // The exact endpoints come from the API itself, so the boundary cases below
  // test INCLUSIVITY rather than the reproducibility of a hand-typed decimal.
  const double t2 = spectra[2].metadata().retention_time.value();
  const double t5 = spectra[5].metadata().retention_time.value();

  // ...but they are still pinned to the stored value, so a dropped or doubled
  // minutes->seconds conversion fails here rather than sliding through: the
  // file stores 0.011218333333 min for index 2 and 0.04862 min for index 5.
  BOOST_TEST(t2 == 0.011218333333 * 60.0, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(t5 == 0.04862 * 60.0, boost::test_tools::tolerance(1e-9));

  // Inclusive sub-range [t2, t5] -> exactly {2, 3, 4, 5}.
  auto sub = spectra.indices_in_time_range(t2, t5);
  std::vector<std::size_t> expected{2, 3, 4, 5};
  BOOST_TEST(sub == expected, boost::test_tools::per_element());

  // Inclusive boundaries: endpoints are included.
  auto only_two = spectra.indices_in_time_range(t2, t2);
  std::vector<std::size_t> just2{2};
  BOOST_TEST(only_two == just2, boost::test_tools::per_element());

  // Empty range: a gap between two adjacent spectrum times yields nothing.
  // Indices 0 and 1 sit at 0.2961 s and 0.4738 s.
  BOOST_TEST(spectra.indices_in_time_range(0.35, 0.40).empty());

  // An inverted range is NORMALISED, not rejected: indices_in_time_range swaps
  // the bounds, so [t5, t2] returns the same window as [t2, t5].  (An earlier
  // revision of this test expected empty; the reader swaps instead, and that
  // is the shipped contract.)
  auto inverted = spectra.indices_in_time_range(t5, t2);
  BOOST_TEST(inverted == expected, boost::test_tools::per_element());

  // Full range covers every spectrum, in ascending order.
  auto all = spectra.indices_in_time_range(0.0, 60.0);
  BOOST_TEST(all.size() == spectra.size());
  BOOST_TEST(all.size() == 48u);
  for (std::size_t i = 0; i < all.size(); ++i)
    BOOST_TEST(all[i] == i);
}
