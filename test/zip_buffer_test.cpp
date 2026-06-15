/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE ZipBuffer
#include <boost/test/included/unit_test.hpp>

#include <cstddef>
#include <fstream>
#include <vector>

#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"
#include "mzpeak/wavelength_spectra.h"
#include "mzpeak/wavelength_spectrum.h"

/******************************************************************************/
// RDR-22 — slurp a `.mzpeak` archive off disk into an in-memory byte vector.
static std::vector<std::byte> slurp(const std::string& path)
{
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  BOOST_REQUIRE_MESSAGE(in, "cannot open fixture: " << path);

  std::streamsize size = in.tellg();
  in.seekg(0, std::ios::beg);

  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  BOOST_REQUIRE(in.read(reinterpret_cast<char*>(bytes.data()), size).good() ||
                in.gcount() == size);
  return bytes;
}

/******************************************************************************/
// RDR-22 — reading a `.mzpeak` from an in-memory buffer yields identical
// results to reading the same file off disk.  This also exercises a
// multi-table read: spectrum 0 is profile (spectra_data.parquet) while
// spectrum 2 is centroid (spectra_peaks.parquet), and the index itself comes
// from a third member — proving the per-member buffer-source pattern handles
// repeated/concurrent opens over the one owned buffer.
BOOST_AUTO_TEST_CASE(reads_identically_from_buffer)
{
  const std::string path = "../test/files/small.mzpeak";

  auto from_file = MzPeak::open(path);
  auto from_buf = MzPeak::open_buffer(slurp(path));

  auto file_spectra = from_file.spectra();
  auto buf_spectra = from_buf.spectra();

  BOOST_TEST(file_spectra.size() == 48u);
  BOOST_TEST(buf_spectra.size() == file_spectra.size());

  // Spectrum 0 — profile, served from spectra_data.parquet.
  auto file_mz = file_spectra[0].mz();
  auto buf_mz = buf_spectra[0].mz();
  auto file_in = file_spectra[0].intensity();
  auto buf_in = buf_spectra[0].intensity();

  BOOST_TEST(file_mz.size() == 13589u);
  BOOST_TEST(buf_mz.size() == file_mz.size());
  BOOST_TEST(buf_in.size() == file_in.size());

  BOOST_TEST(buf_mz.front() == file_mz.front(), boost::test_tools::tolerance(1e-9));
  BOOST_TEST(buf_mz.back() == file_mz.back(), boost::test_tools::tolerance(1e-9));
  BOOST_TEST(buf_in.front() == file_in.front(), boost::test_tools::tolerance(1e-6f));
  BOOST_TEST(buf_in.back() == file_in.back(), boost::test_tools::tolerance(1e-6f));

  // Spectrum 2 — centroid, served from a different member
  // (spectra_peaks.parquet): the buffer source is reopened independently.
  auto file_mz2 = file_spectra[2].mz();
  auto buf_mz2 = buf_spectra[2].mz();
  BOOST_TEST(buf_mz2.size() == file_mz2.size());
  BOOST_TEST(buf_mz2.front() == file_mz2.front(),
             boost::test_tools::tolerance(1e-9));
}

/******************************************************************************/
// RDR-22 — an empty buffer has no ZIP central directory; libzip rejects it
// during zip_open_from_source, so open_buffer must throw std::invalid_argument.
BOOST_AUTO_TEST_CASE(empty_buffer_throws)
{
  BOOST_CHECK_THROW(MzPeak::open_buffer({}), std::invalid_argument);
}

/******************************************************************************/
// RDR-22 — 64 zero bytes are not a valid ZIP file; libzip rejects the buffer,
// so open_buffer must throw std::invalid_argument.
BOOST_AUTO_TEST_CASE(all_zeros_buffer_throws)
{
  std::vector<std::byte> zeros(64, std::byte{0});
  BOOST_CHECK_THROW(MzPeak::open_buffer(std::move(zeros)), std::invalid_argument);
}

/******************************************************************************/
// RDR-22 — the same for has_uv.mzpeak, exercising wavelength spectra read
// from a buffer.
BOOST_AUTO_TEST_CASE(reads_wavelength_spectra_from_buffer)
{
  const std::string path = "../test/files/has_uv.mzpeak";

  auto from_buf = MzPeak::open_buffer(slurp(path));
  auto wavelength_spectra = from_buf.wavelength_spectra();

  BOOST_TEST(wavelength_spectra.size() == 520u);

  auto ws = wavelength_spectra[0];
  BOOST_TEST(ws.wavelength().size() == 96u);
  BOOST_TEST(ws.intensity().size() == 96u);
  BOOST_TEST(ws.wavelength().front() == 210.0, boost::test_tools::tolerance(1e-9));
  BOOST_TEST(ws.wavelength().back() == 400.0, boost::test_tools::tolerance(1e-9));
}
