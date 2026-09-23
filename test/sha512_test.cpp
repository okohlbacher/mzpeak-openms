/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * SHA-512 is implemented in this project rather than taken from a crypto
 * library, so it carries the burden of proving itself.  Three layers:
 *
 *   1. the published FIPS 180-4 / NIST example digests;
 *   2. the padding boundaries, which is where a hand-written implementation
 *      actually goes wrong -- 111, 112, 127, 128 and 129 bytes straddle the
 *      "does the length field still fit in this block" decision;
 *   3. agreement with the REFERENCE implementation, by recomputing a digest
 *      the Rust writer put in its own index.  That last one is the only test
 *      here that could catch a defect shared between this code and its author's
 *      understanding of the specification, so it runs whenever the reference
 *      checkout is present.
 */
#define BOOST_TEST_MODULE Sha512
#include <boost/test/included/unit_test.hpp>

#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <zip.h>

#include "mzpeak/util/sha512.h"
#include "mzpeak/writer.h"

using MzPeak::Util::Sha512;
using MzPeak::Util::sha512_hex;

BOOST_AUTO_TEST_CASE(nist_published_vectors)
{
  BOOST_TEST(sha512_hex("") ==
             "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
             "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");

  BOOST_TEST(sha512_hex("abc") ==
             "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
             "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");

  BOOST_TEST(
      sha512_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
      "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c335"
      "96fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445");
}

BOOST_AUTO_TEST_CASE(padding_boundaries_are_handled)
{
  // A million 'a' is the classic long vector; it also exercises many blocks.
  BOOST_TEST(sha512_hex(std::string(1000000, 'a')) ==
             "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
             "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");

  // These straddle the point where the 128-bit length field stops fitting
  // beside the message in the final block, which is where a hand-written
  // implementation actually goes wrong.  Values computed independently.
  struct Case {
    std::size_t length;
    const char* digest;
  };
  static const Case kCases[] = {
      {111, "fa9121c7b32b9e01733d034cfc78cbf67f926c7ed83e82200ef8681819692176"
            "0b4beff48404df811b953828274461673c68d04e297b0eb7b2b4d60fc6b566a2"},
      {112, "c01d080efd492776a1c43bd23dd99d0a2e626d481e16782e75d54c2503b5dc32"
            "bd05f0f1ba33e568b88fd2d970929b719ecbb152f58f130a407c8830604b70ca"},
      {127, "828613968b501dc00a97e08c73b118aa8876c26b8aac93df128502ab360f91ba"
            "b50a51e088769a5c1eff4782ace147dce3642554199876374291f5d921629502"},
      {128, "b73d1929aa615934e61a871596b3f3b33359f42b8175602e89f7e06e5f658a24"
            "3667807ed300314b95cacdd579f3e33abdfbe351909519a846d465c59582f321"},
      {129, "4f681e0bd53cda4b5a2041cc8a06f2eabde44fb16c951fbd5b87702f07aeab61"
            "1565b19c47fde30587177ebb852e3971bbd8d3fd30da18d71037dfbd98420429"},
  };

  for (const auto& c : kCases) {
    BOOST_TEST(sha512_hex(std::string(c.length, 'a')) == c.digest,
               "wrong digest at length " << c.length);
  }
}

BOOST_AUTO_TEST_CASE(chunking_does_not_change_the_digest)
{
  // The result must depend only on the byte sequence, never on how the caller
  // split it -- the writer feeds whole members, but a streaming caller would
  // not, and a broken buffer top-up shows up only here.
  const std::string message(300, 'x');
  const std::string whole = sha512_hex(message);

  for (std::size_t split : {std::size_t{1}, std::size_t{63}, std::size_t{127},
                            std::size_t{128}, std::size_t{129}}) {
    Sha512 piecewise;
    for (std::size_t i = 0; i < message.size(); i += split) {
      piecewise.update(message.data() + i, std::min(split, message.size() - i));
    }
    BOOST_TEST(piecewise.hex_digest() == whole,
               "splitting at " << split << " changed the digest");
  }
}

BOOST_AUTO_TEST_CASE(agrees_with_the_reference_implementation)
{
  // The reference writes a SHA-512 per member into its own index.  Recomputing
  // one proves this implementation agrees with the other implementation of the
  // specification, which no self-contained vector can.
  const std::filesystem::path root("../../hupo-mzpeak/small.unpacked.mzpeak");
  if (!std::filesystem::exists(root / "mzpeak_index.json")) {
    BOOST_TEST_MESSAGE("reference checkout absent -- cross-check skipped");
    return;
  }

  std::ifstream data(root / "spectra_data.parquet", std::ios::binary);
  BOOST_REQUIRE(data);
  std::ostringstream buffer;
  buffer << data.rdbuf();

  // The digest the reference recorded for spectra_data.parquet; it is also the
  // first `checksum` example in the specification's own JSON schema.
  BOOST_TEST(sha512_hex(buffer.str()) ==
             "b3350bbd8ff55e3aca1f3e3b0e20da79b8ffcf7a8a0de1dad09b51fc7ad6c01d"
             "cd6179e76d7dd06a1a6d7162edd3e2d0be790b2b9bb35b7bd4113952b7fe665d");
}

