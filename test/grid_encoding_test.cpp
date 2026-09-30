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
#include <arrow/api.h>
#include <limits>
#include <optional>
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

/// Assert that the archive at @p path decodes to EXACTLY what grid.dir does --
/// every m/z, every intensity, every mobility, bit for bit.  The variants that
/// use it rewrite grid.dir's rows into another representation of the same
/// values, so anything short of identity is a decoding error.
void expect_same_as_grid_dir(const char* path)
{
  auto good_index = MzPeak::open(kFixture);
  auto other_index = MzPeak::open(path);
  auto good = peaks_of(good_index);
  auto other = peaks_of(other_index);
  BOOST_REQUIRE_EQUAL(other.size(), good.size());

  for (std::size_t i = 0; i < good.size(); ++i) {
    const auto g = good[i];
    const auto o = other[i];
    BOOST_REQUIRE_EQUAL(o.mz().size(), g.mz().size());
    BOOST_REQUIRE_EQUAL(o.ion_mobility_array().size(),
                        g.ion_mobility_array().size());
    BOOST_TEST(o.mz() == g.mz(), path << " spectrum " << i << ": m/z differs");
    BOOST_TEST(o.intensity() == g.intensity(),
               path << " spectrum " << i << ": intensity differs");
    BOOST_TEST(o.ion_mobility_array() == g.ion_mobility_array(),
               path << " spectrum " << i << ": ion mobility differs");
  }
}

/// Assert that @p path decodes exactly as @p reference does, on @p source.
void expect_same_as(const char* reference,
                    const char* path,
                    MzPeak::Index::SpectraSource source)
{
  auto ref_index = MzPeak::open(reference);
  auto other_index = MzPeak::open(path);
  auto ref = ref_index.spectra(MzPeak::MetadataDetail::Full, source);
  auto other = other_index.spectra(MzPeak::MetadataDetail::Full, source);
  BOOST_REQUIRE_EQUAL(other.size(), ref.size());
  for (std::size_t i = 0; i < ref.size(); ++i) {
    const auto a = ref[i];
    const auto b = other[i];
    BOOST_TEST(b.mz() == a.mz(), path << " spectrum " << i << ": m/z differs");
    BOOST_TEST(b.intensity() == a.intensity(),
               path << " spectrum " << i << ": intensity differs");
  }
}

/// The message of the exception reading @p path's first spectrum throws, or
/// empty if it does not throw.
template <typename E> std::string refusal_of(const char* path)
{
  auto index = MzPeak::open(path);
  auto spectra = peaks_of(index);
  try {
    const auto s = spectra[0];
    (void)s.mz();
    (void)s.ion_mobility_array();
  } catch (const E& e) {
    return e.what();
  }
  return {};
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
  // EXACT: these were evaluated independently with the reference arithmetic,
  // and a tolerance would admit the very errors this test exists to catch.
  BOOST_TEST(mz[0] == 95.1085459409012);
  BOOST_TEST(mz[1] == 95.50575242076872);
  BOOST_TEST(mz.back() == 1703.9464397733252);

  // Ion mobility: MS:9999001, four parameters, indices ABSOLUTE because it is a
  // secondary axis.  Delta-accumulating these would run the scan numbers away
  // to nonsense, so this pins the main/secondary distinction.
  BOOST_TEST(mobility[0] == 1.363232213510821);
  BOOST_TEST(mobility[1] == 1.5209798315812844);

  const auto s1 = spectra[1];
  BOOST_REQUIRE_EQUAL(s1.mz().size(), 10566u);
  BOOST_TEST(s1.mz()[0] == 95.14729914189232);
  BOOST_TEST(s1.ion_mobility_array()[0] == 0.9297464058254469);

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
  // chunk_end = 0.0 on every ONE-POINT chunk -- the only chunks that bug hit. That
  // is exactly the shape of the real diaPASEF conversion that aborted FASTag. It has
  // to decode to precisely what the conforming fixture decodes to -- not "close",
  // equal, because both are the same model evaluated at the same indices.
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

BOOST_AUTO_TEST_CASE(the_timstof_model_matches_the_reference_arithmetic_exactly)
{
  namespace Grid = MzPeak::Data::Transformer::Grid;

  // Reference values from the `grid.rs` arithmetic evaluated in Python, where
  // nothing is contracted and math.fma is exact.  They agree with the values an
  // independent review computed: index 5195 gives ...499 uncontracted, and a
  // build that lets the compiler fuse the discriminant's multiply-add gives
  // ...496.  EXACT equality is therefore what enforces the no-contraction
  // contract -- a tolerance would pass a contracting build.
  const std::vector<double> quad = {
      320.480481, 2515.8383621118483, 0.00211500318015718, 0.0, -0.068565, 0.2,
      24833.0};
  std::vector<double> cubic = quad;
  cubic[3] = 1.0e-6; // no committed calibration has C3 != 0: the Newton branch

  struct Case {
    std::uint32_t idx;
    double quadratic;
    double cubic;
  };
  const Case cases[] = {
      {5195, 103.21650959138499, 103.21650113351625},
      {34253, 155.47318769816712, 155.47316849972754},
      {150000, 469.54221185610294, 469.5420366482397},
      {396257, 1701.0405121164963, 1701.0382122087242},
  };
  for (const auto& c : cases) {
    BOOST_TEST(*Grid::value_at("MS:9999002", quad, c.idx) == c.quadratic,
               "quadratic, index " << c.idx);
    BOOST_TEST(*Grid::value_at("MS:9999002", cubic, c.idx) == c.cubic,
               "cubic, index " << c.idx);
  }
}

BOOST_AUTO_TEST_CASE(non_finite_coordinates_are_refused)
{
  namespace Grid = MzPeak::Data::Transformer::Grid;

  // A linear grid of scale 0 evaluates to 0/0.  NaN defeats every comparison,
  // so a reader that only compared against bounds would pass it straight out.
  Grid::Row row{"MS:1003824", {0.0, 0.0, 0.0}, {0, 1}};
  std::vector<double> out;
  BOOST_CHECK_THROW(Grid::evaluate(row, true, "mz", out),
                    MzPeak::InvalidFormatError);

  // And the bound check itself must fail on NaN rather than pass it.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  BOOST_CHECK_THROW(Grid::check_bounds(nan, nan, 2, 100.0, 101.0, 1.0, 1.0, "mz", 0),
                    MzPeak::InvalidFormatError);
}

