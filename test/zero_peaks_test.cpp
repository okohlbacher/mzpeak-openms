/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

// The reader defects behind doc/PLAN-zero-peaks-2026-09.md, each pinned by a
// fixture this library's own writer produces plus a targeted rewrite of one
// member:
//
//   1. A declared per-spectrum count must be enforced even when the metadata
//      names no representation.  The check used to be representation-gated, so
//      a zero-point decode against a non-zero declared count was silent.
//   2. A failed decode must not leave half a spectrum behind for the next
//      attempt to append to.
//   3. `Spectra` must be sized from the metadata, not from a signal table's
//      per-file `spectrum_count`: newer converters stamp that key with the
//      spectra in THAT table, and a mixed profile/centroid run then lost its
//      tail silently (48 spectra reported as 34 on a converted small.mzML).
//   4. A selected-ion m/z of 0 is a writer's encoding of "absent" and must
//      read as absent, so consumers fall back to the isolation window rather
//      than tagging against precursor m/z 0.
//   5. An entity straddling a row-group boundary must decode in full however
//      the run is traversed -- the planner's group hint used to hide the
//      entity's first group from a non-monotonic read.

#define BOOST_TEST_MODULE ZeroPeaks
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <arrow/util/key_value_metadata.h>
#include <boost/json.hpp>

#include "mzpeak/exception.h"
#include "mzpeak/index.h"
#include "mzpeak/io/directory.h"
#include "mzpeak/open.h"
#include "mzpeak/schema/file.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"
#include "mzpeak/util/parquet.h"
#include "mzpeak/util/parquet_writer.h"
#include "mzpeak/writer.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  TempDir()
      : path(fs::temp_directory_path() /
             ("mzpeak_zero_peaks_test_" + std::to_string(next_id())))
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
    static std::atomic<unsigned> counter{0};
    return counter.fetch_add(1);
  }
};

/// A centroid spectrum of `n` ascending peaks, or a profile one.
MzPeak::SpectrumData spectrum(std::size_t n, bool centroid = true)
{
  MzPeak::SpectrumData s;
  s.centroid = centroid;
  s.ms_level = centroid ? 2 : 1;
  for (std::size_t j = 0; j < n; ++j) {
    s.mz.push_back(100.0 + 10.0 * static_cast<double>(j));
    s.intensity.push_back(static_cast<float>(1 + j));
  }
  return s;
}

/// The file-level key/value metadata of one member of a written directory,
/// minus Arrow's own schema blob.  Reused when rewriting that member so its
/// array index and counts stay exactly what the writer emitted.
std::map<std::string, std::string> read_kv(const fs::path& dir,
                                           const std::string& member,
                                           const char* data_kind)
{
  MzPeak::IO::Directory archive(dir);
  std::unique_ptr<MzPeak::IO::File> file(archive.read_file(member));
  MzPeak::Schema::File schema_file(boost::json::object{
      {"name", member}, {"data_kind", data_kind}, {"entity_type", "spectrum"}});
  MzPeak::Util::Parquet parquet(std::move(file), schema_file);
  auto fmd = parquet.file_metadata();

  std::map<std::string, std::string> kv;
  for (const std::string& key : fmd->key_value_metadata()->keys()) {
    if (key.rfind("ARROW:", 0) == 0) continue;
    kv[key] = parquet.kv_string(fmd, key).value_or("");
  }
  return kv;
}

/// Rewrite one point table keeping, per spectrum index, the first `n` of its
/// rows.  An index absent from `keep` loses every row.
void rewrite_points(const fs::path& path,
                    const std::vector<MzPeak::SpectrumData>& spectra,
                    const std::map<std::size_t, std::size_t>& keep,
                    const std::map<std::string, std::string>& kv)
{
  std::vector<uint64_t> index;
  std::vector<double> mz;
  std::vector<float> intensity;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    auto it = keep.find(i);
    if (it == keep.end()) continue;
    const std::size_t n = std::min(it->second, spectra[i].mz.size());
    for (std::size_t j = 0; j < n; ++j) {
      index.push_back(static_cast<uint64_t>(i));
      mz.push_back(spectra[i].mz[j]);
      intensity.push_back(spectra[i].intensity[j]);
    }
  }
  MzPeak::Util::write_point_spectra_data(path.string(), index, mz, intensity, kv);
}

/// Every row of every spectrum.
std::map<std::size_t, std::size_t> all_of(const std::vector<MzPeak::SpectrumData>& s)
{
  std::map<std::size_t, std::size_t> keep;
  for (std::size_t i = 0; i < s.size(); ++i) keep[i] = s[i].mz.size();
  return keep;
}

