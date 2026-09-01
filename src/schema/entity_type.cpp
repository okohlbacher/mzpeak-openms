/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

#include "mzpeak/schema/entity_type.h"

namespace MzPeak::Schema {

std::string entity_type_to_string(EntityType et)
{
  using enum EntityType;

  switch (et) {
  case Spectrum:
    return "spectrum";
  case Chromatogram:
    return "chromatogram";
  case WavelengthSpectrum:
    // Current spelling; the space form is still accepted when parsing.
    return "wavelength_spectrum";
  case Other:
    return "other";
  }

  // Make the compiler happy.
  return "other";
}

EntityType entity_type_from_string(std::string_view s)
{
  using enum EntityType;

  // The two spellings of `WavelengthSpectrum` are for backwards compatibility:
  // upstream renamed the space form to the underscore form, mirroring the
  // `data arrays` -> `data_arrays` rename handled in data_kind.cpp.  Accepting
  // only one silently degrades every entry to `Other`, which makes the whole
  // wavelength collection come back empty (has_uv.mzpeak: 520 rows).

  if (s == "spectrum") {
    return Spectrum;
  } else if (s == "chromatogram") {
    return Chromatogram;
  } else if (s == "wavelength_spectrum") {
    return WavelengthSpectrum;
  } else if (s == "wavelength spectrum") {
    return WavelengthSpectrum;
  } else {
    return Other;
  }
}
} // namespace MzPeak::Schema
