/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * What happens when a file LIES about being sorted.
 *
 * The read path binary-searches the entity index whenever the row group
 * declares that column sorted ascending (Parquet `sorting_columns`), because
 * verifying the claim per query costs exactly the O(n) scan the search exists
 * to avoid.  The declaration is therefore taken on trust -- and a third-party
 * writer could get it wrong.
 *
 * On unsorted input `lower_bound`/`upper_bound` do not fail; they return some
 * range.  The danger is not a crash, it is a plausible WRONG answer: peaks
 * belonging to another spectrum, with nothing to indicate anything went wrong.
 *
 * What actually saves the reader is an INDEPENDENT cross-check: after decoding,
 * Spectrum compares the number of points it got against the count the file
 * declares for that spectrum (number_of_data_points / number_of_peaks,
 * src/spectrum.cpp).  A mis-located run is almost always the wrong length, so
 * the read throws instead of returning fiction.
 *
 * These tests pin that.  The point is NOT that the binary search copes -- it
 * cannot -- but that a malformed file yields an ERROR rather than silent
 * corruption, which is the invariant that matters in this domain.  If someone
 * ever removes the count cross-check as redundant, this fails.
 *
 * Fixtures: declares_sorted_honest.dir and declares_sorted_lies.dir differ ONLY
 * in the order the spectrum groups are written (0,1,2,3 vs 0,2,1,3); both
 * declare the index column sorted.  See make_declares_sorted_fixtures.py.
 */

#define BOOST_TEST_MODULE SortingDeclaration
#include <boost/test/included/unit_test.hpp>

#include <vector>

#include "mzpeak/exception.h"
#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"

namespace {

constexpr const char* kHonest = "../test/files/declares_sorted_honest.dir";
constexpr const char* kLies = "../test/files/declares_sorted_lies.dir";

} // namespace

/******************************************************************************/
// Control: the honest file reads completely.  Without this, the test below
// could pass because the fixture is broken in some unrelated way.
BOOST_AUTO_TEST_CASE(honest_file_reads_every_spectrum)
{
  auto spectra = MzPeak::open(kHonest).spectra();
  BOOST_TEST_REQUIRE(spectra.size() == 4u);

  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto spectrum = spectra[i];
    BOOST_TEST(spectrum.mz().size() == 5u, "spectrum " << i);
    BOOST_TEST(spectrum.intensity().size() == 5u, "spectrum " << i);
  }
}

/******************************************************************************/
// The core invariant: on a file whose sorted declaration is false, every
// spectrum either reads back CORRECTLY or throws.  None comes back quietly
// wrong.
//
// Spectra 0 and 3 are unaffected by the reordering and must match the honest
// file value for value; 1 and 2 swapped places and must throw.
BOOST_AUTO_TEST_CASE(a_false_sorted_declaration_never_yields_silent_wrong_data)
{
  auto honest = MzPeak::open(kHonest).spectra();
  auto lies = MzPeak::open(kLies).spectra();

  BOOST_TEST_REQUIRE(lies.size() == honest.size());

  int threw = 0;
  int matched = 0;

  for (std::size_t i = 0; i < lies.size(); ++i) {
    // The truth for this spectrum, read from the honest file.
    const std::vector<double> expected_mz = honest[i].mz();

    try {
      auto spectrum = lies[i];
      const std::vector<double>& mz = spectrum.mz();

      // Read succeeded -- then it MUST be the right data.  This is the
      // assertion that would catch silent corruption.
      BOOST_TEST(mz == expected_mz, boost::test_tools::per_element());
      ++matched;
    } catch (const MzPeak::ParquetError&) {
      // Detected and refused: the acceptable outcome for a malformed file.
      ++threw;
    }
  }

  // Every spectrum is accounted for by one of the two acceptable outcomes.
  BOOST_TEST(threw + matched == static_cast<int>(lies.size()));

  // And the lie really is being exercised: the two displaced spectra are
  // caught.  If this drops to zero the fixture has stopped being a lie (e.g.
  // regenerated in sorted order) and the test above became vacuous.
  BOOST_TEST(threw == 2);
}

/******************************************************************************/
// The mechanism, pinned explicitly: the count cross-check is what turns a
// mis-located run into an error.  Named separately so that removing that check
// produces a failure that says why it mattered.
BOOST_AUTO_TEST_CASE(displaced_spectra_are_refused_by_the_declared_count_check)
{
  auto lies = MzPeak::open(kLies).spectra();

  for (std::size_t i : {1u, 2u}) {
    BOOST_CHECK_THROW(
        {
          auto spectrum = lies[i];
          (void)spectrum.mz();
        },
        MzPeak::ParquetError);
  }
}
