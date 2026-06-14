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

/// Drive the full reader API over one fixture and assert internal consistency.
/// `expected` is the known spectrum count when we have one (nullopt = just
/// require a non-empty file).
void exercise_reader(const std::string& path, std::optional<std::size_t> expected)
{
  MzPeak::Index index = MzPeak::open(path);
  MzPeak::Spectra spectra = index.spectra();

  const std::size_t n = spectra.size();
  BOOST_TEST(n > 0u);
  if (expected) BOOST_TEST(n == *expected);

  // Every spectrum must read with paired m/z and intensity arrays.  This is
  // the one full decode pass over the fixture; capture the retention times so
  // the EIC check below need not decode everything a second time.
  std::size_t with_time = 0;
  std::string first_id;
  std::size_t first_mz_size = 0;
  std::vector<double> times;
  for (std::size_t i = 0; i < n; ++i) {
    auto s = spectra[i];
    BOOST_TEST(s.mz().size() == s.intensity().size());
    if (i == 0) {
      first_id = s.metadata().id;
      first_mz_size = s.mz().size();
      BOOST_TEST(s.metadata().index == 0u);
    }
    if (s.retention_time().has_value()) {
      ++with_time;
      times.push_back(s.retention_time().value());
    }
  }

  // RDR-15: by-id access round-trips when ids are present.
  if (!first_id.empty()) {
    auto idx = spectra.index_for_id(first_id);
    BOOST_TEST(idx.has_value());
    if (idx) {
      BOOST_TEST(*idx == 0u);
      auto s = spectra.by_id(first_id);
      BOOST_TEST(s.mz().size() == first_mz_size);
    }
    // An id that cannot exist yields nullopt (and by_id throws).
    BOOST_TEST(!spectra.index_for_id("\x01 no such id").has_value());
    BOOST_CHECK_THROW(spectra.by_id("\x01 no such id"), std::exception);
  }

  // RDR-16: a fully-open RT range selects exactly the time-bearing spectra,
  // in ascending order.
  auto in_range = spectra.indices_in_time_range(-1e30, 1e30);
  BOOST_TEST(in_range.size() == with_time);
  BOOST_TEST(std::ranges::is_sorted(in_range));

  // RDR-17: an EIC over a narrow RT window (derived from the times captured
  // above, so we decode only a handful of scans) yields exactly the scans the
  // RT index selects for the same window, in ascending time order.  The full
  // m/z range makes it a total-ion trace over those scans.
  if (!times.empty()) {
    std::ranges::sort(times);
    double lo = times.front();
    double hi = times[std::min<std::size_t>(2, times.size() - 1)];
    auto window = spectra.indices_in_time_range(lo, hi);
    auto eic = spectra.extract_ion_chromatogram(-1e30, 1e30, lo, hi);
    BOOST_TEST(eic.size() == window.size());
    for (std::size_t i = 1; i < eic.size(); ++i) {
      BOOST_TEST(eic[i - 1].time <= eic[i].time);
    }
  }

  // RDR-18: batch read preserves input order and bounds out-of-range indices.
  std::vector<std::size_t> req{n - 1, 0u, n - 1};
  auto batch = spectra.get_spectra_batch(req);
  BOOST_TEST(batch.size() == req.size());
  {
    auto s0 = spectra[0];
    auto slast = spectra[n - 1];
    BOOST_TEST(batch[0].mz().size() == slast.mz().size());
    BOOST_TEST(batch[1].mz().size() == s0.mz().size());
  }
  auto oob = spectra.get_spectra_batch({n + 100});
  BOOST_TEST(oob.size() == 1u);
  BOOST_TEST(oob[0].mz().empty());

  // NOTE: the auxiliary chromatogram / wavelength tables are validated in the
  // per-fixture cases below rather than here, because their reader support is
  // currently uneven across fixtures (see docs/reader-backlog.md, RDR-28):
  // chunked-layout chromatograms now decode (small.chunked / small.numpress,
  // RDR-28a), as do point chromatograms including has_uv's multi-intensity
  // case (RDR-29).  Chunked-layout WAVELENGTH spectra remain deferred
  // (RDR-28b).  This e2e codifies the capability that exists today.
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
