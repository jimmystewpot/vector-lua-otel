#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "vector_otel/core/timestamp.hpp"
#include "vector_otel/extractors/metrics_extractor.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace vector_otel {
namespace extractors {
namespace {

class MetricsExtractorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    L_ = luaL_newstate();
    ASSERT_NE(L_, nullptr);
    luaL_openlibs(L_);

    const char* init_script = R"(
      emitted = {}
      function mock_emit(event)
        table.insert(emitted, event)
      end
    )";
    ASSERT_EQ(luaL_dostring(L_, init_script), LUA_OK);

    // Push mock_emit onto stack at index 1
    lua_getglobal(L_, "mock_emit");
    ASSERT_TRUE(lua_isfunction(L_, -1));
  }

  void TearDown() override {
    if (L_ != nullptr) {
      lua_close(L_);
      L_ = nullptr;
    }
  }

  std::string ReadFixture(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
      file.open("../" + path);
    }
    if (!file.is_open()) {
      file.open("../../" + path);
    }
    if (!file.is_open()) {
      return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  lua_State* L_{nullptr};
};

TEST_F(MetricsExtractorTest, SampleFixtureAllMetricTypes) {
  std::string json_data = ReadFixture("tests/fixtures/metrics_sample.json");
  ASSERT_FALSE(json_data.empty()) << "Failed to read tests/fixtures/metrics_sample.json";

  int initial_top = lua_gettop(L_);
  auto result = ProcessMetrics(L_, json_data, 1);

  EXPECT_TRUE(result.success) << "ProcessMetrics failed: " << result.error;
  EXPECT_EQ(result.emitted_count, 5);
  EXPECT_EQ(lua_gettop(L_), initial_top);

  // Validate emitted events via Lua
  const char* verify_script = R"(
    assert(#emitted == 5, "expected 5 emitted events, got " .. #emitted)

    -- Event 1: Gauge (asDouble)
    local e1 = emitted[1].log
    assert(e1 ~= nil, "event 1 log table missing")
    assert(e1.MetricName == "memory_usage", "e1 MetricName mismatch: " .. tostring(e1.MetricName))
    assert(e1.MetricDescription == "Process memory usage", "e1 MetricDescription mismatch")
    assert(e1.MetricUnit == "By", "e1 MetricUnit mismatch")
    assert(e1.MetricType == "gauge", "e1 MetricType mismatch: " .. tostring(e1.MetricType))
    assert(e1.ServiceName == "payment-service", "e1 ServiceName mismatch: " .. tostring(e1.ServiceName))
    assert(e1.ResourceAttributes ~= nil, "e1 ResourceAttributes missing")
    assert(e1.ResourceAttributes["deployment.environment"] == "production", "e1 ResourceAttributes value mismatch")
    assert(e1.ScopeName == "com.acme.metrics", "e1 ScopeName mismatch")
    assert(e1.ScopeVersion == "1.2.3", "e1 ScopeVersion mismatch")
    assert(e1.ScopeAttributes ~= nil and e1.ScopeAttributes["scope.key"] == "scope.val", "e1 ScopeAttributes mismatch")
    assert(e1.Attributes ~= nil and e1.Attributes["host"] == "node1", "e1 Attributes mismatch")
    assert(e1.StartTimeUnix == "2021-08-26T17:45:00.000000000Z", "e1 StartTimeUnix mismatch: " .. tostring(e1.StartTimeUnix))
    assert(e1.TimeUnix == "2021-08-26T17:46:40.000000000Z", "e1 TimeUnix mismatch: " .. tostring(e1.TimeUnix))
    assert(math.abs(e1.Value - 1048576.5) < 0.001, "e1 Value mismatch: " .. tostring(e1.Value))
    assert(e1.Flags == 0, "e1 Flags mismatch")
    assert(e1.Histogram == nil, "e1 Histogram should be nil")
    assert(type(e1.processed_at) == "string" and string.len(e1.processed_at) > 0, "e1 processed_at missing")

    -- Event 2: Gauge (asInt)
    local e2 = emitted[2].log
    assert(e2 ~= nil, "event 2 log table missing")
    assert(e2.MetricName == "memory_usage", "e2 MetricName mismatch")
    assert(e2.MetricType == "gauge", "e2 MetricType mismatch")
    assert(e2.Attributes["host"] == "node2", "e2 Attributes mismatch")
    assert(e2.Value == 2097152, "e2 Value mismatch: " .. tostring(e2.Value))
    assert(e2.Flags == 1, "e2 Flags mismatch")

    -- Event 3: Sum with Exemplar
    local e3 = emitted[3].log
    assert(e3 ~= nil, "event 3 log table missing")
    assert(e3.MetricName == "http_requests_total", "e3 MetricName mismatch")
    assert(e3.MetricDescription == "Total HTTP requests", "e3 MetricDescription mismatch")
    assert(e3.MetricUnit == "1", "e3 MetricUnit mismatch")
    assert(e3.MetricType == "sum", "e3 MetricType mismatch")
    assert(e3.Value == 5000, "e3 Value mismatch: " .. tostring(e3.Value))
    assert(e3.Attributes["status"] == 200, "e3 Attributes status mismatch")
    assert(e3.Flags == 0, "e3 Flags mismatch")
    assert(e3.Exemplars ~= nil and #e3.Exemplars == 1, "e3 Exemplars count mismatch")
    local ex3 = e3.Exemplars[1]
    assert(math.abs(ex3.Value - 1.25) < 0.001, "e3 Exemplar Value mismatch")
    assert(ex3.TraceId == "0123456789abcdef0123456789abcdef", "e3 Exemplar TraceId mismatch")
    assert(ex3.SpanId == "0123456789abcdef", "e3 Exemplar SpanId mismatch")
    assert(ex3.FilteredAttributes ~= nil and ex3.FilteredAttributes["user_id"] == "usr_99", "e3 Exemplar FilteredAttributes mismatch")
    assert(type(ex3.TimeUnix) == "string" and string.len(ex3.TimeUnix) > 0, "e3 Exemplar TimeUnix missing")

    -- Event 4: Histogram with Buckets and Bounds
    local e4 = emitted[4].log
    assert(e4 ~= nil, "event 4 log table missing")
    assert(e4.MetricName == "request_duration_seconds", "e4 MetricName mismatch")
    assert(e4.MetricType == "histogram", "e4 MetricType mismatch")
    assert(e4.Value == nil, "e4 Value must be nil for histogram")
    assert(e4.Attributes["method"] == "POST", "e4 Attributes method mismatch")
    assert(e4.Histogram ~= nil, "e4 Histogram table missing")
    assert(e4.Histogram.count == 100, "e4 Histogram count mismatch: " .. tostring(e4.Histogram.count))
    assert(math.abs(e4.Histogram.sum - 25.5) < 0.001, "e4 Histogram sum mismatch: " .. tostring(e4.Histogram.sum))
    assert(math.abs(e4.Histogram.min - 0.05) < 0.001, "e4 Histogram min mismatch: " .. tostring(e4.Histogram.min))
    assert(math.abs(e4.Histogram.max - 5.2) < 0.001, "e4 Histogram max mismatch: " .. tostring(e4.Histogram.max))
    assert(#e4.Histogram.bucketCounts == 5, "e4 bucketCounts length mismatch: " .. tostring(#e4.Histogram.bucketCounts))
    assert(e4.Histogram.bucketCounts[1] == 10 and e4.Histogram.bucketCounts[2] == 40 and e4.Histogram.bucketCounts[3] == 30 and e4.Histogram.bucketCounts[4] == 15 and e4.Histogram.bucketCounts[5] == 5, "e4 bucketCounts values mismatch")
    assert(#e4.Histogram.explicitBounds == 4, "e4 explicitBounds length mismatch: " .. tostring(#e4.Histogram.explicitBounds))
    assert(math.abs(e4.Histogram.explicitBounds[1] - 0.1) < 0.001, "e4 explicitBounds[1] mismatch")
    assert(math.abs(e4.Histogram.explicitBounds[2] - 0.5) < 0.001, "e4 explicitBounds[2] mismatch")
    assert(math.abs(e4.Histogram.explicitBounds[3] - 1.0) < 0.001, "e4 explicitBounds[3] mismatch")
    assert(math.abs(e4.Histogram.explicitBounds[4] - 2.5) < 0.001, "e4 explicitBounds[4] mismatch")
    assert(e4.Exemplars ~= nil and #e4.Exemplars == 1, "e4 Exemplars count mismatch")
    local ex4 = e4.Exemplars[1]
    assert(math.abs(ex4.Value - 2.45) < 0.001, "e4 Exemplar Value mismatch")
    assert(ex4.TraceId == "fedcba9876543210fedcba9876543210", "e4 Exemplar TraceId mismatch")
    assert(ex4.SpanId == "fedcba9876543210", "e4 Exemplar SpanId mismatch")
    assert(ex4.FilteredAttributes["client_ip"] == "192.168.1.1", "e4 Exemplar FilteredAttributes mismatch")

    -- Event 5: Summary
    local e5 = emitted[5].log
    assert(e5 ~= nil, "event 5 log table missing")
    assert(e5.MetricName == "rpc_latency", "e5 MetricName mismatch")
    assert(e5.MetricType == "summary", "e5 MetricType mismatch")
    assert(e5.Value == nil, "e5 Value should be nil for summary")
    assert(e5.Attributes["service"] == "auth", "e5 Attributes mismatch")
  )";

  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, EmptyAndSparsePayloads) {
  // Empty JSON string
  auto res1 = ProcessMetrics(L_, "", 1);
  EXPECT_FALSE(res1.success);
  EXPECT_FALSE(res1.error.empty());

  // Malformed JSON
  auto res2 = ProcessMetrics(L_, "{invalid json", 1);
  EXPECT_FALSE(res2.success);
  EXPECT_FALSE(res2.error.empty());

  // Empty object
  auto res3 = ProcessMetrics(L_, "{}", 1);
  EXPECT_TRUE(res3.success);
  EXPECT_EQ(res3.emitted_count, 0);

  // Empty resourceMetrics array
  auto res4 = ProcessMetrics(L_, "{\"resourceMetrics\": []}", 1);
  EXPECT_TRUE(res4.success);
  EXPECT_EQ(res4.emitted_count, 0);

  // resourceMetrics with empty scopeMetrics
  auto res5 = ProcessMetrics(L_, "{\"resourceMetrics\": [{\"scopeMetrics\": []}]}", 1);
  EXPECT_TRUE(res5.success);
  EXPECT_EQ(res5.emitted_count, 0);

  // scopeMetrics with empty metrics
  auto res6 =
      ProcessMetrics(L_, "{\"resourceMetrics\": [{\"scopeMetrics\": [{\"metrics\": []}]}]}", 1);
  EXPECT_TRUE(res6.success);
  EXPECT_EQ(res6.emitted_count, 0);
}

TEST_F(MetricsExtractorTest, MissingServiceNameDefaultsToUnknownService) {
  const char* json_data = R"({
    "resourceMetrics": [
      {
        "resource": {
          "attributes": [
            {"key": "cluster", "value": {"stringValue": "prod-1"}}
          ]
        },
        "scopeMetrics": [
          {
            "scope": {"name": "test_scope"},
            "metrics": [
              {
                "name": "counter_no_service",
                "sum": {
                  "dataPoints": [
                    {
                      "timeUnixNano": "1630000000000000000",
                      "asInt": "42"
                    }
                  ]
                }
              }
            ]
          }
        ]
      }
    ]
  })";

  auto result = ProcessMetrics(L_, json_data, 1);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.emitted_count, 1);

  const char* verify_script = R"(
    assert(#emitted == 1, "expected 1 emitted event")
    local e = emitted[1].log
    assert(e.ServiceName == "unknown_service", "expected unknown_service, got " .. tostring(e.ServiceName))
    assert(e.Value == 42, "expected 42, got " .. tostring(e.Value))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, Int64ExtremeValuesPreserved) {
  const char* json_data = R"({
    "resourceMetrics": [
      {
        "scopeMetrics": [
          {
            "metrics": [
              {
                "name": "int64_extremes",
                "gauge": {
                  "dataPoints": [
                    {
                      "timeUnixNano": "1630000000000000000",
                      "asInt": "9223372036854775807"
                    },
                    {
                      "timeUnixNano": "1630000000000000000",
                      "asInt": "-9223372036854775808"
                    }
                  ]
                }
              }
            ]
          }
        ]
      }
    ]
  })";

  auto result = ProcessMetrics(L_, json_data, 1);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.emitted_count, 2);

  const char* verify_script = R"(
    assert(#emitted == 2, "expected 2 emitted events")
    local max_int = emitted[1].log.Value
    local min_int = emitted[2].log.Value
    assert(max_int == 9223372036854775807, "int64 max not preserved: " .. tostring(max_int))
    assert(min_int == -9223372036854775808, "int64 min not preserved: " .. tostring(min_int))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, UnorderedKeysAndSnakeCase) {
  // Key order variation: dataPoints before name, description after gauge,
  // snake_case fields
  const char* json_data = R"({
    "resourceMetrics": [
      {
        "scopeMetrics": [
          {
            "metrics": [
              {
                "gauge": {
                  "data_points": [
                    {
                      "attributes": [{"key": "env", "value": {"string_value": "staging"}}],
                      "as_double": 99.9,
                      "start_time_unix_nano": "1629999900000000000",
                      "time_unix_nano": "1630000000000000000"
                    }
                  ]
                },
                "name": "unordered_gauge",
                "unit": "ms",
                "description": "gauge with unordered keys"
              }
            ],
            "scope": {
              "attributes": [{"key": "scope_attr", "value": {"string_value": "val"}}],
              "version": "0.9.0",
              "name": "unordered_scope"
            }
          }
        ],
        "resource": {
          "attributes": [{"key": "service.name", "value": {"string_value": "unordered_svc"}}]
        }
      }
    ]
  })";

  auto result = ProcessMetrics(L_, json_data, 1);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.emitted_count, 1);

  const char* verify_script = R"(
    assert(#emitted == 1, "expected 1 emitted event")
    local e = emitted[1].log
    assert(e.MetricName == "unordered_gauge", "MetricName mismatch: " .. tostring(e.MetricName))
    assert(e.MetricDescription == "gauge with unordered keys", "MetricDescription mismatch")
    assert(e.MetricUnit == "ms", "MetricUnit mismatch")
    assert(e.ServiceName == "unordered_svc", "ServiceName mismatch")
    assert(e.ScopeName == "unordered_scope", "ScopeName mismatch")
    assert(e.ScopeVersion == "0.9.0", "ScopeVersion mismatch")
    assert(e.ScopeAttributes["scope_attr"] == "val", "ScopeAttributes mismatch")
    assert(e.Attributes["env"] == "staging", "Attributes mismatch")
    assert(math.abs(e.Value - 99.9) < 0.001, "Value mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, ExemplarsAndAttrsCleanedUpOnExtractionFailure) {
  // Test with invalid payload that has exemplars and attributes
  int initial_top = lua_gettop(L_);

  // Probe registry freelist head before extraction
  lua_newtable(L_);
  int probe_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
  luaL_unref(L_, LUA_REGISTRYINDEX, probe_ref);

  const std::string invalid_payload =
      R"({"resourceMetrics": [{"scopeMetrics": [{"metrics": [{"gauge": {"dataPoints": [{"exemplars": [{"timeUnixNano": "1000"}], "asDouble": "invalid_double"}]}}]}]}]})";
  auto res = ProcessMetrics(L_, invalid_payload, 1);
  EXPECT_EQ(lua_gettop(L_), initial_top);

  // Probe registry freelist head after extraction: no references leaked
  lua_newtable(L_);
  int after_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
  luaL_unref(L_, LUA_REGISTRYINDEX, after_ref);
  EXPECT_EQ(after_ref, probe_ref);
}

TEST_F(MetricsExtractorTest, RegistryCleanedUpWhenEmitFails) {
  const char* fail_script = R"(
    function mock_emit(event)
      error("simulated sink failure")
    end
  )";
  ASSERT_EQ(luaL_dostring(L_, fail_script), LUA_OK);
  lua_getglobal(L_, "mock_emit");
  lua_replace(L_, 1);

  int initial_top = lua_gettop(L_);

  lua_newtable(L_);
  int probe_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
  luaL_unref(L_, LUA_REGISTRYINDEX, probe_ref);

  const std::string payload =
      R"({"resourceMetrics": [{"scopeMetrics": [{"metrics": [{"gauge": {"dataPoints": [{"exemplars": [{"timeUnixNano": "1000", "filteredAttributes": [{"key": "f", "value": {"stringValue": "v"}}]}], "attributes": [{"key": "k", "value": {"stringValue": "v"}}], "asDouble": 1.0}]}}]}]}]})";
  auto res = ProcessMetrics(L_, payload, 1);
  EXPECT_FALSE(res.success);
  EXPECT_EQ(lua_gettop(L_), initial_top);

  lua_newtable(L_);
  int after_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
  luaL_unref(L_, LUA_REGISTRYINDEX, after_ref);
  EXPECT_EQ(after_ref, probe_ref);
}

