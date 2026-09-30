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
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <arrow/builder.h>

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

/// One row of an `<array>_grid` struct column, built by hand, so that the
/// bound check can be driven with rows no bundled archive contains.
MzPeak::Util::Slice::Raw grid_row(const char* grid_type,
                                  const std::vector<double>& parameters,
                                  const std::vector<std::uint32_t>& indices)
{
  arrow::StringBuilder type_builder;
  BOOST_REQUIRE(type_builder.Append(grid_type).ok());

  arrow::ListBuilder p_builder(arrow::default_memory_pool(),
                               std::make_shared<arrow::DoubleBuilder>());
  BOOST_REQUIRE(p_builder.Append().ok());
  BOOST_REQUIRE(static_cast<arrow::DoubleBuilder*>(p_builder.value_builder())
                    ->AppendValues(parameters)
                    .ok());

  arrow::ListBuilder i_builder(arrow::default_memory_pool(),
                               std::make_shared<arrow::UInt32Builder>());
  BOOST_REQUIRE(i_builder.Append().ok());
  BOOST_REQUIRE(static_cast<arrow::UInt32Builder*>(i_builder.value_builder())
                    ->AppendValues(indices)
                    .ok());

  std::shared_ptr<arrow::Array> type, p, i;
  BOOST_REQUIRE(type_builder.Finish(&type).ok());
  BOOST_REQUIRE(p_builder.Finish(&p).ok());
  BOOST_REQUIRE(i_builder.Finish(&i).ok());

  auto row = arrow::StructArray::Make({type, p, i},
                                      std::vector<std::string>{
                                          "grid_type", "parameters", "indices"});
  BOOST_REQUIRE(row.ok());
  return MzPeak::Util::Slice::Raw{*row};
}

/// Mobility scans of the bundled acquisition and the 1/K0 they stand for,
/// read off the archive that writes its coordinates out -- physical Vs/cm^2,
/// not this reader's own output.
struct ScanMobility {
  std::uint32_t scan;
  double one_over_k0;
};

constexpr ScanMobility kKnownMobilities[] = {
    {33, 1.6010956012790702},
    {250, 1.3621252945398825},
    {400, 1.1954873123229730},
    {533, 1.0467308802733821},
    {926, 0.60156745661641153},
};

/// The bundled acquisition's TimsCalibration row, in the order the reference
/// settled on: `[C6, C7, offset, slope]`.
const std::vector<double> kMobilityModel = {0.020932469715718494,
                                            131.22279563838268, 222.46486824959476,
                                            -0.1539079919581615};

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

BOOST_AUTO_TEST_CASE(the_mobility_model_lands_on_physical_one_over_k0)
{
  namespace Grid = MzPeak::Data::Transformer::Grid;

  // Equality, not a tolerance: a grid index IS the native representation on a
  // timsTOF, so the decode must land on the value the written-out archive of
  // the same acquisition holds, bit for bit.  `mobility_stays_in_the_physical
  // _range` only asks for plausible; these are the numbers themselves.
  for (const auto& [scan, one_over_k0] : kKnownMobilities) {
    BOOST_TEST_CONTEXT("scan " << scan)
    {
      BOOST_TEST(*Grid::value_at("MS:9999001", kMobilityModel, scan) == one_over_k0);
    }
  }

  // Extrapolated to scan 0, i.e. the full ramp voltage.
  BOOST_TEST(*Grid::value_at("MS:9999001", kMobilityModel, 0) == 1.637221297889762);
}

BOOST_AUTO_TEST_CASE(the_mobility_model_reads_the_pre_eb08ba0_parameter_order)
{
  namespace Grid = MzPeak::Data::Transformer::Grid;

  // The reference WRITER emitted `[C6, C7, slope, offset]` until its eb08ba0,
  // while mzdata's `as_param` and the reference's own `from_parameters` use
  // `[C6, C7, offset, slope]`.  Archives written before that fix are in the
  // wild -- mzdata's own diaPASEF.d, converted 2026-09-22 -- and nothing in
  // the file says which spelling it used.  Read the wrong way the ramp
  // collapses from 222 V to 0.15 V and every mobility comes back near 1/C6,
  // about 45 Vs/cm^2 where a timsTOF holds 0.6 - 1.6.
  //
  // The offset is the larger of the two by a factor of the scan count, which
  // is how they are told apart; a file in either order must decode the same.
  std::vector<double> swapped = kMobilityModel;
  std::swap(swapped[2], swapped[3]);

  for (const auto& [scan, one_over_k0] : kKnownMobilities) {
    BOOST_TEST_CONTEXT("scan " << scan)
    {
      BOOST_TEST(*Grid::value_at("MS:9999001", swapped, scan) == one_over_k0);
    }
  }

  // The ramp falls with scan; reading the pair the other way round inverts
  // that as well as the magnitude.
  BOOST_TEST(*Grid::value_at("MS:9999001", swapped, 33) >
             *Grid::value_at("MS:9999001", swapped, 926));
}

BOOST_AUTO_TEST_CASE(a_one_point_chunk_keeps_its_unset_end_bound)
{
  namespace Grid = MzPeak::Data::Transformer::Grid;

  // The reference writer read `chunk_start` and `chunk_end` from ONE iterator
  // -- `next()`, then `next_back()` on what remained -- so a chunk that
  // reduces to a single point left the end at its 0.0 default.  Two chunks of
  // the reference's own diaPASEF conversion are like that, `chunk_start` near
  // 1700 Th.  Refusing them would reject archives the reference reads, so a
  // 0.0 end below a positive start is taken as unset rather than as a
  // contradiction -- but only for a chunk that really does hold one point,
  // and `chunk_start` is still checked.
  const auto raw = grid_row("MS:1003824", {10.0, 0.5}, {4});

  Grid::Bounds unset;
  unset.start = {12.0};
  unset.end = {0.0};

  std::vector<double> out;
  BOOST_CHECK_NO_THROW(
      Grid::decode<double>(raw, true, &unset, "mz_chunk_start", out));
  BOOST_REQUIRE_EQUAL(out.size(), 1u);
  BOOST_TEST(out[0] == 12.0);

  // Still checked: a start that disagrees is a refusal on the same row.
  Grid::Bounds wrong_start;
  wrong_start.start = {11.0};
  wrong_start.end = {0.0};
  out.clear();
  BOOST_CHECK_THROW(
      Grid::decode<double>(raw, true, &wrong_start, "mz_chunk_start", out),
      MzPeak::InvalidFormatError);

  // And an end that is simply wrong rather than unset is still a refusal.
  Grid::Bounds wrong_end;
  wrong_end.start = {12.0};
  wrong_end.end = {11.0};
  out.clear();
  BOOST_CHECK_THROW(
      Grid::decode<double>(raw, true, &wrong_end, "mz_chunk_start", out),
      MzPeak::InvalidFormatError);

  // A MULTI-point chunk gets no such licence: 0.0 there is a contradiction,
  // not the writer's default, and letting it pass would disarm the check that
  // `bounds_are_enforced` exists for.
  const auto pair = grid_row("MS:1003824", {10.0, 0.5}, {4, 2});
  Grid::Bounds zero_end;
  zero_end.start = {12.0};
  zero_end.end = {0.0};
  out.clear();
  BOOST_CHECK_THROW(
      Grid::decode<double>(pair, true, &zero_end, "mz_chunk_start", out),
      MzPeak::InvalidFormatError);
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
