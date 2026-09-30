/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * A declared-sorted key column held as runs instead of decoded (KeyRuns).
 *
 * The claim under test is not "the fast path works" but "it changes nothing
 * except memory": every read through a group held as runs must give exactly
 * the bytes the full decode gives.  So most tests write the SAME points twice
 * -- once declaring the key sorted (held as runs), once not (decoded in full,
 * answered by the linear scan) -- and compare the two reads value for value.
 *
 * And the other half: a group whose key must NOT be held as runs -- a sorted
 * declaration that lies, a null in the key -- keeps its column, and metadata
 * point counts that disagree with the data are never believed.
 */

#define BOOST_TEST_MODULE KeyRuns
#include <boost/test/included/unit_test.hpp>

#include <arrow/api.h>
#include <arrow/array/concatenate.h>
#include <arrow/io/file.h>
#include <arrow/util/byte_size.h>
#include <parquet/api/reader.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>
#include <parquet/statistics.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mzpeak/exception.h"
#include "mzpeak/io/directory.h"
#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/spectrum.h"
#include "mzpeak/util/arrow.h"
#include "mzpeak/util/key_runs.h"
#include "mzpeak/util/manager.h"
#include "mzpeak/util/parquet.h"
#include "mzpeak/util/planner.h"
#include "mzpeak/util/projection.h"
#include "mzpeak/util/query.h"

