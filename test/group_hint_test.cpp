/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * The planner's saved row-group hint must not change the ANSWER.
 *
 * `Planner::Impl::plan()` remembers the row group its last equality query
 * matched and starts the next scan there.  That is a pure cost optimisation --
 * a reader walking entities in order finds its group in one statistics
 * evaluation instead of `num_row_groups` of them -- and the code says so:
 * "the result cannot depend on the hint -- only the cost".
 *
 * It did.  An entity whose rows STRADDLE a row-group boundary lives in two
 * groups.  The forward scan starts AT the hint, so once the hint had moved to
 * such an entity's SECOND group, re-planning it matched that group, set
 * `found`, stopped at the next non-matching group, and never looked at the
 * group BEFORE the hint -- the one holding the entity's first rows.  The
 * prefix rescan was guarded by `if (!found)`, so it did not run either.
 *
 * The result is a spectrum returned SHORT: some of its peaks, no error, no
 * warning.  A file carrying a per-spectrum count turns this into a throw from
 * the cross-check in `Spectrum::decode_`; a file without one -- which the
 * reference writer is free to produce, and which this fixture reproduces --
 * hands the caller a plausible, wrong array.
 *
 * Reaching it needs only a non-monotonic access order: a reverse sweep, a
 * subsampled or random-access read, or simply fetching the same spectrum
 * twice.  All three are below.
 *
 * The fixture is deliberately tiny and deliberately MISALIGNED: 7 points per
 * spectrum against 10-row row groups, so 24 of the 40 spectra straddle a
 * boundary.  Every bundled fixture has one row group, and the existing
 * multi-group test (row_group_test.cpp) uses 20 points against 100-row groups
 * -- an exact division, so nothing there ever straddles and this defect
 * survived it.
 */

#define BOOST_TEST_MODULE GroupHint
#include <boost/test/included/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <parquet/api/reader.h>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"
#include "mzpeak/util/parquet_writer.h"

