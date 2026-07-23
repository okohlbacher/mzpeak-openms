/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

// Full end-to-end integration test: drive the ENTIRE public reader API over
// every bundled fixture in one pass (point / chunked / numpress / wavelength /
// imaging layouts), then close the loop with a reverse round trip through the
// writer (read -> write archive -> read -> compare).  Where the per-feature
// unit tests each pin one decoder against ground truth, this test guards the
// system: that the whole API surface stays mutually consistent on real files
// across all layouts, and that the writer faithfully re-encodes whatever the
// reader decodes.

#define BOOST_TEST_MODULE E2E
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"
#include "mzpeak/writer.h"

namespace fs = std::filesystem;
using boost::test_tools::tolerance;

namespace {

/// Self-cleaning temp path for a written-back archive.
struct TempFile {
  fs::path path;
  TempFile()
      : path(fs::temp_directory_path() /
             ("mzpeak_e2e_" + std::to_string(next_id()) + ".mzpeak"))
  {
    std::error_code ec;
    fs::remove(path, ec);
  }
  ~TempFile()
  {
    std::error_code ec;
    fs::remove(path, ec);
  }
  static unsigned next_id()
  {
    static std::atomic<unsigned> c{0};
    return c.fetch_add(1);
  }
};

/// A spectrum's m/z+intensity, sorted ascending by m/z -- the canonical form
/// the writer emits, so the round-trip comparison is order-independent.
MzPeak::SpectrumData sorted_copy(const MzPeak::Spectrum& s)
{
  const auto& mz = s.mz();
  const auto& it = s.intensity();
  std::vector<std::size_t> order(mz.size());
  for (std::size_t j = 0; j < order.size(); ++j)
    order[j] = j;
  std::ranges::sort(order,
                    [&](std::size_t a, std::size_t b) { return mz[a] < mz[b]; });

  MzPeak::SpectrumData out;
  out.mz.reserve(mz.size());
  out.intensity.reserve(it.size());
  for (std::size_t k : order) {
    out.mz.push_back(mz[k]);
    out.intensity.push_back(it[k]);
  }
  return out;
}

/// Read the first `k` spectra of a file into the canonical sorted form.
/// Bounded on purpose: decoding every point of the large profile fixtures
/// would dominate the suite runtime, and the round trip only re-writes `k`.
std::vector<MzPeak::SpectrumData> read_first_sorted(const std::string& path,
                                                    std::size_t k)
{
  MzPeak::Index index = MzPeak::open(path);
  MzPeak::Spectra spectra = index.spectra();
  const std::size_t n = std::min(k, spectra.size());
  std::vector<MzPeak::SpectrumData> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    auto s = spectra[i]; // operator[] returns by value -- hold it.
    out.push_back(sorted_copy(s));
  }
  return out;
}

/// Drive the reader API over one fixture and assert internal consistency.
/// `expected` is the known spectrum count when we have one (nullopt = just
/// require a non-empty file).
void exercise_reader(const std::string& path, std::optional<std::size_t> expected)
{
  MzPeak::Index index = MzPeak::open(path);
  MzPeak::Spectra spectra = index.spectra();

  const std::size_t n = spectra.size();
  BOOST_TEST(n > 0u);
  if (expected) BOOST_TEST(n == *expected);

  for (std::size_t i = 0; i < n; ++i) {
    auto s = spectra[i];
    BOOST_TEST(s.mz().size() == s.intensity().size());
  }
}

/// Reverse round trip: read a fixture, write the first `cap` spectra back out
/// as a C++ archive, read them again, and confirm the values survive.  Bounded
/// because writing every point of the large profile fixtures would dominate the
/// suite runtime; the full-content round trip lives in roundtrip_test.
void exercise_round_trip(const std::string& path, std::size_t cap)
{
  std::vector<MzPeak::SpectrumData> subset = read_first_sorted(path, cap);
  BOOST_TEST(!subset.empty());

  TempFile tmp;
  MzPeak::write_spectra_archive(tmp.path, subset);

  std::vector<MzPeak::SpectrumData> out =
      read_first_sorted(tmp.path.string(), subset.size());
  BOOST_TEST(out.size() == subset.size());

  for (std::size_t i = 0; i < subset.size(); ++i) {
    BOOST_TEST(out[i].mz.size() == subset[i].mz.size());
    BOOST_TEST(out[i].intensity.size() == subset[i].intensity.size());
    const std::size_t m = std::min(out[i].mz.size(), subset[i].mz.size());
    for (std::size_t j = 0; j < m; ++j) {
      BOOST_TEST(out[i].mz[j] == subset[i].mz[j], tolerance(1e-9));
      BOOST_TEST(out[i].intensity[j] == subset[i].intensity[j], tolerance(1e-6f));
    }
  }
}

} // namespace