namespace {

namespace fs = std::filesystem;
using namespace MzPeak;

/// A scratch directory removed on scope exit.
struct Scratch {
  fs::path path;
  explicit Scratch(const std::string& name)
      : path(fs::temp_directory_path() / name)
  {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~Scratch()
  {
    // error_code, not a throw: a destructor that throws terminates the process
    // before Boost.Test reports anything.  Windows refuses to delete a file
    // that is still open, so a leaked handle shows up HERE, with its name --
    // after a few retries, because a Windows runner's antivirus can hold a
    // freshly written file open for a moment (see row_group_test.cpp).
    std::error_code ec;
    for (int attempt = 0; attempt < 20; ++attempt) {
      fs::remove_all(path, ec);
      if (!ec) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    BOOST_TEST(!ec, "remove_all " << path.string() << ": " << ec.message());
  }
};

void check(const arrow::Status& status)
{
  if (!status.ok()) throw std::runtime_error(status.ToString());
}

template <typename T> T value(arrow::Result<T> result)
{
  check(result.status());
  return std::move(result).ValueOrDie();
}

/// One point table: a key per row (nullopt = null), and the values.
struct Points {
  std::vector<std::optional<int64_t>> key;
  std::vector<double> mz;
  std::vector<float> intensity;
};

double mz_of(int64_t s, uint64_t p) { return 100.0 + static_cast<double>(s) + p / 1000.0; }
float intensity_of(int64_t s, uint64_t p) { return static_cast<float>(s * 1000 + static_cast<int64_t>(p)); }

/// Varied spectrum sizes, every seventh spectrum EMPTY, so runs skip values,
/// and spectra of hundreds of points straddle 64 K batch and group boundaries.
uint64_t points_of(uint64_t s) { return s % 7 == 3 ? 0 : 50 + (s * 37) % 400; }

void add_spectrum(Points& points, int64_t s, uint64_t n)
{
  for (uint64_t p = 0; p < n; ++p) {
    points.key.push_back(s);
    points.mz.push_back(mz_of(s, p));
    points.intensity.push_back(intensity_of(s, p));
  }
}

constexpr uint64_t kSpectra = 1600;
constexpr int64_t kGroupRows = 150'000; // > 2 Arrow batches of 65,536 per group

Points honest_points()
{
  Points points;
  for (uint64_t s = 0; s < kSpectra; ++s) add_spectrum(points, static_cast<int64_t>(s), points_of(s));
  return points;
}

const char* kArrayIndex =
    R"({"prefix":"point","entries":[)"
    R"({"context":"spectrum","path":"point.mz","data_type":"MS:1000523",)"
    R"("array_type":"MS:1000514","array_name":"m/z array","unit":"MS:1000040",)"
    R"("buffer_format":"point","transform":"MS:1003902",)"
    R"("data_processing_id":null,"buffer_priority":"primary","sorting_rank":0},)"
    R"({"context":"spectrum","path":"point.intensity","data_type":"MS:1000521",)"
    R"("array_type":"MS:1000515","array_name":"intensity array",)"
    R"("unit":"MS:1000131","buffer_format":"point","transform":"MS:1003901",)"
    R"("data_processing_id":null,"buffer_priority":"primary",)"
    R"("sorting_rank":null}]})";

/// Write @p points as a point-layout spectra_data.parquet, declaring the key
/// sorted or not, with its own key/value metadata or the fixture defaults, and
/// any further writer properties @p tune sets.
void write_points(const fs::path& path,
                  const Points& points,
                  bool declare_sorted,
                  int64_t group_rows,
                  bool signed_key = false,
                  std::shared_ptr<const arrow::KeyValueMetadata> kv = nullptr,
                  bool statistics = true,
                  const std::function<void(parquet::WriterProperties::Builder&)>& tune = {})
{
  std::shared_ptr<arrow::Array> key;
  if (signed_key) {
    arrow::Int64Builder builder;
    for (const auto& k : points.key) check(k ? builder.Append(*k) : builder.AppendNull());
    key = value(builder.Finish());
  } else {
    arrow::UInt64Builder builder;
    for (const auto& k : points.key) {
      check(k ? builder.Append(static_cast<uint64_t>(*k)) : builder.AppendNull());
    }
    key = value(builder.Finish());
  }
  arrow::DoubleBuilder mz;
  check(mz.AppendValues(points.mz));
  arrow::FloatBuilder intensity;
  check(intensity.AppendValues(points.intensity));

  auto point = value(arrow::StructArray::Make(
      {key, value(mz.Finish()), value(intensity.Finish())},
      std::vector<std::string>{"spectrum_index", "mz", "intensity"}));

  if (!kv) {
    kv = arrow::key_value_metadata(
        {"spectrum_count", "spectrum_data_point_count", "spectrum_array_index"},
        {std::to_string(kSpectra), std::to_string(points.key.size()), kArrayIndex});
  }
  auto schema =
      arrow::schema({arrow::field("point", point->type(), true)})->WithMetadata(kv);
  auto table = arrow::Table::Make(schema, {point});

  parquet::WriterProperties::Builder props;
  props.compression(arrow::Compression::ZSTD);
  if (statistics) {
    props.enable_statistics();
  } else {
    props.disable_statistics();
  }
  props.max_row_group_length(group_rows);
  if (declare_sorted) {
    props.set_sorting_columns({parquet::SortingColumn{0, false, false}});
  }
  if (tune) tune(props);
  auto arrow_props = parquet::ArrowWriterProperties::Builder().store_schema()->build();
  auto out = value(arrow::io::FileOutputStream::Open(path.string()));
  check(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, group_rows,
                                   props.build(), arrow_props));
  check(out->Close());
}

/// A one-member archive directory around write_points().
fs::path write_archive(const fs::path& dir,
                       const Points& points,
                       bool declare_sorted,
                       int64_t group_rows = kGroupRows,
                       bool signed_key = false,
                       bool statistics = true)
{
  fs::create_directories(dir);
  write_points(dir / "spectra_data.parquet", points, declare_sorted, group_rows, signed_key,
               nullptr, statistics);
  std::ofstream json(dir / "mzpeak_index.json");
  json << R"({"files":[{"name":"spectra_data.parquet","entity_type":"spectrum",)"
       << R"("data_kind":"data_arrays","column_mapping":[],"parameters":[]}],)"
       << R"("metadata":{"version":"0.9.0"}})";
  return dir;
}

/// Write @p points as a FLAT table: spectrum_index, mz and intensity all
/// top-level columns, the key at column @p key_at, declared sorted or not.
void write_flat(const fs::path& path, const Points& points, int key_at, bool declare_sorted)
{
  arrow::UInt64Builder key;
  for (const auto& k : points.key) check(key.Append(static_cast<uint64_t>(*k)));
  arrow::DoubleBuilder mz;
  check(mz.AppendValues(points.mz));
  arrow::FloatBuilder intensity;
  check(intensity.AppendValues(points.intensity));

  arrow::FieldVector fields{arrow::field("mz", arrow::float64()),
                            arrow::field("intensity", arrow::float32())};
  arrow::ArrayVector columns{value(mz.Finish()), value(intensity.Finish())};
  fields.insert(fields.begin() + key_at, arrow::field("spectrum_index", arrow::uint64()));
  columns.insert(columns.begin() + key_at, value(key.Finish()));
  auto table = arrow::Table::Make(arrow::schema(fields), columns);

  parquet::WriterProperties::Builder props;
  props.compression(arrow::Compression::ZSTD);
  props.max_row_group_length(kGroupRows);
  if (declare_sorted) {
    props.set_sorting_columns({parquet::SortingColumn{key_at, false, false}});
  }
  auto out = value(arrow::io::FileOutputStream::Open(path.string()));
  check(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, kGroupRows,
                                   props.build()));
  check(out->Close());
}

/// One Parquet file on its own, outside any archive, as a @p data_kind member.
std::unique_ptr<Util::Parquet> open_table(const fs::path& path, const char* data_kind)
{
  IO::Directory directory(path.parent_path());
  return std::make_unique<Util::Parquet>(
      directory.read_file(path.filename()),
      Schema::File(boost::json::object{{"name", path.filename().string()},
                                       {"data_kind", data_kind},
                                       {"entity_type", "spectrum"}}));
}

/// Rewrite the key's dictionary indices in @p path -- one RLE run of five
/// zeros, `01 0a 00` (bit width 1, header 5 << 1, value 0) -- as one
/// bit-packed group of eight, `01 03 00` (header 1 << 1 | 1, eight zero bits):
/// the same five indices, then three zeros of padding.  The key must be the
/// first leaf, uncompressed, and those three bytes the end of its chunk.
void pad_key_indices(const fs::path& path)
{
  int64_t end = 0;
  {
    auto file = value(arrow::io::ReadableFile::Open(path.string()));
    const auto metadata = parquet::ReadMetaData(file); // the chunk points into it
    const auto chunk = metadata->RowGroup(0)->ColumnChunk(0);
    BOOST_TEST_REQUIRE(chunk->has_dictionary_page());
    end = chunk->dictionary_page_offset() + chunk->total_compressed_size();
    check(file->Close());
  }
  std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
  char tail[3] = {};
  stream.seekg(end - 3);
  stream.read(tail, 3);
  BOOST_TEST_REQUIRE((tail[0] == 0x01 && tail[1] == 0x0a && tail[2] == 0x00));
  stream.seekp(end - 2);
  stream.put(0x03);
  stream.close();
  BOOST_TEST_REQUIRE(!stream.fail());
}

/// Cut the key's footer statistics in @p path short: its @p min and @p max,
/// each stored as eight bytes (`08`, then the value little-endian), become
/// seven (`07`, then the first seven), which no longer decode as an INT64.
/// The footer shrinks by two bytes, and its length is rewritten to match.
void truncate_key_statistics(const fs::path& path, uint64_t min, uint64_t max)
{
  const auto le = [](uint64_t v, int bytes) {
    std::string out;
    for (int i = 0; i < bytes; ++i) {
      out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
    return out;
  };
  std::string file;
  {
    std::ifstream in(path, std::ios::binary);
    const std::istreambuf_iterator<char> begin(in), end;
    file.assign(begin, end);
  }
  BOOST_TEST_REQUIRE(file.size() > 12u);
  const std::size_t tail = file.size() - 8; // footer length, then "PAR1"
  std::size_t length = 0;
  for (int i = 3; i >= 0; --i) {
    length = length << 8 | static_cast<unsigned char>(file[tail + i]);
  }
  BOOST_TEST_REQUIRE(length <= tail);
  std::string footer = file.substr(tail - length, length);
  for (const uint64_t v : {min, max}) {
    const std::string whole = std::string(1, '\x08') + le(v, 8);
    const std::size_t at = footer.find(whole);
    BOOST_TEST_REQUIRE(at != std::string::npos, "no statistic " << v);
    footer.replace(at, whole.size(), std::string(1, '\x07') + le(v, 7));
  }
  file.resize(tail - length);
  file += footer + le(footer.size(), 4) + "PAR1";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(file.data(), static_cast<std::streamsize>(file.size()));
  out.close();
  BOOST_TEST_REQUIRE(!out.fail());
}

/// The signal table of an archive, through the archive's shared cache.
std::unique_ptr<Util::Parquet> signal_table(const Index& index)
{
  auto file = index.find_file(Schema::EntityType::Spectrum, Schema::DataKind::Type::DataArray);
  if (file == index.files().end()) throw std::runtime_error("no signal table");
  return index.manager()->parquet(*file);
}

/// Is group @p g held as runs, with the placeholder in the key's place?
bool held_as_runs(Util::Parquet& table, int g)
{
  auto group = table.row_group(g);
  if (!group->key_runs) return false;
  for (const auto& batch : group->batches) {
    const auto& point = static_cast<const arrow::StructArray&>(*batch->column(0));
    if (point.field(0)->type_id() != arrow::Type::NA) return false;
  }
  return true;
}

/// Is group @p g held as runs of a TOP-LEVEL key, with the placeholder in the
/// key's own column @p top of every batch?
bool held_as_top_level_runs(Util::Parquet& table, int g, int top)
{
  auto group = table.row_group(g);
  if (!group->key_runs || group->key_runs->child >= 0 || group->key_runs->top != top) {
    return false;
  }
  for (const auto& batch : group->batches) {
    if (batch->num_columns() != 3 || batch->column(top)->type_id() != arrow::Type::NA ||
        batch->schema()->field(top)->name() != "spectrum_index") {
      return false;
    }
  }
  return true;
}

/// A query's projected columns, decoded.
struct Read {
  std::vector<uint64_t> key;
  std::vector<double> mz;
  std::vector<float> intensity;
  bool operator==(const Read&) const = default;
};

Read run_query(Util::Parquet& table,
               const Util::Query& query,
               bool with_key,
               std::string_view group = "point")
{
  const auto key = table.field(group, "spectrum_index").value();
  const auto mz = table.field(group, "mz").value();
  const auto intensity = table.field(group, "intensity").value();
  Util::Projection projection;
  if (with_key) projection.project(key);
  projection.project(mz);
  projection.project(intensity);

  auto plan = table.planner(query).plan();
  auto slice = table.executor(projection).execute(plan);
  Read read;
  if (with_key) slice->array<Util::Decoders::Scalar<uint64_t>>(key, read.key);
  slice->array<Util::Decoders::Scalar<double>>(mz, read.mz);
  slice->array<Util::Decoders::Scalar<float>>(intensity, read.intensity);
  return read;
}

/// Bitwise: 0.0 and -0.0, or two NaNs, are not the same bytes.
template <typename T> bool same_bytes(const std::vector<T>& a, const std::vector<T>& b)
{
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

} // namespace

/******************************************************************************/
// The fast path is taken: every group of an honest declared-sorted file is
// held as runs, one run per spectrum that has points (empty spectra have none).
BOOST_AUTO_TEST_CASE(an_honest_sorted_key_is_held_as_runs)
{
  Scratch scratch("mzp-test-keyruns-held");
  const Points points = honest_points();
  auto index = MzPeak::open(write_archive(scratch.path / "sorted", points, true));
  auto table = signal_table(index);
  const int groups = table->file_metadata()->num_row_groups();
  BOOST_TEST_REQUIRE(groups >= 3);

  int64_t first_row = 0;
  for (int g = 0; g < groups; ++g) {
    BOOST_TEST(held_as_runs(*table, g), "group " << g);
    const int64_t rows = table->file_metadata()->RowGroup(g)->num_rows();
    std::set<int64_t> distinct;
    for (int64_t r = first_row; r < first_row + rows; ++r) distinct.insert(*points.key[r]);
    const auto& runs = *table->row_group(g)->key_runs;
    BOOST_TEST(runs.key.size() == distinct.size(), "group " << g);
    BOOST_TEST(runs.start.back() == rows);
    BOOST_TEST(runs.is_unsigned);
    BOOST_TEST(table->row_group(g)->batches.size() >= 1u);
    first_row += rows;
  }
  // At least one group holds more than one Arrow batch, so batch boundaries
  // inside a run are exercised.
  BOOST_TEST(table->row_group(0)->batches.size() > 1u);
}

/******************************************************************************/
// ...and changes nothing: every spectrum, including empty ones and ones that
// straddle batch and group boundaries, reads back byte for byte as the full
// decode of the same points reads it -- forwards, and backwards on a fresh
// reader, which walks the planner's hint the other way.
BOOST_AUTO_TEST_CASE(spectra_read_identically_with_and_without_runs)
{
  Scratch scratch("mzp-test-keyruns-identical");
  const Points points = honest_points();
  auto runs = MzPeak::open(write_archive(scratch.path / "sorted", points, true));
  auto full = MzPeak::open(write_archive(scratch.path / "unsorted", points, false));
  BOOST_TEST_REQUIRE(!signal_table(full)->row_group(0)->key_runs);

  auto fast = runs.spectra();
  auto slow = full.spectra();
  BOOST_TEST_REQUIRE(fast.size() == kSpectra);
  BOOST_TEST_REQUIRE(slow.size() == kSpectra);

  int mismatches = 0;
  for (uint64_t s = 0; s < kSpectra; ++s) {
    const auto a = fast[s];
    const auto b = slow[s];
    if (!same_bytes(a.mz(), b.mz()) || !same_bytes(a.intensity(), b.intensity()) ||
        a.mz().size() != points_of(s)) {
      ++mismatches;
      continue;
    }
    for (uint64_t p = 0; p < points_of(s); ++p) {
      if (a.mz()[p] != mz_of(static_cast<int64_t>(s), p) ||
          a.intensity()[p] != intensity_of(static_cast<int64_t>(s), p)) {
        ++mismatches;
        break;
      }
    }
  }
  BOOST_TEST(mismatches == 0);

  auto backwards = MzPeak::open(scratch.path / "sorted").spectra();
  mismatches = 0;
  for (uint64_t s = kSpectra; s-- > 0;) {
    const auto a = backwards[s];
    const auto b = slow[s];
    if (!same_bytes(a.mz(), b.mz()) || !same_bytes(a.intensity(), b.intensity())) {
      ++mismatches;
    }
  }
  BOOST_TEST(mismatches == 0);
}

/******************************************************************************/
// What it is for: the key's 8 bytes (and its validity byte) a row are neither
// held nor charged.  The cache charges what decoded_row_group_bytes() says,
// which is what a consumer sizes its budget from, and the batches really are
// that much smaller.
BOOST_AUTO_TEST_CASE(a_group_held_as_runs_costs_no_key_bytes)
{
  Scratch scratch("mzp-test-keyruns-bytes");
  const Points points = honest_points();
  auto runs = MzPeak::open(write_archive(scratch.path / "sorted", points, true));
  auto full = MzPeak::open(write_archive(scratch.path / "unsorted", points, false));
  auto runs_table = signal_table(runs);
  auto full_table = signal_table(full);

  std::size_t expected_held = 0;
  const auto fmd = runs_table->file_metadata();
  for (int g = 0; g < fmd->num_row_groups(); ++g) {
    const auto rows = static_cast<std::size_t>(fmd->RowGroup(g)->num_rows());
    BOOST_TEST(Util::decoded_row_group_bytes(*fmd->RowGroup(g), *fmd->schema()) ==
               rows * (9 + 5)); // m/z and intensity, each + validity
    const auto ffmd = full_table->file_metadata();
    BOOST_TEST(Util::decoded_row_group_bytes(*ffmd->RowGroup(g), *ffmd->schema()) ==
               rows * (9 + 9 + 5));
    expected_held += rows * 14;
  }

  auto spectra = runs.spectra();
  for (uint64_t s = 0; s < kSpectra; ++s) (void)spectra[s].mz();
  BOOST_TEST(runs.manager()->row_group_cache().stats().held_bytes == expected_held);

  std::size_t with_runs = 0, without = 0;
  for (const auto& batch : runs_table->row_group(0)->batches) with_runs += arrow::util::TotalBufferSize(*batch);
  for (const auto& batch : full_table->row_group(0)->batches) without += arrow::util::TotalBufferSize(*batch);
  BOOST_TEST_MESSAGE("group 0: " << with_runs << " bytes held as runs, " << without << " decoded in full");
  BOOST_TEST(with_runs * 100 < without * 65);
}

/******************************************************************************/
// Queries the runs cannot answer -- a range on the key, an equality that also
// projects the key -- get the key column rebuilt from the runs, and give what
// the full decode gives.  The cached group stays held as runs.
BOOST_AUTO_TEST_CASE(other_queries_see_the_key_restored)
{
  Scratch scratch("mzp-test-keyruns-restore");
  const Points points = honest_points();
  auto runs = MzPeak::open(write_archive(scratch.path / "sorted", points, true));
  auto full = MzPeak::open(write_archive(scratch.path / "unsorted", points, false));
  auto a = signal_table(runs);
  auto b = signal_table(full);
  const auto key = a->field("point", "spectrum_index").value();
  const auto key_b = b->field("point", "spectrum_index").value();

  // A range crossing the first group boundary.
  uint64_t boundary = 0;
  for (int64_t r = 0, s = 0; s < static_cast<int64_t>(kSpectra); r += points_of(s), ++s) {
    if (r + static_cast<int64_t>(points_of(s)) > kGroupRows) {
      boundary = static_cast<uint64_t>(s);
      break;
    }
  }
  const auto range = [&](const Schema::Column& k) {
    return Util::Query::Builder(k).ge<uint64_t>(boundary - 5).and_then(
        Util::Query::Builder(k).le<uint64_t>(boundary + 5));
  };
  const Read ra = run_query(*a, range(key), true);
  const Read rb = run_query(*b, range(key_b), true);
  BOOST_TEST(!ra.key.empty());
  BOOST_TEST((ra == rb));
  BOOST_TEST(same_bytes(ra.mz, rb.mz));
  for (uint64_t k : ra.key) BOOST_TEST((k >= boundary - 5 && k <= boundary + 5));

  // Equalities that project the key, and ones that do not.
  for (uint64_t s : {uint64_t{0}, boundary, boundary + 1, uint64_t{3}, kSpectra - 1}) {
    for (bool with_key : {true, false}) {
      const Read ea = run_query(*a, Util::Query::Builder(key).eq<uint64_t>(s), with_key);
      const Read eb = run_query(*b, Util::Query::Builder(key_b).eq<uint64_t>(s), with_key);
      BOOST_TEST((ea == eb), "spectrum " << s << " with_key " << with_key);
      BOOST_TEST(ea.mz.size() == points_of(s));
      for (uint64_t k : ea.key) BOOST_TEST(k == s);
    }
  }

  for (int g = 0; g < a->file_metadata()->num_row_groups(); ++g) {
    BOOST_TEST(held_as_runs(*a, g), "group " << g << " after restoring");
  }
}

/******************************************************************************/
// A TOP-LEVEL key -- a column of the table itself, as in a metadata table, not
// a struct's child -- is held as runs the same way: the placeholder goes into
// the batch at the key's own column, whether that is first, between the others
// or last, and a query that needs the key gets it back in that column.  Every
// read gives what the full decode of the same table gives.
BOOST_AUTO_TEST_CASE(a_top_level_key_is_held_as_runs)
{
  Scratch scratch("mzp-test-keyruns-top");
  const Points points = honest_points();

  // A spectrum that straddles the first group boundary.
  uint64_t boundary = 0;
  for (int64_t r = 0, s = 0; s < static_cast<int64_t>(kSpectra); r += points_of(s), ++s) {
    if (r + static_cast<int64_t>(points_of(s)) > kGroupRows) {
      boundary = static_cast<uint64_t>(s);
      break;
    }
  }

  for (int key_at : {0, 1, 2}) {
    const std::string at = std::to_string(key_at);
    write_flat(scratch.path / ("sorted" + at + ".parquet"), points, key_at, true);
    write_flat(scratch.path / ("unsorted" + at + ".parquet"), points, key_at, false);
    auto a = open_table(scratch.path / ("sorted" + at + ".parquet"), "metadata");
    auto b = open_table(scratch.path / ("unsorted" + at + ".parquet"), "metadata");
    BOOST_TEST_REQUIRE(!b->row_group(0)->key_runs);

    const int groups = a->file_metadata()->num_row_groups();
    BOOST_TEST_REQUIRE(groups >= 3);
    for (int g = 0; g < groups; ++g) {
      BOOST_TEST(held_as_top_level_runs(*a, g, key_at), "key at " << key_at << " group " << g);
    }
    BOOST_TEST(a->row_group(0)->batches.size() > 1u);

    const auto key = a->field("root", "spectrum_index").value();
    const auto key_b = b->field("root", "spectrum_index").value();

    // A range, which the runs cannot answer: the key is restored.
    const auto range = [&](const Schema::Column& k) {
      return Util::Query::Builder(k).ge<uint64_t>(boundary - 5).and_then(
          Util::Query::Builder(k).le<uint64_t>(boundary + 5));
    };
    const Read ra = run_query(*a, range(key), true, "root");
    const Read rb = run_query(*b, range(key_b), true, "root");
    BOOST_TEST(!ra.key.empty());
    BOOST_TEST((ra == rb), "key at " << key_at << " range");
    BOOST_TEST(same_bytes(ra.mz, rb.mz));

    // Equalities: answered from the runs without the key, restored with it.
    for (uint64_t s :
         {uint64_t{0}, boundary, boundary + 1, uint64_t{3}, kSpectra - 1, kSpectra}) {
      for (bool with_key : {true, false}) {
        const Read ea =
            run_query(*a, Util::Query::Builder(key).eq<uint64_t>(s), with_key, "root");
        const Read eb =
            run_query(*b, Util::Query::Builder(key_b).eq<uint64_t>(s), with_key, "root");
        BOOST_TEST((ea == eb),
                   "key at " << key_at << " spectrum " << s << " with_key " << with_key);
        BOOST_TEST(same_bytes(ea.mz, eb.mz));
        BOOST_TEST(ea.mz.size() == (s < kSpectra ? points_of(s) : 0u));
        for (uint64_t k : ea.key) BOOST_TEST(k == s);
      }
    }

    for (int g = 0; g < groups; ++g) {
      BOOST_TEST(held_as_top_level_runs(*a, g, key_at), "key at " << key_at << " group " << g);
    }
  }
}

/******************************************************************************/
// A signed key is held in SIGNED order: negative values sort first.  Were the
// runs checked in unsigned order, -3 would look larger than 2 and the group
// would (safely) keep its column; were they SEARCHED in the wrong order, a
// value would be missed.
BOOST_AUTO_TEST_CASE(a_signed_key_is_held_in_signed_order)
{
  Scratch scratch("mzp-test-keyruns-signed");
  Points points;
  for (int64_t s = -40; s < 40; ++s) add_spectrum(points, s, s % 5 == 0 ? 0 : 20 + (s + 40) % 13);
  auto index = MzPeak::open(write_archive(scratch.path / "signed", points, true, 300, true));
  auto table = signal_table(index);
  BOOST_TEST_REQUIRE(table->file_metadata()->num_row_groups() > 2);
  for (int g = 0; g < table->file_metadata()->num_row_groups(); ++g) {
    BOOST_TEST(held_as_runs(*table, g), "group " << g);
    BOOST_TEST(!table->row_group(g)->key_runs->is_unsigned);
  }

  const auto key = table->field("point", "spectrum_index").value();
  for (int64_t s = -41; s <= 40; ++s) {
    const Read read = run_query(*table, Util::Query::Builder(key).eq<int64_t>(s), false);
    const uint64_t n = (s < -40 || s >= 40 || s % 5 == 0) ? 0 : 20 + (s + 40) % 13;
    BOOST_TEST_REQUIRE(read.mz.size() == n, "spectrum " << s);
    for (uint64_t p = 0; p < n; ++p) {
      BOOST_TEST(read.mz[p] == mz_of(s, p));
      BOOST_TEST(read.intensity[p] == intensity_of(s, p));
    }
  }
}

/******************************************************************************/
// The fallback: a group that declares its key sorted and is not keeps its
// column -- and with it exactly the behaviour the read path always had for it.
// Its honest neighbours are still held as runs and still read correctly.
BOOST_AUTO_TEST_CASE(a_false_sorted_declaration_keeps_the_column)
{
  Scratch scratch("mzp-test-keyruns-lies");
  Points points;
  // 30 spectra x 100 points over 1,000-row groups: group 1 holds 10..19, and
  // writes 15 before 12.
  for (int64_t s = 0; s < 30; ++s) {
    const int64_t written = s == 12 ? 15 : s == 15 ? 12 : s;
    add_spectrum(points, written, 100);
  }
  auto index = MzPeak::open(write_archive(scratch.path / "lies", points, true, 1000));
  auto table = signal_table(index);
  BOOST_TEST_REQUIRE(table->file_metadata()->num_row_groups() == 3);
  BOOST_TEST(held_as_runs(*table, 0));
  BOOST_TEST(held_as_runs(*table, 2));

  auto group = table->row_group(1);
  BOOST_TEST(!group->key_runs);
  const auto& point = static_cast<const arrow::StructArray&>(*group->batches[0]->column(0));
  BOOST_TEST(point.field(0)->type_id() == arrow::Type::UINT64);
  // Charged in full, too: the footer cannot see the lie, so the prediction
  // (runs) and the decode (full) disagree only for a group like this one.
  const auto fmd = table->file_metadata();
  BOOST_TEST(Util::decoded_row_group_bytes(*fmd->RowGroup(1), *fmd->schema()) == 1000u * 14);

  auto spectra = index.spectra();
  for (uint64_t s : {0u, 5u, 9u, 20u, 29u}) {
    const auto spectrum = spectra[s];
    BOOST_TEST_REQUIRE(spectrum.mz().size() == 100u, "spectrum " << s);
    BOOST_TEST(spectrum.mz().front() == mz_of(static_cast<int64_t>(s), 0));
    BOOST_TEST(spectrum.intensity().back() == intensity_of(static_cast<int64_t>(s), 99));
  }
}

/******************************************************************************/
// A null in a declared-sorted key keeps the column: here the footer counts it
// (without a count, the definition levels show it -- see
// a_padded_dictionary_page_does_not_hide_a_null).  Both a trailing null (still
// "sorted, nulls last") and one mid-group are caught.
BOOST_AUTO_TEST_CASE(a_nullable_key_with_a_null_keeps_the_column)
{
  Scratch scratch("mzp-test-keyruns-null");
  Points points;
  for (int64_t s = 0; s < 40; ++s) add_spectrum(points, s, 100);
  points.key[1999] = std::nullopt; // last row of group 1
  points.key[2500] = std::nullopt; // middle of group 2
  auto index = MzPeak::open(write_archive(scratch.path / "null", points, true, 1000));
  auto table = signal_table(index);
  BOOST_TEST(held_as_runs(*table, 0));
  BOOST_TEST(!table->row_group(1)->key_runs);
  BOOST_TEST(!table->row_group(2)->key_runs);
  BOOST_TEST(held_as_runs(*table, 3));
  BOOST_TEST(!Util::scan_key_runs(table->reader(), 1));
  BOOST_TEST(!Util::scan_key_runs(table->reader(), 2));
  const auto fmd = table->file_metadata();
  BOOST_TEST(Util::decoded_row_group_bytes(*fmd->RowGroup(1), *fmd->schema()) == 1000u * 23);
  BOOST_TEST(Util::decoded_row_group_bytes(*fmd->RowGroup(3), *fmd->schema()) == 1000u * 14);
}

/******************************************************************************/
// No statistics at all: the data still proves the key sorted and null-free,
// so the group is held as runs -- and, the footer having no word on nulls, it
// is charged in full: an over-count, the safe direction.
BOOST_AUTO_TEST_CASE(without_statistics_the_data_decides)
{
  Scratch scratch("mzp-test-keyruns-nostats");
  Points points;
  for (int64_t s = 0; s < 30; ++s) add_spectrum(points, s, 100);
  auto index =
      MzPeak::open(write_archive(scratch.path / "nostats", points, true, 1000, false, false));
  auto table = signal_table(index);
  const auto fmd = table->file_metadata();
  BOOST_TEST_REQUIRE(!fmd->RowGroup(0)->ColumnChunk(0)->is_stats_set());
  for (int g = 0; g < fmd->num_row_groups(); ++g) {
    BOOST_TEST(held_as_runs(*table, g), "group " << g);
    BOOST_TEST(Util::decoded_row_group_bytes(*fmd->RowGroup(g), *fmd->schema()) == 1000u * 23);
  }
  auto spectra = index.spectra();
  for (uint64_t s = 0; s < 30; ++s) {
    BOOST_TEST_REQUIRE(spectra[s].mz().size() == 100u);
    BOOST_TEST(spectra[s].mz()[42] == mz_of(static_cast<int64_t>(s), 42));
  }
}

/******************************************************************************/
// The null check must not rest on the key's values alone.  A dictionary page
// holds one index per non-null row, but may end its index stream in a
// bit-packed group zero-padded to eight, and the padding reads back as the
// dictionary's first value.  Here five points of spectrum 0 and two null-key
// rows end that way (patched in: Arrow writes the five indices as an RLE run),
// so a values-only read finds seven values, all 0 -- one sorted run over every
// row.  The footer's null count refuses it; without statistics, the definition
// levels do; on data page V1 and V2 alike.  Spectrum 0, because a read with
// levels fills only the five slots that have a value and the scan's buffer
// starts zeroed: under any other key the two zeros after it would fall below
// it and refuse the group on order alone, and the level count would go
// untested.  The group keeps its column, and reads back as the same points
// declared unsorted read.
BOOST_AUTO_TEST_CASE(a_padded_dictionary_page_does_not_hide_a_null)
{
  Scratch scratch("mzp-test-keyruns-padded");
  Points points;
  add_spectrum(points, 0, 5);
  for (uint64_t p = 0; p < 2; ++p) {
    points.key.push_back(std::nullopt);
    points.mz.push_back(mz_of(1, p));
    points.intensity.push_back(intensity_of(1, p));
  }

  for (const auto version :
       {parquet::ParquetDataPageVersion::V1, parquet::ParquetDataPageVersion::V2}) {
    for (bool statistics : {true, false}) {
      const std::string name = std::string(version == parquet::ParquetDataPageVersion::V1
                                               ? "v1"
                                               : "v2") +
                               (statistics ? "-stats" : "-nostats");
      const auto tune = [version](parquet::WriterProperties::Builder& props) {
        props.compression(arrow::Compression::UNCOMPRESSED);
        props.data_page_version(version);
      };
      const fs::path sorted = scratch.path / (name + "-sorted.parquet");
      const fs::path unsorted = scratch.path / (name + "-unsorted.parquet");
      write_points(sorted, points, true, 1 << 20, false, nullptr, statistics, tune);
      write_points(unsorted, points, false, 1 << 20, false, nullptr, statistics, tune);
      pad_key_indices(sorted);
      pad_key_indices(unsorted);

      auto a = open_table(sorted, "data_arrays");
      auto b = open_table(unsorted, "data_arrays");
      BOOST_TEST_REQUIRE(a->file_metadata()->RowGroup(0)->ColumnChunk(0)->is_stats_set() ==
                         statistics);
      BOOST_TEST(!Util::scan_key_runs(a->reader(), 0), name);
      auto group = a->row_group(0);
      BOOST_TEST(!group->key_runs, name);

      // The full decode: 0 five times, then the two nulls.
      BOOST_TEST_REQUIRE(group->batches.size() == 1u);
      const auto& point = static_cast<const arrow::StructArray&>(*group->batches[0]->column(0));
      const auto key = std::dynamic_pointer_cast<arrow::UInt64Array>(point.field(0));
      const bool decoded = key && key->length() == 7;
      BOOST_TEST(decoded, name << ": the key column is not decoded");
      for (int64_t r = 0; decoded && r < 7; ++r) {
        BOOST_TEST(key->IsNull(r) == (r >= 5), name << " row " << r);
        if (r < 5) BOOST_TEST(key->Value(r) == 0u, name << " row " << r);
      }

      const auto key_a = a->field("point", "spectrum_index").value();
      const auto key_b = b->field("point", "spectrum_index").value();
      for (uint64_t s : {uint64_t{0}, uint64_t{7}}) {
        for (bool with_key : {true, false}) {
          const Read ra = run_query(*a, Util::Query::Builder(key_a).eq<uint64_t>(s), with_key);
          const Read rb = run_query(*b, Util::Query::Builder(key_b).eq<uint64_t>(s), with_key);
          BOOST_TEST((ra == rb), name << " spectrum " << s << " with_key " << with_key);
          BOOST_TEST(same_bytes(ra.mz, rb.mz), name << " spectrum " << s);
        }
      }
    }
  }
}

/******************************************************************************/
// A footer whose statistics do not decode gives no null count: the definition
// levels decide, as when there are no statistics at all.  Here the key's min
// and max are cut to seven bytes, which Arrow refuses to decode as an INT64.
// The key is held as runs, and restored it reads back as the same points
// declared unsorted read.  Keeping the column would not do: Arrow's full
// decode of the key decodes the same statistics, and fails.
BOOST_AUTO_TEST_CASE(key_statistics_that_do_not_decode_leave_it_to_the_levels)
{
  Scratch scratch("mzp-test-keyruns-badstats");
  Points points;
  add_spectrum(points, 42, 5);
  add_spectrum(points, 43, 5);
  const fs::path sorted = scratch.path / "sorted.parquet";
  const fs::path unsorted = scratch.path / "unsorted.parquet";
  write_points(sorted, points, true, 1 << 20);
  write_points(unsorted, points, false, 1 << 20);
  truncate_key_statistics(sorted, 42, 43);

  auto a = open_table(sorted, "data_arrays");
  auto b = open_table(unsorted, "data_arrays");
  const auto fmd = a->file_metadata();
  const auto chunk = fmd->RowGroup(0)->ColumnChunk(0);
  BOOST_CHECK_THROW((void)(chunk->is_stats_set() && chunk->statistics()),
                    std::exception);

  std::shared_ptr<const Util::RowGroupBatches> group;
  BOOST_REQUIRE_NO_THROW(group = a->row_group(0));
  BOOST_TEST_REQUIRE(held_as_runs(*a, 0));
  BOOST_TEST((group->key_runs->key == std::vector<uint64_t>{42, 43}));
  BOOST_TEST((group->key_runs->start == std::vector<int64_t>{0, 5, 10}));
  const auto restored = Util::restore_key_column(*group);
  const auto full = b->row_group(0);
  BOOST_TEST_REQUIRE(restored->batches.size() == full->batches.size());
  for (std::size_t i = 0; i < full->batches.size(); ++i) {
    BOOST_TEST(restored->batches[i]->Equals(*full->batches[i]), "batch " << i);
  }
}

/******************************************************************************/
// The bundled fixtures: the honest one is held as runs, the lying one is not,
// so sorting_declaration_test's guarantees are about the unchanged full path.
BOOST_AUTO_TEST_CASE(the_sorting_declaration_fixtures)
{
  auto honest = MzPeak::open("../test/files/declares_sorted_honest.dir");
  auto lies = MzPeak::open("../test/files/declares_sorted_lies.dir");
  BOOST_TEST(held_as_runs(*signal_table(honest), 0));
  BOOST_TEST(!signal_table(lies)->row_group(0)->key_runs);
}

/******************************************************************************/
// Why the runs come from the COLUMN and not from the metadata's point counts.
//
// Here spectrum 1 really has 6 points and spectrum 2 has 4, while the metadata
// still declares 5 each.  Every check a count-based fast path could make at
// open passes: the declared counts sum to the table's 20 rows, the key is
// declared AND truly sorted, and the footer's min/max (0..3) agree.  A range
// built from the counts would hand spectrum 1 five of its points and spectrum 2
// one of spectrum 1's -- and the decode's count check could not object, since
// it compares against the same counts.  Runs read from the column find the true
// 6 and 4, the group is still held as runs (the DATA is honest), and the count
// check refuses both spectra exactly as the full decode does.
BOOST_AUTO_TEST_CASE(inconsistent_metadata_counts_are_never_trusted)
{
  Scratch scratch("mzp-test-keyruns-counts");
  const fs::path source = "../test/files/declares_sorted_honest.dir";
  const fs::path copy = scratch.path / "counts";
  fs::copy(source, copy, fs::copy_options::recursive);

  // Same rows, same values; only the key's split between spectra 1 and 2 moves.
  auto file = value(arrow::io::ReadableFile::Open((source / "spectra_data.parquet").string()));
  auto reader = value(parquet::arrow::OpenFile(file, arrow::default_memory_pool()));
  std::shared_ptr<arrow::Table> table = value(Util::read_table(*reader));
  auto point = std::static_pointer_cast<arrow::StructArray>(
      value(arrow::Concatenate(table->column(0)->chunks())));
  BOOST_TEST_REQUIRE(point->length() == 20);
  const auto& mz = static_cast<const arrow::DoubleArray&>(*point->field(1));
  const auto& intensity = static_cast<const arrow::FloatArray&>(*point->field(2));
  Points points;
  for (int64_t r = 0; r < 20; ++r) {
    points.key.push_back(r < 5 ? 0 : r < 11 ? 1 : r < 15 ? 2 : 3);
    points.mz.push_back(mz.Value(r));
    points.intensity.push_back(intensity.Value(r));
  }
  auto kv = table->schema()->metadata()->Copy();
  if (kv->FindKey("ARROW:schema") >= 0) check(kv->Delete("ARROW:schema"));
  write_points(copy / "spectra_data.parquet", points, true, 1 << 20, false, kv);

  auto honest = MzPeak::open(source).spectra();
  auto index = MzPeak::open(copy);
  auto table_copy = signal_table(index);
  BOOST_TEST(held_as_runs(*table_copy, 0));

  // What an open-time check would have seen.
  const auto fmd = table_copy->file_metadata();
  BOOST_TEST(fmd->num_rows() == 20);
  auto stats = std::static_pointer_cast<parquet::Int64Statistics>(
      fmd->RowGroup(0)->ColumnChunk(0)->statistics());
  BOOST_TEST(stats->min() == 0);
  BOOST_TEST(stats->max() == 3);
  std::size_t declared = 0;
  for (std::size_t s = 0; s < honest.size(); ++s) declared += honest[s].mz().size();
  BOOST_TEST(declared == 20u);

  auto spectra = index.spectra();
  for (std::size_t s : {std::size_t{0}, std::size_t{3}}) {
    BOOST_TEST(same_bytes(spectra[s].mz(), honest[s].mz()), "spectrum " << s);
  }
  for (std::size_t s : {std::size_t{1}, std::size_t{2}}) {
    BOOST_CHECK_THROW(
        {
          auto spectrum = spectra[s];
          (void)spectrum.mz();
        },
        ParquetError);
  }
}
