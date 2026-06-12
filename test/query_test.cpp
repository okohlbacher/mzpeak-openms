/*

This file is part of the package mzpeak.  It is subject to the license
in the LICENSE file found in the top-level directory of this project.

*/

#define BOOST_TEST_MODULE Query
#include <boost/test/included/unit_test.hpp>

#include <functional>
#include <memory>
#include <ranges>

#include "mzpeak/schema/group.h"
#include "mzpeak/util/compat.h" // IWYU pragma: keep
#include "mzpeak/util/query.h"

/******************************************************************************/
BOOST_AUTO_TEST_CASE(valid_query_logic)
{
  using namespace MzPeak;

  // This makes me want to make the query class a template class.
  Schema::Group::Field field("fake", 0, 0);
  field.type(Util::Type::Int32);

  Schema::Column column =
      std::make_pair(nullptr, std::make_shared<Schema::Group::Field>(field));

  std::vector<int32_t> data = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

  auto get =
      [data](std::size_t index,
             const Schema::Column&) -> Util::Query::Result<Util::Query::value_t> {
    return Util::Query::Result<Util::Query::value_t>(data[index]);
  };

  auto expect =
      [data](std::move_only_function<bool(int32_t)>&& f) -> std::vector<bool> {
    std::vector<bool> res(data.size(), false);

    for (std::size_t i : std::views::iota(0ul, data.size())) {
      res[i] = f(data[i]);
    }

    return res;
  };

  auto run = [&data, &get](const Util::Query& q) -> std::vector<bool> {
    std::vector<bool> results(data.size(), false);

    for (std::size_t i : std::views::iota(0ul, data.size())) {
      auto r = q.eval(std::bind(get, i, std::placeholders::_1));
      results[i] = r.is(true);
    }

    return results;
  };

  { // EQ
    auto e = expect([](auto n) { return n == 1; });
    auto r = run(Util::Query::Builder(column).eq<int32_t>(1));
    BOOST_TEST(r == e, "eq");
  }

  { // GT
    auto e = expect([](auto n) { return n > 5; });
    auto r = run(Util::Query::Builder(column).gt<int32_t>(5));
    BOOST_TEST(r == e, "gt");
  }

  { // LT
    auto e = expect([](auto n) { return n < 5; });
    auto r = run(Util::Query::Builder(column).lt<int32_t>(5));
    BOOST_TEST(r == e, "lt");
  }

  { // GE
    auto e = expect([](auto n) { return n >= 5; });
    auto r = run(Util::Query::Builder(column).ge<int32_t>(5));
    BOOST_TEST(r == e, "ge");
  }

  { // LE
    auto e = expect([](auto n) { return n <= 5; });
    auto r = run(Util::Query::Builder(column).le<int32_t>(5));
    BOOST_TEST(r == e, "le");
  }

  { // Compound AND
    auto q = Util::Query::Builder(column).le<int32_t>(3).and_then(
        Util::Query::Builder(column).eq<int32_t>(1));

    auto e = expect([](auto n) { return n <= 3 && n == 1; });
    auto r = run(q);
    BOOST_TEST(r == e, "&&");
  }

  { // Compound OR
    auto q = Util::Query::Builder(column).le<int32_t>(3).or_else(
        Util::Query::Builder(column).gt<int32_t>(5));

    auto e = expect([](auto n) { return n <= 3 || n > 5; });
    auto r = run(q);
    BOOST_TEST(r == e, "||");
  }

  { // Simple negation.
    auto q = Util::Query::Builder(column).gt<int32_t>(3).negate();
    auto e = expect([](auto n) { return !(n > 3); });
    auto r = run(q);
    BOOST_TEST(r == e, "!");
  }

  { // Negated compound
    auto q = Util::Query::Builder(column)
                 .lt<int32_t>(3)
                 .or_else(Util::Query::Builder(column).gt<int32_t>(5))
                 .negate();

    auto e = expect([](auto n) { return !(n < 3 || n > 5); });
    auto r = run(q);
    BOOST_TEST(r == e, "! ||");
  }
}

/******************************************************************************/
// Regression: Op::LE evaluated `v >= bound` (greater-equal logic) in BOTH the
// scalar matcher (src/query.cpp match(value_t)) and the range matcher
// (match(range_t)).  Both paths are exercised here.
BOOST_AUTO_TEST_CASE(less_equal_predicate_matches_correctly)
{
  using namespace MzPeak;

  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto entry = std::ranges::find(index.files(), Schema::EntityType::Spectrum,
                                 &Schema::File::entity_type);

  BOOST_TEST((entry != index.files().end()));

  auto parquet = index.parquet(*entry);
  auto dest = parquet->field("point", "spectrum_index");
  BOOST_TEST(dest.has_value());

  // Build a "spectrum_index <= 5" query.
  Query le = Query::Builder(*dest).le<int64_t>(5);

  // Scalar callback: return a fixed value regardless of destination.
  auto value = [](int64_t x) {
    return [x](Query::destination_t) -> Query::Result<Query::value_t> {
      return Query::Result<Query::value_t>(Query::value_t{x});
    };
  };

  // Range callback: return a fixed [min, max] range regardless of destination.
  auto range = [](int64_t lo, int64_t hi) {
    return [lo, hi](Query::destination_t) -> Query::Result<Query::range_t> {
      return Query::Result<Query::range_t>(
          Query::range_t{std::pair<int64_t, int64_t>{lo, hi}});
    };
  };

  // Scalar matcher.
  auto r3 = le.eval(value(3));
  BOOST_TEST(r3.has_value());
  BOOST_TEST(r3.value() == true); // 3 <= 5

  auto r5 = le.eval(value(5));
  BOOST_TEST(r5.has_value());
  BOOST_TEST(r5.value() == true); // boundary inclusive

  auto r9 = le.eval(value(9));
  BOOST_TEST(r9.has_value());
  BOOST_TEST(r9.value() == false); // 9 <= 5 is false

  // Range matcher: a [min,max] range can satisfy "<= 5" iff min <= 5.
  auto rng34 = le.eval(range(3, 4));
  BOOST_TEST(rng34.has_value());
  BOOST_TEST(rng34.value() == true); // min 3 <= 5

  auto rng69 = le.eval(range(6, 9));
  BOOST_TEST(rng69.has_value());
  BOOST_TEST(rng69.value() == false); // min 6 <= 5 is false
}
