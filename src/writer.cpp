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
// The flattened point columns of a set of spectra plus the file-level
// key/value metadata that describes them.  This is the common payload
// shared by both the directory and archive writers.
struct PointPayload {
  std::vector<uint64_t> spectrum_index;
  std::vector<double> mz;
  std::vector<float> intensity;
  std::map<std::string, std::string> file_kv;
};

/******************************************************************************/
// Validate, flatten and sort the per-spectrum arrays into the parallel
// point columns and build the matching file-level metadata.
//
// Validation happens BEFORE any output so a bad input cannot leave a
// partial artifact behind.  Within each spectrum the points are emitted in
// ascending m/z order so the array index's sorting_rank:0 claim holds and
// the reader's per-spectrum slicing (which assumes the ranked axis is
// sorted) is valid.
PointPayload flatten_spectra(const std::vector<SpectrumData>& spectra,
                             const char* context)
{
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    if (spectra[i].mz.size() != spectra[i].intensity.size()) {
      throw ParquetError(std::string(context) + ": spectrum " +
                         std::to_string(i) +
                         " has mismatched mz/intensity lengths");
    }
  }

  PointPayload payload;

  for (std::size_t i = 0; i < spectra.size(); ++i) {
    const SpectrumData& s = spectra[i];

    std::vector<std::size_t> order(s.mz.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::sort(
        order, [&](std::size_t a, std::size_t b) { return s.mz[a] < s.mz[b]; });

    for (std::size_t k : order) {
      payload.spectrum_index.push_back(static_cast<uint64_t>(i));
      payload.mz.push_back(s.mz[k]);
      payload.intensity.push_back(s.intensity[k]);
    }
  }

  payload.file_kv = {
      {"spectrum_array_index", Util::point_spectra_array_index_json()},
      {"spectrum_count", std::to_string(spectra.size())},
      {"spectrum_data_point_count", std::to_string(payload.mz.size())},
  };

  return payload;
}

/******************************************************************************/
// The mzpeak_index.json describing the point-layout data table and its
// companion metadata table.
std::string spectra_index_json()
{
  std::vector<Util::IndexFileEntry> files{
      {"spectra_data.parquet", "spectrum", "data arrays"},
      {"spectra_metadata.parquet", "spectrum", "metadata"},
  };
  return Util::mzpeak_index_json(files, "0.9.0");
}

/******************************************************************************/
// Build the per-spectrum metadata rows for spectra_metadata.parquet.  The
// data-point count gates whether a reader loads the profile arrays, so it
// must equal the number of points written for that spectrum.
std::vector<Util::SpectrumMetaRow>
build_metadata_rows(const std::vector<SpectrumData>& spectra)
{
  std::vector<Util::SpectrumMetaRow> rows;
  rows.reserve(spectra.size());
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    rows.push_back({/*index=*/static_cast<uint64_t>(i),
                    /*id=*/"index=" + std::to_string(i),
                    /*ms_level=*/uint8_t{1},
                    /*number_of_data_points=*/spectra[i].mz.size(),
                    /*number_of_peaks=*/uint64_t{0}});
  }
  return rows;
}

/******************************************************************************/
// Add a STORED (uncompressed) member to a zip archive from an in-memory
// buffer.  The buffer must outlive zip_close (libzip reads the source
// lazily), so callers keep the backing std::string in scope.
//
// Throws ParquetError on any libzip error.
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
  if (zip_set_file_compression(archive, static_cast<zip_uint64_t>(idx),
                               ZIP_CM_STORE, 0) != 0) {
    throw ParquetError(std::string("write_spectra_archive: "
                                   "zip_set_file_compression failed for ") +
                       name + ": " + zip_strerror(archive));
  }
}

} // namespace

/******************************************************************************/
void write_spectra_directory(const fs::path& dir,
                             const std::vector<SpectrumData>& spectra)
{
  PointPayload payload(flatten_spectra(spectra, "write_spectra_directory"));

  fs::create_directories(dir);

  // Data table.
  Util::write_point_spectra_data((dir / "spectra_data.parquet").string(),
                                 payload.spectrum_index, payload.mz,
                                 payload.intensity, payload.file_kv);

  // Metadata table.
  Util::write_spectra_metadata((dir / "spectra_metadata.parquet").string(),
                               build_metadata_rows(spectra));

  // Index.
  std::string index_json(spectra_index_json());

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

/******************************************************************************/
void write_spectra_archive(const fs::path& zip_path,
                           const std::vector<SpectrumData>& spectra)
{
  PointPayload payload(flatten_spectra(spectra, "write_spectra_archive"));

  // Encode the members in memory first.  These strings back the libzip
  // sources and MUST stay alive until zip_close returns.
  std::string parquet_bytes(Util::point_spectra_data_bytes(
      payload.spectrum_index, payload.mz, payload.intensity, payload.file_kv));
  std::string metadata_bytes(
      Util::spectra_metadata_bytes(build_metadata_rows(spectra)));
  std::string index_json(spectra_index_json());

  int errnum = 0;
  zip_t* archive =
      zip_open(zip_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errnum);
  if (archive == nullptr) {
    zip_error_t error;
    zip_error_init_with_code(&error, errnum);
    std::string msg("write_spectra_archive: failed to create archive " +
                    zip_path.string() + ": " + zip_error_strerror(&error));
    zip_error_fini(&error);
    throw ParquetError(msg);
  }

  // Add the members as STORED.  On any error, discard the (unwritten)
  // archive with zip_discard so no partial file is left behind, then
  // rethrow.
  try {
    add_stored_member(archive, "spectra_data.parquet", parquet_bytes);
    add_stored_member(archive, "spectra_metadata.parquet", metadata_bytes);
    add_stored_member(archive, "mzpeak_index.json", index_json);
  } catch (...) {
    zip_discard(archive);
    throw;
  }

  // zip_close flushes the archive to disk and reads the source buffers;
  // the backing strings above are still in scope here.
  if (zip_close(archive) != 0) {
    std::string msg("write_spectra_archive: failed to finalize archive " +
                    zip_path.string() + ": " + zip_strerror(archive));
    zip_discard(archive);
    throw ParquetError(msg);
  }
}

} // namespace MzPeak
