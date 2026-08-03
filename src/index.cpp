/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/index.h"

#include <arrow/util/key_value_metadata.h>
#include <charconv>
#include <memory>

#include "mzpeak/data/signals.h"
#include "mzpeak/exception.h"
#include "mzpeak/io/archive.h"
#include "mzpeak/metadata/table.h"
#include "mzpeak/util/metadata_model.h"

/*
 * Boost JSON:
 *   https://www.boost.org/doc/libs/latest/libs/json/doc/html/index.html
 */
namespace MzPeak {

/******************************************************************************/
const static char* INDEX_FILE_NAME = "mzpeak_index.json";

/******************************************************************************/
namespace json = boost::json;

/******************************************************************************/
struct Index::Impl {

  /// Constructor.
  Impl(std::unique_ptr<MzPeak::IO::Archive> archive)
      : archive_(std::move(archive))
      , files_()
  {
    parse_index();
  }

  /// Parse the JSON that makes up the MzPeak index.
  void parse_index();

  // The archive we are reading files out of.
  std::unique_ptr<MzPeak::IO::Archive> archive_;

  // Parsed file entries.
  std::vector<Schema::File> files_;

  // The mzPeak format version from metadata.version (empty if absent).
  std::string version_;

  // TOF -> m/z calibration for the ims-compact layout (valid=false if absent).
  ImsCalibration ims_;

