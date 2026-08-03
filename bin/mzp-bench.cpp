/*

This file is part of the mzpeak project.  It is subject to the license
specified in the LICENSE file which can be found in the top-level
directory of this repository.

*/

/*
 * A benchmark harness for the reader's peak path.
 *
 * The numbers that motivated this live on files nobody can commit (a 21 M-peak
 * Orbitrap DIA run, a 512 M-peak Astral run), so the harness generates a
 * synthetic run of the same SHAPE instead: the writer bounds row groups at
 * 1 << 20 rows, which is exactly what those files use, so ~1600 points per
 * spectrum reproduces the ~650 spectra-per-row-group regime that makes a
 * per-spectrum decode pathological.
 *
 * Every mode reports wall clock AND peak RSS.  Speed alone would hide the
 * obvious way to "fix" this — decode the whole file up front — which is not a
 * fix for a 512 M-peak run.
 */

#include <arrow/array.h>
#include <arrow/io/file.h>
#include <arrow/record_batch.h>
#include <arrow/table.h>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <numeric>
#include <parquet/arrow/reader.h>
#include <print>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <vector>

#include "mzpeak/open.h"
#include "mzpeak/spectra.h"
#include "mzpeak/writer.h"

namespace {

using clock_type = std::chrono::steady_clock;

/// Seconds elapsed since `start`.
double since(clock_type::time_point start)
{
  return std::chrono::duration<double>(clock_type::now() - start).count();
}

/// Peak resident set size in mebibytes.
///
/// `ru_maxrss` is BYTES on macOS and KILOBYTES on Linux — the same field, two
/// units, no macro to ask.  Getting this wrong reports 1 GB as 1 MB, which is
/// exactly the direction that makes a memory regression invisible.
double peak_rss_mb()
{
  struct rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
#ifdef __APPLE__
  return static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);
#else
  return static_cast<double>(usage.ru_maxrss) / 1024.0;
#endif
}

/// Report a timing line in the same shape for every mode.
void report(std::string_view label,
            double seconds,
            std::size_t spectra,
            std::size_t peaks)
{
  const double ms_per_spectrum =
      spectra > 0 ? (seconds * 1000.0) / static_cast<double>(spectra) : 0.0;
  std::println("{:<22} {:>9.3f} s  {:>10.4f} ms/spectrum  {:>12} peaks  "
               "{:>8.1f} MB peak RSS",
               label, seconds, ms_per_spectrum, peaks, peak_rss_mb());
}

/// A deterministic generator, so a regression is a regression and not a
/// different random file.  Nothing here needs statistical quality.
struct Lcg {
  uint64_t state;
  double next()
  {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53);
  }
};

/*
 * Write a synthetic point-layout run to a directory.
 *
 * m/z ascends within each spectrum (the layout's sorting_rank 0 contract) and
 * the spacing wanders slightly so a delta model has something to model.
 */
int generate(const std::filesystem::path& dir, std::size_t count, std::size_t points)
{
  auto start = clock_type::now();
  Lcg rng{0x9e3779b97f4a7c15ULL};

  std::vector<MzPeak::SpectrumData> spectra;
  spectra.reserve(count);

  for (std::size_t i = 0; i < count; ++i) {
    MzPeak::SpectrumData s;
    s.mz.reserve(points);
    s.intensity.reserve(points);

    double mz = 200.0 + rng.next();
    for (std::size_t j = 0; j < points; ++j) {
      mz += 0.01 + rng.next() * 0.002;
      s.mz.push_back(mz);
      s.intensity.push_back(static_cast<float>(rng.next() * 10000.0));
    }

    s.centroid = false;
    s.ms_level = (i % 11 == 0) ? 1 : 2;
    s.retention_time = static_cast<double>(i) * 0.25; // seconds
    s.id = "index=" + std::to_string(i);
    spectra.push_back(std::move(s));
  }

  std::println("built {} spectra x {} points in memory ({:.1f} MB peak RSS)", count,
               points, peak_rss_mb());

  MzPeak::write_spectra_directory(dir, spectra);
  std::println("wrote {} in {:.1f} s ({:.1f} MB peak RSS)", dir.string(),
               since(start), peak_rss_mb());
  return 0;
}

/// Full forward pass: every spectrum, in order, peaks decoded.
int forward_pass(const std::filesystem::path& path, std::size_t limit)
{
  auto index = MzPeak::open(path);
  auto spectra = index.spectra();
  const std::size_t n = limit > 0 ? std::min(limit, spectra.size()) : spectra.size();

  auto start = clock_type::now();
  std::size_t peaks = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const MzPeak::Spectrum s = spectra[i];
    peaks += s.mz().size();
  }
  report("forward pass", since(start), n, peaks);
  return 0;
}

/// The access pattern the handoff measured: batches through get_spectra_batch.
int batch_pass(const std::filesystem::path& path,
               std::size_t limit,
               std::size_t batch)
{
  auto index = MzPeak::open(path);
  auto spectra = index.spectra();
  const std::size_t n = limit > 0 ? std::min(limit, spectra.size()) : spectra.size();

  auto start = clock_type::now();
  std::size_t peaks = 0;
  for (std::size_t b = 0; b < n; b += batch) {
    std::vector<std::size_t> want(std::min(batch, n - b));
    std::iota(want.begin(), want.end(), b);
    for (const auto& s : spectra.get_spectra_batch(want))
      peaks += s.mz().size();
  }
  report("batch pass", since(start), n, peaks);
  return 0;
}

