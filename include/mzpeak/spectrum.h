/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mzpeak/data/array_index.h"
#include "mzpeak/data/encoding.h"
#include "mzpeak/data/signals.h"
#include "mzpeak/metadata/spectrum.h"
#include "mzpeak/metadata/table.h"
#include "mzpeak/spectrum_metadata.h"
#include "mzpeak/util/slice.h"

namespace MzPeak {

// Forward declaration.
class Spectra;

/**
 * Access to a single spectrum in an MzPeak file.
 */
class Spectrum final {
public:
  /// The type of decoder used.
  using decoder_type = Data::Encoding::Decoder<double>;

  // Special members are implicit (rule of zero): copyable AND movable, so
  // returning a Spectrum by value moves its arrays rather than copying.

  /**
   * Mass-to-charge values.  Decodes the peak arrays on first access (lazy):
   * calling only the metadata accessors below never touches the peak data.
   */
  const std::vector<double>& mz() const;

  /**
   * Intensity values.  Lazily decoded (see mz()).
   */
  const std::vector<float>& intensity() const;

  /**
   * Stage number achieved in a multi stage mass spectrometry
   * acquisition.  Read from metadata; does NOT decode peaks.
   */
  uint8_t ms_level() const;

  /**
   * Full per-spectrum descriptive metadata (RT, precursors, isolation windows,
   * selected ions, scan windows, ion mobility).  Resolved from the cached
   * spectra_metadata table WITHOUT decoding the peak arrays.
   */
  const SpectrumMetadata& metadata() const;

  /// Retention time in SECONDS (handoff P0).  See SpectrumMetadata::retention_time.
  std::optional<double> retention_time() const { return metadata().retention_time; }

  /// Precursor list (empty for MS1); isolation window is target_mz ± offsets.
  const std::vector<PrecursorInfo>& precursors() const
  {
    return metadata().precursors;
  }

  /// Scan-level ion mobility value (1/K0 for diaPASEF/timsTOF) — handoff P1.
  std::optional<double> ion_mobility() const { return metadata().ion_mobility; }

  /// Scan-level ion mobility type — handoff P1.
  std::optional<std::string> ion_mobility_type() const
  {
    return metadata().ion_mobility_type;
  }

protected:
  friend class Spectra;

  /// Internal constructor.  The peak arrays (and the signal-file read that backs
  /// them) are deferred to first mz()/intensity(); @p md_map is the shared,
  /// cached spectra_metadata map (may be null when no metadata file).
  Spectrum(uint64_t index,
           std::shared_ptr<Data::Signals>,
           std::vector<Data::ArrayIndex::Dimension>,
           std::shared_ptr<Metadata::Table>,
           std::shared_ptr<const std::map<uint64_t, SpectrumMetadata>> md_map);

private:
  /// Read + decode the peak arrays into mz_/intensity_ on first access
  /// (idempotent).  This is the ONLY place the signal (peak) file is touched, so
  /// metadata-only iteration never reads peak data.
  void decode_() const;

  uint64_t index_;
  std::shared_ptr<Metadata::Table> md_table_;
  std::shared_ptr<const std::map<uint64_t, SpectrumMetadata>> md_map_;

  // Ingredients retained for the lazy peak read + decode.
  std::shared_ptr<Data::Signals> signals_;
  std::vector<Data::ArrayIndex::Dimension> dims_;

  mutable bool decoded_ = false;
  mutable std::vector<double> mz_;
  mutable std::vector<float> intensity_;
};

} // namespace MzPeak