BOOST_AUTO_TEST_CASE(a_lossy_grid_is_accepted_within_one_step)
{
  namespace Grid = MzPeak::Data::Transformer::Grid;

  // The specification calls grid encoding "likely to be a lossy transformation"
  // and does not say the bounds are the model's evaluation.  A writer that
  // records the ORIGINAL coordinate has moved it by at most half a step when
  // snapping to the grid: linear [100, 1] reconstructs 100 and 101 from data
  // that was 100.00001 and 101.00001.
  BOOST_CHECK_NO_THROW(
      Grid::check_bounds(100.0, 101.0, 2, 100.00001, 101.00001, 1.0, 1.0, "mz", 0));

  // Three steps out is not rounding; it is a wrong model.
  BOOST_CHECK_THROW(
      Grid::check_bounds(100.0, 101.0, 2, 103.0, 101.0, 1.0, 1.0, "mz", 0),
      MzPeak::InvalidFormatError);
}

BOOST_AUTO_TEST_CASE(a_zero_end_is_excused_only_on_a_single_point_chunk)
{
  namespace Grid = MzPeak::Data::Transformer::Grid;

  // The writer bug consumed a ONE-value chunk's value for its start and left
  // the end at 0.0.  That shape is excused ...
  BOOST_CHECK_NO_THROW(Grid::check_bounds(1701.04, 1701.04, 1, 1701.04, 0.0, 0.006,
                                          0.006, "mz", 131));
  // ... and nothing wider: on a chunk of several points a zero end is a real
  // contradiction, and excusing it would accept any file whose end was zeroed.
  BOOST_CHECK_THROW(
      Grid::check_bounds(1700.0, 1701.04, 4, 1700.0, 0.0, 0.006, 0.006, "mz", 33),
      MzPeak::InvalidFormatError);
}

BOOST_AUTO_TEST_CASE(an_absent_bound_is_skipped_not_read)
{
  namespace Grid = MzPeak::Data::Transformer::Grid;

  BOOST_CHECK_NO_THROW(Grid::check_bounds(100.0, 101.0, 2, std::nullopt,
                                          std::nullopt, 1.0, 1.0, "mz", 0));

  // The accessor that replaced a raw index: a shorter or empty bound column
  // yields "unrecorded", never an out-of-bounds read.
  const std::vector<std::optional<double>> starts = {100.0, 200.0};
  const std::vector<std::optional<double>> none;
  BOOST_TEST(Grid::detail::at(starts, 1).value() == 200.0);
  BOOST_TEST(!Grid::detail::at(starts, 2).has_value());
  BOOST_TEST(!Grid::detail::at(none, 0).has_value());
}

BOOST_AUTO_TEST_CASE(a_dimension_mixing_grid_and_plain_rows_decodes)
{
  // The specification tells a writer to fall back to another encoding per
  // chunk when the grid model's error is too large, so one dimension holds
  // grid rows beside plain ones.  `grid_mixed.dir` cycles its m/z rows through
  // grid, uncompressed and delta, each carrying the values the grid row held.
  // This reader used to refuse the whole file on the first non-grid row.
  expect_same_as_grid_dir("../test/files/grid_mixed.dir");
}

