/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include "mzpeak/chromatogram.h"
#include "mzpeak/util/enumerable_proxy.h"

namespace MzPeak::Data {
class Signals;
}

namespace MzPeak {

/**
 * Access all chromatograms in a MzPeak file.
 */
class Chromatograms final : public Util::EnumerableProxy<Chromatogram> {
public:
  /// Default constructor — empty (no chromatogram table present).
  Chromatograms();

  /// Destructor.
  ~Chromatograms() = default;

  /// Low-level constructor.
  explicit Chromatograms(std::unique_ptr<Data::Signals>,
                          std::optional<std::size_t> count = std::nullopt);

private:
  std::shared_ptr<Data::Signals> data_;

  // Function to fetch a specific chromatogram.
  Chromatogram fetch(uint64_t);
};

} // namespace MzPeak
