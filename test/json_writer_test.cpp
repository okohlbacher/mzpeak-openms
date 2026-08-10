/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#define BOOST_TEST_MODULE JsonWriter
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <boost/json.hpp>

#include "mzpeak/data/array_index.h"
#include "mzpeak/schema/file.h"
#include "mzpeak/util/json_writer.h"

namespace json = boost::json;
using namespace MzPeak;

/******************************************************************************/
// Mirror the reader's parse entry point (see parse_array_index in
// src/util/parquet.cpp): parse the string and hand the object to the
// public ArrayIndex constructor, which is the actual parsing code path.
static Data::ArrayIndex parse_array_index(const std::string& str,
                                          Schema::EntityType entity_type)
{
  json::value v(json::parse(str));
  BOOST_TEST(v.is_object());
  return Data::ArrayIndex(entity_type, v.as_object());
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(point_spectra_array_index_round_trips)
{
  const std::string js = Util::point_spectra_array_index_json();

  Data::ArrayIndex ai = parse_array_index(js, Schema::EntityType::Spectrum);

  BOOST_TEST(ai.prefix() == "point");

  const auto& entries = ai.entries();
  // spectrum_index is a synthetic entry, not in the data entries list.
  BOOST_TEST(entries.size() == 2u);

  auto find = [&](const std::string& path) -> const Data::ArrayIndex::Entry* {
    auto it = std::ranges::find(entries, path, &Data::ArrayIndex::Entry::path);
    return it == entries.end() ? nullptr : &*it;
  };

  const auto* mz = find("point.mz");
  const auto* intensity = find("point.intensity");

  BOOST_TEST((mz != nullptr), "missing point.mz column");
  BOOST_TEST((intensity != nullptr), "missing point.intensity column");

  if (mz) {
    BOOST_TEST((mz->array_type == Schema::PSI::ArrayType::Mz));
    BOOST_TEST((mz->sorting_rank.has_value()));
    if (mz->sorting_rank) BOOST_TEST(*mz->sorting_rank == 0u);
  }

  if (intensity) {
    BOOST_TEST((intensity->array_type == Schema::PSI::ArrayType::Intensity));
  }
}

/******************************************************************************/
BOOST_AUTO_TEST_CASE(mzpeak_index_round_trips)
{
  std::vector<Util::IndexFileEntry> files = {
      {"spectra_data.parquet", "spectrum", "data arrays"},
      {"spectra_peaks.parquet", "spectrum", "peaks"},
  };

  const std::string js = Util::mzpeak_index_json(files, "0.9.0");

  json::value v(json::parse(js));
  BOOST_TEST(v.is_object());
  const json::object& root = v.as_object();

  // metadata.version present.
  auto md = root.find("metadata");
  BOOST_TEST((md != root.end() && md->value().is_object()));
  const json::object& metadata = md->value().as_object();
  auto version = metadata.find("version");
  BOOST_TEST((version != metadata.end() && version->value().is_string()));

  // files[].name present, and each constructs a valid Schema::File.
  auto fit = root.find("files");
  BOOST_TEST((fit != root.end() && fit->value().is_array()));
  const json::array& file_array = fit->value().as_array();
  BOOST_TEST(file_array.size() == 2u);

  for (const auto& fv : file_array) {
    BOOST_TEST(fv.is_object());
    const json::object& fo = fv.as_object();
    BOOST_TEST((fo.contains("name") && fo.at("name").is_string()));

    // Constructs via the JSON ctor without throwing.
    Schema::File file(fo);
    BOOST_TEST(!file.file_name().empty());
    BOOST_TEST((file.entity_type() == Schema::EntityType::Spectrum));
  }

  // Spot-check the data-kind mapping for the two entries.
  Schema::File data(file_array.at(0).as_object());
  Schema::File peaks(file_array.at(1).as_object());
  BOOST_TEST((data.data_kind() == Schema::DataKind::DataArray));
  BOOST_TEST((peaks.data_kind() == Schema::DataKind::Peaks));
}
