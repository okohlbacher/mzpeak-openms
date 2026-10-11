#define BOOST_TEST_MODULE Partition
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/writer.h"

// Spectra::partitions() / Spectra::decode(): every spectrum arrives exactly
// once, in ascending index order, with the arrays the lazy Spectrum decode
// returns for it, for any partition count.

namespace {

// Point, chunked, Numpress, grid, null-marked profile, ion mobility, UV.
const char* const kFixtures[] = {
    "../test/files/small.mzpeak",
    "../test/files/small.chunked.mzpeak",
    "../test/files/small.numpress.mzpeak",
    "../test/files/small.dir",
    "../test/files/diapasef.dir",
    "../test/files/grid.dir",
    "../test/files/grid_mixed.dir",
    "../test/files/has_uv.mzpeak",
    "../test/files/Example_Processed.img.mzpeak",
};

struct Decoded {
  std::vector<uint64_t> order;
  std::vector<std::vector<double>> mz;
  std::vector<std::vector<float>> intensity;
};

// Bit for bit: value equality would accept -0.0 for 0.0 and reject equal NaNs.
template <typename T>
bool same_bits(const std::vector<T>& a, const std::vector<T>& b)
{
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

Decoded decode_all(const MzPeak::Spectra& spectra, std::size_t parts)
{
  Decoded d;
  d.mz.resize(spectra.size());
  d.intensity.resize(spectra.size());
  for (const auto& p : spectra.partitions(parts)) {
    spectra.decode(p, [&](uint64_t i, std::span<const double> mz,
                          std::span<const float> intensity) {
      d.order.push_back(i);
      d.mz[i].assign(mz.begin(), mz.end());
      d.intensity[i].assign(intensity.begin(), intensity.end());
    });
  }
  return d;
}

} // namespace

BOOST_AUTO_TEST_CASE(partitions_cover_the_run_once_in_order)
{
  for (const char* path : kFixtures) {
    BOOST_TEST_CONTEXT(path)
    {
      auto index = MzPeak::open(path);
      MzPeak::Spectra spectra = index.spectra();
      for (std::size_t parts :
           {std::size_t(1), std::size_t(3), std::size_t(7), std::size_t(1000)}) {
        const auto ps = spectra.partitions(parts);
        BOOST_TEST(ps.size() <= parts);
        uint64_t next = 0;
        for (const auto& p : ps) {
          BOOST_TEST(p.first_index == next);
          BOOST_TEST(p.last_index >= p.first_index);
          next = p.last_index + 1;
        }
        BOOST_TEST(next == spectra.size());
      }
    }
  }
}

// Under ThreadSanitizer the exhaustive comparison runs ~180x slower than in a
// release build and outlasts the 120 s test timeout.  There it covers one file
// per layout (point, chunked, grid); every file is compared in the other builds.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define MZPEAK_PARTITION_TEST_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define MZPEAK_PARTITION_TEST_TSAN 1
#endif

#ifdef MZPEAK_PARTITION_TEST_TSAN
const char* const kCompared[] = {"../test/files/small.numpress.mzpeak",
                                 "../test/files/grid.dir",
                                 "../test/files/Example_Processed.img.mzpeak"};
#else
const auto& kCompared = kFixtures;
#endif

BOOST_AUTO_TEST_CASE(decode_matches_the_lazy_spectrum_for_any_partition_count)
{
  for (const char* path : kCompared) {
    BOOST_TEST_CONTEXT(path)
    {
      auto index = MzPeak::open(path);
      MzPeak::Spectra spectra = index.spectra();

      // The lazy decode, once, as the reference.
      Decoded lazy;
      {
        MzPeak::Spectra reference = index.spectra();
        for (std::size_t i = 0; i < reference.size(); ++i) {
          auto s = reference[i];
          lazy.mz.push_back(s.mz());
          lazy.intensity.push_back(s.intensity());
        }
      }

      // One partition, and more partitions than the small files have row groups.
      for (std::size_t parts : {std::size_t(1), std::size_t(7)}) {
        BOOST_TEST_CONTEXT("partitions " << parts)
        {
          const Decoded d = decode_all(spectra, parts);
          BOOST_REQUIRE_EQUAL(d.order.size(), spectra.size());
          for (std::size_t k = 0; k < d.order.size(); ++k)
            BOOST_TEST(d.order[k] == k);
          for (std::size_t i = 0; i < spectra.size(); ++i) {
            BOOST_TEST(same_bits(d.mz[i], lazy.mz[i]), "m/z of spectrum " << i);
            BOOST_TEST(same_bits(d.intensity[i], lazy.intensity[i]),
                       "intensity of spectrum " << i);
          }
        }
      }
    }
  }
}

// One Spectra per thread over one Index, as the class documents.
BOOST_AUTO_TEST_CASE(partitions_decode_concurrently_to_the_same_result)
{
  auto index = MzPeak::open("../test/files/small.chunked.mzpeak");
  MzPeak::Spectra serial_spectra = index.spectra();
  const Decoded serial = decode_all(serial_spectra, 1);

  const auto ps = serial_spectra.partitions(4);
  std::vector<Decoded> per(ps.size());
  std::vector<std::thread> pool;
  for (std::size_t t = 0; t < ps.size(); ++t) {
    pool.emplace_back([&, t] {
      MzPeak::Spectra mine = index.spectra();
      per[t].mz.resize(mine.size());
      per[t].intensity.resize(mine.size());
      mine.decode(ps[t], [&](uint64_t i, std::span<const double> mz,
                             std::span<const float> intensity) {
        per[t].order.push_back(i);
        per[t].mz[i].assign(mz.begin(), mz.end());
        per[t].intensity[i].assign(intensity.begin(), intensity.end());
      });
    });
  }
  for (auto& th : pool)
    th.join();

  // Each worker delivered exactly its partition, in order; together, every
  // spectrum once.
  std::vector<int> times(serial_spectra.size(), 0);
  std::size_t seen = 0;
  for (std::size_t t = 0; t < ps.size(); ++t) {
    BOOST_REQUIRE_EQUAL(per[t].order.size(),
                        ps[t].last_index - ps[t].first_index + 1);
    for (std::size_t k = 0; k < per[t].order.size(); ++k)
      BOOST_TEST(per[t].order[k] == ps[t].first_index + k);
    for (uint64_t i : per[t].order) {
      ++times[i];
      ++seen;
      BOOST_TEST(same_bits(per[t].mz[i], serial.mz[i]));
      BOOST_TEST(same_bits(per[t].intensity[i], serial.intensity[i]));
    }
  }
  BOOST_TEST(seen == serial_spectra.size());
  BOOST_TEST(std::ranges::all_of(times, [](int n) { return n == 1; }));
}

// Spectra with no peaks have metadata rows but no signal rows.  They must still
// be delivered, in place: a consumer that counts scan cycles shifts otherwise.
// None of the bundled fixtures has one, so write a run that does.
BOOST_AUTO_TEST_CASE(spectra_without_peaks_are_delivered_in_place)
{
  namespace fs = std::filesystem;
  const fs::path path =
      fs::temp_directory_path() /
      ("mzpeak_partition_test_empty_" + std::to_string(::getpid()) + ".mzpeak");
  std::error_code ec;
  fs::remove(path, ec);

  std::vector<MzPeak::SpectrumData> in(6);
  in[0] = {{100.0, 200.0}, {1.0f, 2.0f}, /*centroid=*/true};
  in[1] = {{}, {}, true, 2}; // empty MS2
  in[2] = {{150.0}, {3.0f}, true, 2};
  in[3] = {{110.0, 120.0, 130.0}, {4.0f, 5.0f, 6.0f}, /*centroid=*/false};
  in[4] = {{}, {}, true, 2}; // empty MS2
  in[5] = {{}, {}, true, 2}; // empty, last
  MzPeak::write_spectra_archive(path, in);

  auto index = MzPeak::open(path);
  MzPeak::Spectra spectra = index.spectra();
  BOOST_REQUIRE_EQUAL(spectra.size(), in.size());
  for (std::size_t parts :
       {std::size_t(1), std::size_t(2), std::size_t(4), std::size_t(6)}) {
    BOOST_TEST_CONTEXT("partitions " << parts)
    {
      const Decoded d = decode_all(spectra, parts);
      BOOST_REQUIRE_EQUAL(d.order.size(), in.size());
      for (std::size_t i = 0; i < in.size(); ++i) {
        BOOST_TEST(d.order[i] == i);
        BOOST_TEST(d.mz[i] == in[i].mz);
        BOOST_TEST(d.intensity[i] == in[i].intensity);
      }
    }
  }
  fs::remove(path, ec);
}
