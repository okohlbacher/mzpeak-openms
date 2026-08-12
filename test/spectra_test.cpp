/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE Spectra
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <ranges>
#include <thread>
#include <vector>

#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"

/******************************************************************************/
BOOST_AUTO_TEST_CASE(can_read_spectra)
{
  using namespace MzPeak;

  auto go = [](const std::string file_name) {
    auto mzpeak = MzPeak::open("../test/files/" + file_name);
    auto spectra = mzpeak.spectra();

    BOOST_TEST_CONTEXT("while using the " << file_name << "file")
    {
      BOOST_TEST((spectra.size() == 48));

      auto spectrum = spectra[0];
      auto mz = spectrum.mz();

      auto tolerance = boost::test_tools::tolerance(0.001);

      if (file_name == "small.numpress.mzpeak") {
        // Some numpress linear values are less precise than their
        // matching point or delta values.
        tolerance = boost::test_tools::tolerance(0.1);
      } else {
        // Null-marked m/z values.  Nulls come in PAIRS that separate two runs
        // of real values; each null is reconstructed from the run it adjoins --
        // the first from the run on its left, the second from the run on its
        // right.
        //
        // Ground truth is the Rust reference reader
        // (hupo-mzpeak examples/read_spectrum small.mzpeak 0), which emits
        // 202.60831465843808 / 204.75933490116418 / 204.76085793161354 /
        // 204.77812053474582 at these positions.  Position 8 is NOT 202.6086 --
        // that is what extrapolating the second null from the first used to
        // produce.
        //
        // Held at 1e-6 rather than the loop's 0.001: the point of these four is
        // the PRECISION of the reconstruction, so they are excluded from the
        // lossy numpress fixture rather than loosened for it.
        BOOST_TEST(mz[7] == 202.6083147, boost::test_tools::tolerance(1e-6));
        BOOST_TEST(mz[8] == 204.7593349, boost::test_tools::tolerance(1e-6));
        // Same pattern around the next gap: position 13 = 204.76060409341508,
        // position 16 = 204.77837441952326.
        BOOST_TEST(mz[14] == 204.7608579, boost::test_tools::tolerance(1e-6));
        BOOST_TEST(mz[15] == 204.7781206, boost::test_tools::tolerance(1e-6));
      }

      BOOST_TEST(mz.size() == 13589);
      BOOST_TEST(mz[0] == 202.607, tolerance);
      BOOST_TEST(mz[mz.size() - 1] == 1999.840, tolerance);

      // Test some NULL values.
      //
      // Nulls come in PAIRS that separate two runs.  The FIRST of a pair
      // continues the run on its left; the SECOND belongs to the run on its
      // right and must be reconstructed backwards from it.
      //
      // The raw column (pyarrow, small.dir/spectra_data.parquet) is
      //   row  6  202.60806612940473
      //   rows 7,8  null, null
      //   row  9  204.75958873936264
      // so the two runs are ~2.15 apart.  Position 8 is therefore
      // 204.75959 - delta = 204.75933, NOT 202.60856: that latter value is what
      // extrapolating BOTH nulls forward from the left run produces, and it
      // disagrees with the reference reader (hupo-mzpeak
      // examples/read_spectrum small.mzpeak 0 emits 204.75933490116418 here).
      // Same for position 15 against row 16 = 204.77837441952326.
      BOOST_TEST_REQUIRE(mz[7] == 202.60831, tolerance);
      BOOST_TEST_REQUIRE(mz[8] == 204.75933, tolerance);
      BOOST_TEST_REQUIRE(mz[14] == 204.76086, tolerance);
      BOOST_TEST_REQUIRE(mz[15] == 204.77812, tolerance);

      // The m/z values should be monotonically increasing.
      for (std::size_t i : std::views::iota(1ul, mz.size())) {
        BOOST_TEST_REQUIRE(mz[i] > mz[i - 1]);
      }

      auto intensity = spectrum.intensity();
      BOOST_TEST_REQUIRE((intensity.size() == mz.size()));
      BOOST_TEST_REQUIRE(intensity[0] == 0.0, tolerance);
      BOOST_TEST_REQUIRE(intensity[1] == 1938.12, tolerance);
      BOOST_TEST_REQUIRE(intensity[7] == 0.0, tolerance);
      BOOST_TEST_REQUIRE(intensity[8] == 0.0, tolerance);
      BOOST_TEST_REQUIRE(intensity[9] == 1422.17, tolerance);
      BOOST_TEST_REQUIRE(intensity[intensity.size() - 1] == 0.0, tolerance);

      BOOST_TEST_REQUIRE(spectrum.ms_level() == 1u);
    }
  };

  go("small.mzpeak");
  go("small.chunked.mzpeak");
  go("small.numpress.mzpeak");
}

/******************************************************************************/
// Regression: profile spectra whose first matching row is not at the start of
// its record batch were over-read (data_arrays.cpp slice length used an
// absolute end index instead of a row count).  Spectra 1 and 7 exercise this;
// spectrum 0 (which starts at row 0) always read correctly.
BOOST_AUTO_TEST_CASE(reads_profile_arrays_beyond_first_spectrum)
{
  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  auto s1 = spectra[1].mz();
  BOOST_TEST(s1.size() == 18177);
  BOOST_TEST(s1.front() == 200.0909, boost::test_tools::tolerance(0.001));
  BOOST_TEST(s1.back() == 1999.8182, boost::test_tools::tolerance(0.001));

  auto s7 = spectra[7].mz();
  BOOST_TEST(s7.size() == 10329);
  BOOST_TEST(s7.back() == 1832.2386, boost::test_tools::tolerance(0.001));
}

