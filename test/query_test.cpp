/*

This file is part of the package mzpeak.  It is subject to the license
in the LICENSE file found in the top-level directory of this project.

*/

#define BOOST_TEST_MODULE Query
#include <boost/test/included/unit_test.hpp>

#include <functional>
#include <memory>
#include <ranges>

#include "mzpeak/index.h"
#include "mzpeak/open.h"
#include "mzpeak/schema/file.h"
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

  // Build a "spectrum_index <= 5" query (uint64 column).
  Util::Query le = Util::Query::Builder(*dest).le<uint64_t>(5);

  // Scalar callback: return a fixed value regardless of destination.
  auto value = [](uint64_t x) {
    return [x](Schema::Column) -> Util::Query::Result<Util::Query::value_t> {
      return Util::Query::Result<Util::Query::value_t>(Util::Query::value_t{x});
    };
  };

  // Range callback: return a fixed [min, max] range regardless of destination.
  auto range = [](uint64_t lo, uint64_t hi) {
    return [lo, hi](Schema::Column) -> Util::Query::Result<Util::Query::range_t> {
      return Util::Query::Result<Util::Query::range_t>(
          Util::Query::range_t{std::pair<uint64_t, uint64_t>{lo, hi}});
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

/******************************************************************************/
// Regression: negating a compound query was ignored because the AND/OR switch
// returned before the not_ handling ran.  Both the AND and OR branches changed,
// so both are exercised here.
BOOST_AUTO_TEST_CASE(negation_applies_to_compound_queries)
{
  using namespace MzPeak;

  auto index = MzPeak::open("../test/files/small.mzpeak");
  auto entry = std::ranges::find(index.files(), Schema::EntityType::Spectrum,
                                 &Schema::File::entity_type);

  BOOST_TEST((entry != index.files().end()));

  auto parquet = index.parquet(*entry);
  auto dest = parquet->field("point", "spectrum_index");
  BOOST_TEST(dest.has_value());

  // Scalar callback: return a fixed value regardless of destination.
  auto value = [](uint64_t x) {
    return [x](Schema::Column) -> Util::Query::Result<Util::Query::value_t> {
      return Util::Query::Result<Util::Query::value_t>(Util::Query::value_t{x});
    };
  };

  // AND branch: (x == 3) && (x >= 1)
  Util::Query both =
      Util::Query::Builder(*dest).eq<uint64_t>(3).and_then(Util::Query::Builder(*dest).ge<uint64_t>(1));
  auto both3 = both.eval(value(3));
  BOOST_TEST(both3.has_value());
  BOOST_TEST(both3.value() == true); // 3 == 3 && 3 >= 1

  auto notboth3 = both.negate().eval(value(3));
  BOOST_TEST(notboth3.has_value());
  BOOST_TEST(notboth3.value() == false); // negation must invert the AND

  auto notboth7 = both.negate().eval(value(7));
  BOOST_TEST(notboth7.has_value());
  BOOST_TEST(notboth7.value() == true); // 7 != 3 -> AND false -> negated true

  // OR branch: (x == 3) || (x == 5)
  Util::Query either =
      Util::Query::Builder(*dest).eq<uint64_t>(3).or_else(Util::Query::Builder(*dest).eq<uint64_t>(5));
  auto either3 = either.eval(value(3));
  BOOST_TEST(either3.has_value());
  BOOST_TEST(either3.value() == true); // 3 == 3

  auto noteither3 = either.negate().eval(value(3));
  BOOST_TEST(noteither3.has_value());
  BOOST_TEST(noteither3.value() == false); // negation must invert the OR

  auto noteither7 = either.negate().eval(value(7));
  BOOST_TEST(noteither7.has_value());
  BOOST_TEST(noteither7.value() == true); // 7 in neither -> OR false -> negated true
}