TEST_F(MetricsExtractorTest, ExemplarsFilteredAttributesScopedRefLifecycle) {
  int initial_top = lua_gettop(L_);

  lua_newtable(L_);
  int probe_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
  luaL_unref(L_, LUA_REGISTRYINDEX, probe_ref);

  const std::string payload =
      R"({"resourceMetrics": [{"scopeMetrics": [{"metrics": [{"gauge": {"dataPoints": [{"exemplars": [{"timeUnixNano": "1000000000", "traceId": "0123456789abcdef0123456789abcdef", "spanId": "0123456789abcdef", "asDouble": 3.14, "filteredAttributes": [{"key": "tenant", "value": {"stringValue": "acme"}}]}], "attributes": [{"key": "cluster", "value": {"stringValue": "prod-1"}}], "asDouble": 100.0}]}}]}]}]})";
  auto res = ProcessMetrics(L_, payload, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);
  EXPECT_EQ(lua_gettop(L_), initial_top);

  lua_newtable(L_);
  int after_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
  luaL_unref(L_, LUA_REGISTRYINDEX, after_ref);
  EXPECT_EQ(after_ref, probe_ref);

  const char* verify_script = R"(
    local e = emitted[#emitted].log
    assert(e ~= nil)
    assert(e.Attributes["cluster"] == "prod-1")
    assert(e.Exemplars ~= nil and #e.Exemplars == 1)
    local ex = e.Exemplars[1]
    assert(ex.FilteredAttributes["tenant"] == "acme")
    assert(math.abs(ex.Value - 3.14) < 1e-5)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

}  // namespace

}  // namespace extractors
}  // namespace vector_otel
