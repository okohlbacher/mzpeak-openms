/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <map>
#include <memory>
#include <vector>

#include "mzpeak/chromatogram_metadata.h"
#include "mzpeak/data/array_index.h"
#include "mzpeak/data/encoding.h"
#include "mzpeak/data/signals.h"
#include "mzpeak/util/slice.h"

namespace MzPeak {

// Forward declaration.
class Chromatograms;

/**
 * Access to a single chromatogram in an MzPeak file.
 */
class Chromatogram final {
public:
  using time_type = double;
  using intensity_type = float;

  // Special members are implicit (rule of zero): copyable AND movable.

  /**
   * Time values (the MS:1000595 relative-time-offset array).
   */
  const std::vector<time_type>& time() const;

  /**
   * Intensity values.
   *
   * @note The unit is not yet exposed.  A file may carry several intensity
   * arrays in different units -- detector counts and absorbance units both
   * appear in the bundled `has_uv` fixture -- and this returns whichever one
   * belongs to this chromatogram without saying which it was.
   */
  const std::vector<intensity_type>& intensity() const;

  /**
   * Descriptive metadata for this chromatogram (id, type, polarity,
   * precursors).  Returns an empty record when the file carries no
   * chromatogram metadata table.
   */
  const ChromatogramMetadata& metadata() const;

protected:
  friend class Chromatograms;

  Chromatogram(uint64_t index,
               std::shared_ptr<Data::Signals>,
               const std::vector<Data::ArrayIndex::Dimension>&,
               std::unique_ptr<Util::Slice>,
               std::shared_ptr<const std::map<uint64_t, ChromatogramMetadata>> = {});

private:
  using decoder_type = Data::Encoding::Decoder<double>;

  uint64_t index_;
  decoder_type decoder_;
  std::vector<time_type> time_;
  std::vector<intensity_type> intensity_;
  std::shared_ptr<const std::map<uint64_t, ChromatogramMetadata>> md_map_;
};

} // namespace MzPeak
