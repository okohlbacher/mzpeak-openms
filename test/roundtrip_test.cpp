/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE RoundTrip
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
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

// Everything the writer accepts for one spectrum, lifted from what the reader
// returns for it.  What this does NOT carry is the point of the reverse test
// below: if a field is dropped here it is dropped in the file too.
MzPeak::SpectrumData to_spectrum_data(const MzPeak::Spectrum& s)
{
  MzPeak::SpectrumData sd;
  sd.mz = s.mz();
  sd.intensity = s.intensity();
  sd.centroid = s.metadata().representation == "MS:1000127";
  sd.ms_level = s.ms_level();
  sd.retention_time = s.retention_time();
  sd.polarity = s.metadata().polarity;
  sd.id = s.metadata().id;
  for (const auto& p : s.precursors()) {
    MzPeak::PrecursorData pd;
    pd.isolation_target_mz = p.isolation_window.target_mz;
    pd.isolation_lower_offset = p.isolation_window.lower_offset;
    pd.isolation_upper_offset = p.isolation_window.upper_offset;
    for (const auto& ion : p.selected_ions)
      pd.selected_ions.push_back({ion.selected_ion_mz, ion.charge_state, ion.intensity});
    sd.precursors.push_back(std::move(pd));
  }
  return sd;
}

} // namespace

/******************************************************************************/
// Precursors round-trip through BOTH entry points: the isolation window, each
// selected ion's m/z, charge and intensity, a window-only precursor (DIA), two
// ions under one precursor, and none at all for MS1.
BOOST_AUTO_TEST_CASE(precursors_round_trip)
{
  using namespace MzPeak;

  SpectrumData ms1;
  ms1.mz = {100.0, 200.0};
  ms1.intensity = {1.0f, 2.0f};
  ms1.centroid = true;
  ms1.ms_level = 1;

  SpectrumData ms2;
  ms2.mz = {150.0, 250.0};
  ms2.intensity = {3.0f, 4.0f};
  ms2.centroid = true;
  ms2.ms_level = 2;
  PrecursorData p;
  p.isolation_target_mz = 500.25f;
  p.isolation_lower_offset = 1.0f;
  p.isolation_upper_offset = 1.5f;
  p.selected_ions.push_back({500.2501, 2, 1234.5f});
  ms2.precursors.push_back(p);

  SpectrumData dia;
  dia.mz = {300.0};
  dia.intensity = {5.0f};
  dia.centroid = true;
  dia.ms_level = 2;
  PrecursorData two_ions;
  two_ions.isolation_target_mz = 600.0f;
  two_ions.selected_ions.push_back({599.9, 1, std::nullopt});
  two_ions.selected_ions.push_back({600.1, 3, 7.0f});
  PrecursorData window_only;
  window_only.isolation_target_mz = 700.0f;
  window_only.isolation_lower_offset = 12.5f;
  window_only.isolation_upper_offset = 12.5f;
  dia.precursors = {two_ions, window_only};

  const std::vector<SpectrumData> in{ms1, ms2, dia};

  auto verify = [](const std::string& path) {
    Index index = MzPeak::open(path);
    Spectra spectra = index.spectra();
    BOOST_TEST_REQUIRE(spectra.size() == 3u);

    auto s0 = spectra[0];
    BOOST_TEST(s0.precursors().empty());

    auto s1 = spectra[1];
    BOOST_TEST_REQUIRE(s1.precursors().size() == 1u);
    const auto& q = s1.precursors()[0];
    BOOST_TEST_REQUIRE(q.isolation_window.target_mz.has_value());
    BOOST_TEST(*q.isolation_window.target_mz == 500.25f);
    BOOST_TEST(*q.isolation_window.lower_offset == 1.0f);
    BOOST_TEST(*q.isolation_window.upper_offset == 1.5f);
    BOOST_TEST_REQUIRE(q.selected_ions.size() == 1u);
    BOOST_TEST(*q.selected_ions[0].selected_ion_mz == 500.2501,
               boost::test_tools::tolerance(1e-9));
    BOOST_TEST(*q.selected_ions[0].charge_state == 2);
    BOOST_TEST(*q.selected_ions[0].intensity == 1234.5f);

    auto s2 = spectra[2];
    BOOST_TEST_REQUIRE(s2.precursors().size() == 2u);
    const auto& a = s2.precursors()[0];
    BOOST_TEST_REQUIRE(a.selected_ions.size() == 2u);
    BOOST_TEST(*a.selected_ions[0].selected_ion_mz == 599.9,
               boost::test_tools::tolerance(1e-9));
    BOOST_TEST(*a.selected_ions[0].charge_state == 1);
    BOOST_TEST(!a.selected_ions[0].intensity.has_value());
    BOOST_TEST(*a.selected_ions[1].charge_state == 3);
    const auto& b = s2.precursors()[1];
    BOOST_TEST(b.selected_ions.empty());
    BOOST_TEST(*b.isolation_window.target_mz == 700.0f);
    BOOST_TEST(*b.isolation_window.lower_offset == 12.5f);
  };

  TempDir dir;
  write_spectra_directory(dir.path, in);
  verify(dir.path.string());

  TempDir zip;
  RunContents run;
  run.spectra = in;
  const fs::path archive = zip.path.string() + ".mzpeak";
  write_run_archive(archive, run);
  verify(archive.string());
  std::error_code ec;
  fs::remove(archive, ec);
}

