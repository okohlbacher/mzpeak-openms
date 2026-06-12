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
  ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
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
    for (std::size_t j = 0; j < order.size(); ++j) order[j] = j;
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
