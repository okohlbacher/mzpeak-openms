/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE RoundTrip
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <vector>

#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"
#include "mzpeak/writer.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  TempDir()
      : path(fs::temp_directory_path() /
             ("mzpeak_roundtrip_" + std::to_string(next_id())))
  {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  static unsigned next_id()
  {
    static std::atomic<unsigned> c{0};
    return c.fetch_add(1);
  }
};

// Read every spectrum's m/z + intensity, with each spectrum sorted by
// ascending m/z (the writer sorts, so this is the canonical comparison form).
std::vector<MzPeak::SpectrumData> read_all_sorted(const std::string& path)
{
  MzPeak::Index index = MzPeak::open(path);
  MzPeak::Spectra spectra = index.spectra();

  std::vector<MzPeak::SpectrumData> out;
  out.reserve(spectra.size());

  for (std::size_t i = 0; i < spectra.size(); ++i) {
    const auto& s = spectra[i];
    const auto& mz = s.mz();
    const auto& it = s.intensity();

    std::vector<std::size_t> order(mz.size());
    for (std::size_t j = 0; j < order.size(); ++j)
      order[j] = j;
    std::ranges::sort(order,
                      [&](std::size_t a, std::size_t b) { return mz[a] < mz[b]; });

    MzPeak::SpectrumData sd;
    sd.mz.reserve(mz.size());
    sd.intensity.reserve(it.size());
    for (std::size_t k : order) {
      sd.mz.push_back(mz[k]);
      sd.intensity.push_back(it[k]);
    }
    out.push_back(std::move(sd));
  }
  return out;
}

} // namespace

/******************************************************************************/
// Retention time, polarity, and ms_level round-trip through write → read.
BOOST_AUTO_TEST_CASE(metadata_fields_round_trip)
{
  using namespace MzPeak;

  std::vector<SpectrumData> in{
      {{100.0, 200.0},
       {1.0f, 2.0f},
       /*centroid=*/false,
       /*ms_level=*/1,
       /*retention_time=*/120.5,
       /*polarity=*/1},
      {{300.0, 400.0},
       {3.0f, 4.0f},
       /*centroid=*/false,
       /*ms_level=*/2,
       /*retention_time=*/240.0,
       /*polarity=*/-1},
  };

  TempDir dir;
  write_spectra_directory(dir.path, in);

  MzPeak::Index index = MzPeak::open(dir.path.string());
  MzPeak::Spectra spectra = index.spectra();

  BOOST_TEST(spectra.size() == 2u);

  const auto& s0 = spectra[0];
  BOOST_TEST((s0.ms_level().has_value() && *s0.ms_level() == 1));
  BOOST_TEST((s0.retention_time().has_value() && *s0.retention_time() == 120.5),
             boost::test_tools::tolerance(1e-9));
  BOOST_TEST((s0.polarity().has_value() && *s0.polarity() == 1));

  const auto& s1 = spectra[1];
  BOOST_TEST((s1.ms_level().has_value() && *s1.ms_level() == 2));
  BOOST_TEST((s1.retention_time().has_value() && *s1.retention_time() == 240.0),
             boost::test_tools::tolerance(1e-9));
  BOOST_TEST((s1.polarity().has_value() && *s1.polarity() == -1));
}

/******************************************************************************/
// All-centroid round-trip: centroid spectra go to spectra_peaks.parquet
// and read back with correct m/z and intensity values.
BOOST_AUTO_TEST_CASE(centroid_only_round_trip)
{
  using namespace MzPeak;

  std::vector<SpectrumData> in{
      {{100.0, 200.0}, {1.0f, 2.0f}, /*centroid=*/true},
      {{150.0, 250.0, 350.0}, {3.0f, 4.0f, 5.0f}, /*centroid=*/true},
  };

  TempDir dir;
  write_spectra_directory(dir.path, in);

  std::vector<SpectrumData> out = read_all_sorted(dir.path.string());

  BOOST_TEST(out.size() == 2u);

  BOOST_TEST(out[0].mz.size() == 2u);
  BOOST_TEST(out[0].mz[0] == 100.0, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(out[0].mz[1] == 200.0, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(out[0].intensity[0] == 1.0f, boost::test_tools::tolerance(1e-6f));
  BOOST_TEST(out[0].intensity[1] == 2.0f, boost::test_tools::tolerance(1e-6f));

  BOOST_TEST(out[1].mz.size() == 3u);
  BOOST_TEST(out[1].mz[0] == 150.0, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(out[1].mz[2] == 350.0, boost::test_tools::tolerance(1e-9));
}

/******************************************************************************/
// Spectrum ID round-trip: custom id survives; absent id auto-generates "index=N".
BOOST_AUTO_TEST_CASE(spectrum_id_round_trip)
{
  using namespace MzPeak;

  std::vector<SpectrumData> in{
      {{100.0}, {1.0f}, false, 1, {}, {}, std::string("my_spectrum_42")},
      {{200.0}, {2.0f}}, // no id → auto
  };

  TempDir dir;
  write_spectra_directory(dir.path, in);

  MzPeak::Index index = MzPeak::open(dir.path.string());
  MzPeak::Spectra spectra = index.spectra();

  BOOST_TEST(spectra.size() == 2u);
  BOOST_TEST(spectra[0].metadata().id == "my_spectrum_42");
  BOOST_TEST(spectra[1].metadata().id == "index=1");
}

/******************************************************************************/
// Reverse round trip: read a real reference file with the reader, write it
// back with the writer, read it again, and confirm the spectra are
// preserved.  Exercises reader -> writer -> reader on genuine data.
BOOST_AUTO_TEST_CASE(reader_writer_reader_preserves_spectra)
{
  const std::string source("../test/files/Example_Processed.img.mzpeak");

  std::vector<MzPeak::SpectrumData> ref(read_all_sorted(source));
  BOOST_TEST(ref.size() == 9u); // known content of the bundled imaging file

  TempDir dir;
  MzPeak::write_spectra_directory(dir.path, ref);

  std::vector<MzPeak::SpectrumData> out(read_all_sorted(dir.path.string()));

  BOOST_TEST(out.size() == ref.size());
  for (std::size_t i = 0; i < ref.size(); ++i) {
    BOOST_TEST(out[i].mz.size() == ref[i].mz.size());
    BOOST_TEST(out[i].intensity.size() == ref[i].intensity.size());
    for (std::size_t j = 0; j < ref[i].mz.size(); ++j) {
      BOOST_TEST(out[i].mz[j] == ref[i].mz[j], boost::test_tools::tolerance(1e-9));
      BOOST_TEST(out[i].intensity[j] == ref[i].intensity[j],
                 boost::test_tools::tolerance(1e-6f));
    }
  }
}
