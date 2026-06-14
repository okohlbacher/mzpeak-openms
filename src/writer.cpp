/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include <algorithm>
#include <fstream>
#include <numeric>
#include <string>

#include <zip.h>

#include "mzpeak/exception.h"
#include "mzpeak/util/json_writer.h"
#include "mzpeak/util/parquet_writer.h"
#include "mzpeak/writer.h"

namespace MzPeak {

namespace fs = std::filesystem;

namespace {

/******************************************************************************/
// The flattened parallel point columns of one table (data or peaks).
struct PointColumns {
  std::vector<uint64_t> spectrum_index;
  std::vector<double> mz;
  std::vector<float> intensity;
};

/******************************************************************************/
// Validate all spectra up front so a bad input cannot leave a partial
// artifact behind.
void validate(const std::vector<SpectrumData>& spectra, const char* context)
{
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    if (spectra[i].mz.size() != spectra[i].intensity.size()) {
      throw ParquetError(std::string(context) + ": spectrum " + std::to_string(i) +
                         " has mismatched mz/intensity lengths");
    }
  }
}

/******************************************************************************/
// Flatten the spectra whose `centroid` flag matches `want_centroid` into the
// parallel point columns, carrying the GLOBAL spectrum index and emitting each
// spectrum's points in ascending m/z order (so sorting_rank:0 holds).
PointColumns flatten(const std::vector<SpectrumData>& spectra, bool want_centroid)
{
  PointColumns c;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    const SpectrumData& s = spectra[i];
    if (s.centroid != want_centroid) continue;

    std::vector<std::size_t> order(s.mz.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::sort(
        order, [&](std::size_t a, std::size_t b) { return s.mz[a] < s.mz[b]; });

    for (std::size_t k : order) {
      c.spectrum_index.push_back(static_cast<uint64_t>(i));
      c.mz.push_back(s.mz[k]);
      c.intensity.push_back(s.intensity[k]);
    }
  }
  return c;
}

/******************************************************************************/
// File-level key/value metadata for a point table.  `total_spectra` is the
// full spectrum count (drives the reader's record_count, the same on every
// table); `n_points` is the number of points in THIS table.
std::map<std::string, std::string> point_file_kv(std::size_t total_spectra,
                                                 std::size_t n_points)
{
  return {
      {"spectrum_array_index", Util::point_spectra_array_index_json()},
      {"spectrum_count", std::to_string(total_spectra)},
      {"spectrum_data_point_count", std::to_string(n_points)},
  };
}

/******************************************************************************/
bool any_centroid(const std::vector<SpectrumData>& spectra)
{
  return std::ranges::any_of(spectra,
                             [](const SpectrumData& s) { return s.centroid; });
}

/******************************************************************************/
// mzpeak_index.json describing the data + metadata tables (+ peaks if present).
std::string spectra_index_json(bool with_peaks)
{
  std::vector<Util::IndexFileEntry> files{
      {"spectra_data.parquet", "spectrum", "data arrays"},
      {"spectra_metadata.parquet", "spectrum", "metadata"},
  };
  if (with_peaks) {
    files.push_back({"spectra_peaks.parquet", "spectrum", "peaks"});
  }
  return Util::mzpeak_index_json(files, "0.9.0");
}

/******************************************************************************/
// Per-spectrum metadata rows.  The data-point / peak counts gate which table
// a reader loads a spectrum from, so they must match where the points went.
std::vector<Util::SpectrumMetaRow>
build_metadata_rows(const std::vector<SpectrumData>& spectra)
{
  std::vector<Util::SpectrumMetaRow> rows;
  rows.reserve(spectra.size());
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    const SpectrumData& s = spectra[i];
    rows.push_back({/*index=*/static_cast<uint64_t>(i),
                    /*id=*/"index=" + std::to_string(i),
                    /*ms_level=*/uint8_t{1},
                    /*number_of_data_points=*/s.centroid ? uint64_t{0} : s.mz.size(),
                    /*number_of_peaks=*/s.centroid ? s.mz.size() : uint64_t{0},
                    /*representation=*/s.centroid ? "MS:1000127" : "MS:1000128"});
  }
  return rows;
}

