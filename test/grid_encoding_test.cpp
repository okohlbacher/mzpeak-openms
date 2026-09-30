/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * Coordinate grid encoding, `MS:1003826`.
 *
 * A grid-encoded chunk stores NO coordinate values: `mz_chunk_values` is null
 * and the coordinates are integer indices into a model whose parameters ride
 * in the same row, in a struct column `<array>_grid`.  Two consequences shape
 * these tests:
 *
 *   1. "refuses" and "silently returns an empty array" are both plausible
 *      outcomes for a reader that does not implement it, and only one is
 *      acceptable.  The decode is therefore asserted against VALUES, never
 *      merely against a count.
 *   2. The file carries `chunk_start`/`chunk_end` -- the writer's own
 *      evaluation of the same model at each chunk's first and last index -- so
 *      every row has an oracle.  The decoder checks against it on every row;
 *      `bounds_are_enforced` is what makes sure that check is real.
 *
 * Expected values below were computed from the fixture's model parameters with
 * pyarrow, independently of this reader.
 */
#define BOOST_TEST_MODULE GridEncoding
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "mzpeak/data/transformer/grid.h"
#include "mzpeak/exception.h"
#include "mzpeak/open.h"
#include "mzpeak/spectra.h"

namespace {

const char* kFixture = "../test/files/grid.dir";

MzPeak::Spectra peaks_of(const MzPeak::Index& index)
{
  return index.spectra(MzPeak::MetadataDetail::Full,
                       MzPeak::Index::SpectraSource::Peaks);
}

} // namespace

BOOST_AUTO_TEST_CASE(the_models_evaluate_as_the_reference_does)
{
  namespace Grid = MzPeak::Data::Transformer::Grid;

  // The two open models, against the reference implementation's own unit test.
  BOOST_TEST(*Grid::value_at("MS:1003824", {10.0, 0.5}, 4) == 12.0);
  BOOST_TEST(*Grid::value_at("MS:1003824", {10.0, 0.5, 2.0}, 4) == 6.0);
  BOOST_TEST(*Grid::value_at("MS:1003825", {1.0, 1.0}, 2) == 9.0);

  // A model this reader does not know, and a known model with the wrong number
  // of parameters, must both come back as "cannot", never as a guess.
  BOOST_TEST(!Grid::value_at("MS:1003825", {1.0}, 2).has_value());
  BOOST_TEST(!Grid::value_at("MS:1000000", {1.0, 1.0}, 2).has_value());
  BOOST_TEST(!Grid::value_at("MS:9999002", {1.0, 2.0, 3.0}, 7).has_value());
}

BOOST_AUTO_TEST_CASE(a_grid_encoded_archive_decodes_to_its_values)
{
  auto index = MzPeak::open(kFixture);
  auto spectra = peaks_of(index);
  BOOST_REQUIRE_EQUAL(spectra.size(), 2u);

  const auto s0 = spectra[0];
  const auto& mz = s0.mz();
  const auto& mobility = s0.ion_mobility_array();

  // Not just "non-empty": the exact count the 34 chunk rows of this frame hold.
  BOOST_REQUIRE_EQUAL(mz.size(), 9365u);
  BOOST_REQUIRE_EQUAL(mobility.size(), mz.size());
  BOOST_REQUIRE_EQUAL(s0.intensity().size(), mz.size());

  // m/z: MS:9999002, seven parameters, indices delta-coded because m/z is the
  // main axis.  A reader that forgot to accumulate the deltas would produce a
  // plausible-looking ascending array that is wrong from the second point on,
  // so the second and last values matter as much as the first.
  BOOST_TEST(mz[0] == 95.1085459409012, boost::test_tools::tolerance(1e-12));
  BOOST_TEST(mz[1] == 95.50575242076872, boost::test_tools::tolerance(1e-12));
  BOOST_TEST(mz.back() == 1703.9464397733252, boost::test_tools::tolerance(1e-10));

  // Ion mobility: MS:9999001, four parameters, indices ABSOLUTE because it is a
  // secondary axis.  Delta-accumulating these would run the scan numbers away
  // to nonsense, so this pins the main/secondary distinction.
  BOOST_TEST(mobility[0] == 1.363232213510821, boost::test_tools::tolerance(1e-13));
  BOOST_TEST(mobility[1] == 1.5209798315812844, boost::test_tools::tolerance(1e-13));

  const auto s1 = spectra[1];
  BOOST_REQUIRE_EQUAL(s1.mz().size(), 10566u);
  BOOST_TEST(s1.mz()[0] == 95.14729914189232, boost::test_tools::tolerance(1e-12));
  BOOST_TEST(s1.ion_mobility_array()[0] == 0.9297464058254469,
             boost::test_tools::tolerance(1e-13));

  // The second frame is a different window: not one row echoed twice.
  BOOST_TEST(s1.mz()[0] != mz[0]);
}

