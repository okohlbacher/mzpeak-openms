/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * Reader-side integrity checking: `open(path, Validate::Checksums)` and the
 * `Index::verify_checksums()` it wraps.
 *
 * The case that matters is the CORRUPTED one.  A verifier that always returns
 * "fine" passes every other test in this file, so the byte-flip below is what
 * actually establishes that the flag does anything at all.
 *
 * The second case that matters is an archive recording NO digests -- every
 * archive written before the specification required them.  Such an archive has
 * nothing to disagree with, so verification succeeds having proved nothing.
 * That distinction is reported rather than hidden, and is pinned here so it
 * cannot quietly become indistinguishable from a real verification.
 */
#define BOOST_TEST_MODULE Validation
#include <boost/test/included/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "mzpeak/exception.h"
#include "mzpeak/open.h"
#include "mzpeak/writer.h"

namespace {

namespace fs = std::filesystem;

/// A run written by this project's writer, so it carries digests.
fs::path write_archive(const char* name)
{
  const fs::path dir(fs::temp_directory_path() / name);
  fs::remove_all(dir);

  MzPeak::RunContents run;
  for (int i = 0; i < 3; ++i) {
    MzPeak::SpectrumData s;
    s.mz = {100.0 + i, 200.0 + i, 300.0 + i};
    s.intensity = {10.0f, 20.0f, 30.0f};
    s.ms_level = 1;
    s.retention_time = 1.0 * i;
    s.id = "scan=" + std::to_string(i);
    run.spectra.push_back(s);
  }
  MzPeak::write_run_directory(dir, run);
  return dir;
}

/// Flip one bit deep inside a member, leaving its length untouched so nothing
/// but the digest can notice.
void corrupt(const fs::path& file)
{
  std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
  BOOST_REQUIRE(f);
  f.seekg(0, std::ios::end);
  const std::streamoff size = f.tellg();
  BOOST_REQUIRE(size > 64);

  const std::streamoff at = size / 2;
  f.seekg(at);
  char byte = 0;
  f.read(&byte, 1);
  byte = static_cast<char>(byte ^ 0x01);
  f.seekp(at);
  f.write(&byte, 1);
  f.close();
}

} // namespace

BOOST_AUTO_TEST_CASE(a_freshly_written_archive_verifies)
{
  const fs::path dir = write_archive("mzp-validate-good.mzpeak");

  auto index = MzPeak::open(dir);
  const MzPeak::ChecksumReport report = index.verify_checksums();

  BOOST_TEST(report.ok());
  BOOST_TEST(report.mismatches.empty());
  BOOST_TEST(report.unchecked == 0u);
  // Not merely "no mismatches": something was actually checked.
  BOOST_TEST(report.verified >= 5u);

  BOOST_CHECK_NO_THROW(MzPeak::open(dir, MzPeak::Validate::Checksums));

  fs::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(a_flipped_bit_is_caught)
{
  const fs::path dir = write_archive("mzp-validate-bad.mzpeak");
  corrupt(dir / "spectra_metadata.parquet");

  auto index = MzPeak::open(dir);
  const MzPeak::ChecksumReport report = index.verify_checksums();

  BOOST_TEST(!report.ok());
  BOOST_REQUIRE_EQUAL(report.mismatches.size(), 1u);
  BOOST_TEST(report.mismatches.front().file_name == "spectra_metadata.parquet");
  // The report must carry both digests; "something is wrong" is not actionable.
  BOOST_TEST(report.mismatches.front().expected != report.mismatches.front().actual);
  BOOST_TEST(report.mismatches.front().actual.size() == 128u);

  // Every OTHER member still verifies, so the check is per member and not a
  // single verdict over the whole archive.
  BOOST_TEST(report.verified >= 4u);

  BOOST_CHECK_THROW(MzPeak::open(dir, MzPeak::Validate::Checksums),
                    MzPeak::ChecksumError);

  fs::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(the_default_open_does_not_verify)
{
  // Verification costs a full pass over the archive, so it must stay opt in:
  // a corrupted archive still opens, and still reads, unless asked otherwise.
  const fs::path dir = write_archive("mzp-validate-default.mzpeak");
  corrupt(dir / "spectra_metadata.parquet");

  BOOST_CHECK_NO_THROW(MzPeak::open(dir));

  fs::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(an_archive_without_digests_proves_nothing)
{
  // small.mzpeak predates the requirement and records no checksums.  It opens,
  // and it "passes" -- but with nothing verified, which is the distinction a
  // caller has to be able to see.
  auto index = MzPeak::open("../test/files/small.mzpeak");
  const MzPeak::ChecksumReport report = index.verify_checksums();

  BOOST_TEST(report.ok());
  BOOST_TEST(report.verified == 0u);
  BOOST_TEST(report.unchecked > 0u);

  // And it must not be refused: rejecting every archive older than the rule
  // would be a worse reader, not a stricter one.
  BOOST_CHECK_NO_THROW(
      MzPeak::open("../test/files/small.mzpeak", MzPeak::Validate::Checksums));
}