BOOST_AUTO_TEST_CASE(a_secondary_axis_split_between_plain_and_grid_is_not_truncated)
{
  // Mobility as a plain `chunk_secondary` list on odd rows and a grid on even
  // ones.  This is the defect that did not throw: the dimension's values entry
  // named the plain column, the grid was never read, and half the mobilities
  // simply vanished.
  expect_same_as_grid_dir("../test/files/grid_mixed_secondary.dir");
}

BOOST_AUTO_TEST_CASE(an_archive_without_chunk_end_decodes)
{
  // No `chunk_end` column, which nothing requires.  The end bounds used to be
  // indexed by the start column's length -- undefined behaviour on an empty
  // vector.  The start bound is still checked on every row.
  expect_same_as_grid_dir("../test/files/grid_no_end.dir");
}

BOOST_AUTO_TEST_CASE(a_zero_end_on_a_multi_point_chunk_is_refused)
{
  const std::string what = refusal_of<MzPeak::InvalidFormatError>(
      "../test/files/grid_zero_end_multi.dir");
  // "disagrees with chunk_end" is check_bounds() refusing the bound.  Matching
  // plain "chunk_end" would also accept the structural check's "chunk_start
  // exceeds chunk_end", which fires even if the zero-end exemption were wrongly
  // widened again -- so the test would pass for the wrong reason.
  BOOST_TEST(what.find("disagrees with chunk_end") != std::string::npos,
             "a zeroed end on an 11-point chunk was accepted; message: " << what);
}

BOOST_AUTO_TEST_CASE(a_short_mobility_array_is_refused)
{
  // One chunk's mobility grid is null while its peaks are not.  Every mobility
  // after the gap would be paired with the wrong peak, and nothing but a check
  // that the parallel arrays are parallel can see it.
  const std::string what =
      refusal_of<MzPeak::ParquetError>("../test/files/grid_short_mobility.dir");
  BOOST_TEST(what.find("ion mobilities") != std::string::npos,
             "a short mobility array was returned; message: " << what);
}

BOOST_AUTO_TEST_CASE(numpress_rows_are_read_from_their_own_column)
{
  // Numpress keeps its bytes in a chunk_transform column, with chunk_values
  // null.  Registering an unused, all-null grid column routes the dimension to
  // the grid reader, whose plain-row path first read every row from
  // chunk_values -- and so decoded an otherwise ordinary Numpress archive to
  // nothing.  It must decode exactly as the archive it was built from.
  expect_same_as("../test/files/small.numpress.mzpeak",
                 "../test/files/grid_numpress.dir",
                 MzPeak::Index::SpectraSource::Data);
}

BOOST_AUTO_TEST_CASE(every_list_layout_the_ordinary_decoder_reads_is_read)
{
  namespace Grid = MzPeak::Data::Transformer::Grid;

  // Util::Decoders::visit accepts five list layouts.  The grid reader's row
  // accessor accepted two and returned null for the rest, which the decoder
  // then dereferenced.
  // Built with Arrow builders, which own their memory.  (Buffer::Wrap over a
  // temporary vector would leave the array pointing at freed storage.)
  const auto build = []<typename B, typename V>(std::initializer_list<V> items) {
    B builder;
    for (const V item : items) {
      if (!builder.Append(item).ok()) throw std::runtime_error("append");
    }
    return builder.Finish().ValueOrDie();
  };
  const auto values = build.template operator()<arrow::DoubleBuilder, double>(
      {101.0, 102.0, 201.0, 202.0});

  auto fixed = arrow::FixedSizeListArray::FromArrays(values, 2).ValueOrDie();
  const auto row = Grid::detail::list_row(*fixed, 1);
  BOOST_REQUIRE(row != nullptr);
  BOOST_REQUIRE_EQUAL(row->length(), 2);
  BOOST_TEST(std::static_pointer_cast<arrow::DoubleArray>(row)->Value(0) == 201.0);

  const auto offsets =
      build.template operator()<arrow::Int32Builder, int32_t>({0, 2});
  const auto sizes = build.template operator()<arrow::Int32Builder, int32_t>({2, 2});
  auto view =
      arrow::ListViewArray::FromArrays(*offsets, *sizes, *values).ValueOrDie();
  const auto vrow = Grid::detail::list_row(*view, 1);
  BOOST_REQUIRE(vrow != nullptr);
  BOOST_TEST(std::static_pointer_cast<arrow::DoubleArray>(vrow)->Value(1) == 202.0);

  // Not a list at all: refused with a type error, never a null to dereference.
  BOOST_CHECK_THROW((void)Grid::detail::list_row(*values, 0), MzPeak::TypeError);
}