/// Metadata rows for `spectra` with an explicit representation, so a test can
/// state exactly what the file declares about itself.
std::vector<MzPeak::Util::SpectrumMetaRow>
metadata_rows(const std::vector<MzPeak::SpectrumData>& spectra,
              const std::string& representation)
{
  std::vector<MzPeak::Util::SpectrumMetaRow> rows;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    const auto& s = spectra[i];
    rows.push_back({static_cast<uint64_t>(i), "index=" + std::to_string(i),
                    s.ms_level, s.retention_time, s.polarity,
                    /*number_of_data_points=*/s.centroid ? 0 : s.mz.size(),
                    /*number_of_peaks=*/s.centroid ? s.mz.size() : 0,
                    representation, s.precursors});
  }
  return rows;
}

} // namespace

/******************************************************************************/
// Control: the representation-driven check still fires when a centroid
// spectrum's rows are missing from the peaks table.
BOOST_AUTO_TEST_CASE(missing_rows_throw_against_the_declared_peak_count)
{
  TempDir dir;
  std::vector<MzPeak::SpectrumData> in{spectrum(3), spectrum(4), spectrum(5)};
  MzPeak::write_spectra_directory(dir.path, in);

  auto keep = all_of(in);
  keep.erase(1);
  rewrite_points(dir.path / "spectra_peaks.parquet", in, keep,
                 read_kv(dir.path, "spectra_peaks.parquet", "peaks"));

  auto spectra = MzPeak::open(dir.path).spectra();
  BOOST_TEST_REQUIRE(spectra.size() == 3u);
  auto s0 = spectra[0];
  BOOST_TEST(s0.mz().size() == 3u);
  auto s1 = spectra[1];
  BOOST_CHECK_THROW(s1.mz(), MzPeak::ParquetError);
  auto s2 = spectra[2];
  BOOST_TEST(s2.mz().size() == 5u);
}

/******************************************************************************/
// The loud check: with NO representation the file still declares 4 peaks for
// spectrum 1, and decoding nothing against that must throw rather than hand
// back an empty spectrum.  Spectrum 0 still reads -- an unknown representation
// resolves to the peaks table through the counts, exactly as fetch_ does.
BOOST_AUTO_TEST_CASE(a_declared_count_is_enforced_without_a_representation)
{
  TempDir dir;
  std::vector<MzPeak::SpectrumData> in{spectrum(3), spectrum(4), spectrum(5)};
  MzPeak::write_spectra_directory(dir.path, in);
  MzPeak::Util::write_spectra_metadata(
      (dir.path / "spectra_metadata.parquet").string(), metadata_rows(in, ""));

  auto keep = all_of(in);
  keep.erase(1);
  rewrite_points(dir.path / "spectra_peaks.parquet", in, keep,
                 read_kv(dir.path, "spectra_peaks.parquet", "peaks"));

  auto spectra = MzPeak::open(dir.path).spectra();
  BOOST_TEST_REQUIRE(spectra.size() == 3u);
  auto s0 = spectra[0];
  BOOST_TEST(s0.metadata().representation.empty());
  BOOST_TEST(s0.mz().size() == 3u);
  auto s1 = spectra[1];
  BOOST_CHECK_THROW(s1.mz(), MzPeak::ParquetError);
}

/******************************************************************************/
// What must NOT throw.  The rule may only ever consult the count belonging to
// the array actually read; measuring a decode against the OTHER array's count
// would reject perfectly good files.
BOOST_AUTO_TEST_CASE(legitimate_counts_do_not_throw)
{
  {
    // An empty spectrum declaring 0, with and without a representation.
    TempDir dir;
    std::vector<MzPeak::SpectrumData> in{spectrum(3), spectrum(0), spectrum(2)};
    MzPeak::write_spectra_directory(dir.path, in);
    auto spectra = MzPeak::open(dir.path).spectra();
    BOOST_TEST_REQUIRE(spectra.size() == 3u);
    auto s1 = spectra[1];
    BOOST_CHECK_NO_THROW(s1.mz());
    BOOST_TEST(s1.mz().empty());
    BOOST_TEST(s1.intensity().empty());

    MzPeak::Util::write_spectra_metadata(
        (dir.path / "spectra_metadata.parquet").string(), metadata_rows(in, ""));
    auto again = MzPeak::open(dir.path).spectra();
    auto t1 = again[1];
    BOOST_CHECK_NO_THROW(t1.mz());
    BOOST_TEST(t1.mz().empty());
    auto t2 = again[2];
    BOOST_TEST(t2.mz().size() == 2u);
  }
  {
    // Profile carrying BOTH counts (small.chunked's spectrum 0 shape): the
    // representation picks the profile count and the peak count is ignored.
    TempDir dir;
    std::vector<MzPeak::SpectrumData> in{spectrum(3, /*centroid=*/false)};
    MzPeak::write_spectra_directory(dir.path, in);
    auto rows = metadata_rows(in, "MS:1000128");
    rows[0].number_of_peaks = 99;
    MzPeak::Util::write_spectra_metadata(
        (dir.path / "spectra_metadata.parquet").string(), rows);
    auto spectra = MzPeak::open(dir.path).spectra();
    auto s0 = spectra[0];
    BOOST_CHECK_NO_THROW(s0.mz());
    BOOST_TEST(s0.mz().size() == 3u);
  }
  {
    // MS1-only, profile only, no precursors anywhere.
    TempDir dir;
    std::vector<MzPeak::SpectrumData> in{spectrum(4, false), spectrum(0, false),
                                         spectrum(6, false)};
    MzPeak::write_spectra_directory(dir.path, in);
    auto spectra = MzPeak::open(dir.path).spectra();
    BOOST_TEST_REQUIRE(spectra.size() == 3u);
    for (std::size_t i = 0; i < 3; ++i) {
      auto s = spectra[i];
      BOOST_CHECK_NO_THROW(s.mz());
      BOOST_TEST(s.mz().size() == in[i].mz.size());
    }
  }
}