/******************************************************************************/
// Reverse round trip on a real DDA archive: every precursor and selected ion
// the reader finds must come back identical after write -> read.  The fixture
// is asserted to actually contain selected ions, so this cannot pass vacuously.
BOOST_AUTO_TEST_CASE(reader_writer_reader_preserves_precursors)
{
  using namespace MzPeak;
  const std::string source("../test/files/v2/small.mzpeak");

  std::vector<SpectrumData> ref;
  {
    Index index = MzPeak::open(source);
    Spectra spectra = index.spectra();
    for (std::size_t i = 0; i < spectra.size(); ++i) ref.push_back(to_spectrum_data(spectra[i]));
  }
  std::size_t ions = 0;
  for (const auto& s : ref)
    for (const auto& p : s.precursors) ions += p.selected_ions.size();
  BOOST_TEST_REQUIRE(ions > 0u);

  TempDir dir;
  RunContents run;
  run.spectra = ref;
  write_run_directory(dir.path, run);

  Index index = MzPeak::open(dir.path.string());
  Spectra out = index.spectra();
  BOOST_TEST_REQUIRE(out.size() == ref.size());
  for (std::size_t i = 0; i < ref.size(); ++i) {
    auto s = out[i];
    const auto& got = s.precursors();
    BOOST_TEST_REQUIRE(got.size() == ref[i].precursors.size());
    for (std::size_t k = 0; k < got.size(); ++k) {
      const auto& want = ref[i].precursors[k];
      BOOST_TEST((got[k].isolation_window.target_mz == want.isolation_target_mz));
      BOOST_TEST((got[k].isolation_window.lower_offset == want.isolation_lower_offset));
      BOOST_TEST((got[k].isolation_window.upper_offset == want.isolation_upper_offset));
      BOOST_TEST_REQUIRE(got[k].selected_ions.size() == want.selected_ions.size());
      for (std::size_t j = 0; j < want.selected_ions.size(); ++j) {
        BOOST_TEST((got[k].selected_ions[j].selected_ion_mz == want.selected_ions[j].mz));
        BOOST_TEST((got[k].selected_ions[j].charge_state == want.selected_ions[j].charge));
        BOOST_TEST((got[k].selected_ions[j].intensity == want.selected_ions[j].intensity));
      }
    }
  }
}

/******************************************************************************/
// ms_level round-trips through write → read.
// retention_time and polarity are written to the Parquet metadata table and
// read back through Spectrum::metadata().
//
// The retention-time assertion is the point of this test: SpectrumData carries
// SECONDS, the format stores MINUTES, and the reader converts on the way back.
// Writing the seconds value straight into the minutes column made every round
// trip 60x too large, and this test previously wrote 120.5 without ever
// asserting what came back — so it passed throughout.
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

  BOOST_TEST(spectra[0].ms_level() == 1u);
  BOOST_TEST(spectra[1].ms_level() == 2u);

  auto s0 = spectra[0];
  auto s1 = spectra[1];
  BOOST_TEST_REQUIRE(s0.retention_time().has_value());
  BOOST_TEST_REQUIRE(s1.retention_time().has_value());
  // Seconds in, seconds out — not 7230 and 14400.
  BOOST_TEST(std::abs(*s0.retention_time() - 120.5) < 1e-3);
  BOOST_TEST(std::abs(*s1.retention_time() - 240.0) < 1e-3);

  BOOST_TEST((s0.metadata().polarity == std::optional<int>(1)));
  BOOST_TEST((s1.metadata().polarity == std::optional<int>(-1)));
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