/******************************************************************************/
// Point layout (zip archive) -- the canonical small file.
BOOST_AUTO_TEST_CASE(point_archive)
{
  const std::string path("../test/files/small.mzpeak");
  exercise_reader(path, 48u);
  exercise_round_trip(path, 8u);

  // Point chromatograms are fully supported here: a single TIC with one time
  // value per spectrum.
  auto chroms = MzPeak::open(path).chromatograms();
  BOOST_TEST(chroms.size() == 1u);
  auto c = chroms[0];
  BOOST_TEST(c.time().size() == 48u);
  BOOST_TEST(c.intensity().size() == 48u);
}

/******************************************************************************/
// Point layout (unpacked directory) -- same content, directory container.
BOOST_AUTO_TEST_CASE(point_directory)
{
  exercise_reader("../test/files/small.dir", 48u);
  exercise_round_trip("../test/files/small.dir", 8u);
}

/******************************************************************************/
// Chunked layout (delta / basic encoded chunks).
BOOST_AUTO_TEST_CASE(chunked)
{
  exercise_reader("../test/files/small.chunked.mzpeak", 48u);
  exercise_round_trip("../test/files/small.chunked.mzpeak", 8u);

  // RDR-28a: chunked chromatograms now read.  Single TIC, 48 paired samples.
  auto chroms = MzPeak::open("../test/files/small.chunked.mzpeak").chromatograms();
  BOOST_TEST(chroms.size() == 1u);
  auto c = chroms[0];
  BOOST_TEST(c.time().size() == c.intensity().size());
  BOOST_TEST(c.time().size() == 48u);
  BOOST_TEST(c.intensity().front() == 15245068.0f, tolerance(1e-3f));
}

/******************************************************************************/
// Numpress layout (linear m/z + SLOF intensity).
BOOST_AUTO_TEST_CASE(numpress)
{
  exercise_reader("../test/files/small.numpress.mzpeak", 48u);
  exercise_round_trip("../test/files/small.numpress.mzpeak", 8u);

  // RDR-28a: chunked chromatogram with numpress-SLOF intensity now reads.
  // Same underlying TIC as small.chunked; SLOF is lossy so use a relative
  // tolerance on intensity.
  auto chroms = MzPeak::open("../test/files/small.numpress.mzpeak").chromatograms();
  BOOST_TEST(chroms.size() == 1u);
  auto c = chroms[0];
  BOOST_TEST(c.time().size() == c.intensity().size());
  BOOST_TEST(c.time().size() == 48u);
  BOOST_TEST(c.intensity().front() == 15245068.0f, tolerance(2e-3f));
}

/******************************************************************************/
// UV file: mass spectra plus a wavelength-spectra table.
BOOST_AUTO_TEST_CASE(has_uv)
{
  const std::string path("../test/files/has_uv.mzpeak");
  exercise_reader(path, std::nullopt);
  exercise_round_trip(path, 8u);

  // Wavelength spectra are fully supported: 520 spectra, the first spanning
  // 210..400 nm in 96 samples with paired intensity.
  auto wls = MzPeak::open(path).wavelength_spectra();
  BOOST_TEST(wls.size() == 520u);
  auto w = wls[0];
  BOOST_TEST(w.wavelength().size() == 96u);
  BOOST_TEST(w.intensity().size() == 96u);

  // Point chromatograms now decode intensity as well as time (RDR-29).  This
  // fixture has two chromatograms that each store their intensity in a
  // different physical "intensity array" column (both tagged MS:1000515): the
  // first uses the primary `intensity` counts column, the second the secondary
  // `intensity_f32_au` absorbance column.  intensity() coalesces these so every
  // chromatogram reads paired time/intensity arrays.
  auto chroms = MzPeak::open(path).chromatograms();
  BOOST_TEST(chroms.size() == 2u);
  for (std::size_t i = 0; i < chroms.size(); ++i) {
    auto c = chroms[i];
    BOOST_TEST(c.time().size() == c.intensity().size());
  }

  // Ground-truth values (pyarrow over chromatograms_data.parquet):
  //   chrom 0: 212 points, primary `intensity` (counts)  [0]=168514.375
  //   chrom 1: 526 points, `intensity_f32_au` (absorbance) [0]=-0.02479553
  auto c0 = chroms[0];
  BOOST_TEST(c0.time().size() == 212u);
  BOOST_TEST(c0.intensity().size() == 212u);
  BOOST_TEST(c0.intensity().front() == 168514.375f, tolerance(1e-3f));
  BOOST_TEST(c0.intensity().back() == 166080.78125f, tolerance(1e-3f));

  auto c1 = chroms[1];
  BOOST_TEST(c1.time().size() == 526u);
  BOOST_TEST(c1.intensity().size() == 526u);
  BOOST_TEST(c1.intensity().front() == -0.0247955322f, tolerance(1e-6f));
  BOOST_TEST(c1.intensity().back() == -0.0214576721f, tolerance(1e-6f));
}

/******************************************************************************/
// Imaging file: small, centroid; round-trip the whole thing.
BOOST_AUTO_TEST_CASE(imaging)
{
  exercise_reader("../test/files/Example_Processed.img.mzpeak", 9u);
  exercise_round_trip("../test/files/Example_Processed.img.mzpeak", 9u);
}
