/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * Multi-row-group reads.
 *
 * Every bundled fixture has exactly ONE Parquet row group.  Three pieces of the
 * read path are therefore never reached by the rest of the suite:
 *
 *   - the row-group cache in `Util::Parquet` holds two groups and evicts the
 *     oldest (`kCachedGroups = 2`).  With one group there is nothing to evict,
 *     so the eviction branch has never executed under test;
 *   - the planner's job is to name WHICH row group a row lives in.  With one
 *     group, a planner that ignored the query and always returned group 0
 *     would pass;
 *   - the cache is shared mutable state behind one mutex that also covers the
 *     decode.  Contention between threads landing in DIFFERENT groups -- the
 *     case the mutex exists for -- cannot happen with a single group.
 *
 * These tests synthesise a file with a small row-group cap so all three are
 * exercised, and check VALUES rather than absence of a crash: a race here
 * corrupts silently (Parquet pages carry no CRC).
 */

#define BOOST_TEST_MODULE RowGroup
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <parquet/api/reader.h>
#include <string>
#include <thread>
#include <vector>

#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"
#include "mzpeak/util/parquet_writer.h"

namespace {

namespace fs = std::filesystem;

/// Points per spectrum, and spectra, in the synthetic file.
constexpr uint64_t kSpectra = 60;
constexpr uint64_t kPoints = 20;

/// Rows per row group.  60*20 = 1200 rows over 100-row groups = 12 groups,
/// comfortably more than the 2 the cache holds.
constexpr int64_t kRowGroupSize = 100;

/// A scratch directory removed on scope exit.
struct Scratch {
  fs::path path;
  explicit Scratch(const char* name)
      : path(fs::temp_directory_path() / name)
  {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~Scratch() { fs::remove_all(path); }
};

/// m/z and intensity are pure functions of (spectrum, point), so any thread can
/// verify any spectrum without sharing expected values.
double expected_mz(uint64_t s, uint64_t p)
{
  return 100.0 + static_cast<double>(s) + static_cast<double>(p) / 1000.0;
}

float expected_intensity(uint64_t s, uint64_t p)
{
  return static_cast<float>(s * kPoints + p);
}

/// Write the synthetic multi-row-group point file and return its path.
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

  const fs::path path = scratch.path / "spectra_data.parquet";
  // The array index is what tells the reader which columns carry m/z and
  // intensity; without it the file is unreadable regardless of its contents.
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

  std::map<std::string, std::string> kv{
      {"spectrum_count", std::to_string(kSpectra)},
      {"spectrum_data_point_count", std::to_string(index.size())},
      {"spectrum_array_index", array_index},
  };

  MzPeak::Util::write_point_spectra_data(path.string(), index, mz, intensity, kv,
                                         kRowGroupSize);

  // Minimal index: one signal-data member, no metadata table.
  std::ofstream json(scratch.path / "mzpeak_index.json");
  json << R"({"files":[{"name":"spectra_data.parquet","entity_type":"spectrum",)"
       << R"("data_kind":"data_arrays","column_mapping":[],"parameters":[]}],)"
       << R"("metadata":{"version":"0.9.0"}})";
  json.close();

  return scratch.path;
}

} // namespace

/******************************************************************************/
// The fixture really does have more row groups than the cache holds.  Without
// this the tests below would silently degrade into single-group tests -- which
// is exactly how this gap went unnoticed in the first place.
BOOST_AUTO_TEST_CASE(fixture_has_more_row_groups_than_the_cache_holds)
{
  Scratch scratch("mzp-test-rowgroups-shape");
  const fs::path dir = write_fixture(scratch);

  auto reader = parquet::ParquetFileReader::OpenFile(
      (dir / "spectra_data.parquet").string(), false);
  const int groups = reader->metadata()->num_row_groups();

  // kCachedGroups is 2; anything above that exercises eviction.
  BOOST_TEST(groups > 2, "synthetic file must span >2 row groups, got " << groups);
  BOOST_TEST(reader->metadata()->num_rows() ==
             static_cast<int64_t>(kSpectra * kPoints));
}

/******************************************************************************/
// Every spectrum reads back correctly when the run spans many row groups, read
// in ascending order.  This walks the cache past its capacity repeatedly, so
// the eviction branch runs for real.
BOOST_AUTO_TEST_CASE(all_spectra_read_across_row_groups)
{
  Scratch scratch("mzp-test-rowgroups-forward");
  auto spectra = MzPeak::open(write_fixture(scratch)).spectra();

  BOOST_TEST_REQUIRE(spectra.size() == kSpectra);

  for (uint64_t s = 0; s < kSpectra; ++s) {
    auto spectrum = spectra[s];
    const auto& mz = spectrum.mz();
    const auto& intensity = spectrum.intensity();

    BOOST_TEST_REQUIRE(mz.size() == kPoints, "spectrum " << s);
    BOOST_TEST_REQUIRE(intensity.size() == kPoints, "spectrum " << s);

    for (uint64_t p = 0; p < kPoints; ++p) {
      BOOST_TEST(mz[p] == expected_mz(s, p), boost::test_tools::tolerance(1e-12));
      BOOST_TEST(intensity[p] == expected_intensity(s, p));
    }
  }
}