BOOST_AUTO_TEST_CASE(mobility_stays_in_the_physical_range)
{
  // Inverse reduced ion mobility on a timsTOF is ~0.6-1.6 Vs/cm^2.  A model
  // evaluated with a transposed parameter or an unaccumulated index still
  // yields finite numbers, just not ones a mass spectrometer could produce.
  auto index = MzPeak::open(kFixture);
  auto spectra = peaks_of(index);

  for (std::size_t i = 0; i < spectra.size(); ++i) {
    const auto s = spectra[i];
    const auto& mobility = s.ion_mobility_array();
    BOOST_REQUIRE(!mobility.empty());
    const auto [lo, hi] = std::ranges::minmax_element(mobility);
    BOOST_TEST(*lo > 0.5, "spectrum " << i << " minimum mobility " << *lo);
    BOOST_TEST(*hi < 2.0, "spectrum " << i << " maximum mobility " << *hi);
  }
}

BOOST_AUTO_TEST_CASE(mz_is_ascending_within_every_chunk_run)
{
  // The main axis is declared sorted, and the delta accumulation is the step
  // most likely to break it.
  auto index = MzPeak::open(kFixture);
  auto spectra = peaks_of(index);
  const auto s0 = spectra[0];
  const auto& mz = s0.mz();
  BOOST_TEST(
      std::ranges::is_sorted(mz),
      "decoded m/z is not non-decreasing, so the delta accumulation is wrong");
}

BOOST_AUTO_TEST_CASE(bounds_are_enforced)
{
  // `grid_bad_bounds.dir` is the good fixture with ONE chunk's `mz_chunk_start`
  // moved by 1 Th -- far outside any rounding, far inside the plausible range.
  // A reader that evaluates the model but never compares it against the bound
  // the writer recorded reads this as perfectly good data, which is exactly how
  // a mis-ported model would present: plausible numbers, quietly wrong. The
  // decode must refuse, and the message must name the chunk and both values,
  // because "grid decode failed" would leave nobody able to tell a corrupted
  // file from a reader bug.
  auto index = MzPeak::open("../test/files/grid_bad_bounds.dir");
  auto spectra = peaks_of(index);
  BOOST_REQUIRE_EQUAL(spectra.size(), 2u);

  bool refused = false;
  try {
    const auto s0 = spectra[0];
    (void)s0.mz();
  } catch (const MzPeak::InvalidFormatError& e) {
    refused = true;
    const std::string what(e.what());
    BOOST_TEST(what.find("chunk_start") != std::string::npos, "message: " << what);
    BOOST_TEST(what.find("chunk 0") != std::string::npos, "message: " << what);
  }
  BOOST_TEST(refused, "a contradicted chunk bound was accepted");

  // The good fixture must still decode, so the check is not simply always-on.
  auto good = MzPeak::open(kFixture);
  BOOST_CHECK_NO_THROW((void)peaks_of(good)[0].mz());
}

BOOST_AUTO_TEST_CASE(the_legacy_mobility_parameter_order_reads_the_same)
{
  namespace Grid = MzPeak::Data::Transformer::Grid;

  // `MS:9999001` is `[c6, c7, offset, slope]`, but the reference writer emitted
  // `[c6, c7, slope, offset]` until its `eb08ba0`, and nothing in such a file
  // says so. Read literally the old spelling gives ~45 Vs/cm^2 for a 1/K0 of
  // ~1.05. Both spellings must evaluate identically, at every scan.
  const std::vector<double> current = {0.020932469715718497, 131.22279563838268,
                                       222.46486824959476, -0.1539079919581615};
  const std::vector<double> legacy = {current[0], current[1], current[3],
                                      current[2]};

  for (std::uint32_t scan : {33u, 100u, 481u, 533u, 926u}) {
    const auto a = Grid::value_at("MS:9999001", current, scan);
    const auto b = Grid::value_at("MS:9999001", legacy, scan);
    BOOST_REQUIRE(a.has_value());
    BOOST_REQUIRE(b.has_value());
    BOOST_TEST(*a == *b, "scan " << scan << ": " << *a << " vs " << *b);
    BOOST_TEST(*a > 0.5);
    BOOST_TEST(*a < 2.0);
  }
}