/******************************************************************************/
// A decode that threw must throw again, not quietly return the same rows
// twice.  `call_once` re-runs its callable after an exception and the decoders
// append, so a short read used to become a plausible doubled spectrum on the
// second access.
BOOST_AUTO_TEST_CASE(a_failed_decode_does_not_accumulate_on_retry)
{
  TempDir dir;
  std::vector<MzPeak::SpectrumData> in{spectrum(2), spectrum(3)};
  MzPeak::write_spectra_directory(dir.path, in);

  auto keep = all_of(in);
  keep[0] = 1; // declared 2 peaks, only the first row survives
  rewrite_points(dir.path / "spectra_peaks.parquet", in, keep,
                 read_kv(dir.path, "spectra_peaks.parquet", "peaks"));

  auto spectra = MzPeak::open(dir.path).spectra();
  auto s0 = spectra[0];
  BOOST_CHECK_THROW(s0.mz(), MzPeak::ParquetError);
  BOOST_CHECK_THROW(s0.mz(), MzPeak::ParquetError);
  // A copy shares the decode, and must not resurrect it either.
  auto copy = s0;
  BOOST_CHECK_THROW(copy.intensity(), MzPeak::ParquetError);
}

/******************************************************************************/
// Per-table spectrum counts, which is what mzpeak-convert >= 0.11 writes: the
// data table says 1, the peaks table says 2, the metadata says 3.  All three
// spectra must be reachable; the old size rule reported 2.
BOOST_AUTO_TEST_CASE(mixed_tables_with_per_table_counts_are_all_visible)
{
  TempDir dir;
  std::vector<MzPeak::SpectrumData> in{spectrum(2, /*centroid=*/false),
                                       spectrum(3), spectrum(4)};
  MzPeak::write_spectra_directory(dir.path, in);

  auto data_kv = read_kv(dir.path, "spectra_data.parquet", "data_arrays");
  auto peaks_kv = read_kv(dir.path, "spectra_peaks.parquet", "peaks");
  BOOST_TEST_REQUIRE(data_kv.at("spectrum_count") == "3");
  data_kv["spectrum_count"] = "1";
  peaks_kv["spectrum_count"] = "2";
  rewrite_points(dir.path / "spectra_data.parquet", in, {{0, 2}}, data_kv);
  rewrite_points(dir.path / "spectra_peaks.parquet", in, {{1, 3}, {2, 4}},
                 peaks_kv);

  auto spectra = MzPeak::open(dir.path).spectra();
  BOOST_TEST_REQUIRE(spectra.size() == 3u);
  auto s0 = spectra[0];
  BOOST_TEST(s0.mz().size() == 2u);
  auto s2 = spectra[2];
  BOOST_TEST(s2.ms_level() == 2);
  BOOST_TEST(s2.mz().size() == 4u);

  // Iteration and the checked batch path see the tail too, and so does a Lean
  // reader (a second reader over the same archive shares the cached map).
  std::size_t iterated = 0;
  for (const auto& s : spectra) {
    (void)s;
    ++iterated;
  }
  BOOST_TEST(iterated == 3u);
  auto batch = spectra.get_spectra_batch({0, 1, 2});
  BOOST_TEST_REQUIRE(batch.size() == 3u);
  BOOST_TEST(batch[2].mz().size() == 4u);
  auto lean = MzPeak::open(dir.path).spectra(MzPeak::MetadataDetail::Lean);
  BOOST_TEST(lean.size() == 3u);
}