namespace {

namespace fs = std::filesystem;

/// 40 spectra of 7 points is 280 rows; at 10 rows per group that is 28 groups
/// and, because 7 does not divide 10, a boundary that falls INSIDE a spectrum
/// 24 times over -- six spectra in every ten.
constexpr uint64_t kSpectra = 40;
constexpr uint64_t kPoints = 7;
constexpr int64_t kRowGroupSize = 10;

/// A scratch directory removed on scope exit.  Same shape as row_group_test's:
/// Windows refuses to delete a file a virus scanner still holds open, so the
/// removal retries rather than failing the test for an unrelated reason.
struct Scratch {
  fs::path path;
  explicit Scratch(const char* name)
      : path(fs::temp_directory_path() / name)
  {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~Scratch()
  {
    std::error_code ec;
    for (int attempt = 0; attempt < 20; ++attempt) {
      fs::remove_all(path, ec);
      if (!ec) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    BOOST_TEST(!ec, "remove_all " << path.string() << ": " << ec.message());
  }
};

/// m/z and intensity are pure functions of (spectrum, point) so any read order
/// can be checked against them without carrying expected values around.
double expected_mz(uint64_t s, uint64_t p)
{
  return 100.0 + static_cast<double>(s) + static_cast<double>(p) / 1000.0;
}

float expected_intensity(uint64_t s, uint64_t p)
{
  return static_cast<float>(s * kPoints + p);
}

/// True when spectrum @p s has rows in two row groups.
bool straddles(uint64_t s)
{
  const int64_t first = static_cast<int64_t>(s * kPoints);
  const int64_t last = first + static_cast<int64_t>(kPoints) - 1;
  return first / kRowGroupSize != last / kRowGroupSize;
}

/// Write the misaligned multi-row-group point file and return its directory.
///
/// NO metadata table, on purpose.  `Spectrum::decode_` cross-checks the decoded
/// length against a declared per-spectrum count when one exists, which would
/// convert this defect into a throw and hide the thing under test: that the
/// reader hands back a SHORT array and says nothing.  The declared sorting
/// column on the index is what puts the planner on the hinted path at all.
fs::path write_fixture(const Scratch& scratch)
{
  std::vector<uint64_t> index;
  std::vector<double> mz;
  std::vector<float> intensity;

  for (uint64_t s = 0; s < kSpectra; ++s) {
    for (uint64_t p = 0; p < kPoints; ++p) {
      index.push_back(s);
      mz.push_back(expected_mz(s, p));
      intensity.push_back(expected_intensity(s, p));
    }
  }

  const std::string array_index =
      R"({"prefix":"point","entries":[)"
      R"({"context":"spectrum","path":"point.mz","data_type":"MS:1000523",)"
      R"("array_type":"MS:1000514","array_name":"m/z array","unit":"MS:1000040",)"
      R"("buffer_format":"point","transform":"MS:1003902",)"
      R"("data_processing_id":null,"buffer_priority":"primary","sorting_rank":0},)"
      R"({"context":"spectrum","path":"point.intensity","data_type":"MS:1000521",)"
      R"("array_type":"MS:1000515","array_name":"intensity array",)"
      R"("unit":"MS:1000131","buffer_format":"point","transform":"MS:1003901",)"
      R"("data_processing_id":null,"buffer_priority":"primary",)"
      R"("sorting_rank":null}]})";

  const std::map<std::string, std::string> kv{
      {"spectrum_count", std::to_string(kSpectra)},
      {"spectrum_data_point_count", std::to_string(index.size())},
      {"spectrum_array_index", array_index},
  };

  MzPeak::Util::write_point_spectra_data((scratch.path / "spectra_data.parquet").string(),
                                         index, mz, intensity, kv, kRowGroupSize);

  std::ofstream json(scratch.path / "mzpeak_index.json");
  json << R"({"files":[{"name":"spectra_data.parquet","entity_type":"spectrum",)"
       << R"("data_kind":"data_arrays","column_mapping":[],"parameters":[]}],)"
       << R"("metadata":{"version":"0.9.0"}})";
  json.close();

  return scratch.path;
}

/// Read spectrum @p s and check it whole.  Every failure is reported against
/// the spectrum number so a partial read names itself.
void check(MzPeak::Spectra& spectra, uint64_t s)
{
  auto spectrum = spectra[s];
  const auto& mz = spectrum.mz();
  const auto& intensity = spectrum.intensity();

  BOOST_TEST_REQUIRE(mz.size() == kPoints,
                     "spectrum " << s << (straddles(s) ? " (straddles a group boundary)"
                                                       : ""));
  BOOST_TEST_REQUIRE(intensity.size() == kPoints, "spectrum " << s);

  for (uint64_t p = 0; p < kPoints; ++p) {
    BOOST_TEST(mz[p] == expected_mz(s, p), boost::test_tools::tolerance(1e-12));
    BOOST_TEST(intensity[p] == expected_intensity(s, p));
  }
}

} // namespace

/******************************************************************************/
// The fixture really is misaligned.  Without this the tests below could pass
// because every spectrum happened to sit inside one row group -- which is
// precisely how the existing multi-row-group test misses this.
BOOST_AUTO_TEST_CASE(fixture_really_straddles_row_group_boundaries)
{
  Scratch scratch("mzp-test-hint-shape");
  const fs::path dir = write_fixture(scratch);

  auto reader = parquet::ParquetFileReader::OpenFile(
      (dir / "spectra_data.parquet").string(), false);

  BOOST_TEST(reader->metadata()->num_row_groups() == 28);
  BOOST_TEST(reader->metadata()->num_rows() ==
             static_cast<int64_t>(kSpectra * kPoints));

  std::size_t straddling = 0;
  for (uint64_t s = 0; s < kSpectra; ++s)
    if (straddles(s)) ++straddling;
  BOOST_TEST(straddling == 24u);

  // And the declaration that puts the planner on the hinted path in the first
  // place: every row group must claim the index column sorted ascending.
  for (int g = 0; g < reader->metadata()->num_row_groups(); ++g) {
    BOOST_TEST(reader->metadata()->RowGroup(g)->sorting_columns().size() == 1u,
               "row group " << g);
  }
}

/******************************************************************************/
// Control: ascending order, the access pattern the hint was built for.  This
// passes with or without the fix; it is here so a failure below can be read as
// "the ORDER broke it" rather than "the fixture is broken".
BOOST_AUTO_TEST_CASE(ascending_order_reads_every_spectrum_whole)
{
  Scratch scratch("mzp-test-hint-ascending");
  auto spectra = MzPeak::open(write_fixture(scratch)).spectra();

  BOOST_TEST_REQUIRE(spectra.size() == kSpectra);
  for (uint64_t s = 0; s < kSpectra; ++s)
    check(spectra, s);
}

/******************************************************************************/
// THE REGRESSION.  The same 40 spectra, read from the back.
//
// Descending order walks the hint down one group at a time, so it arrives at
// straddling spectra with the hint already sitting in the spectrum's UPPER
// group -- exactly the state in which the unfixed forward-only scan returns
// the upper half and stops.  Before the fix this fails at spectrum 38 with a
// three-point array; after it, all 40 read whole.
BOOST_AUTO_TEST_CASE(descending_order_reads_every_spectrum_whole)
{
  Scratch scratch("mzp-test-hint-descending");
  auto spectra = MzPeak::open(write_fixture(scratch)).spectra();

  BOOST_TEST_REQUIRE(spectra.size() == kSpectra);
  for (uint64_t s = kSpectra; s-- > 0;)
    check(spectra, s);
}

/******************************************************************************/
// A fixed, reproducible scatter rather than a shuffle: a subsampling reader
// (FASTag's `-subsample_spectra`, an EIC over a retention-time window) hits the
// file in no particular order, and a test that depends on a random seed reports
// a different spectrum every run.  Stepping by a stride coprime with the run
// length visits all 40 exactly once in an order that jumps in both directions.
//
// This particular stride happens NOT to fail on the unfixed planner -- it never
// lands on a straddling spectrum while the hint sits in that spectrum's upper
// group.  It is kept as coverage of an arbitrary access order, not as the
// reproduction; the two cases around it are the ones that fail before the fix.
BOOST_AUTO_TEST_CASE(scattered_order_reads_every_spectrum_whole)
{
  Scratch scratch("mzp-test-hint-scattered");
  auto spectra = MzPeak::open(write_fixture(scratch)).spectra();

  BOOST_TEST_REQUIRE(spectra.size() == kSpectra);
  for (uint64_t i = 0; i < kSpectra; ++i)
    check(spectra, (i * 23) % kSpectra);
}

/******************************************************************************/
// The smallest reproduction there is: read ONE straddling spectrum twice.
//
// The first read leaves the hint on the spectrum's upper group.  The second
// starts there, matches, and never looks lower.  No exotic access order is
// needed -- two `spectra[s]` calls in a row, each returning a fresh Spectrum
// with its own decode, disagree about how many peaks the file holds.  Before
// the fix this returns 7 then 1.
BOOST_AUTO_TEST_CASE(a_straddling_spectrum_reads_the_same_twice)
{
  Scratch scratch("mzp-test-hint-twice");
  auto spectra = MzPeak::open(write_fixture(scratch)).spectra();

  // Spectrum 2 covers rows 14..20 and row group 1 ends at row 19, so it is the
  // first spectrum to span two groups: six rows below the boundary, one above.
  constexpr uint64_t kStraddler = 2;
  BOOST_TEST_REQUIRE(straddles(kStraddler));

  const std::size_t first = spectra[kStraddler].mz().size();
  const std::size_t second = spectra[kStraddler].mz().size();

  BOOST_TEST(first == kPoints);
  BOOST_TEST(second == kPoints);
  BOOST_TEST(first == second, "the same spectrum read twice returned "
                                  << first << " then " << second << " peaks");
}

/******************************************************************************/
// The case the backward walk must NOT mishandle: a spectrum wholly inside the
// hinted group, read straight after its predecessor.  The group below holds a
// different spectrum, so the walk must stop after one statistics evaluation --
// this is the common path, and an O(groups) rescan here is the regression the
// hint was introduced to remove.  Timing is not assertable, so what is pinned
// is the answer; the cost is left to the benchmark.
BOOST_AUTO_TEST_CASE(a_non_straddling_spectrum_after_its_predecessor_is_whole)
{
  Scratch scratch("mzp-test-hint-adjacent");
  auto spectra = MzPeak::open(write_fixture(scratch)).spectra();

  for (uint64_t s = 1; s < kSpectra; ++s) {
    if (straddles(s)) continue;
    check(spectra, s - 1); // move the hint
    check(spectra, s);     // ...and read the contained spectrum from there
  }
}
