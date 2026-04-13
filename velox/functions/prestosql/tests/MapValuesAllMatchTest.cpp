/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/functions/prestosql/tests/utils/LambdaParameterizedBaseTest.h"

namespace facebook::velox::functions {
namespace {

class MapValuesAllMatchTest
    : public functions::test::LambdaParameterizedBaseTest {
 protected:
  void allMatch(
      const VectorPtr& input,
      const std::string& lambda,
      const std::vector<std::optional<bool>>& expected) {
    const std::string expr =
        fmt::format("map_values_all_match(c0, x -> ({}))", lambda);
    SCOPED_TRACE(expr);
    auto result = evaluateParameterized(expr, makeRowVector({input}));
    velox::test::assertEqualVectors(
        makeNullableFlatVector<bool>(expected), result);
  }
};

TEST_P(MapValuesAllMatchTest, basic) {
  auto data = makeMapVectorFromJson<int32_t, int64_t>({
      "{1: 10, 2: 20, 3: 30}",
      "{-1: 11, -2: 22}",
  });

  allMatch(data, "x > 0", {true, true});
  allMatch(data, "x > 15", {false, false});
  allMatch(data, "x > 25", {false, false});
  allMatch(data, "x <= 30", {true, true});
  allMatch(data, "x <= 100", {true, true});
  allMatch(data, "x >= 10", {true, true});
  allMatch(data, "x >= 20", {false, false});
}

TEST_P(MapValuesAllMatchTest, nullPredicate) {
  auto data = makeMapVectorFromJson<int32_t, int64_t>({
      "{1: 10, 2: 20, 3: 30}",
      "{-1: 11, -2: 22}",
  });

  // Predicate returns NULL for x=20 and false otherwise -> false (early exit).
  allMatch(data, "if(x = 20, null::boolean, false)", {false, false});
  // Predicate returns NULL for x=20 and true otherwise -> NULL for row 0,
  // true for row 1 (no 20 in second map).
  allMatch(data, "if(x = 20, null::boolean, true)", {std::nullopt, true});
}

TEST_P(MapValuesAllMatchTest, emptyMap) {
  auto data = makeMapVectorFromJson<int32_t, int64_t>({
      "{}",
  });
  // Vacuously true.
  allMatch(data, "x > 0", {true});
  allMatch(data, "x < 0", {true});
}

TEST_P(MapValuesAllMatchTest, stringValues) {
  auto data = makeMapVectorFromJson<int32_t, std::string>({
      R"({1: "apple", 2: "banana", 3: "cherry"})",
      R"({1: "dog", 2: "cat"})",
  });

  allMatch(data, "length(x) > 2", {true, true});
  // apple=5, banana=6, cherry=6 all > 4; dog=3, cat=3 not > 4
  allMatch(data, "length(x) > 4", {true, false});
}

TEST_P(MapValuesAllMatchTest, doubleValues) {
  auto data = makeMapVectorFromJson<int32_t, double>({
      "{1: 1.5, 2: 2.5, 3: 3.5}",
      "{1: 0.1, 2: 0.2}",
  });

  allMatch(data, "x > 1.0", {true, false});
  allMatch(data, "x > 0.0", {true, true});
  allMatch(data, "x < 4.0", {true, true});
}

TEST_P(MapValuesAllMatchTest, varcharKeys) {
  auto data = makeMapVectorFromJson<std::string, int64_t>({
      R"({"a": 10, "b": 20, "c": 30})",
      R"({"x": 5, "y": 15})",
  });

  allMatch(data, "x > 5", {true, false});
  allMatch(data, "x > 0", {true, true});
}

TEST_P(MapValuesAllMatchTest, nullMapValues) {
  // Map with null values in the value vector.
  auto data = makeNullableMapVector<int32_t, int64_t>({
      {{{1, 10}, {2, std::nullopt}, {3, 30}}},
      {{{1, std::nullopt}, {2, std::nullopt}}},
  });

  // The predicate is applied to the value vector; null values should
  // produce null predicate results -> three-valued logic applies.
  // Row 0: 10>0=true, null->null, 30>0=true => NULL
  // Row 1: null->null, null->null => NULL
  allMatch(data, "x > 0", {std::nullopt, std::nullopt});
}

TEST_P(MapValuesAllMatchTest, equivalenceWithMapValues) {
  // Verify map_values_all_match(m, pred) == all_match(map_values(m), pred)
  auto data = makeMapVectorFromJson<int32_t, int64_t>({
      "{1: 10, 2: 20, 3: 30}",
      "{-1: 11, -2: 22}",
      "{}",
      "{1: 5}",
  });

  auto lambdas = {"x > 0", "x > 15", "x <= 30", "x = 10", "x % 2 = 0"};
  for (const auto& lambda : lambdas) {
    SCOPED_TRACE(lambda);
    auto direct = evaluateParameterized(
        fmt::format("map_values_all_match(c0, x -> ({}))", lambda),
        makeRowVector({data}));
    auto viaMapValues = evaluateParameterized(
        fmt::format("all_match(map_values(c0), x -> ({}))", lambda),
        makeRowVector({data}));
    velox::test::assertEqualVectors(viaMapValues, direct);
  }
}

TEST_P(MapValuesAllMatchTest, errorPropagation) {
  // For all_match, error priority: false > error > null > true.
  // If any non-error element is false, result is false (error swallowed).
  // If all non-error elements are true, error propagates.
  auto data = makeMapVectorFromJson<int32_t, int64_t>({
      "{1: 10, 2: 0, 3: 20}",
      "{1: 10, 2: 20}",
  });

  // Row 0: 10>0=true, error(1/0), 20>0=true -> all non-error true,
  //        error propagates -> try wraps to NULL
  // Row 1: 10>0=true, 20>0=true -> true
  auto result = evaluateParameterized(
      "try(map_values_all_match(c0, x -> (if(x = 0, 1/0 > 0, true))))",
      makeRowVector({data}));
  auto expected = makeNullableFlatVector<bool>({std::nullopt, true});
  velox::test::assertEqualVectors(expected, result);
}

VELOX_INSTANTIATE_TEST_SUITE_P(
    MapValuesAllMatchTest,
    MapValuesAllMatchTest,
    testing::ValuesIn(MapValuesAllMatchTest::getTestParams()));

} // namespace
} // namespace facebook::velox::functions
