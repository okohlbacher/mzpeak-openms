/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE ArchiveWriter
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include <zip.h>

#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"
#include "mzpeak/writer.h"

namespace fs = std::filesystem;

namespace {

// RAII temp file with a unique name, removed on scope exit.
struct TempFile {
  fs::path path;
  TempFile()
      : path(fs::temp_directory_path() /
             ("mzpeak_archive_writer_test_" + std::to_string(next_id()) + ".mzpeak"))
  {
    std::error_code ec;
    fs::remove(path, ec); // start clean
  }
  ~TempFile()
  {
    std::error_code ec;
    fs::remove(path, ec);
  }

  static unsigned next_id()
  {
    static std::atomic<unsigned> counter{0};
    return counter.fetch_add(1);
  }
};

// Return the compression method libzip records for a named archive member,
// or -1 if the member is absent / cannot be stat-ed.
zip_int32_t member_compression(const fs::path& archive_path, const char* name)
{
  int errnum = 0;
  zip_t* archive = zip_open(archive_path.c_str(), ZIP_RDONLY, &errnum);
  if (archive == nullptr) return -1;

  zip_stat_t stat;
  zip_stat_init(&stat);
  zip_int32_t method = -1;
  if (zip_stat(archive, name, ZIP_FL_UNCHANGED, &stat) == 0 &&
      (stat.valid & ZIP_STAT_COMP_METHOD)) {
    method = static_cast<zip_int32_t>(stat.comp_method);
  }

  zip_close(archive);
  return method;
}

} // namespace

/******************************************************************************/
// Write a .mzpeak ZIP archive, read it back through the real reader, and
// confirm the (m/z-sorted) m/z and intensity arrays of every spectrum
// survive exactly.  The input m/z are intentionally unsorted to also
// exercise the per-spectrum sort.
BOOST_AUTO_TEST_CASE(round_trips_through_the_reader)
{
  using namespace MzPeak;

  // Unsorted m/z within each spectrum; the writer must sort them ascending
  // (carrying intensity along).
  std::vector<SpectrumData> in{
      {{300.0, 100.0, 200.5}, {3.0f, 1.0f, 2.5f}},
      {{250.75, 150.0}, {5.0f, 4.0f}},
      {{475.25, 175.5, 375.5, 275.0}, {4.5f, 1.5f, 3.5f, 2.5f}},
  };

  // Expected per-spectrum arrays after sorting by ascending m/z.
  std::vector<SpectrumData> expected{
      {{100.0, 200.5, 300.0}, {1.0f, 2.5f, 3.0f}},
      {{150.0, 250.75}, {4.0f, 5.0f}},
      {{175.5, 275.0, 375.5, 475.25}, {1.5f, 2.5f, 3.5f, 4.5f}},
  };

  TempFile archive;
  write_spectra_archive(archive.path, in);

  BOOST_TEST(fs::exists(archive.path));
  BOOST_TEST(fs::is_regular_file(archive.path));

  // Read back through the real reader (opens the file as a zip).
  Index index = MzPeak::open(archive.path.string());
  Spectra spectra = index.spectra();

  BOOST_TEST(spectra.size() == expected.size());

  for (std::size_t i = 0; i < expected.size(); ++i) {
    // Bind the spectrum first: operator[] returns by value, so
    // mz()/intensity() must reference a named spectrum, not a temporary.
    const auto& s = spectra[i];
    const auto& mz = s.mz();
    const auto& it = s.intensity();

    BOOST_TEST(mz.size() == expected[i].mz.size());
    BOOST_TEST(it.size() == expected[i].intensity.size());

    for (std::size_t j = 0; j < expected[i].mz.size(); ++j) {
      BOOST_TEST(mz[j] == expected[i].mz[j], boost::test_tools::tolerance(1e-9));
      BOOST_TEST(it[j] == expected[i].intensity[j],
                 boost::test_tools::tolerance(1e-6f));
    }
  }
}

/******************************************************************************/
// The archive members must be STORED (uncompressed): the mzPeak spec
// mandates ZIP_CM_STORE and the reader rejects compressed members.
BOOST_AUTO_TEST_CASE(members_are_stored_uncompressed)
{
  using namespace MzPeak;

  std::vector<SpectrumData> in{
      {{100.0, 200.0}, {10.0f, 20.0f}},
      {{150.0, 250.0, 350.0}, {1.0f, 2.0f, 3.0f}},
  };

  TempFile archive;
  write_spectra_archive(archive.path, in);

  BOOST_TEST(member_compression(archive.path, "spectra_data.parquet") ==
             static_cast<zip_int32_t>(ZIP_CM_STORE));
  BOOST_TEST(member_compression(archive.path, "mzpeak_index.json") ==
             static_cast<zip_int32_t>(ZIP_CM_STORE));
}

/******************************************************************************/
// Empty input writes a valid archive the reader can open with zero spectra.
BOOST_AUTO_TEST_CASE(handles_empty_input)
{
  using namespace MzPeak;

  TempFile archive;
  write_spectra_archive(archive.path, {});

  BOOST_TEST(fs::exists(archive.path));

  Index index = MzPeak::open(archive.path.string());
  BOOST_TEST(index.spectra().size() == 0);

  BOOST_TEST(member_compression(archive.path, "spectra_data.parquet") ==
             static_cast<zip_int32_t>(ZIP_CM_STORE));
}
