/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#pragma once

#include <memory>

#include "mzpeak/data/array_index.h"
#include "mzpeak/data/encoding.h"
#include "mzpeak/data/signals.h"
#include "mzpeak/metadata/spectrum.h"
#include "mzpeak/metadata/table.h"
#include "mzpeak/util/slice.h"

namespace MzPeak {

// Forward declaration.
class Spectra;

/// RDR-25: how much of a spectrum a read should materialize.  `Full` decodes
/// the m/z + intensity arrays (the default, unchanged behavior); `MetadataOnly`
/// returns only the per-spectrum scalar metadata and touches no array data —
/// a large speedup for metadata-only scans.  Mirrors the Rust reader's
/// `set_detail_level`.
enum class DetailLevel { Full, MetadataOnly };

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
   * Mass-to-charge values.
   */
  const std::vector<double>& mz() const;

  /**
   * Intensity values.
   */
  const std::vector<float>& intensity() const;

  /**
   * Stage number achieved in a multi stage mass spectrometry
   * acquisition.
   */
  uint8_t ms_level() const;

protected:
  friend class Spectra;

  /// Internal constructor.
  Spectrum(uint64_t index,
           std::shared_ptr<Data::Signals>,
           const std::vector<Data::ArrayIndex::Dimension>&,
           std::unique_ptr<Util::Slice>,
           std::shared_ptr<Metadata::Table>);

  /// RDR-25: tag selecting the no-decode construction path below.
  struct MetadataOnlyTag {};

  /// RDR-25: metadata-only constructor.  Leaves `mz_`/`intensity_` empty and
  /// `map_`/`array_index_` default, carrying only the scalar `metadata`.  No
  /// Parquet array is read and no decode runs — `mz()`/`intensity()` return
  /// empty vectors while `metadata()` and its accessors work normally.
  Spectrum(MetadataOnlyTag, SpectrumMetadata metadata);

private:
  uint64_t index_;
  std::shared_ptr<Metadata::Table> md_table_;
  Metadata::Spectrum md_spec_;
  decoder_type decoder_;
  std::vector<double> mz_;
  std::vector<float> intensity_;
};

} // namespace MzPeak
