/*

This file is part of the mzpeak.h project.  It is subject to the
license specified in the LICENSE file which can be found in the
top-level directory of this repository.

*/

#include "mzpeak/util/executor.h"

#include <algorithm>
#include <arrow/array.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <memory>
#include <parquet/arrow/reader.h>
#include <ranges>
#include <string_view>
#include <type_traits>

#include "mzpeak/util/algorithm.h"
#include "mzpeak/util/parquet.h"
#include "mzpeak/util/types.h"

namespace MzPeak::Util {

/******************************************************************************/
struct Executor::Impl {
  Impl(Parquet& source, const Projection& fields)
      : source_(source)
      , projection_(fields)
      , slice_(nullptr)
  {
  }

  /// Helper to check a result and throw an error if necessary.
  template <typename T> T check(const std::string_view& msg, arrow::Result<T> r)
  {
    if (!r.ok()) {
      std::string error("while executing a query: " + std::string(msg) + ": ");
      throw ParquetError(error + r.status().ToString());
    }

    return std::move(r.ValueOrDie());
  }

  /// Run a query on the given record batch and return a vector
  /// indicating which rows should be kept.
  ///
  /// When the query is an equality on a column the row group declares sorted,
  /// `run` receives the matching [offset, end) instead and the returned vector
  /// is empty -- see EqualityScan.
  std::vector<bool> filter(const Planner::Plan&,
                           std::shared_ptr<arrow::RecordBatch>&,
                           std::pair<int64_t, int64_t>& run,
                           bool& have_run);

  /// Capture the requested columns.
  void project(std::shared_ptr<arrow::RecordBatch>&);

  /// Return the array associated with the given field.
  std::shared_ptr<arrow::Array> array(std::shared_ptr<arrow::RecordBatch>& batch,
                                      const Schema::Column& field);

  /// Helper to extract a single value from an array.
  struct ArrayValueHelper {
    template <Type T> Query::Result<Query::value_t> operator()();
    Executor::Impl& impl_;
    const Schema::Column& field_;
    std::shared_ptr<arrow::RecordBatch>& batch_;
    int64_t row_index_;
  };

  Parquet& source_;
  Projection projection_;
  std::unique_ptr<Slice> slice_;

  /// Whether the row group being read declares the predicate's column sorted.
  /// Set per row group in execute(); see EqualityScan.
  bool sorted_column_ = false;
};

/******************************************************************************/
template <Type T>
Query::Result<Query::value_t> Executor::Impl::ArrayValueHelper::operator()()
{
  std::shared_ptr<arrow::Array> a(impl_.array(batch_, field_));

  if (row_index_ < 0 || row_index_ >= a->length() || a->IsNull(row_index_)) {
    return Query::Result<Query::value_t>::skip();
  }

  auto casted = std::static_pointer_cast<typename type_traits<T>::array_type>(a);
  return Query::Result<Query::value_t>(casted->Value(row_index_));
}

/******************************************************************************/
/*
 * Mark the rows of one typed column equal to `wanted`.
 *
 * The general path below resolves the column, dispatches on its type and walks
 * the predicate tree once PER ROW.  For the query this library actually runs --
 * "the entity index equals N" -- that is several hundred nanoseconds of
 * machinery around a single integer comparison, repeated across a whole page.
 */
struct EqualityScan {
  std::shared_ptr<arrow::Array> array;
  Query::value_t wanted;
  std::vector<bool>& want_rows;
  bool& handled;
  /// The row group DECLARES this column sorted ascending, nulls last.
  bool sorted;
  /// When the sorted path applies, the matching run as [offset, end).
  std::pair<int64_t, int64_t>& run;
  bool& have_run;