/******************************************************************************/
// A selected-ion m/z of 0 reads as absent; a real one survives; charge,
// intensity, the isolation window and the precursor association are untouched
// either way.
BOOST_AUTO_TEST_CASE(a_zero_selected_ion_mz_reads_as_absent)
{
  TempDir dir;
  std::vector<MzPeak::SpectrumData> in{spectrum(3), spectrum(3)};
  {
    MzPeak::PrecursorData p;
    p.isolation_target_mz = 500.5f;
    p.isolation_lower_offset = 0.5f;
    p.isolation_upper_offset = 0.5f;
    p.selected_ions.push_back({0.0, 2, 1234.0f});
    in[0].precursors.push_back(p);
  }
  {
    MzPeak::PrecursorData p;
    p.isolation_target_mz = 600.5f;
    p.selected_ions.push_back({600.25, 3, 4321.0f});
    in[1].precursors.push_back(p);
  }
  MzPeak::write_spectra_directory(dir.path, in);

  auto spectra = MzPeak::open(dir.path).spectra();
  BOOST_TEST_REQUIRE(spectra.size() == 2u);

  auto s0 = spectra[0];
  BOOST_TEST_REQUIRE(s0.precursors().size() == 1u);
  const auto& p0 = s0.precursors()[0];
  BOOST_TEST_REQUIRE(p0.selected_ions.size() == 1u);
  BOOST_TEST(!p0.selected_ions[0].selected_ion_mz.has_value());
  BOOST_TEST_REQUIRE(p0.selected_ions[0].charge_state.has_value());
  BOOST_TEST(*p0.selected_ions[0].charge_state == 2);
  BOOST_TEST_REQUIRE(p0.selected_ions[0].intensity.has_value());
  BOOST_TEST(*p0.selected_ions[0].intensity == 1234.0f);
  BOOST_TEST_REQUIRE(p0.isolation_window.target_mz.has_value());
  BOOST_TEST(*p0.isolation_window.target_mz == 500.5,
             boost::test_tools::tolerance(1e-4));

  auto s1 = spectra[1];
  BOOST_TEST_REQUIRE(s1.precursors().size() == 1u);
  const auto& p1 = s1.precursors()[0];
  BOOST_TEST_REQUIRE(p1.selected_ions.size() == 1u);
  BOOST_TEST_REQUIRE(p1.selected_ions[0].selected_ion_mz.has_value());
  BOOST_TEST(*p1.selected_ions[0].selected_ion_mz == 600.25,
             boost::test_tools::tolerance(1e-9));
}

/******************************************************************************/
// A spectrum straddling a row-group boundary must decode in full whatever
// order the run is read in.  Reading a LATER spectrum first moves the
// planner's group hint past the straddling spectrum's first group, and the
// forward-only scan then returned just the tail of it.
BOOST_AUTO_TEST_CASE(a_boundary_spanning_spectrum_survives_a_backward_read)
{
  TempDir dir;
  // Declared: 1, 2, 1 peaks.  Spectrum 1 has one peak in each row group.
  std::vector<MzPeak::SpectrumData> in{spectrum(1), spectrum(2), spectrum(1)};
  MzPeak::write_spectra_directory(dir.path, in);
  const auto kv = read_kv(dir.path, "spectra_peaks.parquet", "peaks");

  {
    MzPeak::Util::PointTableStream stream(
        (dir.path / "spectra_peaks.parquet").string());
    stream.write_row_group({0, 1}, {in[0].mz[0], in[1].mz[0]},
                           {in[0].intensity[0], in[1].intensity[0]});
    stream.write_row_group({1, 2}, {in[1].mz[1], in[2].mz[0]},
                           {in[1].intensity[1], in[2].intensity[0]});
    stream.close(kv);
  }

  auto spectra = MzPeak::open(dir.path).spectra();
  BOOST_TEST_REQUIRE(spectra.size() == 3u);

  // Forward: the straddling spectrum reads correctly even on trunk.
  auto forward = spectra[1];
  BOOST_CHECK_NO_THROW(forward.mz());
  BOOST_TEST(forward.mz().size() == 2u);

  // Now the same spectrum AFTER a later one, through the same reader: the hint
  // has moved to the second group.
  auto later = spectra[2];
  BOOST_TEST(later.mz().size() == 1u);
  auto again = spectra[1];
  BOOST_CHECK_NO_THROW(again.mz());
  BOOST_TEST(again.mz().size() == 2u);
  BOOST_TEST(again.intensity().size() == 2u);
}
