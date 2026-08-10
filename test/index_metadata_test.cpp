/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * RDR-24 — verify the run-level metadata blocks of the index `metadata{}`
 * object parse into the typed model.  Ground truth is small.mzpeak's
 * mzpeak_index.json (a zip-STORE archive carrying all seven blocks); the
 * asserted values were read directly from that JSON and are stable.
 */
#define BOOST_TEST_MODULE IndexMetadata
#include <boost/test/included/unit_test.hpp>

#include <ranges>

#include "mzpeak/index.h"
#include "mzpeak/open.h"

/******************************************************************************/
BOOST_AUTO_TEST_CASE(run_block_present)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  const auto& meta = index.metadata();

  BOOST_TEST(!meta.empty(), "metadata{} blocks should have parsed");

  const auto& run = meta.run();
  BOOST_TEST(run.has_value(), "run block should be present");

  // From small.dir/mzpeak_index.json -> metadata.run.
  BOOST_TEST((run->id == std::optional<std::string>("small")));
  BOOST_TEST(
      (run->start_time == std::optional<std::string>("2005-07-20T19:44:22Z")));
  BOOST_TEST((run->default_instrument_id == std::optional<std::int64_t>(0)));
  BOOST_TEST((run->default_data_processing_id ==
              std::optional<std::string>("pwiz_Reader_Thermo_conversion")));
  BOOST_TEST((run->default_source_file_id == std::optional<std::string>("RAW1")));
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(software_list_non_empty)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  const auto& sw = index.metadata().software_list();

  // small.mzpeak lists Xcalibur, pwiz, mzpeak_prototyping_convert1.
  BOOST_TEST(sw.size() == 3u);

  const auto pwiz = std::ranges::find(sw, "pwiz", &MzPeak::Software::id);
  BOOST_TEST((pwiz != sw.end()), "expected a 'pwiz' software entry");
  BOOST_TEST((pwiz->version == std::optional<std::string>("3.0.23307")));
  BOOST_TEST(!pwiz->parameters.empty());
  BOOST_TEST((pwiz->parameters.front().accession ==
              std::optional<std::string>("MS:1000615")));
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(instrument_configuration_accessible)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  const auto& configs = index.metadata().instrument_configurations();

  BOOST_TEST(configs.size() == 2u);

  // The regenerated fixture lists configuration id 1 first, then id 0; the
  // index preserves the file's order rather than sorting by id.
  const auto& first = configs.front();
  BOOST_TEST((first.id == std::optional<std::int64_t>(1)));
  BOOST_TEST((first.software_reference == std::optional<std::string>("Xcalibur")));

  // Top-level params carry the instrument model (MS:1000448 "LTQ FT").
  const auto model =
      std::ranges::find(first.parameters, std::optional<std::string>("MS:1000448"),
                        &MzPeak::CvParam::accession);
  BOOST_TEST((model != first.parameters.end()), "expected MS:1000448 param");
  BOOST_TEST((model->name == std::optional<std::string>("LTQ FT")));

  // Three ordered components: ionsource, analyzer, detector.
  BOOST_TEST(first.components.size() == 3u);
  BOOST_TEST((first.components.front().component_type ==
              std::optional<std::string>("ionsource")));
  BOOST_TEST(!first.components.front().parameters.empty());
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(file_description_and_source_file)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  const auto& fd = index.metadata().file_description();

  BOOST_TEST(fd.has_value(), "file_description block should be present");
  BOOST_TEST(!fd->contents.empty());
  BOOST_TEST(fd->source_files.size() == 1u);

  const auto& sf = fd->source_files.front();
  BOOST_TEST(sf.id == "RAW1");
  BOOST_TEST((sf.name == std::optional<std::string>("small.RAW")));
  // SHA-1 checksum param (MS:1000569) carries a stringified value.
  const auto sha =
      std::ranges::find(sf.parameters, std::optional<std::string>("MS:1000569"),
                        &MzPeak::CvParam::accession);
  BOOST_TEST((sha != sf.parameters.end()), "expected SHA-1 param");
  BOOST_TEST((sha->value == std::optional<std::string>(
                                "b43e9286b40e8b5dbc0dfa2e428495769ca96a96")));
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(data_processing_and_sample)
{
  auto index = MzPeak::open("../test/files/small.mzpeak");
  const auto& meta = index.metadata();

  BOOST_TEST(meta.data_processings().size() == 2u);
  BOOST_TEST(meta.data_processings().front().id == "pwiz_Reader_Thermo_conversion");
  BOOST_TEST(!meta.data_processings().front().methods.empty());

  // sample_list has one entry whose CV param value (a number, 1) is
  // stringified to "1" by the CvParam model.
  BOOST_TEST(meta.samples().size() == 1u);
  const auto& sample = meta.samples().front();
  BOOST_TEST(!sample.parameters.empty());
  BOOST_TEST((sample.parameters.front().value == std::optional<std::string>("1")));

  // The verbatim metadata object is retained for untyped blocks.
  BOOST_TEST(meta.raw().contains("scan_settings_list"));
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(version_unchanged)
{
  // RDR-24 must not regress the format-version handling.  Both branches:
  // small.mzpeak declares metadata.version, the legacy archive declares none
  // and must still read (empty, no throw) rather than being rejected.
  auto index = MzPeak::open("../test/files/small.mzpeak");
  BOOST_TEST(index.version() == "0.9.0");

  auto legacy = MzPeak::open("../test/files/legacy/small.mzpeak");
  BOOST_TEST(legacy.version().empty());
}