  template <Type T> void operator()() const
  {
    using value_type = typename type_traits<T>::value_type;

    // A predicate whose value type does not match the column's is not something
    // this scan can answer; fall back rather than silently selecting nothing.
    if (!std::holds_alternative<value_type>(wanted)) return;

    const value_type target = std::get<value_type>(wanted);
    auto typed =
        std::static_pointer_cast<typename type_traits<T>::array_type>(array);

    // The caller leaves the mask empty so the sorted path never pays for one.
    want_rows.assign(static_cast<std::size_t>(typed->length()), true);
    for (int64_t i = 0; i < typed->length(); ++i) {
      // A null never compares equal, and the general path treats an unreadable
      // value as "no opinion" -- it yields no `false`, so the row stays
      // selected.  Matched here so the two paths agree row for row.
      want_rows[static_cast<std::size_t>(i)] =
          typed->IsNull(i) || typed->Value(i) == target;
    }
    handled = true;
  }
};

/******************************************************************************/
std::vector<bool> Executor::Impl::filter(const Planner::Plan& plan,
                                         std::shared_ptr<arrow::RecordBatch>& batch,
                                         std::pair<int64_t, int64_t>& run,
                                         bool& have_run)
{
  // Fast path FIRST, and only then the mask.  Allocating and initialising a
  // bit per row of the page, for every entity, cost more than the search it was
  // there to record -- and on the sorted path it was thrown away unread.
  std::vector<bool> want_rows;

  if (auto equality = plan.query.as_equality()) {
    const auto& [field, wanted] = *equality;
    if (field.second->type().has_value()) {
      std::shared_ptr<arrow::Array> column(array(batch, field));
      bool handled = false;
      lift_type(field.second->type().value(),
                EqualityScan{column, wanted, want_rows, handled, sorted_column_, run,
                             have_run});
      if (have_run) return {};
      if (handled) return want_rows;
    }
  }

  want_rows.assign(static_cast<std::size_t>(batch->num_rows()), true);

  auto get_value =
      [&](int64_t row_index,
          const Schema::Column& field) -> Query::Result<Query::value_t> {
    if (field.second->type().has_value()) {
      return lift_type(field.second->type().value(),
                       ArrayValueHelper{*this, field, batch, row_index});
    } else {
      return Query::Result<Query::value_t>::fail();
    }
  };

  for (int64_t i : std::views::iota(0, batch->num_rows())) {
    Query::Result<bool> res =
        plan.query.eval(std::bind(get_value, i, std::placeholders::_1));
    want_rows[i] = !res.is(false);
  }

  return want_rows;
}

/******************************************************************************/
void Executor::Impl::project(std::shared_ptr<arrow::RecordBatch>& batch)
{
  for (const auto& field : slice_->fields()) {
    slice_->append(field, array(batch, field));
  }
}

/******************************************************************************/
std::shared_ptr<arrow::Array>
Executor::Impl::array(std::shared_ptr<arrow::RecordBatch>& batch,
                      const Schema::Column& field)
{
  std::shared_ptr<arrow::Array> col(batch->column(field.first->index()));

  if (col && col->type_id() == arrow::Type::STRUCT) {
    auto sa = std::static_pointer_cast<arrow::StructArray>(col);
    return sa->field(field.second->relative_index());
  } else {
    throw ParquetError("column not in batch: " + field.first->name() + "." +
                       field.second->name());
  }
}

/******************************************************************************/
Executor::Executor(Parquet& source, const Projection& fields)
    : impl_(std::make_unique<Impl>(source, fields))
{
}

/******************************************************************************/
Executor::~Executor() = default;

/******************************************************************************/
std::unique_ptr<Executor::Slice> Executor::execute(const Planner::Plan& plan)
{
  impl_->slice_ = std::unique_ptr<Slice>(new Slice(impl_->projection_.get()));
  std::map<Schema::Group::index_type, std::vector<Planner::Range>> ranges;

  for (const auto& range : plan.ranges) {
    auto it = ranges.find(range.row_group);

    if (it != ranges.end()) {
      it->second.push_back(range);
    } else {
      ranges[range.row_group] = {range};
    }
  }

  // Reduce a batch to just those rows that were selected by the query
  // and then capture the projected columns.
  auto project_wanted = [&](std::size_t i, std::size_t j,
                            std::shared_ptr<arrow::RecordBatch>& batch) -> void {
    int64_t offset = i;
    int64_t length = j - i + 1;
    if (length <= 0) return;

    auto sliced = batch->Slice(offset, length);
    impl_->project(sliced);
  };

  for (const auto& row_group : ranges) {
    int64_t row_group_start = 0;

    // The last row any range of this group wants.  Batches past it hold
    // nothing for this query.
    int64_t wanted_end = 0;
    for (const auto& range : row_group.second) {
      wanted_end = std::max(wanted_end, range.offset + range.length);
    }

    // Does THIS row group declare the predicate's column sorted?  Asked per
    // group because the declaration is per group.
    impl_->sorted_column_ = false;
    if (auto equality = plan.query.as_equality()) {
      impl_->sorted_column_ = impl_->source_.sorted_ascending(
          row_group.first, equality->first.second->absolute_index());
    }

    // Decoded once per row group and retained, so reading a run entity by
    // entity costs one decode per group rather than one per entity.
    auto batches = impl_->source_.row_group(row_group.first);

    for (const auto& cached : *batches) {
      if (row_group_start >= wanted_end) break;

      std::shared_ptr<arrow::RecordBatch> batch = cached;
      int64_t rows = batch->num_rows();

      for (const auto& range : row_group.second) {
        // Is this batch within this range?
        if (range.offset + range.length <= row_group_start ||
            range.offset >= row_group_start + rows) {
          continue;
        }

        const auto [offset, length] = Algorithm::intersect_range(
            range.offset, range.length, row_group_start, rows);
        if (length == 0) continue;

        auto sliced = batch->Slice(offset, length);

        std::pair<int64_t, int64_t> run{0, 0};
        bool have_run = false;
        auto want_rows = impl_->filter(plan, sliced, run, have_run);

        if (have_run) {
          if (run.second > run.first) {
            project_wanted(static_cast<std::size_t>(run.first),
                           static_cast<std::size_t>(run.second - 1), sliced);
          }
        } else {
          Algorithm::spans(want_rows, project_wanted, sliced);
        }
      }

      row_group_start += batch->num_rows();
    }
  }

  return std::move(impl_->slice_);
}

} // namespace MzPeak::Util
