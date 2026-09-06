/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

// RunArchiveWriter: spectra appended one at a time land row group by row
// group and read back exactly as the one-shot writer's do.

#define BOOST_TEST_MODULE StreamWriter
#include <boost/test/included/unit_test.hpp>

#include <boost/json.hpp>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "mzpeak/exception.h"
#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"
#include "mzpeak/util/manager.h"
#include "mzpeak/writer.h"

namespace fs = std::filesystem;

namespace {

/// A scratch archive path removed on scope exit, with its working directory.
struct Scratch {
  fs::path path;
  explicit Scratch(const char* name) : path(fs::temp_directory_path() / name)
  {
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove_all(tmp(), ec);
  }
  ~Scratch()
  {
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove_all(tmp(), ec);
  }
  fs::path tmp() const
  {
    fs::path t = path;
    t += ".tmp";
    return t;
  }
};

constexpr std::size_t kSpectra = 30;

/// Spectrum i: 5 + i points, centroided when i is odd, an MS2 with one
/// precursor when i is odd, points deliberately NOT in m/z order.
MzPeak::SpectrumData make_spectrum(std::size_t i)
{
  MzPeak::SpectrumData s;
  const std::size_t n = 5 + i;
  for (std::size_t k = 0; k < n; ++k) {
    // descending, so the writer has to sort
    s.mz.push_back(100.0 + static_cast<double>(n - k) + static_cast<double>(i) / 100.0);
    s.intensity.push_back(static_cast<float>(1000 * i + k));
  }
  s.centroid = (i % 2) == 1;
  s.ms_level = s.centroid ? 2 : 1;
  s.retention_time = 10.0 * static_cast<double>(i);
  s.polarity = 1;
  s.id = "scan=" + std::to_string(i + 1);
  if (s.centroid) {
    MzPeak::PrecursorData p;
    p.isolation_target_mz = static_cast<float>(400 + i);
    p.isolation_lower_offset = 1.0f;
    p.isolation_upper_offset = 1.0f;
    MzPeak::SelectedIonData ion;
    ion.mz = 400.5 + static_cast<double>(i);
    ion.charge = 2;
    p.selected_ions.push_back(ion);
    s.precursors.push_back(p);
  }
  return s;
}

std::vector<MzPeak::SpectrumData> all_spectra()
{
  std::vector<MzPeak::SpectrumData> v;
  for (std::size_t i = 0; i < kSpectra; ++i) v.push_back(make_spectrum(i));
  return v;
}

/// Each spectrum read back against its source: sorted points, level, time,
/// id, representation and the precursor.
void check_archive(const fs::path& path)
{
  auto index = MzPeak::open(path);
  auto spectra = index.spectra();
  BOOST_TEST_REQUIRE(spectra.size() == kSpectra);
  for (std::size_t i = 0; i < kSpectra; ++i) {
    const MzPeak::SpectrumData want = make_spectrum(i);
    auto got = spectra[i];
    const auto& mz = got.mz();
    const auto& intensity = got.intensity();
    BOOST_TEST_REQUIRE(mz.size() == want.mz.size());
    BOOST_TEST_REQUIRE(intensity.size() == want.intensity.size());
    // ascending m/z on disk: the source's points reversed
    for (std::size_t k = 0; k < mz.size(); ++k) {
      const std::size_t src = mz.size() - 1 - k;
      BOOST_TEST(mz[k] == want.mz[src], boost::test_tools::tolerance(1e-9));
      BOOST_TEST(intensity[k] == want.intensity[src]);
    }
    BOOST_TEST(got.ms_level() == want.ms_level);
    BOOST_TEST_REQUIRE(got.metadata().retention_time.has_value());
    BOOST_TEST(*got.metadata().retention_time == *want.retention_time,
               boost::test_tools::tolerance(1e-9));
    BOOST_TEST(got.metadata().id == *want.id);
    BOOST_TEST(got.metadata().representation == (want.centroid ? "MS:1000127" : "MS:1000128"));
    BOOST_TEST_REQUIRE(got.metadata().precursors.size() == want.precursors.size());
    if (!want.precursors.empty()) {
      const auto& p = got.metadata().precursors.front();
      BOOST_TEST_REQUIRE(p.selected_ions.size() == 1u);
      BOOST_TEST_REQUIRE(p.selected_ions[0].selected_ion_mz.has_value());
      BOOST_TEST(*p.selected_ions[0].selected_ion_mz == *want.precursors[0].selected_ions[0].mz,
                 boost::test_tools::tolerance(1e-9));
      BOOST_TEST_REQUIRE(p.selected_ions[0].charge_state.has_value());
      BOOST_TEST(*p.selected_ions[0].charge_state == 2);
      BOOST_TEST_REQUIRE(p.isolation_window.target_mz.has_value());
      BOOST_TEST(*p.isolation_window.target_mz == static_cast<double>(400 + i),
                 boost::test_tools::tolerance(1e-4));
    }
  }
}

int row_groups(const MzPeak::Index& index, MzPeak::Schema::DataKind::Type kind)
{
  const auto file = index.find_file(MzPeak::Schema::EntityType::Spectrum, kind);
  BOOST_TEST_REQUIRE((file != index.files().end()));
  return index.manager()->parquet(*file)->file_metadata()->num_row_groups();
}

} // namespace