BOOST_AUTO_TEST_CASE(a_legacy_archive_decodes_to_the_same_values)
{
  // `grid_legacy.dir` is grid.dir as an archive written before the reference's
  // two writer fixes: the mobility pair in the old order on every row, and
  // chunk_end = 0.0 on the last chunk of each spectrum. That is exactly the
  // shape of the real diaPASEF conversion that aborted FASTag. It has to decode
  // to precisely what the conforming fixture decodes to -- not "close", equal,
  // because both are the same model evaluated at the same indices.
  auto good_index = MzPeak::open(kFixture);
  auto old_index = MzPeak::open("../test/files/grid_legacy.dir");
  auto good = peaks_of(good_index);
  auto old = peaks_of(old_index);
  BOOST_REQUIRE_EQUAL(old.size(), good.size());

  for (std::size_t i = 0; i < good.size(); ++i) {
    const auto g = good[i];
    const auto o = old[i];
    BOOST_REQUIRE_EQUAL(o.mz().size(), g.mz().size());
    BOOST_REQUIRE_EQUAL(o.ion_mobility_array().size(),
                        g.ion_mobility_array().size());
    BOOST_TEST(o.mz() == g.mz(), "spectrum " << i << ": m/z differs");
    BOOST_TEST(o.ion_mobility_array() == g.ion_mobility_array(),
               "spectrum " << i << ": ion mobility differs");
    BOOST_TEST(o.intensity() == g.intensity(),
               "spectrum " << i << ": intensity differs");
  }
}

BOOST_AUTO_TEST_CASE(a_wrong_nonzero_chunk_end_is_still_refused)
{
  // Tolerating a ZERO chunk_end must not tolerate a WRONG one. `grid_bad_end.dir`
  // moves chunk_end[0] by 1 Th and nothing else.
  auto index = MzPeak::open("../test/files/grid_bad_end.dir");
  auto spectra = peaks_of(index);
  bool refused = false;
  try {
    (void)spectra[0].mz();
  } catch (const MzPeak::InvalidFormatError& e) {
    refused = true;
    BOOST_TEST(std::string(e.what()).find("chunk_end") != std::string::npos,
               "message: " << e.what());
  }
  BOOST_TEST(refused, "a contradicted chunk_end was accepted");
}

BOOST_AUTO_TEST_CASE(unphysical_mobility_is_refused_not_returned)
{
  // The offset/slope pair is disambiguated by magnitude, which is an inference,
  // so its output is checked rather than trusted. `grid_bad_mobility.dir` scales
  // c6 so that 1/K0 comes out near 0.05 Vs/cm^2 whichever way the pair is read:
  // no reading rescues it, and returning it would be the silent failure this
  // check exists to stop.
  auto index = MzPeak::open("../test/files/grid_bad_mobility.dir");
  auto spectra = peaks_of(index);
  bool refused = false;
  try {
    (void)spectra[0].ion_mobility_array();
  } catch (const MzPeak::InvalidFormatError& e) {
    refused = true;
    BOOST_TEST(std::string(e.what()).find("physically possible") !=
                   std::string::npos,
               "message: " << e.what());
  }
  BOOST_TEST(refused, "an unphysical 1/K0 was returned");
}

BOOST_AUTO_TEST_CASE(the_fixture_verifies_its_own_checksums)
{
  // The fixture was trimmed, so its digests were recomputed; this keeps that
  // step from being silently dropped.
  auto index = MzPeak::open(kFixture);
  const MzPeak::ChecksumReport report = index.verify_checksums();
  BOOST_TEST(report.ok());
  BOOST_TEST(report.verified >= 5u);
}
