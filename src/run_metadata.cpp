/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/run_metadata.h"

#include <cstdint>
#include <limits>

/*
 * RDR-24 — parse the run-level metadata blocks from the index `metadata{}`
 * object into the typed model declared in run_metadata.h.  Boost.JSON is used
 * throughout to match the `files[]` parsing in src/index.cpp and src/schema.
 *
 * The parsing is intentionally tolerant: every field is optional, missing or
 * mistyped members are skipped rather than throwing, since this metadata is
 * descriptive (not load-bearing for reading the binary arrays) and the spec is
 * a pre-1.0 living standard.
 */
namespace MzPeak {

// Local alias: the public header qualifies Boost.JSON in full; inside this
// translation unit the short form keeps the parsing code readable.
namespace json = boost::json;

/******************************************************************************/
namespace {

/// Read an optional string member; absent or JSON-null yields std::nullopt.
std::optional<std::string> opt_string(const json::object& o, const char* key)
{
  if (const auto it = o.find(key); it != o.end() && it->value().is_string()) {
    return std::string(it->value().as_string().c_str());
  }
  return std::nullopt;
}

/// Read an optional integer member; absent/null/non-integral yields nullopt.
/// Accepts both signed and (in-range) unsigned JSON integers, since Boost.JSON
/// parses a non-negative literal as `uint64` and would otherwise be dropped.
std::optional<std::int64_t> opt_int(const json::object& o, const char* key)
{
  const auto it = o.find(key);
  if (it == o.end()) return std::nullopt;
  const auto& v = it->value();
  if (v.is_int64()) return v.as_int64();
  if (v.is_uint64()) {
    const auto u = v.as_uint64();
    if (u <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return static_cast<std::int64_t>(u);
    }
  }
  return std::nullopt;
}

/// Stringify a CV-param `value` member, which the index mixes between strings
/// and numbers (e.g. `"SN06061F"` vs `1`).  JSON-null yields std::nullopt.
std::optional<std::string> value_to_string(const json::value& v)
{
  switch (v.kind()) {
  case json::kind::string:
    return std::string(v.as_string().c_str());
  case json::kind::int64:
    return std::to_string(v.as_int64());
  case json::kind::uint64:
    return std::to_string(v.as_uint64());
  case json::kind::double_:
    return std::to_string(v.as_double());
  case json::kind::bool_:
    return v.as_bool() ? std::string("true") : std::string("false");
  default:
    return std::nullopt;
  }
}

/// Read an array member as a JSON array (empty if absent or not an array).
json::array array_member(const json::object& o, const char* key)
{
  if (const auto it = o.find(key); it != o.end() && it->value().is_array()) {
    return it->value().as_array();
  }
  return json::array();
}

} // namespace

/******************************************************************************/
CvParam CvParam::from_json(const json::object& o)
{
  CvParam p;
  p.accession = opt_string(o, "accession");
  p.name = opt_string(o, "name");
  p.unit = opt_string(o, "unit");
  if (const auto it = o.find("value"); it != o.end()) {
    p.value = value_to_string(it->value());
  }
  return p;
}

/******************************************************************************/
std::vector<CvParam> cv_params_from_json(const json::array& a)
{
  std::vector<CvParam> params;
  params.reserve(a.size());
  for (const auto& v : a) {
    if (v.is_object()) params.push_back(CvParam::from_json(v.as_object()));
  }
  return params;
}

/******************************************************************************/
Software Software::from_json(const json::object& o)
{
  Software s;
  if (auto id = opt_string(o, "id")) s.id = std::move(*id);
  s.version = opt_string(o, "version");
  s.parameters = cv_params_from_json(array_member(o, "parameters"));
  return s;
}

/******************************************************************************/
Component Component::from_json(const json::object& o)
{
  Component c;
  c.component_type = opt_string(o, "component_type");
  c.order = opt_int(o, "order");
  c.parameters = cv_params_from_json(array_member(o, "parameters"));
  return c;
}

/******************************************************************************/
InstrumentConfiguration InstrumentConfiguration::from_json(const json::object& o)
{
  InstrumentConfiguration ic;
  ic.id = opt_int(o, "id");
  ic.software_reference = opt_string(o, "software_reference");
  ic.parameters = cv_params_from_json(array_member(o, "parameters"));
  for (const auto& v : array_member(o, "components")) {
    if (v.is_object()) ic.components.push_back(Component::from_json(v.as_object()));
  }
  return ic;
}

/******************************************************************************/
ProcessingMethod ProcessingMethod::from_json(const json::object& o)
{
  ProcessingMethod m;
  m.order = opt_int(o, "order");
  m.software_reference = opt_string(o, "software_reference");
  m.parameters = cv_params_from_json(array_member(o, "parameters"));
  return m;
}

/******************************************************************************/
DataProcessing DataProcessing::from_json(const json::object& o)
{
  DataProcessing dp;
  if (auto id = opt_string(o, "id")) dp.id = std::move(*id);
  for (const auto& v : array_member(o, "methods")) {
    if (v.is_object())
      dp.methods.push_back(ProcessingMethod::from_json(v.as_object()));
  }
  return dp;
}

/******************************************************************************/
Sample Sample::from_json(const json::object& o)
{
  Sample s;
  if (auto id = opt_string(o, "id")) s.id = std::move(*id);
  s.name = opt_string(o, "name");
  s.parameters = cv_params_from_json(array_member(o, "parameters"));
  return s;
}

/******************************************************************************/
SourceFile SourceFile::from_json(const json::object& o)
{
  SourceFile sf;
  if (auto id = opt_string(o, "id")) sf.id = std::move(*id);
  sf.name = opt_string(o, "name");
  sf.location = opt_string(o, "location");
  sf.parameters = cv_params_from_json(array_member(o, "parameters"));
  return sf;
}

/******************************************************************************/
FileDescription FileDescription::from_json(const json::object& o)
{
  FileDescription fd;
  fd.contents = cv_params_from_json(array_member(o, "contents"));
  for (const auto& v : array_member(o, "source_files")) {
    if (v.is_object())
      fd.source_files.push_back(SourceFile::from_json(v.as_object()));
  }
  return fd;
}

/******************************************************************************/
Run Run::from_json(const json::object& o)
{
  Run r;
  r.id = opt_string(o, "id");
  r.start_time = opt_string(o, "start_time");
  r.default_instrument_id = opt_int(o, "default_instrument_id");
  r.default_data_processing_id = opt_string(o, "default_data_processing_id");
  r.default_source_file_id = opt_string(o, "default_source_file_id");
  return r;
}

/******************************************************************************/
RunMetadata::RunMetadata(const json::object& metadata)
    : raw_(metadata)
{
  if (const auto it = metadata.find("run");
      it != metadata.end() && it->value().is_object()) {
    run_ = Run::from_json(it->value().as_object());
  }

  if (const auto it = metadata.find("file_description");
      it != metadata.end() && it->value().is_object()) {
    file_description_ = FileDescription::from_json(it->value().as_object());
  }

  for (const auto& v : array_member(metadata, "software_list")) {
    if (v.is_object()) software_list_.push_back(Software::from_json(v.as_object()));
  }

  for (const auto& v : array_member(metadata, "instrument_configuration_list")) {
    if (v.is_object()) {
      instrument_configurations_.push_back(
          InstrumentConfiguration::from_json(v.as_object()));
    }
  }

  for (const auto& v : array_member(metadata, "data_processing_method_list")) {
    if (v.is_object()) {
      data_processings_.push_back(DataProcessing::from_json(v.as_object()));
    }
  }

  for (const auto& v : array_member(metadata, "sample_list")) {
    if (v.is_object()) samples_.push_back(Sample::from_json(v.as_object()));
  }
}

} // namespace MzPeak