/******************************************************************************/
BOOST_AUTO_TEST_CASE(streamed_spectra_round_trip_across_row_groups)
{
  Scratch scratch("mzp-stream-writer.mzpeak");
  {
    // 40 points per group against spectra of 5..34 points: several groups in
    // each table, boundaries falling between spectra.
    MzPeak::RunArchiveWriter writer(scratch.path, /*points_per_row_group=*/40);
    boost::json::object md;
    boost::json::object run;
    run["id"] = "streamed";
    md["run"] = run;
    writer.set_metadata(MzPeak::RunMetadata(md));
    for (std::size_t i = 0; i < kSpectra; ++i) writer.add(make_spectrum(i));
    BOOST_TEST(writer.size() == kSpectra);
    writer.finish();
    writer.finish(); // idempotent
    BOOST_CHECK_THROW(writer.add(make_spectrum(0)), MzPeak::ParquetError);
  }
  BOOST_TEST(fs::exists(scratch.path));
  BOOST_TEST(!fs::exists(scratch.tmp()));

  check_archive(scratch.path);

  auto index = MzPeak::open(scratch.path);
  BOOST_TEST_REQUIRE(index.metadata().run().has_value());
  BOOST_TEST(index.metadata().run()->id.value_or("") == "streamed");
  BOOST_TEST(row_groups(index, MzPeak::Schema::DataKind::Type::DataArray) > 1);
  BOOST_TEST(row_groups(index, MzPeak::Schema::DataKind::Type::Peaks) > 1);
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(streamed_archive_reads_like_the_one_shot_one)
{
  Scratch streamed("mzp-stream-writer-a.mzpeak");
  Scratch oneshot("mzp-stream-writer-b.mzpeak");
  {
    MzPeak::RunArchiveWriter writer(streamed.path, 64);
    for (const auto& s : all_spectra()) writer.add(s);
    writer.finish();
  }
  MzPeak::RunContents contents;
  contents.spectra = all_spectra();
  MzPeak::write_run_archive(oneshot.path, contents);

  check_archive(oneshot.path);
  check_archive(streamed.path);

  auto a = MzPeak::open(streamed.path).spectra();
  auto b = MzPeak::open(oneshot.path).spectra();
  BOOST_TEST_REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    auto sa = a[i];
    auto sb = b[i];
    BOOST_TEST(sa.mz() == sb.mz());
    BOOST_TEST(sa.intensity() == sb.intensity());
    BOOST_TEST(sa.metadata().id == sb.metadata().id);
  }
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(an_abandoned_writer_leaves_nothing_behind)
{
  Scratch scratch("mzp-stream-writer-abandoned.mzpeak");
  {
    MzPeak::RunArchiveWriter writer(scratch.path, 8);
    for (std::size_t i = 0; i < 5; ++i) writer.add(make_spectrum(i));
    BOOST_TEST(fs::exists(scratch.tmp()));
    // no finish()
  }
  BOOST_TEST(!fs::exists(scratch.path));
  BOOST_TEST(!fs::exists(scratch.tmp()));
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(a_bad_spectrum_is_refused_at_add)
{
  Scratch scratch("mzp-stream-writer-bad.mzpeak");
  MzPeak::RunArchiveWriter writer(scratch.path);
  MzPeak::SpectrumData s = make_spectrum(0);
  s.intensity.pop_back();
  BOOST_CHECK_THROW(writer.add(s), MzPeak::ParquetError);
  MzPeak::SpectrumData n = make_spectrum(1);
  n.mz[0] = std::nan("");
  BOOST_CHECK_THROW(writer.add(n), MzPeak::ParquetError);
  BOOST_TEST(writer.size() == 0u);
}
