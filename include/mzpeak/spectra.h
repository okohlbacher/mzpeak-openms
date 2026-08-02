/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#pragma once

#include <cstdint>
#include <map>
#include <memory>

#include "mzpeak/spectrum.h"
#include "mzpeak/spectrum_metadata.h"
#include "mzpeak/util/enumerable_proxy.h"

// Forward declarations:
namespace MzPeak::Data {
class Signals;
} // namespace MzPeak::Data

namespace MzPeak::Metadata {
class Table;
}

namespace MzPeak {

/**
 * Access all spectra in a MzPeak file.
 */
class Spectra final : public Util::EnumerableProxy<Spectrum> {
public:
  /// Default constructor: empty (zero spectra).
  Spectra() = default;

  /// Constructor for profile-only or centroid-only data.
  explicit Spectra(std::unique_ptr<Data::Signals>, std::unique_ptr<Metadata::Table>);

  /// Constructor for mixed profile+centroid data (data = profile file, peaks =
  /// centroid file).
  explicit Spectra(std::unique_ptr<Data::Signals> data,
                   std::unique_ptr<Data::Signals> peaks,
                   std::unique_ptr<Metadata::Table>);

private:
  // Internal data access.
  std::shared_ptr<Data::Signals> data_;
  std::shared_ptr<Data::Signals> peaks_; // centroid-only file (optional)
  std::shared_ptr<Metadata::Table> meta_;

  // Cached per-spectrum descriptive metadata (RT, precursors, ion mobility, …),
  // read once at construction and shared into every Spectrum.  Stays null when
  // there is no metadata table.
  std::shared_ptr<const std::map<uint64_t, SpectrumMetadata>> md_map_;

  // Populate md_map_ from meta_.  Called from the constructors so that fetch()
  // never publishes it lazily (that was a data race between threads).
  void load_metadata_();

  // Function to fetch a specific spectrum.
  Spectrum fetch(uint64_t);
};

} // namespace MzPeak
