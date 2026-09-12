/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * A `term_marker` column states that a CV term APPLIES to a row rather than
 * carrying a value, in the two shapes the specification defines: a boolean
 * column naming the mapping's own accession, and a string column whose value is
 * the CURIE of a child of it.
 *
 * These are sibling COLUMNS of `parameters`, not entries inside it, so a reader
 * that only walks the list sees nothing at all -- which is what this one did
 * until the fixture below existed.  No archive from this project's writer or
 * from the reference writer carries such a column, so `term_markers.dir` is
 * built by test/files/make_term_marker_fixture.py:
 *
 *   row 0   calibration True    dissociation MS:1000422
 *   row 1   calibration False   dissociation MS:1000598
 *   row 2   calibration null    dissociation null
 *   row 3   calibration False   dissociation ""
 *
 * It also flags the STANDARDISED spectrum_representation mapping (MS:1000525),
 * which is already surfaced as a typed field, to pin that it is not reported a
 * second time through `parameters`.
 */
#define BOOST_TEST_MODULE TermMarker
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"
#include "mzpeak/spectrum_metadata.h"

namespace {

const char* kFixture = "../test/files/term_markers.dir";

/// Accessions reported through a spectrum's `parameters` list.
std::vector<std::string> accessions(const MzPeak::SpectrumMetadata& m)
{
  std::vector<std::string> out;
  for (const auto& p : m.parameters) {
    if (p.accession) out.push_back(*p.accession);
  }
  return out;
}

bool has(const std::vector<std::string>& v, std::string_view a)
{
  return std::ranges::find(v, a) != v.end();
}

} // namespace

BOOST_AUTO_TEST_CASE(boolean_marker_reported_only_where_true)
{
  auto index = MzPeak::open(kFixture);
  auto spectra = index.spectra();
  BOOST_REQUIRE_EQUAL(spectra.size(), 4u);

  const auto s0 = spectra[0];
  BOOST_TEST(has(accessions(s0.metadata()), "MS:1000928"));

  // false, null and false respectively -- none of them means "present".
  for (std::size_t i : {1u, 2u, 3u}) {
    const auto s = spectra[i];
    BOOST_TEST(!has(accessions(s.metadata()), "MS:1000928"),
               "spectrum " << i << " must not carry the calibration term");
  }
}

BOOST_AUTO_TEST_CASE(string_marker_reports_the_child_accession)
{
  auto index = MzPeak::open(kFixture);
  auto spectra = index.spectra();

  // The column is mapped to MS:1000044 (dissociation method); what the row
  // carries is the CHILD, and that is what must reach the caller.
  const auto s0 = spectra[0];
  BOOST_TEST(has(accessions(s0.metadata()), "MS:1000422"));
  BOOST_TEST(!has(accessions(s0.metadata()), "MS:1000044"));

  const auto s1 = spectra[1];
  BOOST_TEST(has(accessions(s1.metadata()), "MS:1000598"));
}

BOOST_AUTO_TEST_CASE(null_and_empty_markers_contribute_nothing)
{
  auto index = MzPeak::open(kFixture);
  auto spectra = index.spectra();

  const auto s2 = spectra[2];
  BOOST_TEST(!has(accessions(s2.metadata()), "MS:1000422"));
  BOOST_TEST(!has(accessions(s2.metadata()), "MS:1000598"));

  // An empty CURIE is not a term; it must not arrive as a nameless parameter.
  const auto s3 = spectra[3];
  for (const auto& a : accessions(s3.metadata())) {
    BOOST_TEST(!a.empty(), "an empty accession reached the parameter list");
  }
}

BOOST_AUTO_TEST_CASE(typed_term_is_not_reported_twice)
{
  auto index = MzPeak::open(kFixture);
  auto spectra = index.spectra();

  // spectrum_representation is flagged term_marker in this fixture AND is
  // surfaced as a typed field, so appending it here would double-report it.
  const auto s0 = spectra[0];
  BOOST_TEST(!has(accessions(s0.metadata()), "MS:1000525"));
  BOOST_TEST(!s0.metadata().representation.empty());
}