/******************************************************************************/
// The same reads in an order that defeats the cache: the access pattern jumps
// between distant row groups, so nearly every read evicts.  A cache that
// returned a stale entry, or an eviction that dropped a group still in use,
// shows up here as wrong VALUES rather than as a crash.
BOOST_AUTO_TEST_CASE(spectra_read_correctly_when_jumping_between_row_groups)
{
  Scratch scratch("mzp-test-rowgroups-jump");
  auto spectra = MzPeak::open(write_fixture(scratch)).spectra();

  // Alternate between the front and the back of the run.
  for (uint64_t i = 0; i < kSpectra / 2; ++i) {
    for (uint64_t s : {i, kSpectra - 1 - i}) {
      auto spectrum = spectra[s];
      const auto& mz = spectrum.mz();
      BOOST_TEST_REQUIRE(mz.size() == kPoints, "spectrum " << s);
      BOOST_TEST(mz.front() == expected_mz(s, 0),
                 boost::test_tools::tolerance(1e-12));
      BOOST_TEST(mz.back() == expected_mz(s, kPoints - 1),
                 boost::test_tools::tolerance(1e-12));
    }
  }
}

/******************************************************************************/
// Independent readers over one file, landing in DIFFERENT row groups.
//
// NOTE what this does and does not cover: each thread opens its OWN reader, so
// each has its own row-group cache and its own mutex -- nothing is contended
// here.  This is the documented usage pattern (one Spectra per thread) over a
// multi-row-group file, which nothing else tests; the shared-cache contention
// case is the test below it.
//
// Values are checked rather than mere absence of a crash: Parquet pages carry
// no CRC, so a bad read yields plausible wrong numbers rather than an error.
// A failure flag is reported once at the end (BOOST_TEST is not thread-safe).
BOOST_AUTO_TEST_CASE(concurrent_reads_across_row_groups_agree)
{
  Scratch scratch("mzp-test-rowgroups-threads");
  const fs::path dir = write_fixture(scratch);

  constexpr int kThreads = 8;
  std::atomic<int> mismatches{0};
  std::atomic<int> read_spectra{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      // One reader per thread, as the API requires; the archive-level cache
      // underneath is what is being contended.
      auto spectra = MzPeak::open(dir).spectra();

      // Each thread starts at a different offset so they sit in different row
      // groups at the same moment.
      for (uint64_t k = 0; k < kSpectra; ++k) {
        const uint64_t s = (k + static_cast<uint64_t>(t) * 7) % kSpectra;

        auto spectrum = spectra[s];
        const auto& mz = spectrum.mz();
        const auto& intensity = spectrum.intensity();

        if (mz.size() != kPoints || intensity.size() != kPoints) {
          ++mismatches;
          continue;
        }
        for (uint64_t p = 0; p < kPoints; ++p) {
          if (mz[p] != expected_mz(s, p) ||
              intensity[p] != expected_intensity(s, p)) {
            ++mismatches;
            break;
          }
        }
        ++read_spectra;
      }
    });
  }

  for (auto& thread : threads)
    thread.join();

  BOOST_TEST(mismatches.load() == 0);
  BOOST_TEST(read_spectra.load() == kThreads * static_cast<int>(kSpectra));
}

/******************************************************************************/
// ONE reader shared by reference across threads, each reading a DIFFERENT row
// group.
//
// This is the case `Parquet::cache_mutex_` actually exists for.  The cache and
// the mutex are per-Parquet members, so they are only contended when a single
// reader is used from several threads -- which the const read API permits by
// reference (Spectra is non-copyable, so sharing is exactly how it is done).
//
// Two failure modes it is aimed at, both silent: the 2-deep cache handing back
// a group another thread has since evicted, and two threads interleaving on the
// single shared file position during the decode.
BOOST_AUTO_TEST_CASE(one_shared_reader_across_threads_agrees)
{
  Scratch scratch("mzp-test-rowgroups-shared");
  auto index = MzPeak::open(write_fixture(scratch));
  auto spectra = index.spectra(); // ONE reader, shared below by reference.

  constexpr int kThreads = 8;
  std::atomic<int> mismatches{0};
  std::atomic<int> read_spectra{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&spectra, &mismatches, &read_spectra, t] {
      // Different starting offsets, so at any moment the threads are asking
      // for spectra that live in different row groups.
      for (uint64_t k = 0; k < kSpectra; ++k) {
        const uint64_t s = (k + static_cast<uint64_t>(t) * 7) % kSpectra;

        auto spectrum = spectra[s];
        const auto& mz = spectrum.mz();
        const auto& intensity = spectrum.intensity();

        if (mz.size() != kPoints || intensity.size() != kPoints) {
          ++mismatches;
          continue;
        }
        for (uint64_t p = 0; p < kPoints; ++p) {
          if (mz[p] != expected_mz(s, p) ||
              intensity[p] != expected_intensity(s, p)) {
            ++mismatches;
            break;
          }
        }
        ++read_spectra;
      }
    });
  }

  for (auto& thread : threads)
    thread.join();

  BOOST_TEST(mismatches.load() == 0);
  BOOST_TEST(read_spectra.load() == kThreads * static_cast<int>(kSpectra));
}
