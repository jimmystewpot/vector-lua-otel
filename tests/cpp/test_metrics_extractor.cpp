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
    assert(e1.ResourceAttributes == '{"service.name":"payment-service","deployment.environment":"production"}',
           "e1 ResourceAttributes mismatch: " .. tostring(e1.ResourceAttributes))
    assert(e1.ScopeName == "com.acme.metrics", "e1 ScopeName mismatch")
    assert(e1.ScopeVersion == "1.2.3", "e1 ScopeVersion mismatch")
    assert(e1.ScopeAttributes == '{"scope.key":"scope.val"}',
           "e1 ScopeAttributes mismatch: " .. tostring(e1.ScopeAttributes))
    assert(e1.Attributes == '{"host":"node1"}', "e1 Attributes mismatch: " .. tostring(e1.Attributes))
    assert(e1.StartTimeUnix == "2021-08-26T17:45:00.000000000Z", "e1 StartTimeUnix mismatch: " .. tostring(e1.StartTimeUnix))
    assert(e1.TimeUnix == "2021-08-26T17:46:40.000000000Z", "e1 TimeUnix mismatch: " .. tostring(e1.TimeUnix))
    assert(math.abs(e1.Value - 1048576.5) < 0.001, "e1 Value mismatch: " .. tostring(e1.Value))
    assert(e1.Flags == 0, "e1 Flags mismatch")
    assert(e1.Histogram == nil, "e1 Histogram should be nil")
    assert(e1.ExponentialHistogram == nil, "e1 ExponentialHistogram should be nil")
    assert(e1.Summary == nil, "e1 Summary should be nil")
    assert(e1.Exemplars == "[]", "e1 Exemplars should be '[]': " .. tostring(e1.Exemplars))
    assert(type(e1.processed_at) == "string" and string.len(e1.processed_at) > 0, "e1 processed_at missing")

    -- Event 2: Gauge (asInt)
    local e2 = emitted[2].log
    assert(e2 ~= nil, "event 2 log table missing")
    assert(e2.MetricName == "memory_usage", "e2 MetricName mismatch")
    assert(e2.MetricType == "gauge", "e2 MetricType mismatch")
    assert(e2.Attributes == '{"host":"node2"}', "e2 Attributes mismatch: " .. tostring(e2.Attributes))
    assert(e2.Value == 2097152, "e2 Value mismatch: " .. tostring(e2.Value))
    assert(e2.Flags == 1, "e2 Flags mismatch")
    assert(e2.Exemplars == "[]", "e2 Exemplars should be '[]': " .. tostring(e2.Exemplars))

    -- Event 3: Sum with Exemplar
    local e3 = emitted[3].log
    assert(e3 ~= nil, "event 3 log table missing")
    assert(e3.MetricName == "http_requests_total", "e3 MetricName mismatch")
    assert(e3.MetricDescription == "Total HTTP requests", "e3 MetricDescription mismatch")
    assert(e3.MetricUnit == "1", "e3 MetricUnit mismatch")
    assert(e3.MetricType == "sum", "e3 MetricType mismatch")
    assert(e3.Value == 5000, "e3 Value mismatch: " .. tostring(e3.Value))
    assert(e3.Attributes == '{"status":200}', "e3 Attributes status mismatch: " .. tostring(e3.Attributes))
    assert(e3.Flags == 0, "e3 Flags mismatch")
    assert(e3.Exemplars == '[{"FilteredAttributes":{"user_id":"usr_99"},"TimeUnix":"2021-08-26T17:46:41.500000000Z","Value":1.25,"SpanId":"0123456789abcdef","TraceId":"0123456789abcdef0123456789abcdef"}]',
           "e3 Exemplars mismatch: " .. tostring(e3.Exemplars))

    -- Event 4: Histogram with Buckets and Bounds
    local e4 = emitted[4].log
    assert(e4 ~= nil, "event 4 log table missing")
    assert(e4.MetricName == "request_duration_seconds", "e4 MetricName mismatch")
    assert(e4.MetricType == "histogram", "e4 MetricType mismatch")
    assert(e4.Value == nil, "e4 Value must be nil for histogram")
    assert(e4.Attributes == '{"method":"POST"}', "e4 Attributes method mismatch: " .. tostring(e4.Attributes))
    assert(e4.Histogram ==
      '{"aggregationTemporality":0,"count":100,"sum":25.5,"min":0.05,"max":5.2,"bucketCounts":[10,40,30,15,5],"explicitBounds":[0.1,0.5,1,2.5]}',
      "e4 Histogram mismatch: " .. tostring(e4.Histogram))
    assert(e4.ExponentialHistogram == nil, "e4 ExponentialHistogram must be nil")
    assert(e4.Summary == nil, "e4 Summary must be nil")
    assert(e4.Exemplars == '[{"FilteredAttributes":{"client_ip":"192.168.1.1"},"TimeUnix":"2021-08-26T17:46:42.800000000Z","Value":2.45,"SpanId":"fedcba9876543210","TraceId":"fedcba9876543210fedcba9876543210"}]',
           "e4 Exemplars mismatch: " .. tostring(e4.Exemplars))

    -- Event 5: Summary
    local e5 = emitted[5].log
    assert(e5 ~= nil, "event 5 log table missing")
    assert(e5.MetricName == "rpc_latency", "e5 MetricName mismatch")
    assert(e5.MetricType == "summary", "e5 MetricType mismatch")
    assert(e5.Value == nil, "e5 Value should be nil for summary")
    assert(e5.Histogram == nil, "e5 Histogram should be nil for summary")
    assert(e5.ExponentialHistogram == nil, "e5 ExponentialHistogram should be nil for summary")
    assert(e5.Attributes == '{"service":"auth"}', "e5 Attributes mismatch: " .. tostring(e5.Attributes))
    assert(e5.Summary ==
      '{"count":50,"sum":1234.5,"quantileValues":[{"quantile":0.5,"value":12},{"quantile":0.99,"value":85}]}',
      "e5 Summary mismatch: " .. tostring(e5.Summary))
    assert(e5.Exemplars == "[]", "e5 Exemplars mismatch: " .. tostring(e5.Exemplars))
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
    assert(e.ResourceAttributes == '{"cluster":"prod-1"}', "ResourceAttributes mismatch: " .. tostring(e.ResourceAttributes))
    assert(e.ScopeAttributes == "{}", "ScopeAttributes mismatch: " .. tostring(e.ScopeAttributes))
    assert(e.Attributes == "{}", "Attributes mismatch: " .. tostring(e.Attributes))
    assert(e.Exemplars == "[]", "Exemplars mismatch: " .. tostring(e.Exemplars))
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
    assert(e.ResourceAttributes == '{"service.name":"unordered_svc"}', "ResourceAttributes mismatch: " .. tostring(e.ResourceAttributes))
    assert(e.ScopeAttributes == '{"scope_attr":"val"}', "ScopeAttributes mismatch: " .. tostring(e.ScopeAttributes))
    assert(e.Attributes == '{"env":"staging"}', "Attributes mismatch: " .. tostring(e.Attributes))
    assert(e.Exemplars == "[]", "Exemplars mismatch: " .. tostring(e.Exemplars))
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
    assert(e.Attributes == '{"cluster":"prod-1"}', "Attributes mismatch: " .. tostring(e.Attributes))
    assert(e.Exemplars == '[{"FilteredAttributes":{"tenant":"acme"},"TimeUnix":"1970-01-01T00:00:01.000000000Z","Value":3.14,"SpanId":"0123456789abcdef","TraceId":"0123456789abcdef0123456789abcdef"}]',
           "Exemplars mismatch: " .. tostring(e.Exemplars))
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
    assert(gauge_event.Exemplars == '[{"FilteredAttributes":{},"TimeUnix":"1970-01-01T00:00:01.000000000Z","SpanId":"","TraceId":""}]',
           "Exemplar Value should not be set when asDouble is invalid: " .. tostring(gauge_event.Exemplars))

    local hist_event = emitted[#emitted].log
    assert(hist_event ~= nil)
    assert(hist_event.Histogram ==
      '{"aggregationTemporality":0,"count":1,"sum":0,"bucketCounts":[],"explicitBounds":[]}',
      "Histogram min/max should not be set when invalid: " .. tostring(hist_event.Histogram))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, ProcessMetricsTablePreservesLuaStackDepthAndClampsHistogramCount) {
  // 1. Test exact uint64_t histogram count preservation in ProcessMetrics (JSON path)
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

  // Verify both JSON and Table histogram counts preserve exact UINT64_MAX (18446744073709551615)
  const char* verify_clamp = R"(
    assert(#emitted == 2, "expected 2 emitted events, got " .. #emitted)
    local h1 = emitted[1].log.Histogram
    assert(h1 ==
      '{"aggregationTemporality":0,"count":18446744073709551615,"sum":10,"bucketCounts":[],"explicitBounds":[]}',
      "h1 mismatch: " .. tostring(h1))

    local e2 = emitted[2].log
    local h2 = e2.Histogram
    assert(h2 ==
      '{"aggregationTemporality":0,"count":18446744073709551615,"sum":20,"bucketCounts":[],"explicitBounds":[]}',
      "h2 mismatch: " .. tostring(h2))
    assert(e2.ResourceAttributes == '{"service.name":"tbl-svc"}', "e2.ResourceAttributes mismatch: " .. tostring(e2.ResourceAttributes))
    assert(e2.ScopeAttributes == "{}", "e2.ScopeAttributes mismatch")
    assert(e2.Attributes == "{}", "e2.Attributes mismatch")
    assert(e2.Exemplars == "[]", "e2.Exemplars mismatch")
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
    assert(e1.Histogram ==
      '{"aggregationTemporality":0,"count":50,"sum":12.5,"bucketCounts":[10,40],"explicitBounds":[1]}',
      "e1 Histogram mismatch: " .. tostring(e1.Histogram))
    assert(e1.Exemplars == "[]", "e1 Exemplars mismatch: " .. tostring(e1.Exemplars))

    -- Event 2: Table histogram without min/max
    local e2 = emitted[2].log
    assert(e2.MetricName == "table_hist_no_min_max")
    assert(e2.Histogram ==
      '{"aggregationTemporality":0,"count":60,"sum":18,"bucketCounts":[20,40],"explicitBounds":[2.5]}',
      "e2 Histogram mismatch: " .. tostring(e2.Histogram))
    assert(e2.Exemplars == "[]", "e2 Exemplars mismatch: " .. tostring(e2.Exemplars))
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
            "dataPoints": [
              {
                "startTimeUnixNano": "1630000000000000000",
                "timeUnixNano": "1630000002000000000",
                "count": "18446744073709551615",
                "sum": 50000.0,
                "quantileValues": [
                  {"quantile": 0.5, "value": 45.0},
                  {"quantile": 0.9, "value": 90.0},
                  {"quantile": 0.99, "value": 150.0}
                ],
                "attributes": [{"key": "endpoint", "value": {"stringValue": "/api/v1/checkout"}}],
                "flags": 1
              },
              {
                "timeUnixNano": "1630000002500000000"
              }
            ]
          }
        }]
      }]
    }]
  })";

  auto json_res = ProcessMetrics(L_, json_payload, 1);
  EXPECT_TRUE(json_res.success) << json_res.error;
  EXPECT_EQ(json_res.emitted_count, 2);

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
                        count = "18446744073709551615",
                        sum = 600.0,
                        quantileValues = {
                          { quantile = 0.5, value = 2.5 },
                          { quantile = 0.95, value = 5.0 }
                        },
                        attributes = {
                          { key = "handler", value = { stringValue = "login" } }
                        },
                        flags = 0
                      },
                      {
                        timeUnixNano = "1630000003500000000"
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
  EXPECT_EQ(tbl_res.emitted_count, 2);
  lua_pop(L_, 1);

  const char* verify_script = R"(
    assert(#emitted == 4, "expected 4 emitted events, got " .. #emitted)

    -- Event 1: JSON Summary with quantiles and exact UINT64_MAX count
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
    assert(e1.ExponentialHistogram == nil, "Summary ExponentialHistogram must be nil")
    assert(e1.ResourceAttributes == '{"service.name":"summary-svc"}', "e1 ResourceAttributes mismatch: " .. tostring(e1.ResourceAttributes))
    assert(e1.ScopeAttributes == "{}", "e1 ScopeAttributes mismatch: " .. tostring(e1.ScopeAttributes))
    assert(e1.Attributes == '{"endpoint":"/api/v1/checkout"}', "e1 Attributes mismatch: " .. tostring(e1.Attributes))
    assert(e1.Flags == 1)
    assert(e1.StartTimeUnix == "2021-08-26T17:46:40.000000000Z")
    assert(e1.TimeUnix == "2021-08-26T17:46:42.000000000Z")
    assert(e1.Summary ==
      '{"count":18446744073709551615,"sum":50000,"quantileValues":[{"quantile":0.5,"value":45},{"quantile":0.9,"value":90},{"quantile":0.99,"value":150}]}',
      "e1 Summary mismatch: " .. tostring(e1.Summary))
    assert(e1.Exemplars == "[]", "e1 Exemplars mismatch: " .. tostring(e1.Exemplars))

    -- Event 2: JSON Summary with empty quantileValues
    local e2 = emitted[2].log
    assert(e2.Summary == '{"count":0,"sum":0,"quantileValues":[]}', "e2 Summary mismatch: " .. tostring(e2.Summary))
    assert(e2.Attributes == "{}", "e2 Attributes mismatch")
    assert(e2.Exemplars == "[]", "e2 Exemplars mismatch")

    -- Event 3: Table Summary with quantiles and exact UINT64_MAX count
    local e3 = emitted[3].log
    assert(e3.MetricName == "table_summary_metric")
    assert(e3.MetricDescription == "Table summary metric test")
    assert(e3.MetricUnit == "s")
    assert(e3.MetricType == "summary")
    assert(e3.Value == nil, "Table summary Value must be nil")
    assert(e3.Histogram == nil, "Table summary Histogram must be nil")
    assert(e3.ExponentialHistogram == nil, "Table summary ExponentialHistogram must be nil")
    assert(e3.Attributes == '{"handler":"login"}', "e3 Attributes mismatch: " .. tostring(e3.Attributes))
    assert(e3.Flags == 0)
    assert(e3.StartTimeUnix == "2021-08-26T17:46:40.000000000Z")
    assert(e3.TimeUnix == "2021-08-26T17:46:43.000000000Z")
    assert(e3.Summary ==
      '{"count":18446744073709551615,"sum":600,"quantileValues":[{"quantile":0.5,"value":2.5},{"quantile":0.95,"value":5}]}',
      "e3 Summary mismatch: " .. tostring(e3.Summary))
    assert(e3.Exemplars == "[]", "e3 Exemplars mismatch")

    -- Event 4: Table Summary with omitted quantileValues
    local e4 = emitted[4].log
    assert(e4.Summary == '{"count":0,"sum":0,"quantileValues":[]}', "e4 Summary mismatch: " .. tostring(e4.Summary))
    assert(e4.Attributes == "{}", "e4 Attributes mismatch")
    assert(e4.Exemplars == "[]", "e4 Exemplars mismatch")
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, ExponentialHistogramFullFidelityAndEdgeCases) {
  // 1. JSON mode: mixed batch with full exponentialHistogram + gauge
  const std::string json_payload = R"({
    "resourceMetrics": [{
      "scopeMetrics": [{
        "metrics": [
          {
            "name": "exp_hist_full",
            "description": "Exponential histogram latency",
            "unit": "ms",
            "exponentialHistogram": {
              "aggregationTemporality": 2,
              "dataPoints": [{
                "startTimeUnixNano": "1630000000000000000",
                "timeUnixNano": "1630000001000000000",
                "count": "18446744073709551615",
                "sum": 25.0,
                "min": 0.1,
                "max": 10.5,
                "scale": -2,
                "zeroCount": "9007199254740993",
                "zeroThreshold": 0.001,
                "positive": {
                  "offset": -3,
                  "bucketCounts": ["1", "9007199254740993", "18446744073709551615"]
                },
                "negative": {
                  "offset": 2,
                  "bucket_counts": [4, 5]
                },
                "exemplars": [{
                  "timeUnixNano": "1630000000500000000",
                  "asDouble": 3.25,
                  "traceId": "0123456789abcdef0123456789abcdef",
                  "spanId": "0123456789abcdef",
                  "filteredAttributes": [{"key": "region", "value": {"stringValue": "us-east-1"}}]
                }]
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
  EXPECT_EQ(json_res.emitted_count, 2);

  // 2. Exponential-histogram-only JSON payload (snake_case, omitted optional fields)
  const std::string only_exp_json = R"({
    "resourceMetrics": [{
      "scopeMetrics": [{
        "metrics": [{
          "name": "only_exp_hist_sparse",
          "exponential_histogram": {
            "data_points": [{
              "count": 5,
              "scale": 3,
              "zero_count": 1
            }]
          }
        }]
      }]
    }]
  })";
  auto only_res = ProcessMetrics(L_, only_exp_json, 1);
  EXPECT_TRUE(only_res.success) << only_res.error;
  EXPECT_EQ(only_res.emitted_count, 1);

  // 3. Table mode: full exponentialHistogram, sparse exponential_histogram, and sum
  const char* push_table_exp = R"(
    return {
      resourceMetrics = {
        {
          scopeMetrics = {
            {
              metrics = {
                {
                  name = "table_exp_hist_full",
                  exponentialHistogram = {
                    aggregationTemporality = 2,
                    dataPoints = {
                      {
                        startTimeUnixNano = "1630000000000000000",
                        timeUnixNano = "1630000001000000000",
                        count = "18446744073709551615",
                        sum = 25.0,
                        min = 0.1,
                        max = 10.5,
                        scale = -2,
                        zeroCount = "9007199254740993",
                        zeroThreshold = 0.001,
                        positive = {
                          offset = -3,
                          bucketCounts = { "1", "9007199254740993", "18446744073709551615" }
                        },
                        negative = {
                          offset = 2,
                          bucket_counts = { 4, 5 }
                        },
                        exemplars = {
                          {
                            timeUnixNano = "1630000000500000000",
                            asDouble = 3.25,
                            traceId = "0123456789abcdef0123456789abcdef",
                            spanId = "0123456789abcdef",
                            filteredAttributes = {
                              { key = "region", value = { stringValue = "us-east-1" } }
                            }
                          }
                        }
                      }
                    }
                  }
                },
                {
                  name = "table_exp_hist_sparse",
                  exponential_histogram = {
                    data_points = {
                      { scale = 3, zero_count = 1, count = 5 }
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
  EXPECT_EQ(tbl_res.emitted_count, 3);
  lua_pop(L_, 1);

  // 4. JSON mode: exponentialHistogram with reversed key order
  const std::string reversed_exp_json = R"({
    "resourceMetrics": [{
      "scopeMetrics": [{
        "metrics": [{
          "name": "exp_hist_reversed_keys",
          "exponentialHistogram": {
            "dataPoints": [{
              "exemplars": [{
                "filteredAttributes": [{"key": "region", "value": {"stringValue": "us-east-1"}}],
                "spanId": "0123456789abcdef",
                "traceId": "0123456789abcdef0123456789abcdef",
                "asDouble": 3.25,
                "timeUnixNano": "1630000000500000000"
              }],
              "negative": {
                "bucket_counts": [4, 5],
                "offset": 2
              },
              "positive": {
                "bucketCounts": ["1", "9007199254740993", "18446744073709551615"],
                "offset": -3
              },
              "zeroThreshold": 0.001,
              "zeroCount": "9007199254740993",
              "scale": -2,
              "max": 10.5,
              "min": 0.1,
              "sum": 25.0,
              "count": "18446744073709551615",
              "timeUnixNano": "1630000001000000000",
              "startTimeUnixNano": "1630000000000000000"
            }],
            "aggregationTemporality": 2
          }
        }]
      }]
    }]
  })";
  auto rev_res = ProcessMetrics(L_, reversed_exp_json, 1);
  EXPECT_TRUE(rev_res.success) << rev_res.error;
  EXPECT_EQ(rev_res.emitted_count, 1);

  const char* verify_script = R"(
    assert(#emitted == 7, "expected 7 emitted events, got " .. #emitted)

    local expected_full_eh =
      '{"aggregationTemporality":2,' ..
      '"count":18446744073709551615,' ..
      '"sum":25,"min":0.1,"max":10.5,' ..
      '"scale":-2,' ..
      '"zeroCount":9007199254740993,' ..
      '"zeroThreshold":0.001,' ..
      '"positive":{"offset":-3,"bucketCounts":[1,9007199254740993,18446744073709551615]},' ..
      '"negative":{"offset":2,"bucketCounts":[4,5]}}'

    local function check_full_exp(e, expected_name, is_table_mode, is_reversed_json)
      assert(e.MetricName == expected_name)
      assert(e.MetricType == "exponential_histogram")
      assert(e.Value == nil, "Value must be nil for exponential_histogram")
      assert(e.Histogram == nil, "Histogram must be nil for exponential_histogram")
      assert(e.Summary == nil, "Summary must be nil for exponential_histogram")
      assert(e.AggregationTemporality == 2)
      assert(e.ExponentialHistogram == expected_full_eh,
             "ExponentialHistogram mismatch: " .. tostring(e.ExponentialHistogram))
      assert(type(e.Exemplars) == "string")
      assert(e.Exemplars ==
        '[{"FilteredAttributes":{"region":"us-east-1"},"TimeUnix":"2021-08-26T17:46:40.500000000Z","Value":3.25,"SpanId":"0123456789abcdef","TraceId":"0123456789abcdef0123456789abcdef"}]',
        "Exemplars mismatch: " .. tostring(e.Exemplars))
    end

    local expected_sparse_eh =
      '{"aggregationTemporality":0,' ..
      '"count":5,' ..
      '"scale":3,' ..
      '"zeroCount":1,' ..
      '"zeroThreshold":0,' ..
      '"positive":{"offset":0,"bucketCounts":[]},' ..
      '"negative":{"offset":0,"bucketCounts":[]}}'

    local function check_sparse_exp(e, expected_name)
      assert(e.MetricName == expected_name)
      assert(e.MetricType == "exponential_histogram")
      assert(e.Value == nil)
      assert(e.AggregationTemporality == 0)
      assert(e.ExponentialHistogram == expected_sparse_eh,
             "Sparse ExponentialHistogram mismatch: " .. tostring(e.ExponentialHistogram))
      assert(e.Exemplars == "[]", "Sparse Exemplars mismatch: " .. tostring(e.Exemplars))
    end

    check_full_exp(emitted[1].log, "exp_hist_full", false, false)
    assert(emitted[2].log.MetricName == "valid_gauge_alongside")
    assert(math.abs(emitted[2].log.Value - 77.5) < 0.001)
    check_sparse_exp(emitted[3].log, "only_exp_hist_sparse")
    check_full_exp(emitted[4].log, "table_exp_hist_full", true, false)
    check_sparse_exp(emitted[5].log, "table_exp_hist_sparse")
    assert(emitted[6].log.MetricName == "table_valid_sum")
    assert(emitted[6].log.Value == 500)
    check_full_exp(emitted[7].log, "exp_hist_reversed_keys", false, true)
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
    assert(e.Exemplars ==
      '[{"FilteredAttributes":{"tag_dbl":"val_dbl"},"TimeUnix":"2021-08-26T17:46:40.000000000Z","Value":12.345,"SpanId":"0123456789abcdef","TraceId":"0123456789abcdef0123456789abcdef"},' ..
      '{"FilteredAttributes":{"tag_int":99},"TimeUnix":"2021-08-26T17:46:41.000000000Z","Value":8888,"SpanId":"fedcba9876543210","TraceId":"fedcba9876543210fedcba9876543210"},' ..
      '{"FilteredAttributes":{},"TimeUnix":"","Value":-42,"SpanId":"","TraceId":""}]',
      "Exemplars mismatch: " .. tostring(e.Exemplars))
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
            "name": "cumulative_sum_str",
            "sum": {
              "aggregation_temporality": "AGGREGATION_TEMPORALITY_CUMULATIVE",
              "is_monotonic": false,
              "dataPoints": [{
                "asInt": 250
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
          },
          {
            "name": "delta_exp_hist",
            "exponentialHistogram": {
              "aggregationTemporality": "AGGREGATION_TEMPORALITY_DELTA",
              "dataPoints": [{
                "count": 10,
                "scale": 1
              }]
            }
          },
          {
            "name": "unspec_exp_hist",
            "exponential_histogram": {
              "aggregation_temporality": 0,
              "dataPoints": [{
                "count": 5,
                "scale": 0
              }]
            }
          }
        ]
      }]
    }]
  })";

  auto res = ProcessMetrics(L_, json_payload, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 5);

  const char* push_table = R"(
    return {
      resourceMetrics = {
        {
          scopeMetrics = {
            {
              metrics = {
                {
                  name = "tbl_sum_delta",
                  sum = {
                    aggregationTemporality = "DELTA",
                    isMonotonic = true,
                    dataPoints = { { asInt = 10, flags = 2 } }
                  }
                },
                {
                  name = "tbl_sum_non_mono",
                  sum = {
                    aggregation_temporality = 2,
                    is_monotonic = false,
                    dataPoints = { { asInt = 20 } }
                  }
                },
                {
                  name = "tbl_hist_cum",
                  histogram = {
                    aggregationTemporality = "CUMULATIVE",
                    dataPoints = { { count = 5, sum = 15.0 } }
                  }
                },
                {
                  name = "tbl_exp_delta",
                  exponentialHistogram = {
                    aggregation_temporality = 1,
                    dataPoints = { { count = 7, scale = 2 } }
                  }
                }
              }
            }
          }
        }
      }
    }
  )";
  ASSERT_EQ(luaL_dostring(L_, push_table), LUA_OK) << lua_tostring(L_, -1);
  auto tbl_res = ProcessMetricsTable(L_, -1, 1);
  EXPECT_TRUE(tbl_res.success) << tbl_res.error;
  EXPECT_EQ(tbl_res.emitted_count, 4);
  lua_pop(L_, 1);

  const char* verify_script = R"(
    assert(#emitted == 9, "expected 9 emitted events, got " .. #emitted)

    local e1 = emitted[1].log
    assert(e1.MetricName == "delta_sum")
    assert(e1.AggregationTemporality == 1)
    assert(e1.IsMonotonic == true)
    assert(e1.Flags == 1)

    local e2 = emitted[2].log
    assert(e2.MetricName == "cumulative_sum_str")
    assert(e2.AggregationTemporality == 2)
    assert(e2.IsMonotonic == false)

    local e3 = emitted[3].log
    assert(e3.MetricName == "cumulative_hist")
    assert(e3.AggregationTemporality == 2)
    assert(e3.Histogram ==
      '{"aggregationTemporality":2,"count":25,"sum":100,"bucketCounts":[],"explicitBounds":[]}',
      "e3.Histogram mismatch: " .. tostring(e3.Histogram))
    assert(e3.Flags == 4)

    local e4 = emitted[4].log
    assert(e4.MetricName == "delta_exp_hist")
    assert(e4.AggregationTemporality == 1)
    assert(e4.ExponentialHistogram ==
      '{"aggregationTemporality":1,"count":10,"scale":1,"zeroCount":0,"zeroThreshold":0,"positive":{"offset":0,"bucketCounts":[]},"negative":{"offset":0,"bucketCounts":[]}}',
      "e4.ExponentialHistogram mismatch: " .. tostring(e4.ExponentialHistogram))

    local e5 = emitted[5].log
    assert(e5.MetricName == "unspec_exp_hist")
    assert(e5.AggregationTemporality == 0)
    assert(e5.ExponentialHistogram ==
      '{"aggregationTemporality":0,"count":5,"scale":0,"zeroCount":0,"zeroThreshold":0,"positive":{"offset":0,"bucketCounts":[]},"negative":{"offset":0,"bucketCounts":[]}}',
      "e5.ExponentialHistogram mismatch: " .. tostring(e5.ExponentialHistogram))

    local e6 = emitted[6].log
    assert(e6.MetricName == "tbl_sum_delta")
    assert(e6.AggregationTemporality == 1)
    assert(e6.IsMonotonic == true)
    assert(e6.Flags == 2)

    local e7 = emitted[7].log
    assert(e7.MetricName == "tbl_sum_non_mono")
    assert(e7.AggregationTemporality == 2)
    assert(e7.IsMonotonic == false)

    local e8 = emitted[8].log
    assert(e8.MetricName == "tbl_hist_cum")
    assert(e8.AggregationTemporality == 2)
    assert(e8.Histogram ==
      '{"aggregationTemporality":2,"count":5,"sum":15,"bucketCounts":[],"explicitBounds":[]}',
      "e8.Histogram mismatch: " .. tostring(e8.Histogram))

    local e9 = emitted[9].log
    assert(e9.MetricName == "tbl_exp_delta")
    assert(e9.AggregationTemporality == 1)
    assert(e9.ExponentialHistogram ==
      '{"aggregationTemporality":1,"count":7,"scale":2,"zeroCount":0,"zeroThreshold":0,"positive":{"offset":0,"bucketCounts":[]},"negative":{"offset":0,"bucketCounts":[]}}',
      "e9.ExponentialHistogram mismatch: " .. tostring(e9.ExponentialHistogram))
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
      assert(type(e.ResourceAttributes) == "string" and e.ResourceAttributes:find('"service.name":42', 1, true) ~= nil,
             "Event " .. i .. " ResourceAttributes mismatch: " ..
             tostring(e.ResourceAttributes))
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
    assert(type(uaf_emitted[1].log.ResourceAttributes) == "string" and
           uaf_emitted[1].log.ResourceAttributes:find('"service.name":42', 1, true) ~= nil)
    assert(type(uaf_emitted[2].log.ResourceAttributes) == "string" and
           uaf_emitted[2].log.ResourceAttributes:find('"service.name":42', 1, true) ~= nil)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, JsonModeServiceNameSurvivesCallbackRemovingResourceAttribute) {
  const char* uaf_script = R"(
    uaf_emitted = {}
    function uaf_cb(event)
      table.insert(uaf_emitted, event)
      event.log.ResourceAttributes = nil
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

TEST_F(MetricsExtractorTest, ReviewHardeningEdgeCases) {
  // 1. Null protobuf oneof sibling in JSON + bucket counts edge cases in JSON
  const std::string json_payload = R"({
    "resourceMetrics": [{
      "scopeMetrics": [{
        "metrics": [
          {
            "name": "null_oneof",
            "gauge": null,
            "exponentialHistogram": {
              "dataPoints": [{
                "timeUnixNano": "1",
                "count": "3",
                "scale": 1
              }]
            }
          },
          {
            "name": "json_bucket_edge_cases",
            "histogram": {
              "dataPoints": [{
                "timeUnixNano": "2",
                "count": 1,
                "bucketCounts": [-5, "-5", "inf", "99999999999999999999", "99999999999999999999abc", "18446744073709551615"]
              }]
            }
          }
        ]
      }]
    }]
  })";
  auto json_res = ProcessMetrics(L_, json_payload, 1);
  EXPECT_TRUE(json_res.success) << json_res.error;
  EXPECT_EQ(json_res.emitted_count, 2);

  // 2. Table mode edge cases:
  //    - generic `value = "9007199254740993"` on data point and exemplar preserves exact integer
  //    - `value = 45.2` and `value = "45.2"` emit double 45.2
  //    - `asDouble = "invalid_double"`, `asInt = "bad"`, `value = "bad"` omit Value (nil)
  //    - bucketCounts with -5, "-5", math.huge, "99999999999999999999", "99999999999999999999abc",
  //    "18446744073709551615"
  //    - explicitBounds = "not_a_table" -> {} and explicitBounds = {1.0, "2.5"} -> fresh JSON
  //    [1,2.5]
  //    - negative flags = -1 -> Flags == 0
  const char* push_table = R"(
    shared_bounds_input = { 1.0, "2.5" }
    return {
      resourceMetrics = {
        {
          scopeMetrics = {
            {
              metrics = {
                {
                  name = "tbl_generic_values",
                  gauge = {
                    dataPoints = {
                      {
                        timeUnixNano = "1",
                        value = "9007199254740993",
                        flags = -1,
                        exemplars = {
                          { timeUnixNano = "1", value = "9007199254740993" },
                          { timeUnixNano = "1", value = 45.2 },
                          { timeUnixNano = "1", value = "45.2" },
                          { timeUnixNano = "1", asDouble = "invalid_double" },
                          { timeUnixNano = "1", asInt = "bad" },
                          { timeUnixNano = "1", value = "bad" }
                        }
                      },
                      { timeUnixNano = "2", value = 45.2 },
                      { timeUnixNano = "3", value = "45.2" },
                      { timeUnixNano = "4", asDouble = "invalid_double" },
                      { timeUnixNano = "5", asInt = "bad" },
                      { timeUnixNano = "6", value = "bad" }
                    }
                  }
                },
                {
                  name = "tbl_bucket_and_bounds",
                  histogram = {
                    dataPoints = {
                      {
                        timeUnixNano = "7",
                        count = 1,
                        bucketCounts = { -5, "-5", math.huge, "99999999999999999999", "99999999999999999999abc", "18446744073709551615" },
                        explicitBounds = "not_a_table"
                      },
                      {
                        timeUnixNano = "8",
                        count = 2,
                        explicitBounds = shared_bounds_input
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
  ASSERT_EQ(luaL_dostring(L_, push_table), LUA_OK) << lua_tostring(L_, -1);
  auto tbl_res = ProcessMetricsTable(L_, -1, 1);
  EXPECT_TRUE(tbl_res.success) << tbl_res.error;
  EXPECT_EQ(tbl_res.emitted_count, 8);
  lua_pop(L_, 1);

  const char* verify_script = R"(
    assert(#emitted == 10, "expected 10 emitted events, got " .. #emitted)

    -- Event 1: null_oneof -> exponential_histogram
    local e1 = emitted[1].log
    assert(e1.MetricName == "null_oneof")
    assert(e1.MetricType == "exponential_histogram")
    assert(e1.ExponentialHistogram ==
      '{"aggregationTemporality":0,"count":3,"scale":1,"zeroCount":0,"zeroThreshold":0,"positive":{"offset":0,"bucketCounts":[]},"negative":{"offset":0,"bucketCounts":[]}}',
      "e1.ExponentialHistogram mismatch: " .. tostring(e1.ExponentialHistogram))

    -- Event 2: json_bucket_edge_cases
    assert(emitted[2].log.Histogram ==
      '{"aggregationTemporality":0,"count":1,"sum":0,"bucketCounts":[0,0,0,0,0,18446744073709551615],"explicitBounds":[]}',
      "emitted[2].log.Histogram mismatch: " .. tostring(emitted[2].log.Histogram))

    -- Event 3: table value = "9007199254740993", flags = -1, exemplars
    local e3 = emitted[3].log
    assert(e3.Value == 9007199254740993)
    assert(math.type(e3.Value) == "integer")
    assert(e3.Flags == 0, "negative flags must normalize to 0, got " .. tostring(e3.Flags))
    assert(e3.Exemplars ==
      '[{"FilteredAttributes":{},"TimeUnix":"1970-01-01T00:00:00.000000001Z","Value":9007199254740993,"SpanId":"","TraceId":""},' ..
      '{"FilteredAttributes":{},"TimeUnix":"1970-01-01T00:00:00.000000001Z","Value":45.2,"SpanId":"","TraceId":""},' ..
      '{"FilteredAttributes":{},"TimeUnix":"1970-01-01T00:00:00.000000001Z","Value":45.2,"SpanId":"","TraceId":""},' ..
      '{"FilteredAttributes":{},"TimeUnix":"1970-01-01T00:00:00.000000001Z","SpanId":"","TraceId":""},' ..
      '{"FilteredAttributes":{},"TimeUnix":"1970-01-01T00:00:00.000000001Z","SpanId":"","TraceId":""},' ..
      '{"FilteredAttributes":{},"TimeUnix":"1970-01-01T00:00:00.000000001Z","SpanId":"","TraceId":""}]',
      "e3.Exemplars mismatch: " .. tostring(e3.Exemplars))

    -- Events 4..8: table Value variants
    assert(math.abs(emitted[4].log.Value - 45.2) < 0.001 and math.type(emitted[4].log.Value) == "float")
    assert(math.abs(emitted[5].log.Value - 45.2) < 0.001 and math.type(emitted[5].log.Value) == "float")
    assert(emitted[6].log.Value == nil)
    assert(emitted[7].log.Value == nil)
    assert(emitted[8].log.Value == nil)

    -- Event 9: table bucketCounts and explicitBounds = "not_a_table"
    local h9 = emitted[9].log.Histogram
    assert(h9 ==
      '{"aggregationTemporality":0,"count":1,"sum":0,"bucketCounts":[0,0,0,0,0,18446744073709551615],"explicitBounds":[]}',
      "h9 mismatch: " .. tostring(h9))

    -- Event 10: valid explicitBounds = {1.0, "2.5"} serialized to JSON
    local h10 = emitted[10].log.Histogram
    assert(h10 ==
      '{"aggregationTemporality":0,"count":2,"sum":0,"bucketCounts":[],"explicitBounds":[1,2.5]}',
      "h10 mismatch: " .. tostring(h10))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);

  // 3. Mid-array syntax error in JSON dataPoints returns success == false with emitted_count == 1
  const std::string mid_array_err_json =
      R"({"resourceMetrics":[{"scopeMetrics":[{"metrics":[{"name":"mid_err","gauge":{"dataPoints":[{"timeUnixNano":"1","asInt":1},{"timeUnixNano":"2","asInt":}]}}]}]}]})";
  auto mid_res = ProcessMetrics(L_, mid_array_err_json, 1);
  EXPECT_FALSE(mid_res.success);
  EXPECT_FALSE(mid_res.error.empty());
  EXPECT_EQ(mid_res.emitted_count, 1);

  // 4. Mid-array syntax errors in histogram bucketCounts, explicitBounds, and summary
  // quantileValues
  const std::string mid_bucket_err_json =
      R"({"resourceMetrics":[{"scopeMetrics":[{"metrics":[{"name":"mid_bucket","histogram":{"dataPoints":[{"count":1,"bucketCounts":[1,2,]}]}}]}]}]})";
  auto mid_bucket_res = ProcessMetrics(L_, mid_bucket_err_json, 1);
  EXPECT_FALSE(mid_bucket_res.success);
  EXPECT_FALSE(mid_bucket_res.error.empty());

  const std::string mid_bounds_err_json =
      R"({"resourceMetrics":[{"scopeMetrics":[{"metrics":[{"name":"mid_bounds","histogram":{"dataPoints":[{"count":1,"explicitBounds":[1.0,]}]}}]}]}]})";
  auto mid_bounds_res = ProcessMetrics(L_, mid_bounds_err_json, 1);
  EXPECT_FALSE(mid_bounds_res.success);
  EXPECT_FALSE(mid_bounds_res.error.empty());

  const std::string mid_quantile_err_json =
      R"({"resourceMetrics":[{"scopeMetrics":[{"metrics":[{"name":"mid_q","summary":{"dataPoints":[{"count":1,"quantileValues":[{"quantile":0.5,"value":1.0},{"quantile":0.9,"value":}]}]}}]}]}]})";
  auto mid_q_res = ProcessMetrics(L_, mid_quantile_err_json, 1);
  EXPECT_FALSE(mid_q_res.success);
  EXPECT_FALSE(mid_q_res.error.empty());

  const std::string mid_atom_bucket_err_json =
      R"({"resourceMetrics":[{"scopeMetrics":[{"metrics":[{"name":"mid_atom_b","histogram":{"dataPoints":[{"count":1,"bucketCounts":["1","2",tru]}]}}]}]}]})";
  auto mid_atom_b_res = ProcessMetrics(L_, mid_atom_bucket_err_json, 1);
  EXPECT_FALSE(mid_atom_b_res.success);
  EXPECT_FALSE(mid_atom_b_res.error.empty());
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
    assert(e1.Exemplars ==
      '[{"FilteredAttributes":{"user":"alice"},"TimeUnix":"2023-11-14T22:13:20.123456789Z","Value":85.5,"SpanId":"00f067aa0ba902b7","TraceId":"4bf92f3577b34da6a3ce929d0e0e4736"}]',
      "Gauge exemplar mismatch: " .. tostring(e1.Exemplars))

    -- Event 2: Sum exemplar
    local e2 = emitted[2].log
    assert(e2.MetricName == "request_sum")
    assert(e2.Exemplars ==
      '[{"FilteredAttributes":{"tenant":"acme"},"TimeUnix":"2023-11-14T22:13:20.987654321Z","Value":42,"SpanId":"0123456789abcdef","TraceId":"0123456789abcdef0123456789abcdef"}]',
      "Sum exemplar mismatch: " .. tostring(e2.Exemplars))

    -- Event 3: Histogram exemplar
    local e3 = emitted[3].log
    assert(e3.MetricName == "latency_hist")
    assert(e3.Exemplars ==
      '[{"FilteredAttributes":{},"TimeUnix":"2023-11-14T22:13:20.555555555Z","Value":0.5,"SpanId":"aabbccddeeff0011","TraceId":"aabbccddeeff00112233445566778899"}]',
      "Hist exemplar mismatch: " .. tostring(e3.Exemplars))
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
    assert(e1.Exemplars ==
      '[{"FilteredAttributes":{"pool":"heap"},"TimeUnix":"2023-11-14T22:13:20.123456789Z","Value":512.5,"SpanId":"00f067aa0ba902b7","TraceId":"4bf92f3577b34da6a3ce929d0e0e4736"}]',
      "Gauge exemplar mismatch: " .. tostring(e1.Exemplars))

    -- Event 2: Sum exemplar with uppercase snake_case IDs
    local e2 = emitted[2].log
    assert(e2.MetricName == "conn_sum")
    assert(e2.Exemplars ==
      '[{"FilteredAttributes":{"protocol":"http"},"TimeUnix":"2023-11-14T22:13:20.987654321Z","Value":10,"SpanId":"00f067aa0ba902b7","TraceId":"4bf92f3577b34da6a3ce929d0e0e4736"}]',
      "Sum exemplar mismatch: " .. tostring(e2.Exemplars))

    -- Event 3: Histogram exemplar with binary IDs
    local e3 = emitted[3].log
    assert(e3.MetricName == "dur_hist")
    assert(e3.Exemplars ==
      '[{"FilteredAttributes":{},"TimeUnix":"2023-11-14T22:13:20.555555555Z","Value":1.5,"SpanId":"0123456789abcdef","TraceId":"0123456789abcdef0123456789abcdef"}]',
      "Hist exemplar mismatch: " .. tostring(e3.Exemplars))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, TableModeAbsentExemplarsRetainsEmptyTableContract) {
  // Test Table mode: absent exemplars field or empty exemplars array emits "[]"
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
    assert(e1.Exemplars == "[]", "Exemplars must be '[]' when absent, got: " .. tostring(e1.Exemplars))

    local e2 = emitted[2].log
    assert(e2.MetricName == "sum_empty_ex")
    assert(e2.Exemplars == "[]", "Exemplars must be '[]' when empty, got: " .. tostring(e2.Exemplars))
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
    assert(emitted[1].log.Exemplars ==
      '[{"FilteredAttributes":{},"TimeUnix":"2023-11-14T22:13:20.111111111Z","Value":1,"SpanId":"","TraceId":""}]',
      "JSON mode Exemplars mismatch: " .. tostring(emitted[1].log.Exemplars))
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
    assert(emitted[1].log.Exemplars ==
      '[{"FilteredAttributes":{},"TimeUnix":"2023-11-14T22:13:20.222222222Z","Value":2,"SpanId":"","TraceId":""}]',
      "Table mode Exemplars mismatch: " .. tostring(emitted[1].log.Exemplars))
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
    assert(r.ResourceAttributes:find('"res.hex":"4BF92F3577B34DA6A3CE929D0E0E4736"', 1, true) ~= nil,
           "ResourceAttributes string must not be altered: " .. tostring(r.ResourceAttributes))
    assert(r.ScopeAttributes == '{"scope.exact_16":"1234567812345678"}',
           "ScopeAttributes string must not be altered: " .. tostring(r.ScopeAttributes))
    assert(r.Attributes == '{"dp.raw_upper":"UPPERCASE_AND_HEX_4BF9"}',
           "DataPoint Attributes string must not be altered: " .. tostring(r.Attributes))
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

TEST_F(MetricsExtractorTest, SnakeCaseJsonOtlpFieldsCoverage) {
  ASSERT_EQ(luaL_dostring(L_, "emitted = {}"), LUA_OK);
  std::string json = R"({
    "resource_metrics": [
      {
        "resource": {
          "attributes": [
            {"key": "service.name", "value": {"string_value": "snake-metric-svc"}}
          ]
        },
        "scope_metrics": [
          {
            "scope": {"name": "snake-meter", "version": "1.0"},
            "metrics": [
              {
                "name": "snake_counter",
                "sum": {
                  "data_points": [
                    {
                      "time_unix_nano": "1700000000000000000",
                      "as_int": 55
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
  auto res = ProcessMetrics(L_, json, 1);
  EXPECT_TRUE(res.success) << res.error;
  EXPECT_EQ(res.emitted_count, 1);

  const char* verify_script = R"(
    assert(#emitted == 1)
    local r = emitted[1].log
    assert(r.ServiceName == "snake-metric-svc")
    assert(r.ScopeName == "snake-meter")
    assert(r.MetricName == "snake_counter")
    assert(r.Value == 55)
  )";
  EXPECT_EQ(luaL_dostring(L_, verify_script), LUA_OK) << lua_tostring(L_, -1);
}

}  // namespace

}  // namespace extractors
}  // namespace vector_otel
