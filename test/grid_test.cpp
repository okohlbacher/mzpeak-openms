/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * PSI-MS "coordinate grid encoding" (MS:1003826).
 *
 * A grid-encoded chunk does not store its main-axis coordinates at all: it
 * stores integer indices into a grid, plus the model that maps them back.  A
 * secondary axis (ion mobility) can be stored the same way.  This reader used
 * to refuse such files outright -- Signals::select threw "array entry not
 * present in schema: mz_grid", because a `chunk_transform` STRUCT column is not
 * a leaf and nothing registered it.
 *
 * The first test below is the one that matters, and it is an ORACLE test rather
 * than a table of numbers: `grid_diapasef.dir` and `diapasef.dir` are the same
 * two MS2 spectra of the same acquisition, converted once with grid encoding
 * and once with the coordinates written out.  Decoding the first must reproduce
 * the second exactly -- for a timsTOF the grid index IS the native
 * representation, so the round trip is lossless and "close enough" is the wrong
 * bar.  Both fixtures come from one converter run; see make_grid_fixture.py.
 *
 * The rest pin the grid models themselves, including the arithmetic that has to
 * stay bit-exact and the refusal that has to name an unknown model.
 */
#define BOOST_TEST_MODULE Grid
#include <boost/test/included/unit_test.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "mzpeak/data/grid.h"
#include "mzpeak/exception.h"
#include "mzpeak/open.h"
#include "mzpeak/spectra.h"

namespace {

const char* kGrid = "../test/files/grid_diapasef.dir";
const char* kMixed = "../test/files/mixed_grid_diapasef.dir";
const char* kReference = "../test/files/diapasef.dir";

/// The largest absolute and relative gap between two arrays.
template <typename T>
std::pair<double, double> deviation(const std::vector<T>& got,
                                    const std::vector<T>& want)
{
  double abs_max = 0.0;
  double rel_max = 0.0;

  for (std::size_t i = 0; i < got.size() && i < want.size(); ++i) {
    const double a = static_cast<double>(got[i]);
    const double b = static_cast<double>(want[i]);
    const double d = std::abs(a - b);

    abs_max = std::max(abs_max, d);
    if (b != 0.0) rel_max = std::max(rel_max, d / std::abs(b));
  }

  return {abs_max, rel_max};
}

/// How many elements differ in any bit.
template <typename T>
std::size_t mismatches(const std::vector<T>& got, const std::vector<T>& want)
{
  std::size_t n = 0;
  for (std::size_t i = 0; i < got.size() && i < want.size(); ++i) {
    if (got[i] != want[i]) ++n;
  }
  return n;
}

} // namespace

