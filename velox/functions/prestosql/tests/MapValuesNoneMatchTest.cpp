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

class MapValuesNoneMatchTest
    : public functions::test::LambdaParameterizedBaseTest {
 protected:
  void noneMatch(
      const VectorPtr& input,
      const std::string& lambda,
      const std::vector<std::optional<bool>>& expected) {
    const std::string expr =
        fmt::format("map_values_none_match(c0, x -> ({}))", lambda);
    SCOPED_TRACE(expr);
    auto result = evaluateParameterized(expr, makeRowVector({input}));
    velox::test::assertEqualVectors(
        makeNullableFlatVector<bool>(expected), result);
  }
};

TEST_P(MapValuesNoneMatchTest, basic) {
  auto data = makeMapVectorFromJson<int32_t, int64_t>({
      "{1: 10, 2: 20, 3: 30}",
      "{-1: 11, -2: 22}",
  });

  noneMatch(data, "x = 7", {true, true});
  noneMatch(data, "x > 15", {false, false});
  noneMatch(data, "x > 25", {false, true});
  noneMatch(data, "x % 11 = 0", {true, false});
  noneMatch(data, "x < 0", {true, true});
  noneMatch(data, "x > 0", {false, false});
}

TEST_P(MapValuesNoneMatchTest, nullPredicate) {
  auto data = makeMapVectorFromJson<int32_t, int64_t>({
      "{1: 10, 2: 20, 3: 30}",
      "{-1: 11, -2: 22}",
  });

  // Predicate returns NULL for x=20 and false otherwise.
  // Row 0: false, null, false -> NULL (no true/earlyReturn, has null)
  // Row 1: false, false -> true (initialValue preserved)
  noneMatch(data, "if(x = 20, null::boolean, false)", {std::nullopt, true});
  // Predicate returns NULL for x=20 and true otherwise.
  // Row 0: true -> earlyReturn -> false
  // Row 1: true -> earlyReturn -> false
  noneMatch(data, "if(x = 20, null::boolean, true)", {false, false});
}

TEST_P(MapValuesNoneMatchTest, emptyMap) {
  auto data = makeMapVectorFromJson<int32_t, int64_t>({
      "{}",
  });
  // Vacuously true.
  noneMatch(data, "x > 0", {true});
  noneMatch(data, "true", {true});
}

TEST_P(MapValuesNoneMatchTest, stringValues) {
  auto data = makeMapVectorFromJson<int32_t, std::string>({
      R"({1: "apple", 2: "banana", 3: "cherry"})",
      R"({1: "dog", 2: "cat"})",
  });

  noneMatch(data, "x = 'banana'", {false, true});
  noneMatch(data, "x = 'elephant'", {true, true});
  noneMatch(data, "length(x) > 5", {false, true});
}

TEST_P(MapValuesNoneMatchTest, doubleValues) {
  auto data = makeMapVectorFromJson<int32_t, double>({
      "{1: 1.5, 2: 2.5, 3: 3.5}",
      "{1: 0.1, 2: 0.2}",
  });

  noneMatch(data, "x > 10.0", {true, true});
  noneMatch(data, "x < 1.0", {true, false});
  noneMatch(data, "x > 0.0", {false, false});
}

TEST_P(MapValuesNoneMatchTest, varcharKeys) {
  auto data = makeMapVectorFromJson<std::string, int64_t>({
      R"({"a": 10, "b": 20, "c": 30})",
      R"({"x": 5, "y": 15})",
  });

  noneMatch(data, "x > 25", {false, true});
  noneMatch(data, "x < 0", {true, true});
}

TEST_P(MapValuesNoneMatchTest, nullMapValues) {
  auto data = makeNullableMapVector<int32_t, int64_t>({
      {{{1, 10}, {2, std::nullopt}, {3, 30}}},
      {{{1, std::nullopt}, {2, std::nullopt}}},
  });

  // Row 0: 10>25=false, null->null, 30>25=true => false (early return)
  // Row 1: null->null, null->null => NULL
  noneMatch(data, "x > 25", {false, std::nullopt});
}

TEST_P(MapValuesNoneMatchTest, consistencyWithNoValuesMatch) {
  // Verify map_values_none_match == no_values_match for all predicates.
  auto data = makeMapVectorFromJson<int32_t, int64_t>({
      "{1: 10, 2: 20, 3: 30}",
      "{-1: 11, -2: 22}",
      "{}",
      "{1: 5}",
  });

  auto lambdas = {"x > 15", "x = 10", "x < 0", "x IN (20, 11)", "x % 2 = 0"};
  for (const auto& lambda : lambdas) {
    SCOPED_TRACE(lambda);
    auto newFn = evaluateParameterized(
        fmt::format("map_values_none_match(c0, x -> ({}))", lambda),
        makeRowVector({data}));
    auto oldFn = evaluateParameterized(
        fmt::format("no_values_match(c0, x -> ({}))", lambda),
        makeRowVector({data}));
    velox::test::assertEqualVectors(oldFn, newFn);
  }
}

TEST_P(MapValuesNoneMatchTest, equivalenceWithMapValues) {
  // Verify map_values_none_match(m, pred) == none_match(map_values(m), pred)
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
        fmt::format("map_values_none_match(c0, x -> ({}))", lambda),
        makeRowVector({data}));
    auto viaMapValues = evaluateParameterized(
        fmt::format("none_match(map_values(c0), x -> ({}))", lambda),
        makeRowVector({data}));
    velox::test::assertEqualVectors(viaMapValues, direct);
  }
}

TEST_P(MapValuesNoneMatchTest, errorPropagation) {
  // For none_match, false result (earlyReturn) takes priority over errors.
  auto data = makeMapVectorFromJson<int32_t, int64_t>({
      "{1: 10, 2: 0, 3: 20}",
      "{1: 0, 2: 0}",
  });

  // Row 0: x=10 -> if(10=0, error, 10>5)=true -> earlyReturn -> false
  // Row 1: x=0 -> error, x=0 -> error -> try wraps to NULL
  auto result = evaluateParameterized(
      "try(map_values_none_match(c0, x -> (if(x = 0, 1/0 > 0, x > 5))))",
      makeRowVector({data}));
  auto expected = makeNullableFlatVector<bool>({false, std::nullopt});
  velox::test::assertEqualVectors(expected, result);
}

VELOX_INSTANTIATE_TEST_SUITE_P(
    MapValuesNoneMatchTest,
    MapValuesNoneMatchTest,
    testing::ValuesIn(MapValuesNoneMatchTest::getTestParams()));

} // namespace
} // namespace facebook::velox::functions
