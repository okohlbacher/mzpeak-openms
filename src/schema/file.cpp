/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/schema/file.h"

#include "mzpeak/exception.h"

namespace MzPeak::Schema {

/******************************************************************************/
File::File(const json::object& o)
{
  try {
    // Accept "path" (preferred) with "name" as backward-compatible fallback.
    file_name = std::string(o.contains("path") ? o.at("path").as_string()
                                               : o.at("name").as_string());
    data_kind = data_kind_from_string(o.at("data_kind").as_string());
    entity_type = entity_type_from_string(o.at("entity_type").as_string());

    // column_mapping is optional: signal-data files omit it, and older writers
    // predate it entirely (they encode the CV term in the column name).
    if (auto cm = o.find("column_mapping");
        cm != o.end() && cm->value().is_array()) {
      const json::array& entries = cm->value().as_array();
      column_mapping.reserve(entries.size());

      for (const auto& entry : entries) {
        if (!entry.is_object()) continue;
        const json::object& eo = entry.as_object();

        ColumnMapping m;
        if (auto it = eo.find("name"); it != eo.end() && it->value().is_string()) {
          m.name = std::string(it->value().as_string());
        }
        if (auto it = eo.find("path"); it != eo.end() && it->value().is_string()) {
          m.path = std::string(it->value().as_string());
        }
        // accession and unit are explicitly nullable in the index.
        if (auto it = eo.find("accession");
            it != eo.end() && it->value().is_string()) {
          m.accession = std::string(it->value().as_string());
        }
        if (auto it = eo.find("unit"); it != eo.end() && it->value().is_string()) {
          m.unit = std::string(it->value().as_string());
        }

        // A mapping without a path binds nothing and would only ever produce a
        // lookup that silently misses; drop it at parse time instead.
        if (!m.path.empty()) column_mapping.push_back(std::move(m));
      }
    }
  } catch (const std::exception& e) {
    throw MzPeak::JsonError(std::string("schema::File: ") + e.what());
  }
}

/******************************************************************************/
std::optional<std::string> File::path_for(const std::string_view& accession) const
{
  for (const auto& m : column_mapping) {
    if (m.accession && *m.accession == accession) return m.path;
  }
  return std::nullopt;
}

/******************************************************************************/
std::optional<std::string> File::unit_for(const std::string_view& accession) const
{
  for (const auto& m : column_mapping) {
    if (m.accession && *m.accession == accession) return m.unit;
  }
  return std::nullopt;
}

/******************************************************************************/
bool File::is_associated_with(const File& other) const
{
  std::string::size_type underscore(file_name.find("_"));
  if (underscore == std::string::npos) return false;
  if (other.file_name.size() < underscore) return false;

  if (file_name.compare(0, underscore, other.file_name, 0, underscore) != 0) {
    return false;
  }

  return (data_kind == DataKind::DataArray &&
          other.data_kind == DataKind::Metadata) ||
         (data_kind == DataKind::Metadata && other.data_kind == DataKind::DataArray);
}

} // namespace MzPeak::Schema