/******************************************************************************/
BOOST_AUTO_TEST_CASE(grid_encoded_spectra_equal_the_written_out_reference)
{
  auto grid = MzPeak::open(kGrid);
  auto reference = MzPeak::open(kReference);

  auto got = grid.spectra();
  auto want = reference.spectra();

  BOOST_REQUIRE_EQUAL(got.size(), want.size());
  BOOST_REQUIRE_EQUAL(got.size(), 2u);

  for (std::size_t i = 0; i < got.size(); ++i) {
    auto g = got[i];
    auto w = want[i];

    BOOST_TEST_CONTEXT("spectrum " << i)
    {
      // A short array would silently mis-pair every peak past the cut, so
      // length is checked before the values.
      BOOST_REQUIRE_EQUAL(g.mz().size(), w.mz().size());
      BOOST_REQUIRE_EQUAL(g.intensity().size(), w.intensity().size());
      BOOST_REQUIRE_EQUAL(g.ion_mobility_array().size(),
                          w.ion_mobility_array().size());
      BOOST_REQUIRE_GT(g.mz().size(), 0u);

      // The grid round trip is lossless here, so these are equalities, not
      // tolerances.  If a future writer ever encodes onto a coarser grid this
      // will fail loudly rather than drift.
      BOOST_TEST(mismatches(g.mz(), w.mz()) == 0u);
      BOOST_TEST(mismatches(g.intensity(), w.intensity()) == 0u);
      BOOST_TEST(mismatches(g.ion_mobility_array(), w.ion_mobility_array()) == 0u);

      const auto mz = deviation(g.mz(), w.mz());
      const auto in = deviation(g.intensity(), w.intensity());
      const auto im = deviation(g.ion_mobility_array(), w.ion_mobility_array());

      BOOST_TEST_MESSAGE("spectrum " << i << " n=" << g.mz().size()
                                     << " mz abs=" << mz.first
                                     << " rel=" << mz.second
                                     << " | intensity abs=" << in.first
                                     << " | mobility abs=" << im.first);
    }
  }
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(a_column_mixing_grid_and_plain_chunks_reads_whole)
{
  // One column may hold both: `chunk_encoding` says MS:1000576 for a chunk
  // whose grid struct is null and whose values live in `chunk_values`.  The
  // SCIEX SWATH corpus is full of these, and it is 738 MB; this fixture is the
  // same shape at 226 KB, with every other m/z chunk written out.
  //
  // The failure this guards is quiet: the struct column's null rows are
  // exactly the plainly encoded chunks, so a decoder that skips nulls returns
  // a SHORTER array rather than a wrong one, and every peak past the first
  // such chunk is mis-paired with another peak's intensity.  Length is
  // therefore checked first and separately.
  auto mixed = MzPeak::open(kMixed);
  auto reference = MzPeak::open(kReference);

  auto got = mixed.spectra();
  auto want = reference.spectra();
  BOOST_REQUIRE_EQUAL(got.size(), want.size());

  for (std::size_t i = 0; i < got.size(); ++i) {
    auto g = got[i];
    auto w = want[i];

    BOOST_TEST_CONTEXT("spectrum " << i)
    {
      BOOST_REQUIRE_EQUAL(g.mz().size(), w.mz().size());
      BOOST_REQUIRE_EQUAL(g.intensity().size(), w.intensity().size());
      BOOST_REQUIRE_EQUAL(g.ion_mobility_array().size(),
                          w.ion_mobility_array().size());

      // The un-gridded chunks carry the very coordinates the grid decodes to,
      // so mixing the encodings changes nothing about the values.  Mobility
      // stays gridded throughout, so this also covers a gridded secondary axis
      // beside a plainly encoded main axis.
      BOOST_TEST(mismatches(g.mz(), w.mz()) == 0u);
      BOOST_TEST(mismatches(g.intensity(), w.intensity()) == 0u);
      BOOST_TEST(mismatches(g.ion_mobility_array(), w.ion_mobility_array()) == 0u);
    }
  }
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(grid_mobility_is_in_the_physical_range)
{
  // The mobility model's parameters are [c6, c7, offset, slope], where
  // `offset + slope * scan` is the TIMS ramp VOLTAGE -- hundreds of volts,
  // with a small negative slope.  Reading those two the other way round still
  // produces finite numbers, around 45, so nothing but a look at the value
  // catches it; one reference release did write them swapped, and
  // `timstof_mobility_model_reads_the_pre_eb08ba0_order` pins that case.
  // This one is the whole-file end of it: every decoded mobility of a real
  // acquisition, not just the sampled scans.
  auto grid = MzPeak::open(kGrid);
  auto spectra = grid.spectra();
  auto s = spectra[0];

  const auto& mobility = s.ion_mobility_array();
  BOOST_REQUIRE_GT(mobility.size(), 0u);

  for (double v : mobility) {
    BOOST_REQUIRE(v > 0.4 && v < 2.0);
  }
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(timstof_mz_model_reproduces_the_reference_arithmetic)
{
  // The diaPASEF fixture's own m/z model.  c3 is zero and c2 is not, so this
  // takes the quadratic branch -- the branch mzdata's scalar and wide
  // converters disagree about, and the scalar one is what the grid decoder
  // uses.  Expected values come from the reference implementation, not from
  // this reader.
  const double p[] = {320.480481,   2515.8383621118483, 0.00211500318015718,
                      0.0,          -0.068565,          0.2,
                      24833.0};
  auto model = MzPeak::Data::GridModel::from_parameters("MS:9999002", p, 7);

  BOOST_TEST((model.kind() == MzPeak::Data::GridModel::Kind::TimsTofMz2));
  // Bit-exact: a contraction the compiler inserts on its own moves these.
  BOOST_TEST(model.from_index(0) == 94.998529922851105);
  BOOST_TEST(model.from_index(60) == 95.091497034932047);
  BOOST_TEST(model.from_index(129) == 95.198465462457520);
}

/******************************************************************************/
namespace {

/// Mobility scans of the bundled diaPASEF acquisition and the 1/K0 they stand
/// for, read off the written-out archive of the same run -- physical
/// Vs/cm^2, not this reader's own output.  1/K0 falls as the TIMS ramp
/// voltage falls, so these also pin the sign of the ramp slope.
struct ScanMobility {
  uint32_t scan;
  double one_over_k0;
};

constexpr ScanMobility kKnownMobilities[] = {
    {33, 1.6010956012790702},
    {250, 1.3621252945398825},
    {400, 1.1954873123229730},
    {533, 1.0467308802733821},
    {926, 0.60156745661641153},
};

} // namespace

/******************************************************************************/
BOOST_AUTO_TEST_CASE(timstof_mobility_model_reproduces_the_reference_arithmetic)
{
  // The diaPASEF acquisition's own TimsCalibration row as the four model
  // parameters.  `offset + slope * scan` is the TIMS ramp VOLTAGE: it starts
  // at 222 V and loses 0.154 V per mobility scan, and 1/K0 follows from it
  // through 1/(c6 + c7/V).
  const double p[] = {0.020932469715718494, 131.22279563838268,
                      222.46486824959476, -0.1539079919581615};
  auto model = MzPeak::Data::GridModel::from_parameters("MS:9999001", p, 4);

  BOOST_TEST((model.kind() == MzPeak::Data::GridModel::Kind::TimsTofTims2));

  // Extrapolated to scan 0, i.e. the full ramp voltage.
  BOOST_TEST(model.from_index(0) == 1.637221297889762);

  // Equality, not a tolerance: the grid index IS the native representation
  // here, so decoding has to land on the written-out value bit for bit.
  for (const auto& [scan, one_over_k0] : kKnownMobilities) {
    BOOST_TEST_CONTEXT("scan " << scan)
    {
      BOOST_TEST(model.from_index(scan) == one_over_k0);
    }
  }
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(timstof_mobility_model_reads_the_pre_eb08ba0_order)
{
  // The reference writer emitted [c6, c7, slope, offset] until upstream's
  // eb08ba0, while everything reading the model -- upstream's own reader
  // included -- expects [c6, c7, offset, slope].  Archives from before that
  // fix are in the wild (mzdata's diaPASEF.d converted 2026-09-22), and read
  // literally they give ~45 Vs/cm^2 where the run holds 0.6-1.6, so the
  // mistake is not subtle but it is also not signalled anywhere in the file.
  //
  // The ramp offset is the larger coefficient by a factor of the scan count,
  // which is how the two are told apart; a file in either order must decode
  // to the SAME physical mobilities.
  const double swapped[] = {0.020932469715718494, 131.22279563838268,
                            -0.1539079919581615, 222.46486824959476};
  auto model = MzPeak::Data::GridModel::from_parameters("MS:9999001", swapped, 4);

  for (const auto& [scan, one_over_k0] : kKnownMobilities) {
    BOOST_TEST_CONTEXT("scan " << scan)
    {
      BOOST_TEST(model.from_index(scan) == one_over_k0);
    }
  }

  // Monotonically falling, as the ramp is -- reading the pair the other way
  // round inverts that as well as the magnitude.
  BOOST_TEST(model.from_index(33) > model.from_index(926));
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(linear_and_square_root_models)
{
  // MS:1003824: (index * slope + intercept) / scale.
  const double linear[] = {10.0, 2.0, 4.0};
  auto lin = MzPeak::Data::GridModel::from_parameters("MS:1003824", linear, 3);
  BOOST_TEST(lin.from_index(3) == 4.0, boost::test_tools::tolerance(1e-15));

  // MS:1003825: (index * slope + intercept)^2 / scale.  The reference writer
  // emits only [intercept, slope] -- scale must then default to 1, not 0, or
  // every value comes back infinite.
  const double sqrt_linear[] = {20.283026363407256, 7.01462533510488e-05};
  auto sq = MzPeak::Data::GridModel::from_parameters("MS:1003825", sqrt_linear, 2);
  const double t = 6084.0 * sqrt_linear[1] + sqrt_linear[0];
  BOOST_TEST(sq.from_index(6084) == t * t, boost::test_tools::tolerance(1e-15));
  BOOST_TEST(std::isfinite(sq.from_index(6084)));
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(an_unknown_grid_model_is_named_in_the_error)
{
  // Refusing is fine; refusing without saying which model was met is not --
  // that is the difference between "this file needs a newer reader" and a
  // bug hunt.  It must also not abort: FASTag reads these from many threads.
  const double p[] = {1.0, 2.0};

  BOOST_CHECK_THROW(MzPeak::Data::GridModel::from_parameters("MS:1234567", p, 2),
                    MzPeak::InvalidFormatError);

  try {
    MzPeak::Data::GridModel::from_parameters("MS:1234567", p, 2);
    BOOST_FAIL("expected an exception");
  } catch (const MzPeak::InvalidFormatError& e) {
    BOOST_TEST(std::string(e.what()).find("MS:1234567") != std::string::npos);
  }
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(a_wrong_parameter_count_is_refused)
{
  // Seven parameters is the timsTOF m/z model; handing it four would read
  // past the end of what the file supplied.
  const double p[] = {1.0, 2.0, 3.0, 4.0};

  BOOST_CHECK_THROW(MzPeak::Data::GridModel::from_parameters("MS:9999002", p, 4),
                    MzPeak::InvalidFormatError);
}
