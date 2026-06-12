/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include <algorithm>
#include <fstream>
#include <numeric>
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
  // Validate everything BEFORE creating any output so a bad input cannot
  // leave a partial directory behind.
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    if (spectra[i].mz.size() != spectra[i].intensity.size()) {
      throw ParquetError("write_spectra_directory: spectrum " +
                         std::to_string(i) +
                         " has mismatched mz/intensity lengths");
    }
  }

  // Flatten the per-spectrum arrays into the parallel point columns,
  // assigning each point its spectrum index.  Within each spectrum the
  // points are emitted in ascending m/z order so the array index's
  // sorting_rank:0 claim holds and the reader's per-spectrum slicing
  // (which assumes the ranked axis is sorted) is valid.
  std::vector<uint64_t> spectrum_index;
  std::vector<double> mz;
  std::vector<float> intensity;

  for (std::size_t i = 0; i < spectra.size(); ++i) {
    const SpectrumData& s = spectra[i];

    std::vector<std::size_t> order(s.mz.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::sort(order,
                      [&](std::size_t a, std::size_t b) { return s.mz[a] < s.mz[b]; });

    for (std::size_t k : order) {
      spectrum_index.push_back(static_cast<uint64_t>(i));
      mz.push_back(s.mz[k]);
      intensity.push_back(s.intensity[k]);
    }
  }

  fs::create_directories(dir);

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