namespace {

namespace fs = std::filesystem;

std::string slurp(const fs::path& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

} // namespace

BOOST_AUTO_TEST_CASE(the_writer_stamps_a_correct_digest_on_every_member)
{
  const fs::path dir(fs::temp_directory_path() / "mzp-checksum-roundtrip.mzpeak");
  fs::remove_all(dir);

  MzPeak::RunContents run;
  MzPeak::SpectrumData s;
  s.mz = {100.0, 200.5, 300.25};
  s.intensity = {10.0f, 20.0f, 30.0f};
  s.ms_level = 1;
  s.retention_time = 5.0;
  s.id = "scan=1";
  run.spectra.push_back(s);

  MzPeak::write_run_directory(dir, run);

  const std::string index_text = slurp(dir / "mzpeak_index.json");
  BOOST_REQUIRE(!index_text.empty());
  const boost::json::value parsed = boost::json::parse(index_text);
  const boost::json::array& files = parsed.as_object().at("files").as_array();
  BOOST_REQUIRE(!files.empty());

  std::size_t checked = 0;
  for (const auto& entry : files) {
    const boost::json::object& file = entry.as_object();
    const std::string name(file.at("name").as_string());

    // Every member the index names must carry a digest; a null here is the
    // non-conformance this whole change exists to remove.
    BOOST_REQUIRE_MESSAGE(file.contains("checksum") &&
                              file.at("checksum").is_string(),
                          "no checksum recorded for " << name);

    // And it must be the RIGHT digest -- a writer that stamped a constant, or
    // hashed the wrong member, would satisfy the check above.
    const std::string recorded(file.at("checksum").as_string());
    BOOST_TEST(recorded == sha512_hex(slurp(dir / name)),
               "checksum does not match the bytes of " << name);
    BOOST_TEST(recorded.size() == 128u);
    ++checked;
  }
  BOOST_TEST(checked >= 5u); // data + metadata + three facets, at least

  // The index cannot contain its own digest, and must not claim to.
  for (const auto& entry : files) {
    BOOST_TEST(std::string(entry.as_object().at("name").as_string()) !=
               "mzpeak_index.json");
  }

  fs::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(the_streaming_writer_stamps_its_point_tables_too)
{
  // The streaming writer is the one path where the point tables never pass
  // through memory -- they are streamed from working files -- so its digests
  // are produced by a different code path from every other member's.
  const fs::path archive(fs::temp_directory_path() / "mzp-checksum-stream.mzpeak");
  fs::remove_all(archive);

  {
    MzPeak::RunArchiveWriter writer(archive, /*points_per_row_group=*/8);
    for (int i = 0; i < 4; ++i) {
      MzPeak::SpectrumData s;
      s.mz = {100.0 + i, 200.0 + i, 300.0 + i};
      s.intensity = {10.0f, 20.0f, 30.0f};
      s.ms_level = 1;
      s.retention_time = 1.0 * i;
      s.id = "scan=" + std::to_string(i);
      writer.add(s);
    }
    writer.finish();
  }

  int err = 0;
  zip_t* zip = zip_open(archive.string().c_str(), ZIP_RDONLY, &err);
  BOOST_REQUIRE(zip != nullptr);

  const auto member_bytes = [zip](const std::string& name) -> std::string {
    zip_stat_t st;
    if (zip_stat(zip, name.c_str(), 0, &st) != 0) return {};
    std::string out(static_cast<std::size_t>(st.size), '\0');
    zip_file_t* f = zip_fopen(zip, name.c_str(), 0);
    if (f == nullptr) return {};
    zip_fread(f, out.data(), st.size);
    zip_fclose(f);
    return out;
  };

  const std::string index_text = member_bytes("mzpeak_index.json");
  BOOST_REQUIRE(!index_text.empty());
  const boost::json::value parsed = boost::json::parse(index_text);

  bool saw_point_table = false;
  for (const auto& entry : parsed.as_object().at("files").as_array()) {
    const boost::json::object& file = entry.as_object();
    const std::string name(file.at("name").as_string());
    BOOST_REQUIRE_MESSAGE(file.at("checksum").is_string(),
                          "no checksum recorded for " << name);
    BOOST_TEST(std::string(file.at("checksum").as_string()) ==
                   sha512_hex(member_bytes(name)),
               "checksum does not match the bytes of " << name);
    if (name == "spectra_data.parquet") saw_point_table = true;
  }
  // The point table is the whole reason this case exists; if it stopped being
  // emitted the loop above would pass vacuously.
  BOOST_TEST(saw_point_table);

  zip_close(zip);
  fs::remove_all(archive);
}
