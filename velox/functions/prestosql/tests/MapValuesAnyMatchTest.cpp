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

class MapValuesAnyMatchTest
    : public functions::test::LambdaParameterizedBaseTest {
 protected:
  void anyMatch(
      const VectorPtr& input,
      const std::string& lambda,
      const std::vector<std::optional<bool>>& expected) {
    const std::string expr =
        fmt::format("map_values_any_match(c0, x -> ({}))", lambda);
    SCOPED_TRACE(expr);
    auto result = evaluateParameterized(expr, makeRowVector({input}));
    velox::test::assertEqualVectors(
        makeNullableFlatVector<bool>(expected), result);
  }
};

TEST_P(MapValuesAnyMatchTest, basic) {
  auto data = makeMapVectorFromJson<int32_t, int64_t>({
      "{1: 10, 2: 20, 3: 30}",
      "{-1: 11, -2: 22}",
  });

  anyMatch(data, "x = 10", {true, false});
  anyMatch(data, "x = 22", {false, true});
  anyMatch(data, "x < 15", {true, true});
  anyMatch(data, "x < 0", {false, false});
  anyMatch(data, "x IN (20, 11)", {true, true});
  anyMatch(data, "x > 25", {true, false});
}

TEST_P(MapValuesAnyMatchTest, nullPredicate) {
  auto data = makeMapVectorFromJson<int32_t, int64_t>({
      "{1: 10, 2: 20, 3: 30}",
      "{-1: 11, -2: 22}",
  });

  // Predicate returns NULL for x=20 and false otherwise.
  // Row 0: false, null, false -> NULL (no true, has null)
  // Row 1: false, false -> false
  anyMatch(data, "if(x = 20, null::boolean, false)", {std::nullopt, false});
  // Predicate returns NULL for x=20 and true otherwise.
  // Row 0: true (early return on x=10)
  // Row 1: true (early return on x=11)
  anyMatch(data, "if(x = 20, null::boolean, true)", {true, true});
}

TEST_P(MapValuesAnyMatchTest, emptyMap) {
  auto data = makeMapVectorFromJson<int32_t, int64_t>({
      "{}",
  });
  anyMatch(data, "x > 0", {false});
  anyMatch(data, "true", {false});
}

TEST_P(MapValuesAnyMatchTest, stringValues) {
  auto data = makeMapVectorFromJson<int32_t, std::string>({
      R"({1: "apple", 2: "banana", 3: "cherry"})",
      R"({1: "dog", 2: "cat"})",
  });

  anyMatch(data, "x = 'banana'", {true, false});
  anyMatch(data, "x = 'elephant'", {false, false});
  anyMatch(data, "length(x) > 5", {true, false});
}

TEST_P(MapValuesAnyMatchTest, doubleValues) {
  auto data = makeMapVectorFromJson<int32_t, double>({
      "{1: 1.5, 2: 2.5, 3: 3.5}",
      "{1: 0.1, 2: 0.2}",
  });

  anyMatch(data, "x < 1.0", {false, true});
  anyMatch(data, "x > 3.0", {true, false});
  anyMatch(data, "x > 0.0", {true, true});
}

TEST_P(MapValuesAnyMatchTest, varcharKeys) {
  auto data = makeMapVectorFromJson<std::string, int64_t>({
      R"({"a": 10, "b": 20, "c": 30})",
      R"({"x": 5, "y": 15})",
  });

  anyMatch(data, "x > 25", {true, false});
  anyMatch(data, "x > 0", {true, true});
}

TEST_P(MapValuesAnyMatchTest, nullMapValues) {
  auto data = makeNullableMapVector<int32_t, int64_t>({
      {{{1, 10}, {2, std::nullopt}, {3, 30}}},
      {{{1, std::nullopt}, {2, std::nullopt}}},
  });

  // Row 0: 10>25=false, null->null, 30>25=true => true (early return)
  // Row 1: null->null, null->null => NULL
  anyMatch(data, "x > 25", {true, std::nullopt});
}

TEST_P(MapValuesAnyMatchTest, consistencyWithAnyValuesMatch) {
  // Verify map_values_any_match == any_values_match for all predicates.
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
        fmt::format("map_values_any_match(c0, x -> ({}))", lambda),
        makeRowVector({data}));
    auto oldFn = evaluateParameterized(
        fmt::format("any_values_match(c0, x -> ({}))", lambda),
        makeRowVector({data}));
    velox::test::assertEqualVectors(oldFn, newFn);
  }
}

TEST_P(MapValuesAnyMatchTest, equivalenceWithMapValues) {
  // Verify map_values_any_match(m, pred) == any_match(map_values(m), pred)
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
        fmt::format("map_values_any_match(c0, x -> ({}))", lambda),
        makeRowVector({data}));
    auto viaMapValues = evaluateParameterized(
        fmt::format("any_match(map_values(c0), x -> ({}))", lambda),
        makeRowVector({data}));
    velox::test::assertEqualVectors(viaMapValues, direct);
  }
}

TEST_P(MapValuesAnyMatchTest, errorPropagation) {
  // For any_match, true result takes priority over errors.
  auto data = makeMapVectorFromJson<int32_t, int64_t>({
      "{1: 10, 2: 0, 3: 20}",
      "{1: 0, 2: 0}",
  });

  // Row 0: x=10 -> if(10=0, error, 10>5)=true -> early return true
  // Row 1: x=0 -> error, x=0 -> error -> try wraps to NULL
  auto result = evaluateParameterized(
      "try(map_values_any_match(c0, x -> (if(x = 0, 1/0 > 0, x > 5))))",
      makeRowVector({data}));
  auto expected = makeNullableFlatVector<bool>({true, std::nullopt});
  velox::test::assertEqualVectors(expected, result);
}

VELOX_INSTANTIATE_TEST_SUITE_P(
    MapValuesAnyMatchTest,
    MapValuesAnyMatchTest,
    testing::ValuesIn(MapValuesAnyMatchTest::getTestParams()));

} // namespace
} // namespace facebook::velox::functions
