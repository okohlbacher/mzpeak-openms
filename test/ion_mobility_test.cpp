/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * The first coverage of this reader's ion-mobility and activation paths
 * against REAL vendor data.
 *
 * Until `test/files/diapasef.dir` existed, both were pinned only by hand-built
 * fixtures and by how they behaved when the data was absent -- the README said
 * so explicitly, and it was true for months for want of a single file. The
 * fixture is two MS2 frames from a Bruker diaPASEF acquisition, converted by
 * the reference writer and trimmed by test/files/make_diapasef_fixture.py.
 *
 * Every expected value below was read out of the Parquet with pyarrow before
 * this reader was pointed at it, so the assertions are ground truth rather
 * than this reader's own output written back as a expectation.
 */
#define BOOST_TEST_MODULE IonMobility
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <string>

#include "mzpeak/open.h"
#include "mzpeak/spectra.h"

namespace {

const char* kFixture = "../test/files/diapasef.dir";

/// The CvParam carrying @p accession, or nullptr.
const MzPeak::CvParam* param_with(const std::vector<MzPeak::CvParam>& params,
                                  std::string_view accession)
{
  for (const auto& p : params) {
    if (p.accession && *p.accession == accession) return &p;
  }
  return nullptr;
}

} // namespace

BOOST_AUTO_TEST_CASE(per_peak_mobility_is_read_and_parallel_to_mz)
{
  auto index = MzPeak::open(kFixture);
  auto spectra = index.spectra();
  BOOST_REQUIRE_EQUAL(spectra.size(), 2u);

  const auto s0 = spectra[0];
  const auto& mz = s0.mz();
  const auto& mobility = s0.ion_mobility_array();

  // Parallel is the property that matters: a mobility array shorter than the
  // m/z array would silently mis-pair every peak past the truncation.
  BOOST_REQUIRE_EQUAL(mz.size(), 8377u);
  BOOST_REQUIRE_EQUAL(mobility.size(), mz.size());

  BOOST_TEST(mz[0] == 95.1085459409, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(mobility[0] == 1.3632322135, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(mobility[1] == 1.5209798316, boost::test_tools::tolerance(1e-9));

  // Inverse reduced ion mobility for a timsTOF sits around 0.6-1.6 Vs/cm^2.
  // A unit or scaling error would leave the values in range-of-nothing.
  const auto [lo, hi] = std::ranges::minmax_element(mobility);
  BOOST_TEST(*lo > 0.5);
  BOOST_TEST(*hi < 2.0);

  const auto s1 = spectra[1];
  BOOST_REQUIRE_EQUAL(s1.ion_mobility_array().size(), s1.mz().size());
  BOOST_REQUIRE_EQUAL(s1.mz().size(), 960u);
}

BOOST_AUTO_TEST_CASE(the_selected_ion_carries_its_mobility_value)
{
  auto index = MzPeak::open(kFixture);
  auto spectra = index.spectra();

  const auto s0 = spectra[0];
  const auto& meta = s0.metadata();
  BOOST_TEST(meta.ms_level.value_or(-1) == 2);
  BOOST_REQUIRE_EQUAL(meta.precursors.size(), 1u);

  const auto& precursor = meta.precursors.front();
  BOOST_REQUIRE_EQUAL(precursor.selected_ions.size(), 1u);
  const auto& ion = precursor.selected_ions.front();

  BOOST_REQUIRE(ion.ion_mobility_value.has_value());
  BOOST_TEST(*ion.ion_mobility_value == 1.2517460582524271,
             boost::test_tools::tolerance(1e-12));
  BOOST_REQUIRE(ion.ion_mobility_type.has_value());
  BOOST_TEST(*ion.ion_mobility_type == "MS:1002815"); // inverse reduced IM

  // This acquisition records a mobility VALUE and no window limits, so the
  // limits must read as absent rather than as a default.  Asserting this keeps
  // a future "helpfully" invented bound from passing unnoticed.
  BOOST_TEST(!ion.ion_mobility_lower_limit.has_value());
  BOOST_TEST(!ion.ion_mobility_upper_limit.has_value());

  BOOST_REQUIRE(precursor.isolation_window.target_mz.has_value());
  BOOST_TEST(*precursor.isolation_window.target_mz == 813.0f);
  BOOST_TEST(*precursor.isolation_window.lower_offset == 13.0f);
  BOOST_TEST(*precursor.isolation_window.upper_offset == 13.0f);
}

BOOST_AUTO_TEST_CASE(activation_promoted_to_columns_is_not_lost)
{
  // The activation facet is the one this reader gives no typed fields, so a
  // writer that promotes dissociation method and collision energy out of
  // `parameters` and into columns -- as the reference does here -- used to
  // leave this reader reporting an empty activation.
  auto index = MzPeak::open(kFixture);
  auto spectra = index.spectra();

  const auto s0 = spectra[0];
  const auto& activation = s0.metadata().precursors.front().activation_parameters;
  BOOST_REQUIRE_EQUAL(activation.size(), 2u);

  // dissociation_method is mapped to MS:1000044 but HOLDS the child term, and
  // the child is what applies to the row.  Reporting the parent instead would
  // say "some dissociation happened" and lose which.
  const MzPeak::CvParam* method = param_with(activation, "MS:1000133");
  BOOST_REQUIRE_MESSAGE(method != nullptr,
                        "collision-induced dissociation (MS:1000133) not reported");
  BOOST_TEST(!param_with(activation, "MS:1000044"));

  const MzPeak::CvParam* energy = param_with(activation, "MS:1000045");
  BOOST_REQUIRE_MESSAGE(energy != nullptr, "collision energy not reported");
  BOOST_REQUIRE(energy->value.has_value());
  BOOST_TEST(std::stod(*energy->value) == 41.572227,
             boost::test_tools::tolerance(1e-5));
  // The unit comes from the column mapping; without it an energy in electron
  // volts is indistinguishable from one in anything else.
  BOOST_REQUIRE(energy->unit.has_value());
  BOOST_TEST(*energy->unit == "UO:0000266");

  // The second frame is a different window, so this is not one row echoed.
  const auto s1 = spectra[1];
  const auto& other = s1.metadata().precursors.front().activation_parameters;
  const MzPeak::CvParam* other_energy = param_with(other, "MS:1000045");
  BOOST_REQUIRE(other_energy != nullptr);
  BOOST_TEST(std::stod(*other_energy->value) != std::stod(*energy->value));
}

BOOST_AUTO_TEST_CASE(the_fixture_verifies_its_own_checksums)
{
  // The fixture was trimmed from a larger archive, so its digests were
  // recomputed. If that step were ever dropped the fixture would still read
  // correctly while failing the verification this project now ships.
  auto index = MzPeak::open(kFixture);
  const MzPeak::ChecksumReport report = index.verify_checksums();
  BOOST_TEST(report.ok());
  BOOST_TEST(report.verified >= 5u);
  BOOST_TEST(report.unchecked == 0u);
}