/// Metadata-only pass: must never touch the peak file.
int metadata_pass(const std::filesystem::path& path)
{
  auto index = MzPeak::open(path);
  auto spectra = index.spectra();

  auto start = clock_type::now();
  std::size_t levels = 0;
  for (std::size_t i = 0; i < spectra.size(); ++i)
    levels += spectra[i].ms_level();
  report("metadata pass", since(start), spectra.size(), levels);
  return 0;
}

/// One extracted-ion chromatogram over the whole run.
int xic(const std::filesystem::path& path, double mz_low, double mz_high)
{
  auto index = MzPeak::open(path);
  auto spectra = index.spectra();

  auto start = clock_type::now();
  auto trace = spectra.extract_ion_chromatogram(mz_low, mz_high, 0.0, 1e12);
  double total = 0.0;
  for (const auto& point : trace)
    total += point.intensity;
  report("xic", since(start), trace.size(), trace.size());
  std::println("  summed intensity {:.6e} over {} samples", total, trace.size());
  return 0;
}

/*
 * The correctness gate for every optimisation that follows.
 *
 * Batching changes WHEN rows are decoded, never WHAT they decode to, so this
 * digest must be bit-identical before and after.  It covers per-spectrum peak
 * COUNT — which is what a straddling bug corrupts, and only for the first
 * spectrum of each row group — as well as the values themselves, which a
 * boundary off-by-one would shift without changing any count.
 */
int checksum(const std::filesystem::path& path)
{
  auto index = MzPeak::open(path);
  auto spectra = index.spectra();

  auto start = clock_type::now();
  uint64_t digest = 1469598103934665603ULL; // FNV-1a offset basis
  auto mix = [&digest](uint64_t v) { digest = (digest ^ v) * 1099511628211ULL; };

  std::size_t peaks = 0;
  for (std::size_t i = 0; i < spectra.size(); ++i) {
    const MzPeak::Spectrum s = spectra[i];
    const auto& mz = s.mz();
    const auto& intensity = s.intensity();
    mix(mz.size());
    peaks += mz.size();
    for (std::size_t j = 0; j < mz.size(); ++j) {
      mix(std::bit_cast<uint64_t>(mz[j]));
      mix(std::bit_cast<uint32_t>(intensity[j]));
    }
  }

  report("checksum", since(start), spectra.size(), peaks);
  std::println("  digest {:016x}", digest);
  return 0;
}

/*
 * The floor: what plain Arrow costs to decode the same bytes.
 *
 * This is the number the whole exercise is measured against.  If a full
 * forward pass through the reader approaches this, the remaining cost is
 * Parquet decoding and no amount of restructuring will help; if it is far
 * above, the gap is ours.
 */
int arrow_floor(const std::filesystem::path& parquet_path)
{
  auto file = arrow::io::ReadableFile::Open(parquet_path.string()).ValueOrDie();
  auto reader =
      parquet::arrow::OpenFile(file, arrow::default_memory_pool()).ValueOrDie();
  const int groups = reader->parquet_reader()->metadata()->num_row_groups();
  const int64_t rows = reader->parquet_reader()->metadata()->num_rows();

  auto start = clock_type::now();
  int64_t seen = 0;
  for (int g = 0; g < groups; ++g) {
    auto batches = reader->GetRecordBatchReader({g}).ValueOrDie();
    for (auto maybe_batch : *batches) {
      auto batch = maybe_batch.ValueOrDie();
      seen += batch->num_rows();
    }
  }
  const double seconds = since(start);
  std::println("{:<22} {:>9.3f} s  {} row groups  {} rows  {:.1f} MB peak RSS",
               "arrow floor", seconds, groups, seen, peak_rss_mb());
  if (seen != rows) std::println("WARNING: read {} of {} rows", seen, rows);
  return 0;
}

int usage()
{
  std::println(stderr, R"(usage:
  mzp-bench gen <dir> [spectra] [points]   write a synthetic run
  mzp-bench pass <path> [limit]            forward pass, peaks decoded
  mzp-bench batch <path> [limit] [size]    get_spectra_batch pass
  mzp-bench meta <path>                    metadata-only pass
  mzp-bench xic <path> [mz_low] [mz_high]  one extracted-ion chromatogram
  mzp-bench checksum <path>                digest of every decoded peak
  mzp-bench arrow <file.parquet>           plain-Arrow decode floor)");
  return 2;
}

std::size_t arg_size(int argc, char** argv, int i, std::size_t fallback)
{
  return i < argc ? static_cast<std::size_t>(std::strtoull(argv[i], nullptr, 10))
                  : fallback;
}

double arg_double(int argc, char** argv, int i, double fallback)
{
  return i < argc ? std::strtod(argv[i], nullptr) : fallback;
}

} // namespace

int main(int argc, char** argv)
{
  if (argc < 3) return usage();
  const std::string_view mode(argv[1]);
  const std::filesystem::path path(argv[2]);

  try {
    if (mode == "gen")
      return generate(path, arg_size(argc, argv, 3, 13009),
                      arg_size(argc, argv, 4, 1627));
    if (mode == "pass") return forward_pass(path, arg_size(argc, argv, 3, 0));
    if (mode == "batch")
      return batch_pass(path, arg_size(argc, argv, 3, 0),
                        arg_size(argc, argv, 4, 512));
    if (mode == "meta") return metadata_pass(path);
    if (mode == "checksum") return checksum(path);
    if (mode == "xic")
      return xic(path, arg_double(argc, argv, 3, 400.0),
                 arg_double(argc, argv, 4, 400.01));
    if (mode == "arrow") return arrow_floor(path);
  } catch (const std::exception& e) {
    std::println(stderr, "error: {}", e.what());
    return 1;
  }

  return usage();
}
