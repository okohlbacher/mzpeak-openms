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
//   index 2 -> 0.011218333333, index 5 -> 0.04862.
BOOST_AUTO_TEST_CASE(indices_in_time_range)
{
  using namespace MzPeak;

  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  // Inclusive sub-range [0.011218333333, 0.04862] -> exactly {2, 3, 4, 5}.
  auto sub = spectra.indices_in_time_range(0.011218333333, 0.04862);
  std::vector<std::size_t> expected{2, 3, 4, 5};
  BOOST_TEST(sub == expected, boost::test_tools::per_element());

  // Inclusive boundaries: endpoints are included.
  auto only_two = spectra.indices_in_time_range(0.011218333333, 0.011218333333);
  std::vector<std::size_t> just2{2};
  BOOST_TEST(only_two == just2, boost::test_tools::per_element());

  // Empty range: a gap between two adjacent spectrum times yields nothing.
  BOOST_TEST(spectra.indices_in_time_range(0.005, 0.006).empty());

  // Inverted range (low > high) yields nothing.
  BOOST_TEST(spectra.indices_in_time_range(0.05, 0.01).empty());

  // Full range covers every spectrum, in ascending order.
  auto all = spectra.indices_in_time_range(0.0, 1.0);
  BOOST_TEST(all.size() == spectra.size());
  BOOST_TEST(all.size() == 48u);
  for (std::size_t i = 0; i < all.size(); ++i) BOOST_TEST(all[i] == i);
}
