/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "mzpeak/chromatogram.h"
#include "mzpeak/data/arrays.h"
#include "mzpeak/exception.h"
#include "mzpeak/util/enumerable_proxy.h"

namespace MzPeak {

class Chromatogram;

/**
 * Access all chromatograms in a MzPeak file.
 */
class Chromatograms final : public Util::EnumerableProxy<Chromatogram> {
public:
  /// Default constructor.
  Chromatograms();

  /// Destructor.
  ~Chromatograms() = default;

public:
  /// Low-level constructor.
  ///
  /// @param data        the chromatogram `data arrays` table; drives the
  ///                    chromatogram count when @p count is not provided.
  /// @param count       the number of chromatograms (typically read from the
  ///                    `chromatogram_count` key in the metadata table).  When
  ///                    absent, the count falls back to the data table's record
  ///                    count.
  /// @param id_to_index native-id → index map built from the chromatogram
  ///                    metadata table; empty when no metadata table is present.
  explicit Chromatograms(
      std::unique_ptr<Util::Parquet> data,
      std::optional<std::size_t> count = std::nullopt,
      std::unordered_map<std::string, std::size_t> id_to_index = {});

  /// Fetch the chromatogram with the given native id.  Throws
  /// MzPeak::ParquetError if the id is unknown.
  Chromatogram by_id(const std::string& id) const;

private:
  // The chromatogram data table.
  std::shared_ptr<Data::Arrays> data_;

  // Native id → chromatogram index, derived from the metadata table.
  std::unordered_map<std::string, std::size_t> id_to_index_;

  // Function to fetch a specific chromatogram.
  Chromatogram fetch(std::size_t) const;
};

} // namespace MzPeak
