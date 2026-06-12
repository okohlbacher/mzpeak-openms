/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include <fstream>
#include <string>

#include "mzpeak/exception.h"
#include "mzpeak/util/json_writer.h"
#include "mzpeak/util/parquet_writer.h"
#include "mzpeak/writer.h"

namespace MzPeak {

namespace fs = std::filesystem;

/******************************************************************************/
void write_spectra_directory(const fs::path& dir,
                             const std::vector<SpectrumData>& spectra)
{
  fs::create_directories(dir);

  // Flatten the per-spectrum arrays into the parallel point columns,
  // assigning each point its spectrum index.
  std::vector<uint64_t> spectrum_index;
  std::vector<double> mz;
  std::vector<float> intensity;

  for (std::size_t i = 0; i < spectra.size(); ++i) {
    const SpectrumData& s = spectra[i];

    if (s.mz.size() != s.intensity.size()) {
      throw ParquetError("write_spectra_directory: spectrum " +
                         std::to_string(i) +
                         " has mismatched mz/intensity lengths");
    }

    for (std::size_t j = 0; j < s.mz.size(); ++j) {
      spectrum_index.push_back(static_cast<uint64_t>(i));
      mz.push_back(s.mz[j]);
      intensity.push_back(s.intensity[j]);
    }
  }

  // Data table.
  std::map<std::string, std::string> file_kv{
      {"spectrum_array_index", Util::point_spectra_array_index_json()},
      {"spectrum_count", std::to_string(spectra.size())},
      {"spectrum_data_point_count", std::to_string(mz.size())},
  };

  Util::write_point_spectra_data((dir / "spectra_data.parquet").string(),
                                 spectrum_index, mz, intensity, file_kv);

  // Index.
  std::vector<Util::IndexFileEntry> files{
      {"spectra_data.parquet", "spectrum", "data arrays"},
  };
  std::string index_json(Util::mzpeak_index_json(files, "0.9.0"));

  std::ofstream out(dir / "mzpeak_index.json", std::ios::binary);
  if (!out) {
    throw ParquetError("write_spectra_directory: cannot open index for "
                       "writing in " +
                       dir.string());
  }
  out << index_json;
  out.close();
  if (!out) {
    throw ParquetError("write_spectra_directory: error writing index in " +
                       dir.string());
  }
}

} // namespace MzPeak