/******************************************************************************/
// Regression: null-marked INTENSITY values must decode to 0, not be run through
// the m/z delta-model interpolator.
//
// small.mzpeak spectrum 0 has 2376 nulls in both the mz and intensity columns
// at identical positions (7, 8, 14, 15, 21, 22, ... — pyarrow verified).  The
// reference writer tags the intensity array MS:1003902, which the PSI-MS CV
// defines as the m/z-INTERPOLATING transform, so keying the delta-model
// decision off the transform accession interpolated intensity and produced
// physically impossible negative values (-827.55 at position 8, -2721.54 at 15).
// Null-marking reconstruction must follow sorting_rank == 0 instead.
BOOST_AUTO_TEST_CASE(null_marked_intensity_reads_as_zero_not_interpolated)
{
  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();
  auto spectrum = spectra[0];
  const auto& inten = spectrum.intensity();
  BOOST_TEST_REQUIRE(inten.size() == 13589u);

  // Positions that are null in the source intensity column must read as 0.
  for (std::size_t i : {7u, 8u, 14u, 15u, 21u, 22u}) {
    BOOST_TEST(inten[i] == 0.0f);
  }

  // And no intensity anywhere may be negative.
  for (std::size_t i = 0; i < inten.size(); ++i) {
    BOOST_TEST(inten[i] >= 0.0f);
  }
}

/******************************************************************************/
// Regression: intensity was decoded as Int32 while the array is float32, so
// the FloatArray bytes were reinterpreted as integers (garbage).  Spectrum 0
// reads correctly regardless of the slice bug, isolating the type fix.
BOOST_AUTO_TEST_CASE(decodes_intensity_as_float32)
{
  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  auto inten = spectra[0].intensity();
  BOOST_TEST(inten.size() == 13589);
  BOOST_TEST(inten[1] == 1938.1174f, boost::test_tools::tolerance(0.01f));
  BOOST_TEST(inten[2] == 2572.8389f, boost::test_tools::tolerance(0.01f));
}

/******************************************************************************/
// Thread safety: the lazy peak decode must run exactly once and must not race.
//
// Before std::call_once guarded it, `decoded_`/`mz_`/`intensity_` were plain
// mutable members written from a const method, so concurrent mz() calls on one
// Spectrum (or on copies sharing it) were a data race — undefined behaviour
// that can hand back a half-filled vector. Copies also each re-read the file.
//
// Hammering one Spectrum plus copies of it from many threads is the cheapest
// thing that fails if the synchronisation regresses; run under TSan for the
// strong version.
BOOST_AUTO_TEST_CASE(concurrent_peak_decode_is_safe_and_happens_once)
{
  auto mzpeak = MzPeak::open("../test/files/small.mzpeak");
  auto spectra = mzpeak.spectra();

  // One spectrum, copied before any decode has happened.  Copies share the
  // decode state, so exactly one of these threads performs the read.
  auto original = spectra[0];
  constexpr int kThreads = 16;
  std::vector<MzPeak::Spectrum> copies(kThreads, original);

  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&copies, t, &mismatches] {
      const auto& mz = copies[static_cast<std::size_t>(t)].mz();
      const auto& inten = copies[static_cast<std::size_t>(t)].intensity();
      if (mz.size() != 13589 || inten.size() != 13589) ++mismatches;
      // Spot-check reconstructed and real values seen by every thread.
      if (std::abs(mz[8] - 204.7593349) > 1e-6) ++mismatches;
      if (std::abs(static_cast<double>(inten[1]) - 1938.1174) > 0.01) ++mismatches;
    });
  }
  for (auto& th : threads)
    th.join();

  BOOST_TEST(mismatches.load() == 0);

  // The original observes the same decode its copies triggered.
  BOOST_TEST(original.mz().size() == 13589u);
  BOOST_TEST(std::abs(original.mz()[8] - 204.7593349) < 1e-6);
}

/******************************************************************************/
// A file with NO Parquet page index reads identically to one with it.
//
// The reference writer emits no page index (verified on a real Astral run), so
// the planner cannot narrow within a row group and hands the executor the whole
// group.  The executor binary-searches the declared-sorted index within that
// range instead of scanning it linearly -- O(log n), not O(group) per spectrum,
// which is a ~16x speedup on that layout.  This pins the CORRECTNESS of that
// path; the speed is covered by the benchmark.  no_page_index.dir has the page
// index stripped and the sorting-columns declaration kept.
// NOTE ON PROVENANCE: this fixture was derived from small.dir when small.dir
// was still the legacy NESTED layout.  Upstream has since regenerated
// small.dir into the SPLIT layout, so the comparison below now crosses TWO
// variables, not one.  The property under test is still exercised, and the
// nested arm is now covered nowhere else, so the fixture is kept as is --
// but do not read this as a single-variable experiment.
BOOST_AUTO_TEST_CASE(reads_correctly_without_a_page_index)
{
  auto with_index = MzPeak::open("../test/files/small.dir").spectra();
  auto without = MzPeak::open("../test/files/no_page_index.dir").spectra();
  BOOST_TEST_REQUIRE(without.size() == with_index.size());

  // Every spectrum decodes to the same peaks either way -- the binary-search
  // path and the page-index path must agree value for value.
  for (std::size_t i = 0; i < with_index.size(); ++i) {
    const auto a = with_index[i];
    const auto b = without[i];
    BOOST_TEST_REQUIRE(b.mz().size() == a.mz().size());
    BOOST_TEST_REQUIRE(b.intensity().size() == a.intensity().size());
    if (!a.mz().empty()) {
      BOOST_TEST(b.mz().front() == a.mz().front());
      BOOST_TEST(b.mz().back() == a.mz().back());
      BOOST_TEST(b.intensity().front() == a.intensity().front());
    }
  }
}
