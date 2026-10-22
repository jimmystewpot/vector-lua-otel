#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "vector_otel/core/lua_bridge.hpp"
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

TEST_F(MetricsExtractorTest, InvalidDoubleStringDoesNotSetMetricValueKind) {
  const std::string payload = R"({
    "resourceMetrics": [{
      "scopeMetrics": [{
        "metrics": [{
          "name": "test_gauge",
          "gauge": {
            "dataPoints": [{
              "asDouble": "invalid_double",
              "exemplars": [{
                "timeUnixNano": "1000000000",
                "asDouble": "invalid_double"
              }]
            }]
          }
        }, {
          "name": "test_hist",
          "histogram": {
            "dataPoints": [{
              "count": 1,
              "min": "invalid_min",
              "max": "invalid_max"
            }]
          }
        }]
      }]
    }]
  })";

  auto res = ProcessMetrics(L_, payload, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 2);

  const char* verify_script = R"(
    local gauge_event = emitted[#emitted - 1].log
    assert(gauge_event ~= nil)
    assert(gauge_event.Value == nil, "DataPoint Value should not be set when asDouble is invalid")
    assert(gauge_event.Exemplars ~= nil and #gauge_event.Exemplars == 1)
    local ex = gauge_event.Exemplars[1]
    assert(ex.Value == nil, "Exemplar Value should not be set when asDouble is invalid")

    local hist_event = emitted[#emitted].log
    assert(hist_event ~= nil)
    assert(hist_event.Min == nil, "Histogram Min should not be set when min is invalid")
    assert(hist_event.Max == nil, "Histogram Max should not be set when max is invalid")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, ProcessMetricsTablePreservesLuaStackDepthAndClampsHistogramCount) {
  // 1. Test histogram count clamping in ProcessMetrics (JSON path)
  const std::string json_payload = R"({
    "resourceMetrics": [{
      "scopeMetrics": [{
        "metrics": [{
          "name": "clamped_hist_json",
          "histogram": {
            "dataPoints": [{
              "count": "18446744073709551615",
              "sum": 10.0
            }]
          }
        }]
      }]
    }]
  })";
  int initial_top = lua_gettop(L_);
  auto json_res = ProcessMetrics(L_, json_payload, 1);
  EXPECT_TRUE(json_res.success) << json_res.error;
  EXPECT_EQ(json_res.emitted_count, 1);
  EXPECT_EQ(lua_gettop(L_), initial_top);

  // 2. Test ProcessMetricsTable with { metric = { resourceMetrics = { ... } } }
  // and UINT64_MAX count
  const char* push_nested_metric = R"(
    return {
      metric = {
        resourceMetrics = {
          {
            resource = {
              attributes = {
                { key = "service.name", value = { stringValue = "tbl-svc" } }
              }
            },
            scopeMetrics = {
              {
                metrics = {
                  {
                    name = "clamped_hist_tbl",
                    histogram = {
                      dataPoints = {
                        {
                          startTimeUnixNano = "1630000000000000000",
                          timeUnixNano = "1630000001000000000",
                          count = "18446744073709551615",
                          sum = 20.0
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, push_nested_metric), LUA_OK) << lua_tostring(L_, -1);
  int table_top = lua_gettop(L_);
  auto tbl_res1 = ProcessMetricsTable(L_, -1, 1);
  EXPECT_TRUE(tbl_res1.success) << tbl_res1.error;
  EXPECT_EQ(tbl_res1.emitted_count, 1);
  EXPECT_EQ(lua_gettop(L_), table_top);
  lua_pop(L_, 1);
  EXPECT_EQ(lua_gettop(L_), initial_top);

  // Verify both JSON and Table histogram counts were clamped to INT64_MAX
  // (9223372036854775807)
  const char* verify_clamp = R"(
    assert(#emitted == 2, "expected 2 emitted events, got " .. #emitted)
    local h1 = emitted[1].log.Histogram
    assert(h1 ~= nil, "h1 missing")
    assert(h1.count == 9223372036854775807, "h1.count not clamped: " .. tostring(h1.count))
    assert(h1.Count == 9223372036854775807, "h1.Count not clamped: " .. tostring(h1.Count))

    local e2 = emitted[2].log
    local h2 = e2.Histogram
    assert(h2 ~= nil, "h2 missing")
    assert(h2.count == 9223372036854775807, "h2.count not clamped: " .. tostring(h2.count))
    assert(h2.Count == 9223372036854775807, "h2.Count not clamped: " .. tostring(h2.Count))
    assert(e2.StartTimeUnix == "2021-08-26T17:46:40.000000000Z", "StartTimeUnix mismatch: " .. tostring(e2.StartTimeUnix))
    assert(e2.TimeUnix == "2021-08-26T17:46:41.000000000Z", "TimeUnix mismatch: " .. tostring(e2.TimeUnix))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_clamp), LUA_OK) << lua_tostring(L_, -1);

  // 3. Test ProcessMetricsTable with { log = { resourceMetrics = { ... } } }
  const char* push_nested_log = R"(
    return {
      log = {
        resourceMetrics = {}
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, push_nested_log), LUA_OK) << lua_tostring(L_, -1);
  table_top = lua_gettop(L_);
  auto tbl_res_log = ProcessMetricsTable(L_, -1, 1);
  EXPECT_TRUE(tbl_res_log.success) << tbl_res_log.error;
  EXPECT_EQ(tbl_res_log.emitted_count, 0);
  EXPECT_EQ(lua_gettop(L_), table_top);
  lua_pop(L_, 1);

  // 4. Test ProcessMetricsTable with { metric = { unrelated = 1 } }
  const char* push_unrelated = R"(
    return {
      metric = {
        unrelated = 1
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, push_unrelated), LUA_OK) << lua_tostring(L_, -1);
  table_top = lua_gettop(L_);
  auto tbl_res2 = ProcessMetricsTable(L_, -1, 1);
  EXPECT_TRUE(tbl_res2.success) << tbl_res2.error;
  EXPECT_EQ(tbl_res2.emitted_count, 0);
  EXPECT_EQ(lua_gettop(L_), table_top);
  lua_pop(L_, 1);

  // 5. Test ProcessMetricsTable when emit errors preserves stack depth
  const char* fail_emit_script = R"(
    function mock_emit(event)
      error("simulated emit failure in table path")
    end
  )";
  ASSERT_EQ(luaL_dostring(L_, fail_emit_script), LUA_OK);
  lua_getglobal(L_, "mock_emit");
  lua_replace(L_, 1);

  ASSERT_EQ(luaL_dostring(L_, push_nested_metric), LUA_OK) << lua_tostring(L_, -1);
  table_top = lua_gettop(L_);
  auto tbl_res3 = ProcessMetricsTable(L_, -1, 1);
  EXPECT_FALSE(tbl_res3.success);
  EXPECT_EQ(lua_gettop(L_), table_top);
  lua_pop(L_, 1);
  EXPECT_EQ(lua_gettop(L_), initial_top);
}

TEST_F(MetricsExtractorTest, HistogramWithoutMinAndMax) {
  // 1. JSON mode test
  const std::string json_payload = R"({
    "resourceMetrics": [{
      "scopeMetrics": [{
        "metrics": [{
          "name": "hist_no_min_max",
          "histogram": {
            "dataPoints": [{
              "startTimeUnixNano": "1630000000000000000",
              "timeUnixNano": "1630000001000000000",
              "count": 50,
              "sum": 12.5,
              "bucketCounts": ["10", "40"],
              "explicitBounds": [1.0]
            }]
          }
        }]
      }]
    }]
  })";

  auto json_res = ProcessMetrics(L_, json_payload, 1);
  EXPECT_TRUE(json_res.success) << json_res.error;
  EXPECT_EQ(json_res.emitted_count, 1);

  // 2. Table mode test
  const char* push_table_hist = R"(
    return {
      resourceMetrics = {
        {
          scopeMetrics = {
            {
              metrics = {
                {
                  name = "table_hist_no_min_max",
                  histogram = {
                    dataPoints = {
                      {
                        startTimeUnixNano = "1630000000000000000",
                        timeUnixNano = "1630000001000000000",
                        count = 60,
                        sum = 18.0,
                        bucketCounts = { 20, 40 },
                        explicitBounds = { 2.5 }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, push_table_hist), LUA_OK) << lua_tostring(L_, -1);
  auto tbl_res = ProcessMetricsTable(L_, -1, 1);
  EXPECT_TRUE(tbl_res.success) << tbl_res.error;
  EXPECT_EQ(tbl_res.emitted_count, 1);
  lua_pop(L_, 1);

  const char* verify_script = R"(
    assert(#emitted == 2, "expected 2 emitted events, got " .. #emitted)

    -- Event 1: JSON histogram without min/max
    local e1 = emitted[1].log
    assert(e1.MetricName == "hist_no_min_max")
    assert(e1.Histogram ~= nil, "e1 Histogram table missing")
    assert(e1.Histogram.count == 50)
    assert(math.abs(e1.Histogram.sum - 12.5) < 0.001)
    assert(e1.Histogram.min == nil, "e1.Histogram.min must be nil")
    assert(e1.Histogram.max == nil, "e1.Histogram.max must be nil")
    assert(e1.Histogram.Min == nil, "e1.Histogram.Min must be nil")
    assert(e1.Histogram.Max == nil, "e1.Histogram.Max must be nil")
    assert(#e1.Histogram.bucketCounts == 2)
    assert(e1.Histogram.bucketCounts[1] == 10 and e1.Histogram.bucketCounts[2] == 40)
    assert(#e1.Histogram.explicitBounds == 1 and math.abs(e1.Histogram.explicitBounds[1] - 1.0) < 0.001)

    -- Event 2: Table histogram without min/max
    local e2 = emitted[2].log
    assert(e2.MetricName == "table_hist_no_min_max")
    assert(e2.Histogram ~= nil, "e2 Histogram table missing")
    assert(e2.Histogram.count == 60)
    assert(math.abs(e2.Histogram.sum - 18.0) < 0.001)
    assert(e2.Histogram.min == nil, "e2.Histogram.min must be nil")
    assert(e2.Histogram.max == nil, "e2.Histogram.max must be nil")
    assert(e2.Histogram.Min == nil, "e2.Histogram.Min must be nil")
    assert(e2.Histogram.Max == nil, "e2.Histogram.Max must be nil")
    assert(#e2.Histogram.bucketCounts == 2)
    assert(e2.Histogram.bucketCounts[1] == 20 and e2.Histogram.bucketCounts[2] == 40)
    assert(#e2.Histogram.explicitBounds == 1 and math.abs(e2.Histogram.explicitBounds[1] - 2.5) < 0.001)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, SummaryDataPointsWithQuantilesSumCount) {
  const std::string json_payload = R"({
    "resourceMetrics": [{
      "resource": {
        "attributes": [{"key": "service.name", "value": {"stringValue": "summary-svc"}}]
      },
      "scopeMetrics": [{
        "scope": {"name": "summary_scope", "version": "1.0"},
        "metrics": [{
          "name": "rpc_latency_summary",
          "description": "RPC latency summary test",
          "unit": "ms",
          "summary": {
            "dataPoints": [{
              "startTimeUnixNano": "1630000000000000000",
              "timeUnixNano": "1630000002000000000",
              "count": 1000,
              "sum": 50000.0,
              "quantileValues": [
                {"quantile": 0.5, "value": 45.0},
                {"quantile": 0.9, "value": 90.0},
                {"quantile": 0.99, "value": 150.0}
              ],
              "attributes": [{"key": "endpoint", "value": {"stringValue": "/api/v1/checkout"}}],
              "flags": 1
            }]
          }
        }]
      }]
    }]
  })";

  auto json_res = ProcessMetrics(L_, json_payload, 1);
  EXPECT_TRUE(json_res.success) << json_res.error;
  EXPECT_EQ(json_res.emitted_count, 1);

  // Table mode summary
  const char* push_table_summary = R"(
    return {
      resourceMetrics = {
        {
          scopeMetrics = {
            {
              metrics = {
                {
                  name = "table_summary_metric",
                  description = "Table summary metric test",
                  unit = "s",
                  summary = {
                    dataPoints = {
                      {
                        startTimeUnixNano = "1630000000000000000",
                        timeUnixNano = "1630000003000000000",
                        count = 200,
                        sum = 600.0,
                        quantileValues = {
                          { quantile = 0.5, value = 2.5 },
                          { quantile = 0.95, value = 5.0 }
                        },
                        attributes = {
                          { key = "handler", value = { stringValue = "login" } }
                        },
                        flags = 0
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, push_table_summary), LUA_OK) << lua_tostring(L_, -1);
  auto tbl_res = ProcessMetricsTable(L_, -1, 1);
  EXPECT_TRUE(tbl_res.success) << tbl_res.error;
  EXPECT_EQ(tbl_res.emitted_count, 1);
  lua_pop(L_, 1);

  const char* verify_script = R"(
    assert(#emitted == 2, "expected 2 emitted events, got " .. #emitted)

    -- Event 1: JSON Summary
    local e1 = emitted[1].log
    assert(e1.MetricName == "rpc_latency_summary")
    assert(e1.MetricDescription == "RPC latency summary test")
    assert(e1.MetricUnit == "ms")
    assert(e1.MetricType == "summary")
    assert(e1.ServiceName == "summary-svc")
    assert(e1.ScopeName == "summary_scope")
    assert(e1.ScopeVersion == "1.0")
    assert(e1.Value == nil, "Summary Value must be nil")
    assert(e1.Histogram == nil, "Summary Histogram must be nil")
    assert(e1.Attributes["endpoint"] == "/api/v1/checkout")
    assert(e1.Flags == 1)
    assert(e1.StartTimeUnix == "2021-08-26T17:46:40.000000000Z")
    assert(e1.TimeUnix == "2021-08-26T17:46:42.000000000Z")

    -- Event 2: Table Summary
    local e2 = emitted[2].log
    assert(e2.MetricName == "table_summary_metric")
    assert(e2.MetricDescription == "Table summary metric test")
    assert(e2.MetricUnit == "s")
    assert(e2.MetricType == "summary")
    assert(e2.Value == nil, "Table summary Value must be nil")
    assert(e2.Histogram == nil, "Table summary Histogram must be nil")
    assert(e2.Attributes["handler"] == "login")
    assert(e2.Flags == 0)
    assert(e2.StartTimeUnix == "2021-08-26T17:46:40.000000000Z")
    assert(e2.TimeUnix == "2021-08-26T17:46:43.000000000Z")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, ExponentialHistogramDataPointsSkippedSafely) {
  // 1. JSON mode: exponentialHistogram along with standard gauge
  const std::string json_payload = R"({
    "resourceMetrics": [{
      "scopeMetrics": [{
        "metrics": [
          {
            "name": "exp_hist_skipped",
            "exponentialHistogram": {
              "dataPoints": [{
                "startTimeUnixNano": "1630000000000000000",
                "timeUnixNano": "1630000001000000000",
                "count": 10,
                "sum": 25.0,
                "scale": 1,
                "zeroCount": 2,
                "positive": {
                  "offset": 1,
                  "bucketCounts": ["1", "3", "4"]
                },
                "negative": {
                  "offset": 0,
                  "bucketCounts": []
                }
              }]
            }
          },
          {
            "name": "valid_gauge_alongside",
            "gauge": {
              "dataPoints": [{
                "timeUnixNano": "1630000001000000000",
                "asDouble": 77.5
              }]
            }
          }
        ]
      }]
    }]
  })";

  auto json_res = ProcessMetrics(L_, json_payload, 1);
  EXPECT_TRUE(json_res.success) << json_res.error;
  EXPECT_EQ(json_res.emitted_count, 1);

  // 2. Only exponentialHistogram in JSON payload -> returns success with 0
  // emitted
  const std::string only_exp_json = R"({
    "resourceMetrics": [{
      "scopeMetrics": [{
        "metrics": [{
          "name": "only_exp_hist",
          "exponentialHistogram": {
            "dataPoints": [{
              "count": 5,
              "scale": 3,
              "zeroCount": 1
            }]
          }
        }]
      }]
    }]
  })";
  auto only_res = ProcessMetrics(L_, only_exp_json, 1);
  EXPECT_TRUE(only_res.success);
  EXPECT_EQ(only_res.emitted_count, 0);

  // 3. Table mode: exponentialHistogram along with sum
  const char* push_table_exp = R"(
    return {
      resourceMetrics = {
        {
          scopeMetrics = {
            {
              metrics = {
                {
                  name = "table_exp_hist",
                  exponentialHistogram = {
                    dataPoints = {
                      { scale = 2, zeroCount = 0, count = 10 }
                    }
                  }
                },
                {
                  name = "table_valid_sum",
                  sum = {
                    dataPoints = {
                      { asInt = 500 }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, push_table_exp), LUA_OK) << lua_tostring(L_, -1);
  auto tbl_res = ProcessMetricsTable(L_, -1, 1);
  EXPECT_TRUE(tbl_res.success) << tbl_res.error;
  EXPECT_EQ(tbl_res.emitted_count, 1);
  lua_pop(L_, 1);

  const char* verify_script = R"(
    assert(#emitted == 2, "expected 2 emitted events, got " .. #emitted)
    local e1 = emitted[1].log
    assert(e1.MetricName == "valid_gauge_alongside")
    assert(math.abs(e1.Value - 77.5) < 0.001)

    local e2 = emitted[2].log
    assert(e2.MetricName == "table_valid_sum")
    assert(e2.Value == 500)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, MetricDescriptionAndUnitPopulatedOrOmitted) {
  const std::string json_payload = R"({
    "resourceMetrics": [{
      "scopeMetrics": [{
        "metrics": [
          {
            "name": "metric_with_meta",
            "description": "Network throughput in megabits per second",
            "unit": "Mbit/s",
            "gauge": {
              "dataPoints": [{"asDouble": 128.5}]
            }
          },
          {
            "name": "metric_without_meta",
            "gauge": {
              "dataPoints": [{"asDouble": 64.0}]
            }
          }
        ]
      }]
    }]
  })";

  auto res = ProcessMetrics(L_, json_payload, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 2);

  const char* verify_script = R"(
    assert(#emitted == 2)
    local e1 = emitted[1].log
    assert(e1.MetricName == "metric_with_meta")
    assert(e1.MetricDescription == "Network throughput in megabits per second")
    assert(e1.MetricUnit == "Mbit/s")

    local e2 = emitted[2].log
    assert(e2.MetricName == "metric_without_meta")
    assert(e2.MetricDescription == "")
    assert(e2.MetricUnit == "")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, ExemplarsWithAsDoubleAndAsInt) {
  const std::string json_payload = R"({
    "resourceMetrics": [{
      "scopeMetrics": [{
        "metrics": [{
          "name": "metric_exemplars_mix",
          "sum": {
            "dataPoints": [{
              "asInt": 1000,
              "exemplars": [
                {
                  "timeUnixNano": "1630000000000000000",
                  "traceId": "0123456789abcdef0123456789abcdef",
                  "spanId": "0123456789abcdef",
                  "asDouble": 12.345,
                  "filteredAttributes": [{"key": "tag_dbl", "value": {"stringValue": "val_dbl"}}]
                },
                {
                  "time_unix_nano": "1630000001000000000",
                  "trace_id": "fedcba9876543210fedcba9876543210",
                  "span_id": "fedcba9876543210",
                  "asInt": "8888",
                  "filtered_attributes": [{"key": "tag_int", "value": {"intValue": 99}}]
                },
                {
                  "as_int": -42
                }
              ]
            }]
          }
        }]
      }]
    }]
  })";

  auto res = ProcessMetrics(L_, json_payload, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_script = R"(
    local e = emitted[1].log
    assert(e.MetricName == "metric_exemplars_mix")
    assert(e.Exemplars ~= nil and #e.Exemplars == 3, "expected 3 exemplars, got " .. tostring(#e.Exemplars))

    -- Exemplar 1: asDouble
    local ex1 = e.Exemplars[1]
    assert(math.abs(ex1.Value - 12.345) < 0.001)
    assert(math.type(ex1.Value) == "float", "ex1 Value must be float")
    assert(ex1.TimeUnix == "2021-08-26T17:46:40.000000000Z")
    assert(ex1.TraceId == "0123456789abcdef0123456789abcdef")
    assert(ex1.SpanId == "0123456789abcdef")
    assert(ex1.FilteredAttributes["tag_dbl"] == "val_dbl")

    -- Exemplar 2: asInt string
    local ex2 = e.Exemplars[2]
    assert(ex2.Value == 8888)
    assert(math.type(ex2.Value) == "integer", "ex2 Value must be integer")
    assert(ex2.TimeUnix == "2021-08-26T17:46:41.000000000Z")
    assert(ex2.TraceId == "fedcba9876543210fedcba9876543210")
    assert(ex2.SpanId == "fedcba9876543210")
    assert(ex2.FilteredAttributes["tag_int"] == 99)

    -- Exemplar 3: as_int snake_case negative
    local ex3 = e.Exemplars[3]
    assert(ex3.Value == -42)
    assert(math.type(ex3.Value) == "integer", "ex3 Value must be integer")
    assert(ex3.TimeUnix == "")
    assert(ex3.TraceId == "")
    assert(ex3.SpanId == "")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, MetricsAggregationTemporalityAndFlags) {
  const std::string json_payload = R"({
    "resourceMetrics": [{
      "scopeMetrics": [{
        "metrics": [
          {
            "name": "delta_sum",
            "sum": {
              "aggregationTemporality": 1,
              "isMonotonic": true,
              "dataPoints": [{
                "asInt": 150,
                "flags": 1
              }]
            }
          },
          {
            "name": "cumulative_hist",
            "histogram": {
              "aggregation_temporality": 2,
              "dataPoints": [{
                "count": 25,
                "sum": 100.0,
                "flags": 4
              }]
            }
          }
        ]
      }]
    }]
  })";

  auto res = ProcessMetrics(L_, json_payload, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 2);

  const char* verify_script = R"(
    assert(#emitted == 2)
    local e1 = emitted[1].log
    assert(e1.MetricName == "delta_sum")
    assert(e1.Flags == 1)

    local e2 = emitted[2].log
    assert(e2.MetricName == "cumulative_hist")
    assert(e2.Flags == 4)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, GaugesAndSumsIntegerVsDoubleValues) {
  const std::string json_payload = R"({
    "resourceMetrics": [{
      "scopeMetrics": [{
        "metrics": [
          {
            "name": "gauge_int",
            "gauge": {
              "dataPoints": [{"asInt": "100"}]
            }
          },
          {
            "name": "gauge_double",
            "gauge": {
              "dataPoints": [{"asDouble": 100.5}]
            }
          },
          {
            "name": "sum_int",
            "sum": {
              "dataPoints": [{"as_int": -200}]
            }
          },
          {
            "name": "sum_double",
            "sum": {
              "dataPoints": [{"as_double": -200.75}]
            }
          }
        ]
      }]
    }]
  })";

  auto res = ProcessMetrics(L_, json_payload, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 4);

  const char* verify_script = R"(
    assert(#emitted == 4)
    local g_int = emitted[1].log
    assert(g_int.Value == 100)
    assert(math.type(g_int.Value) == "integer")

    local g_dbl = emitted[2].log
    assert(math.abs(g_dbl.Value - 100.5) < 0.001)
    assert(math.type(g_dbl.Value) == "float")

    local s_int = emitted[3].log
    assert(s_int.Value == -200)
    assert(math.type(s_int.Value) == "integer")

    local s_dbl = emitted[4].log
    assert(math.abs(s_dbl.Value - (-200.75)) < 0.001)
    assert(math.type(s_dbl.Value) == "float")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, TableModeLongMetadataPreserved) {
  const char* setup = R"(
    return {
      resourceMetrics = {
        {
          resource = {
            attributes = {
              {
                key = "service.name",
                value = {
                  stringValue = "long-metrics-service-name-exceeding-sixty-four-characters-for-zero-copy-view-test-123456"
                }
              }
            }
          },
          scopeMetrics = {
            {
              scope = {
                name = "long.metrics.instrumentation.scope.name.exceeding.sixty.four.characters.0123456789",
                version = "2.0.0-long-metrics-version-string-exceeding-sixty-four-characters-for-testing-987654321"
              },
              metrics = {
                {
                  name = "long.metric.name.exceeding.sixty.four.characters.for.zero.copy.testing.0123456789",
                  description = "Long metric description exceeding sixty-four characters to verify zero-copy string_view lifetime across data points",
                  unit = "long_metric_unit_exceeding_sixty_four_characters_for_zero_copy_test_bytes_per_sec",
                  gauge = {
                    dataPoints = {
                      {
                        timeUnixNano = "1700000000000000000",
                        asDouble = 42.5
                      },
                      {
                        timeUnixNano = "1700000001000000000",
                        asDouble = 84.0
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, setup), LUA_OK) << lua_tostring(L_, -1);
  int payload_idx = lua_gettop(L_);
  auto res = ProcessMetricsTable(L_, payload_idx, 1);
  lua_pop(L_, 1);

  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 2);

  const char* verify_script = R"(
    assert(#emitted == 2, "expected 2 emitted events, got " .. #emitted)
    for i = 1, 2 do
      local e = emitted[i].log
      assert(e ~= nil)
      assert(e.ServiceName == "long-metrics-service-name-exceeding-sixty-four-characters-for-zero-copy-view-test-123456",
             "ServiceName mismatch: " .. tostring(e.ServiceName))
      assert(e.ScopeName == "long.metrics.instrumentation.scope.name.exceeding.sixty.four.characters.0123456789",
             "ScopeName mismatch: " .. tostring(e.ScopeName))
      assert(e.ScopeVersion == "2.0.0-long-metrics-version-string-exceeding-sixty-four-characters-for-testing-987654321",
             "ScopeVersion mismatch: " .. tostring(e.ScopeVersion))
      assert(e.MetricName == "long.metric.name.exceeding.sixty.four.characters.for.zero.copy.testing.0123456789",
             "MetricName mismatch: " .. tostring(e.MetricName))
      assert(e.MetricDescription == "Long metric description exceeding sixty-four characters to verify zero-copy string_view lifetime across data points",
             "MetricDescription mismatch: " .. tostring(e.MetricDescription))
      assert(e.MetricUnit == "long_metric_unit_exceeding_sixty_four_characters_for_zero_copy_test_bytes_per_sec",
             "MetricUnit mismatch: " .. tostring(e.MetricUnit))
    end
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, TableModeNumericScopeMetadataStillCoerced) {
  const char* setup = R"(
    return {
      resourceMetrics = {
        {
          resource = {
            attributes = {
              { key = "service.name", value = { stringValue = "num-scope-metrics-svc" } }
            }
          },
          scopeMetrics = {
            {
              scope = {
                name = 999,
                version = 888
              },
              metrics = {
                {
                  name = "coerced_metric",
                  gauge = {
                    dataPoints = {
                      {
                        timeUnixNano = "1700000000000000000",
                        asInt = 7
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, setup), LUA_OK) << lua_tostring(L_, -1);
  int payload_idx = lua_gettop(L_);
  auto res = ProcessMetricsTable(L_, payload_idx, 1);
  lua_pop(L_, 1);

  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_script = R"(
    local e = emitted[#emitted].log
    assert(e ~= nil)
    assert(e.ScopeName == "999", "ScopeName mismatch: " .. tostring(e.ScopeName))
    assert(e.ScopeVersion == "888", "ScopeVersion mismatch: " .. tostring(e.ScopeVersion))
    assert(e.MetricName == "coerced_metric", "MetricName mismatch: " .. tostring(e.MetricName))
    assert(e.Value == 7, "Value mismatch: " .. tostring(e.Value))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, JsonModeDuplicateServiceNameSurvivesGcInEmit) {
  const char* gc_emit_script = R"(
    gc_emitted = {}
    function gc_emit(event)
      table.insert(gc_emitted, event)
      collectgarbage("collect")
      collectgarbage("collect")
    end
  )";
  ASSERT_EQ(luaL_dostring(L_, gc_emit_script), LUA_OK) << lua_tostring(L_, -1);
  lua_getglobal(L_, "gc_emit");
  int gc_emit_idx = lua_gettop(L_);

  // 80-char service.name string (non-interned long string in Lua 5.4) followed
  // by a duplicate service.name with intValue 42 across two gauge data points.
  const std::string long_svc(80, 's');
  std::string json =
      R"({"resourceMetrics":[{"resource":{"attributes":[)"
      R"({"key":"service.name","value":{"stringValue":")" +
      long_svc +
      R"("}},{"key":"service.name","value":{"intValue":"42"}})"
      R"(]},"scopeMetrics":[{"metrics":[{"name":"dup_svc_gauge","gauge":{"dataPoints":[)"
      R"({"timeUnixNano":"1700000000000000000","asInt":"10"},)"
      R"({"timeUnixNano":"1700000001000000000","asInt":"20"})"
      R"(]}}]}]}]})";

  auto res = ProcessMetrics(L_, json, gc_emit_idx);
  lua_pop(L_, 1);

  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 2);

  const char* verify_script = R"(
    assert(#gc_emitted == 2, "Expected 2 events, got " .. #gc_emitted)
    for i = 1, 2 do
      local e = gc_emitted[i].log
      assert(e.ServiceName == "unknown_service",
             "Event " .. i .. " ServiceName mismatch: " .. tostring(e.ServiceName))
      assert(e.ResourceAttributes["service.name"] == 42,
             "Event " .. i .. " ResourceAttributes['service.name'] mismatch: " ..
             tostring(e.ResourceAttributes["service.name"]))
    end
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, DuplicateNonStringServiceNameDoesNotUseAfterFreeUnderGc) {
  const char* gc_emit_script = R"(
    uaf_emitted = {}
    function uaf_gc_emit(event)
      table.insert(uaf_emitted, event)
      collectgarbage("collect")
    end
  )";
  ASSERT_EQ(luaL_dostring(L_, gc_emit_script), LUA_OK) << lua_tostring(L_, -1);
  lua_getglobal(L_, "uaf_gc_emit");
  int gc_emit_idx = lua_gettop(L_);

  const std::string long_svc(64, 'a');
  std::string json = R"({"resourceMetrics":[{"resource":{"attributes":[)"
                     R"({"key":"service.name","value":{"stringValue":")" +
                     long_svc +
                     R"("}},{"key":"service.name","value":{"intValue":"42"}})"
                     R"(]},"scopeMetrics":[{"metrics":[{"name":"uaf_gauge","gauge":{"dataPoints":[)"
                     R"({"timeUnixNano":"1700000000000000000","asInt":"1"},)"
                     R"({"timeUnixNano":"1700000001000000000","asInt":"2"})"
                     R"(]}}]}]}]})";

  auto res = ProcessMetrics(L_, json, gc_emit_idx);
  lua_pop(L_, 1);

  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 2);

  const char* verify_script = R"(
    assert(#uaf_emitted == 2)
    assert(uaf_emitted[1].log.ServiceName == "unknown_service")
    assert(uaf_emitted[2].log.ServiceName == "unknown_service")
    assert(uaf_emitted[1].log.ResourceAttributes["service.name"] == 42)
    assert(uaf_emitted[2].log.ResourceAttributes["service.name"] == 42)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, JsonModeServiceNameSurvivesCallbackRemovingResourceAttribute) {
  const char* uaf_script = R"(
    uaf_emitted = {}
    function uaf_cb(event)
      table.insert(uaf_emitted, event)
      event.log.ResourceAttributes["service.name"] = nil
      collectgarbage("collect")
      collectgarbage("collect")
      for i = 1, 200 do
        local _ = string.rep("x", 80) .. i
      end
    end
  )";
  ASSERT_EQ(luaL_dostring(L_, uaf_script), LUA_OK) << lua_tostring(L_, -1);
  lua_getglobal(L_, "uaf_cb");
  const int cb_idx = lua_gettop(L_);
  const std::string svc(80, 'Z');

  const std::string json =
      R"({"resourceMetrics":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":")" +
      svc +
      R"("}}]},"scopeMetrics":[{"scope":{"name":"s"},"metrics":[{"name":"g","gauge":{"dataPoints":[)"
      R"({"timeUnixNano":"1","asInt":"1"},)"
      R"({"timeUnixNano":"2","asInt":"2"},)"
      R"({"timeUnixNano":"3","asInt":"3"})"
      R"(]}}]}]}]})";

  auto res = ProcessMetrics(L_, json, cb_idx);
  lua_pop(L_, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 3);

  const char* verify_script = R"(
    assert(#uaf_emitted == 3, "expected 3 events, got " .. #uaf_emitted)
    for i = 1, 3 do
      local name = uaf_emitted[i].log.ServiceName
      assert(name == string.rep("Z", 80), "event " .. i .. " ServiceName: " .. tostring(name))
    end
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

// Batch mode only: when a later batch fails in emit, the result reports only the rows whose
// emit call succeeded (the first full batch), not the rows queued for the failed batch.
TEST_F(MetricsExtractorTest, BatchModeEmitFailureReportsOnlyDeliveredRows) {
  if (::vector_otel::core::kCompileTimeBatchSize != 3) {
    GTEST_SKIP() << "requires VECTOR_OTEL_EMIT_BATCH_SIZE == 3";
  }
  ASSERT_EQ(luaL_dostring(L_, R"(
    batch_emit_calls = 0
    function batch_emit(event)
      batch_emit_calls = batch_emit_calls + 1
      if batch_emit_calls == 2 then error("boom") end
    end
  )"),
            LUA_OK);
  lua_getglobal(L_, "batch_emit");
  const int emit_idx = lua_gettop(L_);

  // Four data points: the first batch of 3 is delivered, the final batch of 1 fails.
  const std::string json =
      R"({"resourceMetrics":[{"scopeMetrics":[{"metrics":[{"name":"g","gauge":{"dataPoints":[)"
      R"({"timeUnixNano":"1","asInt":"1"},{"timeUnixNano":"2","asInt":"2"},)"
      R"({"timeUnixNano":"3","asInt":"3"},{"timeUnixNano":"4","asInt":"4"}]}}]}]}]})";
  auto res = ProcessMetrics(L_, json, emit_idx);
  lua_pop(L_, 1);
  EXPECT_FALSE(res.success);
  EXPECT_EQ(res.emitted_count, 3);
  lua_getglobal(L_, "batch_emit_calls");
  EXPECT_EQ(lua_tointeger(L_, -1), 2);
  lua_pop(L_, 1);
}

TEST_F(MetricsExtractorTest, JsonModeExemplarIdsNormalizedToLowercaseHex) {
  // Test JSON mode: uppercase hex IDs in exemplars for gauge, sum, and histogram
  // are normalized to canonical lowercase hex, with full schema parity.
  const std::string json = R"({
    "resourceMetrics": [{
      "resource": {
        "attributes": [{"key": "service.name", "value": {"stringValue": "metrics-json-svc"}}]
      },
      "scopeMetrics": [{
        "scope": {"name": "metrics-scope", "version": "1.0"},
        "metrics": [
          {
            "name": "cpu_gauge",
            "gauge": {
              "dataPoints": [{
                "timeUnixNano": "1700000000000000000",
                "asDouble": 85.5,
                "exemplars": [{
                  "timeUnixNano": "1700000000123456789",
                  "traceId": "4BF92F3577B34DA6A3CE929D0E0E4736",
                  "spanId": "00F067AA0BA902B7",
                  "asDouble": 85.5,
                  "filteredAttributes": [{"key": "user", "value": {"stringValue": "alice"}}]
                }]
              }]
            }
          },
          {
            "name": "request_sum",
            "sum": {
              "dataPoints": [{
                "timeUnixNano": "1700000000000000000",
                "asInt": "100",
                "exemplars": [{
                  "timeUnixNano": "1700000000987654321",
                  "traceId": "0123456789ABCDEF0123456789ABCDEF",
                  "spanId": "0123456789ABCDEF",
                  "asInt": "42",
                  "filteredAttributes": [{"key": "tenant", "value": {"stringValue": "acme"}}]
                }]
              }]
            }
          },
          {
            "name": "latency_hist",
            "histogram": {
              "dataPoints": [{
                "timeUnixNano": "1700000000000000000",
                "count": "1",
                "sum": 0.5,
                "bucketCounts": ["1"],
                "explicitBounds": [1.0],
                "exemplars": [{
                  "timeUnixNano": "1700000000555555555",
                  "traceId": "AABBCCDDEEFF00112233445566778899",
                  "spanId": "AABBCCDDEEFF0011",
                  "value": 0.5
                }]
              }]
            }
          }
        ]
      }]
    }]
  })";

  auto res = ProcessMetrics(L_, json, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 3);

  const char* verify_script = R"(
    assert(#emitted == 3, "expected 3 emitted metrics, got " .. #emitted)

    -- Event 1: Gauge exemplar
    local e1 = emitted[1].log
    assert(e1.MetricName == "cpu_gauge")
    assert(e1.Exemplars ~= nil and #e1.Exemplars == 1)
    local ex1 = e1.Exemplars[1]
    assert(ex1.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736",
           "Gauge exemplar TraceId must be lowercase hex, got: " .. tostring(ex1.TraceId))
    assert(ex1.SpanId == "00f067aa0ba902b7",
           "Gauge exemplar SpanId must be lowercase hex, got: " .. tostring(ex1.SpanId))
    assert(ex1.TimeUnix == "2023-11-14T22:13:20.123456789Z",
           "Gauge exemplar TimeUnix ISO-8601 mismatch: " .. tostring(ex1.TimeUnix))
    assert(math.abs(ex1.Value - 85.5) < 0.001, "Gauge exemplar Value mismatch")
    assert(ex1.FilteredAttributes ~= nil and ex1.FilteredAttributes["user"] == "alice")

    -- Event 2: Sum exemplar
    local e2 = emitted[2].log
    assert(e2.MetricName == "request_sum")
    assert(e2.Exemplars ~= nil and #e2.Exemplars == 1)
    local ex2 = e2.Exemplars[1]
    assert(ex2.TraceId == "0123456789abcdef0123456789abcdef",
           "Sum exemplar TraceId must be lowercase hex, got: " .. tostring(ex2.TraceId))
    assert(ex2.SpanId == "0123456789abcdef",
           "Sum exemplar SpanId must be lowercase hex, got: " .. tostring(ex2.SpanId))
    assert(ex2.TimeUnix == "2023-11-14T22:13:20.987654321Z",
           "Sum exemplar TimeUnix ISO-8601 mismatch: " .. tostring(ex2.TimeUnix))
    assert(ex2.Value == 42, "Sum exemplar Value mismatch: " .. tostring(ex2.Value))
    assert(ex2.FilteredAttributes ~= nil and ex2.FilteredAttributes["tenant"] == "acme")

    -- Event 3: Histogram exemplar
    local e3 = emitted[3].log
    assert(e3.MetricName == "latency_hist")
    assert(e3.Exemplars ~= nil and #e3.Exemplars == 1)
    local ex3 = e3.Exemplars[1]
    assert(ex3.TraceId == "aabbccddeeff00112233445566778899",
           "Hist exemplar TraceId must be lowercase hex, got: " .. tostring(ex3.TraceId))
    assert(ex3.SpanId == "aabbccddeeff0011",
           "Hist exemplar SpanId must be lowercase hex, got: " .. tostring(ex3.SpanId))
    assert(ex3.TimeUnix == "2023-11-14T22:13:20.555555555Z",
           "Hist exemplar TimeUnix mismatch: " .. tostring(ex3.TimeUnix))
    assert(math.abs(ex3.Value - 0.5) < 0.001, "Hist exemplar Value mismatch")
    assert(ex3.FilteredAttributes ~= nil, "Hist exemplar FilteredAttributes must be a table")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, TableModeExemplarIdsNormalizedFromBinaryAndSchemaParity) {
  // Test Table mode: 16B/8B raw binary IDs and uppercase hex IDs in exemplars for
  // gauge, sum, and histogram are normalized to lowercase hex with schema parity.
  ASSERT_EQ(luaL_dostring(L_, "emitted = {}"), LUA_OK);
  const char* script = R"(
    return {
      resourceMetrics = {
        {
          resource = {
            attributes = {
              { key = "service.name", value = { stringValue = "metrics-table-svc" } }
            }
          },
          scopeMetrics = {
            {
              scope = { name = "table-scope", version = "2.0" },
              metrics = {
                {
                  name = "mem_gauge",
                  gauge = {
                    dataPoints = {
                      {
                        timeUnixNano = 1700000000000000000,
                        asDouble = 1024.0,
                        exemplars = {
                          {
                            timeUnixNano = 1700000000123456789,
                            traceId = "\x4b\xf9\x2f\x35\x77\xb3\x4d\xa6\xa3\xce\x92\x9d\x0e\x0e\x47\x36",
                            spanId = "\x00\xf0\x67\xaa\x0b\xa9\x02\xb7",
                            asDouble = 512.5,
                            filteredAttributes = {
                              { key = "pool", value = { stringValue = "heap" } }
                            }
                          }
                        }
                      }
                    }
                  }
                },
                {
                  name = "conn_sum",
                  sum = {
                    dataPoints = {
                      {
                        timeUnixNano = 1700000000000000000,
                        asInt = 200,
                        exemplars = {
                          {
                            time_unix_nano = 1700000000987654321,
                            trace_id = "4BF92F3577B34DA6A3CE929D0E0E4736",
                            span_id = "00F067AA0BA902B7",
                            as_int = 10,
                            filtered_attributes = {
                              { key = "protocol", value = { stringValue = "http" } }
                            }
                          }
                        }
                      }
                    }
                  }
                },
                {
                  name = "dur_hist",
                  histogram = {
                    dataPoints = {
                      {
                        timeUnixNano = 1700000000000000000,
                        count = 1,
                        sum = 1.5,
                        bucketCounts = { 1 },
                        explicitBounds = { 2.0 },
                        exemplars = {
                          {
                            timeUnix = "2023-11-14T22:13:20.555555555Z",
                            traceId = "\x01\x23\x45\x67\x89\xab\xcd\xef\x01\x23\x45\x67\x89\xab\xcd\xef",
                            spanId = "\x01\x23\x45\x67\x89\xab\xcd\xef",
                            value = 1.5
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);

  auto res = ProcessMetricsTable(L_, table_idx, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 3);
  lua_pop(L_, 1);

  const char* verify_script = R"(
    assert(#emitted == 3, "expected 3 emitted metrics, got " .. #emitted)

    -- Event 1: Gauge exemplar with binary IDs
    local e1 = emitted[1].log
    assert(e1.MetricName == "mem_gauge")
    assert(type(e1.Exemplars) == "table" and #e1.Exemplars == 1)
    local ex1 = e1.Exemplars[1]
    assert(ex1.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736",
           "Gauge exemplar TraceId must be 32 lowercase hex chars, got: " .. tostring(ex1.TraceId))
    assert(ex1.SpanId == "00f067aa0ba902b7",
           "Gauge exemplar SpanId must be 16 lowercase hex chars, got: " .. tostring(ex1.SpanId))
    assert(ex1.TimeUnix == "2023-11-14T22:13:20.123456789Z",
           "Gauge exemplar TimeUnix ISO-8601 mismatch: " .. tostring(ex1.TimeUnix))
    assert(math.abs(ex1.Value - 512.5) < 0.001, "Gauge exemplar Value mismatch")
    assert(ex1.FilteredAttributes ~= nil and ex1.FilteredAttributes["pool"] == "heap")

    -- Event 2: Sum exemplar with uppercase snake_case IDs
    local e2 = emitted[2].log
    assert(e2.MetricName == "conn_sum")
    assert(type(e2.Exemplars) == "table" and #e2.Exemplars == 1)
    local ex2 = e2.Exemplars[1]
    assert(ex2.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736",
           "Sum exemplar TraceId must be lowercase hex, got: " .. tostring(ex2.TraceId))
    assert(ex2.SpanId == "00f067aa0ba902b7",
           "Sum exemplar SpanId must be lowercase hex, got: " .. tostring(ex2.SpanId))
    assert(ex2.TimeUnix == "2023-11-14T22:13:20.987654321Z",
           "Sum exemplar TimeUnix ISO-8601 mismatch: " .. tostring(ex2.TimeUnix))
    assert(ex2.Value == 10, "Sum exemplar Value mismatch: " .. tostring(ex2.Value))
    assert(ex2.FilteredAttributes ~= nil and ex2.FilteredAttributes["protocol"] == "http")

    -- Event 3: Histogram exemplar with binary IDs
    local e3 = emitted[3].log
    assert(e3.MetricName == "dur_hist")
    assert(type(e3.Exemplars) == "table" and #e3.Exemplars == 1)
    local ex3 = e3.Exemplars[1]
    assert(ex3.TraceId == "0123456789abcdef0123456789abcdef",
           "Hist exemplar TraceId must be 32 lowercase hex chars, got: " .. tostring(ex3.TraceId))
    assert(ex3.SpanId == "0123456789abcdef",
           "Hist exemplar SpanId must be 16 lowercase hex chars, got: " .. tostring(ex3.SpanId))
    assert(ex3.TimeUnix == "2023-11-14T22:13:20.555555555Z",
           "Hist exemplar TimeUnix mismatch: " .. tostring(ex3.TimeUnix))
    assert(math.abs(ex3.Value - 1.5) < 0.001, "Hist exemplar Value mismatch")
    assert(ex3.FilteredAttributes ~= nil, "Hist exemplar FilteredAttributes must be a table")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, TableModeAbsentExemplarsRetainsEmptyTableContract) {
  // Test Table mode: absent exemplars field or empty exemplars array emits an empty table {}
  // matching the existing contract.
  ASSERT_EQ(luaL_dostring(L_, "emitted = {}"), LUA_OK);
  const char* script = R"(
    return {
      resourceMetrics = {
        {
          scopeMetrics = {
            {
              metrics = {
                {
                  name = "gauge_no_ex",
                  gauge = {
                    dataPoints = {
                      {
                        timeUnixNano = 1700000000000000000,
                        asDouble = 1.0
                        -- exemplars field completely absent
                      }
                    }
                  }
                },
                {
                  name = "sum_empty_ex",
                  sum = {
                    dataPoints = {
                      {
                        timeUnixNano = 1700000000000000000,
                        asInt = 2,
                        exemplars = {}  -- empty array
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);

  auto res = ProcessMetricsTable(L_, table_idx, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 2);
  lua_pop(L_, 1);

  const char* verify_script = R"(
    assert(#emitted == 2)
    local e1 = emitted[1].log
    assert(e1.MetricName == "gauge_no_ex")
    assert(e1.Exemplars ~= nil, "Exemplars table must not be nil when absent")
    assert(type(e1.Exemplars) == "table", "Exemplars must be a table")
    assert(#e1.Exemplars == 0, "Exemplars must be empty array")

    local e2 = emitted[2].log
    assert(e2.MetricName == "sum_empty_ex")
    assert(e2.Exemplars ~= nil, "Exemplars table must not be nil when empty")
    assert(type(e2.Exemplars) == "table", "Exemplars must be a table")
    assert(#e2.Exemplars == 0, "Exemplars must be empty array")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, ExemplarMissingTraceIdAndSpanIdEmitEmptyString) {
  // Missing traceId or spanId in exemplars must default to empty string "" in both modes.
  // 1. JSON mode
  const std::string json = R"({
    "resourceMetrics": [{
      "scopeMetrics": [{
        "metrics": [{
          "name": "g1",
          "gauge": {
            "dataPoints": [{
              "timeUnixNano": "1700000000000000000",
              "asDouble": 1.0,
              "exemplars": [{
                "timeUnixNano": "1700000000111111111",
                "asDouble": 1.0
              }]
            }]
          }
        }]
      }]
    }]
  })";

  auto json_res = ProcessMetrics(L_, json, 1);
  EXPECT_TRUE(json_res.success) << json_res.error;
  EXPECT_EQ(json_res.emitted_count, 1);

  const char* verify_json = R"(
    assert(#emitted == 1)
    local ex = emitted[1].log.Exemplars[1]
    assert(ex.TraceId == "", "Missing TraceId must be empty string in JSON mode, got: " .. tostring(ex.TraceId))
    assert(ex.SpanId == "", "Missing SpanId must be empty string in JSON mode, got: " .. tostring(ex.SpanId))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_json), LUA_OK) << lua_tostring(L_, -1);

  // 2. Table mode
  ASSERT_EQ(luaL_dostring(L_, "emitted = {}"), LUA_OK);
  const char* table_script = R"(
    return {
      resourceMetrics = {
        {
          scopeMetrics = {
            {
              metrics = {
                {
                  name = "g2",
                  gauge = {
                    dataPoints = {
                      {
                        timeUnixNano = 1700000000000000000,
                        asDouble = 2.0,
                        exemplars = {
                          {
                            timeUnixNano = 1700000000222222222,
                            asDouble = 2.0
                            -- traceId and spanId omitted
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, table_script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);

  auto table_res = ProcessMetricsTable(L_, table_idx, 1);
  EXPECT_TRUE(table_res.success) << table_res.error;
  EXPECT_EQ(table_res.emitted_count, 1);
  lua_pop(L_, 1);

  const char* verify_table = R"(
    assert(#emitted == 1)
    local ex = emitted[1].log.Exemplars[1]
    assert(ex.TraceId == "", "Missing TraceId must be empty string in Table mode, got: " .. tostring(ex.TraceId))
    assert(ex.SpanId == "", "Missing SpanId must be empty string in Table mode, got: " .. tostring(ex.SpanId))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_table), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, MetricAttributesNotAlteredByIdNormalization) {
  // Verify that attributes in ResourceAttributes, ScopeAttributes, and metric
  // data point Attributes containing uppercase hex or 16-byte strings are NOT altered.
  ASSERT_EQ(luaL_dostring(L_, "emitted = {}"), LUA_OK);
  const char* script = R"(
    return {
      resourceMetrics = {
        {
          resource = {
            attributes = {
              { key = "service.name", value = { stringValue = "attr-svc" } },
              { key = "res.hex", value = { stringValue = "4BF92F3577B34DA6A3CE929D0E0E4736" } }
            }
          },
          scopeMetrics = {
            {
              scope = {
                name = "scope-test",
                attributes = {
                  { key = "scope.exact_16", value = { stringValue = "1234567812345678" } }
                }
              },
              metrics = {
                {
                  name = "g_attrs",
                  gauge = {
                    dataPoints = {
                      {
                        timeUnixNano = 1700000000000000000,
                        asDouble = 1.0,
                        attributes = {
                          { key = "dp.raw_upper", value = { stringValue = "UPPERCASE_AND_HEX_4BF9" } }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, script), LUA_OK) << lua_tostring(L_, -1);
  int table_idx = lua_gettop(L_);

  auto res = ProcessMetricsTable(L_, table_idx, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);
  lua_pop(L_, 1);

  const char* verify_script = R"(
    assert(#emitted == 1)
    local r = emitted[1].log
    assert(r.ResourceAttributes["res.hex"] == "4BF92F3577B34DA6A3CE929D0E0E4736",
           "ResourceAttributes string must not be altered")
    assert(r.ScopeAttributes["scope.exact_16"] == "1234567812345678",
           "ScopeAttributes string must not be altered")
    assert(r.Attributes["dp.raw_upper"] == "UPPERCASE_AND_HEX_4BF9",
           "DataPoint Attributes string must not be altered")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

}  // namespace

}  // namespace extractors
}  // namespace vector_otel