/******************************************************************************/
// Add a STORED (uncompressed) member to a zip archive from an in-memory
// buffer.  The buffer must outlive zip_close (libzip reads the source
// lazily), so callers keep the backing std::string in scope.
void add_stored_member(zip_t* archive, const char* name, const std::string& data)
{
  zip_source_t* source =
      zip_source_buffer(archive, data.data(), data.size(), /*freep=*/0);
  if (source == nullptr) {
    throw ParquetError(std::string("write_spectra_archive: zip_source_buffer "
                                   "failed for ") +
                       name + ": " + zip_strerror(archive));
  }

  zip_int64_t idx =
      zip_file_add(archive, name, source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
  if (idx < 0) {
    // On failure libzip does not take ownership of the source.
    zip_source_free(source);
    throw ParquetError(std::string("write_spectra_archive: zip_file_add failed "
                                   "for ") +
                       name + ": " + zip_strerror(archive));
  }

  // The mzPeak spec mandates ZIP_CM_STORE; the reader rejects compressed
  // members.
  if (zip_set_file_compression(archive, static_cast<zip_uint64_t>(idx), ZIP_CM_STORE,
                               0) != 0) {
    throw ParquetError(std::string("write_spectra_archive: "
                                   "zip_set_file_compression failed for ") +
                       name + ": " + zip_strerror(archive));
  }
}

/******************************************************************************/
void write_index_file(const fs::path& path, const std::string& json)
{
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw ParquetError("cannot open index for writing: " + path.string());
  }
  out << json;
  out.close();
  if (!out) {
    throw ParquetError("error writing index: " + path.string());
  }
}

} // namespace

/******************************************************************************/
void write_spectra_directory(const fs::path& dir,
                             const std::vector<SpectrumData>& spectra)
{
  validate(spectra, "write_spectra_directory");
  fs::create_directories(dir);

  const std::size_t total = spectra.size();
  const bool with_peaks = any_centroid(spectra);

  // Profile points -> data table (always present; may be empty).
  PointColumns data(flatten(spectra, /*want_centroid=*/false));
  Util::write_point_spectra_data((dir / "spectra_data.parquet").string(),
                                 data.spectrum_index, data.mz, data.intensity,
                                 point_file_kv(total, data.mz.size()));

  // Centroid points -> peaks table (only when present).
  if (with_peaks) {
    PointColumns peaks(flatten(spectra, /*want_centroid=*/true));
    Util::write_point_spectra_data((dir / "spectra_peaks.parquet").string(),
                                   peaks.spectrum_index, peaks.mz, peaks.intensity,
                                   point_file_kv(total, peaks.mz.size()));
  }

  Util::write_spectra_metadata((dir / "spectra_metadata.parquet").string(),
                               build_metadata_rows(spectra));

  write_index_file(dir / "mzpeak_index.json", spectra_index_json(with_peaks));
}

/******************************************************************************/
void write_spectra_archive(const fs::path& zip_path,
                           const std::vector<SpectrumData>& spectra)
{
  validate(spectra, "write_spectra_archive");

  const std::size_t total = spectra.size();
  const bool with_peaks = any_centroid(spectra);

  // Encode all members in memory first.  These strings back the libzip
  // sources and MUST stay alive until zip_close returns.
  PointColumns data(flatten(spectra, /*want_centroid=*/false));
  std::string data_bytes(
      Util::point_spectra_data_bytes(data.spectrum_index, data.mz, data.intensity,
                                     point_file_kv(total, data.mz.size())));

  std::string peaks_bytes;
  if (with_peaks) {
    PointColumns peaks(flatten(spectra, /*want_centroid=*/true));
    peaks_bytes = Util::point_spectra_data_bytes(
        peaks.spectrum_index, peaks.mz, peaks.intensity,
        point_file_kv(total, peaks.mz.size()));
  }

  std::string metadata_bytes(
      Util::spectra_metadata_bytes(build_metadata_rows(spectra)));
  std::string index_json(spectra_index_json(with_peaks));

  int errnum = 0;
  zip_t* archive = zip_open(zip_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errnum);
  if (archive == nullptr) {
    zip_error_t error;
    zip_error_init_with_code(&error, errnum);
    std::string msg("write_spectra_archive: failed to create archive " +
                    zip_path.string() + ": " + zip_error_strerror(&error));
    zip_error_fini(&error);
    throw ParquetError(msg);
  }

  // Add the members as STORED.  On any error, discard the (unwritten) archive
  // so no partial file is left behind, then rethrow.
  try {
    add_stored_member(archive, "spectra_data.parquet", data_bytes);
    if (with_peaks) {
      add_stored_member(archive, "spectra_peaks.parquet", peaks_bytes);
    }
    add_stored_member(archive, "spectra_metadata.parquet", metadata_bytes);
    add_stored_member(archive, "mzpeak_index.json", index_json);
  } catch (...) {
    zip_discard(archive);
    throw;
  }

  // zip_close flushes the archive and reads the source buffers; the backing
  // strings above are still in scope here.
  if (zip_close(archive) != 0) {
    std::string msg("write_spectra_archive: failed to finalize archive " +
                    zip_path.string() + ": " + zip_strerror(archive));
    zip_discard(archive);
    throw ParquetError(msg);
  }
}

} // namespace MzPeak
