/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

// RDR-11: exercise Arrays::record_count()'s statistics fallback.
//
// All the bundled .mzpeak files carry the `spectrum_count` KV, so the
// fallback never runs against them.  To drive it we synthesise two standalone
// Parquet files via write_point_spectra_data(): one WITH the count KV (the
// normal path, which must be returned verbatim) and one WITHOUT it (the
// fallback path, which must derive the count from the index column's MAX as
// max + 1, not max).

#define BOOST_TEST_MODULE RecordCount
#include <boost/test/included/unit_test.hpp>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mzpeak/data/arrays.h"
#include "mzpeak/directory.h"
#include "mzpeak/schema/entity_type.h"
#include "mzpeak/schema/file.h"
#include "mzpeak/util/parquet.h"
#include "mzpeak/util/parquet_writer.h"

namespace {

// Small RAII helper that removes a temporary file on scope exit.
struct TempFile {
  explicit TempFile(std::string p)
      : path(std::move(p))
  {
  }
  ~TempFile() { std::remove(path.c_str()); }
  std::string path;
};

// Build a Data::Arrays reader over a freshly written Parquet file living in the
// current directory (the meson test cwd).  `file_kv` is forwarded verbatim, so
// callers control whether the `spectrum_count` KV is present.
MzPeak::Data::Arrays make_reader(const std::string& file_name,
                                 const std::map<std::string, std::string>& file_kv)
{
  using namespace MzPeak;

  // 3 spectra: indices 0, 1, 2 (0-based, contiguous).  The largest index is 2,
  // so a correct count is 3 (= max + 1).
  std::vector<uint64_t> spectrum_index{0, 0, 1, 2, 2};
  std::vector<double> mz{100.0, 200.0, 300.0, 400.0, 500.0};
  std::vector<float> intensity{1.0f, 2.0f, 3.0f, 4.0f, 5.0f};

  Util::write_point_spectra_data(file_name, spectrum_index, mz, intensity, file_kv);

  // Re-open the file through the public reader stack so record_count() runs
  // against a real ArrayIndex + column statistics, exactly as in production.
  Directory dir(".");
  std::unique_ptr<File> data(dir.read_file(file_name));

  Schema::File schema_file(file_name);
  schema_file.entity_type = Schema::EntityType::Spectrum;

  auto parquet(std::make_unique<Util::Parquet>(std::move(data), schema_file));
  return Data::Arrays(std::move(parquet));
}

// A minimal valid array_index JSON.  The ArrayIndex constructor only prepends
// the synthetic index column (make_index) when an `entries` array is present,
// so we supply an empty one; record_count() then finds it at columns()[0].
const std::string kArrayIndexJson{"{\"prefix\":\"point\",\"entries\":[]}"};

} // namespace

/******************************************************************************/
// When the `spectrum_count` KV is present, record_count() returns it verbatim
// and never touches the statistics fallback.  This pins the "count present"
// path so the RDR-11 change cannot regress it.
BOOST_AUTO_TEST_CASE(uses_count_kv_when_present)
{
  TempFile tmp("record_count_present.parquet");

  // Deliberately set the KV count to a value that does NOT equal max + 1 (which
  // would be 3) so the assertion proves we read the KV rather than the stats.
  std::map<std::string, std::string> file_kv{
      {"spectrum_count", "42"},
      {"spectrum_array_index", kArrayIndexJson},
  };

  auto data(make_reader(tmp.path, file_kv));
  BOOST_TEST(data.record_count() == 42u);
}

/******************************************************************************/
// When the `spectrum_count` KV is absent, record_count() must fall through to
// the index column's statistics.  The largest index is 2, so the count is
// max + 1 = 3 (RDR-11): "absent" must not collapse to 0, and the arithmetic
// must add one to the max index.
BOOST_AUTO_TEST_CASE(falls_back_to_max_plus_one_when_count_absent)
{
  TempFile tmp("record_count_absent.parquet");

  std::map<std::string, std::string> file_kv{
      {"spectrum_array_index", kArrayIndexJson},
      // NOTE: no "spectrum_count" key on purpose.
  };

  auto data(make_reader(tmp.path, file_kv));
  BOOST_TEST(data.record_count() == 3u);
}
