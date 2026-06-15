/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/util/json_writer.h"

#include <boost/json.hpp>

namespace MzPeak::Util {

namespace json = boost::json;

/******************************************************************************/
std::string point_spectra_array_index_json()
{
  // Mirrors the `spectrum_array_index` blob stored in the reference
  // file: the index column (point.spectrum_index) is synthesized by the
  // reader and so only the m/z and intensity entries are emitted here.
  json::object root;
  root["prefix"] = "point";

  json::array entries;

  json::object mz;
  mz["context"] = "spectrum";
  mz["path"] = "point.mz";
  mz["data_type"] = "MS:1000523";  // 64-bit float
  mz["array_type"] = "MS:1000514"; // m/z array
  mz["array_name"] = "m/z array";
  mz["unit"] = "MS:1000040"; // m/z
  mz["buffer_format"] = "point";
  mz["transform"] = nullptr;
  mz["data_processing_id"] = nullptr;
  mz["buffer_priority"] = "primary";
  mz["sorting_rank"] = 0;
  entries.push_back(std::move(mz));

  json::object intensity;
  intensity["context"] = "spectrum";
  intensity["path"] = "point.intensity";
  intensity["data_type"] = "MS:1000521";  // 32-bit float
  intensity["array_type"] = "MS:1000515"; // intensity array
  intensity["array_name"] = "intensity array";
  intensity["unit"] = "MS:1000131"; // number of detector counts
  intensity["buffer_format"] = "point";
  intensity["transform"] = nullptr;
  intensity["data_processing_id"] = nullptr;
  intensity["buffer_priority"] = "primary";
  intensity["sorting_rank"] = nullptr;
  entries.push_back(std::move(intensity));

  root["entries"] = std::move(entries);

  return json::serialize(root);
}

/******************************************************************************/
namespace {

// Build the shared `files` array + `metadata{}` skeleton.  `metadata` is
// supplied pre-populated with the run-level blocks (empty for the no-metadata
// overload); `version` is then stamped on, always overriding any pre-existing
// `version` member.
std::string index_json(const std::vector<IndexFileEntry>& files,
                       const std::string& version,
                       json::object metadata)
{
  json::object root;

  json::array file_array;
  file_array.reserve(files.size());

  for (const auto& file : files) {
    json::object o;
    o["name"] = file.name;
    o["format"] = "parquet";
    o["entity_type"] = file.entity_type;
    o["data_kind"] = file.data_kind;
    file_array.push_back(std::move(o));
  }

  root["files"] = std::move(file_array);

  metadata["version"] = version;
  root["metadata"] = std::move(metadata);

  return json::serialize(root);
}

} // namespace

/******************************************************************************/
std::string mzpeak_index_json(const std::vector<IndexFileEntry>& files,
                              const std::string& version)
{
  return index_json(files, version, json::object{});
}

/******************************************************************************/
// WRT-2 — merge the serialized run-level metadata blocks into `metadata{}`.
std::string mzpeak_index_json(const std::vector<IndexFileEntry>& files,
                              const std::string& version,
                              const json::object& run_metadata)
{
  return index_json(files, version, run_metadata);
}

} // namespace MzPeak::Util