BOOST_AUTO_TEST_CASE(a_model_shifted_by_one_index_is_refused)
{
  namespace Grid = MzPeak::Data::Transformer::Grid;

  // A flight-time delay off by one digitizer tick shifts every index by one --
  // a plausible transcription error, and exactly ONE grid step.  With a
  // one-step tolerance it passed every endpoint of the fixture; half a step,
  // the most a correctly snapped coordinate can move, refuses it.
  Grid::Row row{"MS:9999002",
                {320.480481, 2515.8383621118483, 0.00211500318015718, 0.0, -0.068565,
                 0.2, 24833.0},
                {60}};
  const double right = *Grid::value_at(row.type, row.parameters, 60);
  const double shifted = *Grid::value_at(row.type, row.parameters, 61);
  const double tolerance = Grid::snap_tolerance(row, 60);

  BOOST_CHECK_THROW(Grid::check_bounds(shifted, shifted, 1, right, std::nullopt,
                                       tolerance, tolerance, "mz", 0),
                    MzPeak::InvalidFormatError);
  // A coordinate snapped from anywhere within half a step is still accepted.
  BOOST_CHECK_NO_THROW(
      Grid::check_bounds(right, right, 1, right + 0.4 * (shifted - right),
                         std::nullopt, tolerance, tolerance, "mz", 0));
}

BOOST_AUTO_TEST_CASE(the_largest_index_does_not_wrap)
{
  namespace Grid = MzPeak::Data::Transformer::Grid;

  // The specification's own fitting example spans the whole uint32 range.  At
  // the largest index `idx + 1` wrapped to zero, so the "step" spanned the
  // entire grid and any bound at all was accepted there.
  constexpr std::uint32_t max = std::numeric_limits<std::uint32_t>::max();
  Grid::Row row{"MS:1003824", {100.0, 80.0 / max}, {max}};
  const double tolerance = Grid::snap_tolerance(row, max);
  BOOST_TEST(tolerance < 1e-6, "tolerance at the last index is " << tolerance);

  const double last = *Grid::value_at(row.type, row.parameters, max);
  BOOST_CHECK_THROW(Grid::check_bounds(last, last, 1, std::nullopt, 200.0, tolerance,
                                       tolerance, "mz", 0),
                    MzPeak::InvalidFormatError);
}

BOOST_AUTO_TEST_CASE(a_non_finite_recorded_bound_is_refused)
{
  namespace Grid = MzPeak::Data::Transformer::Grid;

  // A bound of inf made the tolerance infinite, which accepted any value.
  const double inf = std::numeric_limits<double>::infinity();
  BOOST_CHECK_THROW(
      Grid::check_bounds(100.0, 101.0, 2, inf, 101.0, 0.5, 0.5, "mz", 0),
      MzPeak::InvalidFormatError);
}

BOOST_AUTO_TEST_CASE(chunk_order_is_enforced_past_a_missing_end)
{
  // Two chunks exchanged so they descend, and the first one's end removed.
  // Ordering used to be checked only against the previous chunk's END, so a
  // chunk without one let the next go unchecked.
  const std::string what =
      refusal_of<MzPeak::InvalidFormatError>("../test/files/grid_disordered.dir");
  BOOST_TEST(what.find("out of order") != std::string::npos,
             "descending chunks were accepted; message: " << what);
}

BOOST_AUTO_TEST_CASE(a_main_axis_without_chunk_encoding_is_refused)
{
  const std::string what =
      refusal_of<MzPeak::InvalidFormatError>("../test/files/grid_no_encoding.dir");
  BOOST_TEST(what.find("chunk_encoding") != std::string::npos,
             "a main axis without chunk_encoding was decoded; message: " << what);
}

BOOST_AUTO_TEST_CASE(two_distinct_mobility_arrays_do_not_block_the_spectrum)
{
  // A point-layout file, nothing grid-encoded, carrying raw mobility beside the
  // mean mobility it already had.  Every mobility dimension used to be appended
  // into one vector, and the check that mobility is parallel to m/z then refused
  // the spectrum -- m/z included -- over that concatenation.  The first array is
  // exposed, whole.
  auto index = MzPeak::open("../test/files/two_mobility.dir");
  auto spectra = peaks_of(index);
  auto reference_index = MzPeak::open("../test/files/diapasef.dir");
  auto reference = peaks_of(reference_index);

  const auto s = spectra[0];
  const auto r = reference[0];
  BOOST_REQUIRE_NO_THROW((void)s.mz());
  BOOST_TEST(s.mz() == r.mz());
  BOOST_TEST(s.ion_mobility_array() == r.ion_mobility_array());
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