  // Return an iterator to the requested file.
  std::vector<Schema::File>::const_iterator find_file(const std::string_view& name)
  {
    return std::ranges::find(files_, name, &Schema::File::file_name);
  }
};

/******************************************************************************/
Index::Index(std::unique_ptr<MzPeak::IO::Archive> archive)
    : impl_(std::make_unique<Impl>(std::move(archive)))
{
}

/******************************************************************************/
Index::~Index() = default;

/******************************************************************************/
const std::vector<Schema::File>& Index::files() const { return impl_->files_; }

/******************************************************************************/
const std::string& Index::version() const { return impl_->version_; }

/******************************************************************************/
const ImsCalibration& Index::ims_calibration() const { return impl_->ims_; }

/******************************************************************************/
void Index::Impl::parse_index()
{
  auto file = archive_->read_file(INDEX_FILE_NAME);
  uint8_t buffer[64 * 1024];
  std::optional<std::size_t> bytes;

  json::stream_parser parser;
  boost::system::error_code ec;

  do {
    bytes = file->read(buffer, sizeof(buffer));

    if (bytes.has_value() && *bytes > 0) {
      parser.write(reinterpret_cast<char const*>(buffer), *bytes, ec);
    }

  } while (bytes.has_value() && !ec);

  if (!ec) parser.finish(ec);
  if (ec) throw MzPeak::JsonError(ec.message());

  json::value v = parser.release();
  json::object o = v.as_object();

  if (const auto it = o.find("files"); it != o.end() && it->value().is_array()) {
    const json::array files(it->value().as_array());
    files_.reserve(files.size());

    for (const auto& file_obj : files) {
      if (file_obj.is_object()) {
        files_.push_back(Schema::File(file_obj.as_object()));
      }
    }
  }

  // Format version from metadata.version (e.g. "0.9.0").
  if (const auto it = o.find("metadata"); it != o.end() && it->value().is_object()) {
    const json::object& meta = it->value().as_object();
    if (const auto v = meta.find("version");
        v != meta.end() && v->value().is_string()) {
      version_ = v->value().as_string().c_str();
    }

    // ims-compact: {"a": ..., "b": ..., "mz_from_tof": "(a + b*tof)^2"}.
    // Writers emit this either as a JSON object or as a STRING holding that
    // object; accept both rather than silently reading no calibration, which
    // would make every spectrum of such a file come back empty.
    if (const auto ic = meta.find("ims_calibration"); ic != meta.end()) {
      json::value parsed;
      if (ic->value().is_string()) {
        boost::system::error_code pe;
        parsed = json::parse(ic->value().as_string(), pe);
        if (pe) parsed = nullptr;
      }
      const json::value& calv = ic->value().is_string() ? parsed : ic->value();

      if (calv.is_object()) {
        const json::object& cal = calv.as_object();
        const auto number = [&cal](const char* key, double& dst) {
          const auto f = cal.find(key);
          if (f == cal.end() || !f->value().is_number()) return false;
          dst = f->value().to_number<double>();
          return true;
        };
        // Both coefficients are required: half a calibration is not one.
        const bool have_a = number("a", ims_.a);
        const bool have_b = number("b", ims_.b);
        ims_.valid = have_a && have_b;
      }
    }
  }

  // Reject a future, potentially incompatible MAJOR version (the spec is a
  // pre-1.0 living standard; all 0.x are accepted).
  if (!version_.empty()) {
    std::string major(version_.substr(0, version_.find('.')));
    if (major != "0") {
      throw MzPeak::JsonError("unsupported mzPeak format version '" + version_ +
                              "' (this reader supports 0.x)");
    }
  }
}

/******************************************************************************/
Spectra Index::spectra() const
{
  using enum Schema::DataKind;

  const Schema::File* data_file = nullptr;
  const Schema::File* meta_file = nullptr;

  for (const auto& file : impl_->files_) {
    if (file.entity_type != Schema::EntityType::Spectrum) continue;
    if (file.data_kind == DataArray || file.data_kind == Peaks) {
      // Prefer DataArray (profile); fall back to Peaks (centroid-only).
      if (!data_file || file.data_kind == DataArray) data_file = &file;
    } else if (file.data_kind == Metadata) {
      meta_file = &file;
    }
  }

  if (!data_file) return Spectra(); // no spectrum data

  const Schema::File* peaks_file = nullptr;
  for (const auto& file : impl_->files_) {
    if (file.entity_type == Schema::EntityType::Spectrum &&
        file.data_kind == Peaks && data_file->data_kind == DataArray) {
      peaks_file = &file;
    }
  }

  std::unique_ptr<Metadata::Table> meta = nullptr;
  if (meta_file) {
    meta = std::make_unique<Metadata::Table>(parquet(*meta_file));

    // Newer writers put each metadata facet in its own file; attach them so the
    // reader can join them by source_index.  Older writers have none of these.
    for (const auto& file : impl_->files_) {
      if (file.entity_type != Schema::EntityType::Spectrum) continue;
      if (file.data_kind == Scans || file.data_kind == Precursors ||
          file.data_kind == SelectedIons) {
        meta->add_facet(file.data_kind, parquet(file));
      }
    }
  }

  std::unique_ptr<Data::Signals> data =
      std::make_unique<Data::Signals>(parquet(*data_file));

  if (peaks_file) {
    std::unique_ptr<Data::Signals> peaks =
        std::make_unique<Data::Signals>(parquet(*peaks_file));
    return Spectra(std::move(data), std::move(peaks), std::move(meta), impl_->ims_);
  }

  return Spectra(std::move(data), std::move(meta), impl_->ims_);
}

/******************************************************************************/
WavelengthSpectra Index::wavelength_spectra() const
{
  using enum Schema::DataKind;

  const Schema::File* data = nullptr;
  const Schema::File* metadata = nullptr;
  for (const auto& file : impl_->files_) {
    if (file.entity_type != Schema::EntityType::WavelengthSpectrum) continue;
    if (file.data_kind == DataArray) data = &file;
    // FIRST match wins.  Several members can carry data_kind "metadata" for one
    // entity -- the specification's own chromatogram example labels the
    // precursor facet that way -- and the primary table is written first.
    // Taking the last would hand a facet to the primary reader, which finds no
    // index column and returns an empty map with no error.
    else if (file.data_kind == Metadata && metadata == nullptr)
      metadata = &file;
  }

  if (!data) return WavelengthSpectra();

  // Read the declared count and the descriptive metadata from the metadata
  // table.  The Parquet objects must outlive the read, so they are held here.
  std::optional<std::size_t> count;
  std::map<uint64_t, WavelengthSpectrumMetadata> md;
  if (metadata) {
    auto md_parquet(parquet(*metadata));
    if (auto fmd = md_parquet->file_metadata()) {
      if (auto kv = fmd->key_value_metadata()) {
        auto result(kv->Get("wavelength_spectrum_count"));
        if (result.ok()) {
          std::size_t r{};
          const std::string& value(result.ValueOrDie());
          auto [ptr,
                ec]{std::from_chars(value.data(), value.data() + value.size(), r)};
          if (ec == std::errc()) count = r;
        }
      }
    }

    Util::WavelengthMetadataFiles files;
    files.primary = md_parquet.get();

    // A split-layout writer puts the scan facet in its own file.
    std::unique_ptr<Util::Parquet> scans;
    for (const auto& file : impl_->files_) {
      if (file.entity_type != Schema::EntityType::WavelengthSpectrum) continue;
      if (file.data_kind != Scans) continue;
      scans = parquet(file);
      files.scans = scans.get();
    }

    md = Util::read_wavelength_spectrum_metadata(files);
  }

  return WavelengthSpectra(std::make_unique<Data::Signals>(parquet(*data)), count,
                           std::move(md));
}

/******************************************************************************/
Chromatograms Index::chromatograms() const
{
  using enum Schema::DataKind;

  const Schema::File* data = nullptr;
  const Schema::File* metadata = nullptr;
  for (const auto& file : impl_->files_) {
    if (file.entity_type != Schema::EntityType::Chromatogram) continue;
    if (file.data_kind == DataArray) data = &file;
    // FIRST match wins; see the wavelength case above.
    else if (file.data_kind == Metadata && metadata == nullptr)
      metadata = &file;
  }

  if (!data) return Chromatograms();

  std::optional<std::size_t> count;
  std::map<uint64_t, ChromatogramMetadata> md;
  if (metadata) {
    auto md_parquet(parquet(*metadata));
    if (auto fmd = md_parquet->file_metadata()) {
      if (auto kv = fmd->key_value_metadata()) {
        auto result(kv->Get("chromatogram_count"));
        if (result.ok()) {
          std::size_t r{};
          const std::string& value(result.ValueOrDie());
          auto [ptr,
                ec]{std::from_chars(value.data(), value.data() + value.size(), r)};
          if (ec == std::errc()) count = r;
        }
      }
    }

    Util::ChromatogramMetadataFiles files;
    files.primary = md_parquet.get();

    std::unique_ptr<Util::Parquet> precursors;
    std::unique_ptr<Util::Parquet> selected_ions;
    for (const auto& file : impl_->files_) {
      if (file.entity_type != Schema::EntityType::Chromatogram) continue;
      if (file.data_kind == Precursors) {
        precursors = parquet(file);
        files.precursors = precursors.get();
      } else if (file.data_kind == SelectedIons) {
        selected_ions = parquet(file);
        files.selected_ions = selected_ions.get();
      }
    }

    md = Util::read_chromatogram_metadata(files);
  }

  return Chromatograms(std::make_unique<Data::Signals>(parquet(*data)), count,
                       std::move(md));
}

/******************************************************************************/
std::unique_ptr<Util::Parquet> Index::parquet(const Schema::File& file) const
{
  std::unique_ptr<IO::File> data(impl_->archive_->read_file(file.file_name));
  return std::make_unique<Util::Parquet>(std::move(data), file);
}

} // namespace MzPeak
