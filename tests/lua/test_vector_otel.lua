-- test_vector_otel.lua
-- Standalone Lua 5.4 Test Suite & GC Stability for vector_otel

-- 1. Setup package.cpath to ensure vector_otel can be dynamically loaded
local build_dir = os.getenv("BUILD_DIR")
if build_dir and #build_dir > 0 then
    package.cpath = build_dir .. "/?.so;" .. package.cpath
elseif not package.cpath:find("build/%?%.so") then
    package.cpath = "build/?.so;../build/?.so;../../build/?.so;" .. package.cpath
end

local function read_file(path)
    local info = debug.getinfo(1, "S")
    local script_dir = "."
    if info and info.source and info.source:sub(1, 1) == "@" then
        script_dir = info.source:sub(2):match("(.*/)") or "."
    end

    local candidates = {
        path,
        script_dir .. "/../fixtures/" .. path,
        script_dir .. "/../../tests/fixtures/" .. path,
        script_dir .. "/" .. path,
        "tests/fixtures/" .. path,
        "../fixtures/" .. path,
        "../../tests/fixtures/" .. path,
        "tests/" .. path,
        "../" .. path,
        "../../" .. path,
    }
    for _, p in ipairs(candidates) do
        local f = io.open(p, "r")
        if f then
            local content = f:read("*a")
            f:close()
            return content
        end
    end
    error("Unable to find file: " .. path)
end

local ISO_8601_NANO_PATTERN = "^%d%d%d%d%-%d%d%-%d%dT%d%d:%d%d:%d%d%.%d%d%d%d%d%d%d%d%dZ$"
local ISO_8601_GENERAL_PATTERN = "^%d%d%d%d%-%d%d%-%d%dT%d%d:%d%d:%d%d%.%d+Z$"

local function assert_all_columns_primitive(rec, label)
  assert(type(rec) == "table", tostring(label) .. ": record must be a table")
  for k, v in pairs(rec) do
    local vt = type(v)
    assert(
      vt == "string" or vt == "number" or vt == "boolean",
      string.format("%s: column '%s' must be primitive (string|number|boolean), got '%s'", tostring(label), tostring(k), vt)
    )
  end
end

print("=== Running vector_otel Standalone Test Suite ===")

--------------------------------------------------------------------------------
-- Module Loading & Submodule Registration
--------------------------------------------------------------------------------
print(">> Testing module loading and exports...")

local vo = require("vector_otel")
assert(type(vo) == "table", "vector_otel must export a table")
assert(vo._VERSION == "0.3.0", "Expected _VERSION 0.3.0, got: " .. tostring(vo._VERSION))

-- Submodule tables on main module
assert(type(vo.logs) == "table", "vo.logs must be a table")
assert(type(vo.logs.process) == "function", "vo.logs.process must be a function")

assert(type(vo.traces) == "table", "vo.traces must be a table")
assert(type(vo.traces.process) == "function", "vo.traces.process must be a function")

assert(type(vo.metrics) == "table", "vo.metrics must be a table")
assert(type(vo.metrics.process) == "function", "vo.metrics.process must be a function")

-- Direct require of submodules
local logs = require("vector_otel.logs")
assert(logs == vo.logs, "require('vector_otel.logs') must return vo.logs")

local traces = require("vector_otel.traces")
assert(traces == vo.traces, "require('vector_otel.traces') must return vo.traces")

local metrics = require("vector_otel.metrics")
assert(metrics == vo.metrics, "require('vector_otel.metrics') must return vo.metrics")

print("   Module and submodule loading passed.")

--------------------------------------------------------------------------------
-- Error Contracts & Falsy Semantics
--------------------------------------------------------------------------------
print(">> Testing error contracts...")

local dummy_emit = function(event) end

-- Test 1: Logs malformed JSON
local count, err, emitted = vo.logs.process("{invalid json", dummy_emit)
assert(count == nil, "Malformed JSON must return nil as first value, got: " .. tostring(count))
assert(type(err) == "string" and #err > 0, "Malformed JSON must return error string")
assert(emitted == 0, "Malformed JSON must return emitted_count 0")
-- Verify falsy conditional check works in Lua
local failed = false
if not count then
    failed = true
end
assert(failed, "count must be falsy (nil) so 'if not count then' handles errors")

-- Test 2: Traces malformed JSON
count, err, emitted = vo.traces.process("{\"resourceSpans\": [unclosed", dummy_emit)
assert(count == nil, "Malformed JSON must return nil first value for traces")
assert(type(err) == "string" and #err > 0, "Traces error string must be non-empty")
assert(emitted == 0, "Traces error emitted_count must be 0")
if count then
    error("Expected count to be falsy on traces error")
end

-- Test 3: Metrics malformed JSON
count, err, emitted = vo.metrics.process("{invalid json", dummy_emit)
assert(count == nil, "Malformed JSON must return nil first value for metrics")
assert(type(err) == "string" and #err > 0, "Metrics error string must be non-empty")
assert(emitted == 0, "Metrics error emitted_count must be 0")
if count then
    error("Expected count to be falsy on metrics error")
end

-- Test 4: Invalid argument types
count, err, emitted = vo.logs.process(12345, dummy_emit)
assert(count == nil and err == "payload must be a string or table" and emitted == 0)

count, err, emitted = vo.logs.process("{}", "not_a_function")
assert(count == nil and err == "emit callback must be a function" and emitted == 0)

count, err, emitted = vo.traces.process(false, dummy_emit)
assert(count == nil and err == "payload must be a string or table" and emitted == 0)

count, err, emitted = vo.traces.process("{}", nil)
assert(count == nil and err == "emit callback must be a function" and emitted == 0)

count, err, emitted = vo.metrics.process(true, dummy_emit)
assert(count == nil and err == "payload must be a string or table" and emitted == 0)

count, err, emitted = vo.metrics.process("{}", 42)
assert(count == nil and err == "emit callback must be a function" and emitted == 0)

-- Test 5: Empty table input returns 0 events and nil error
count, err = vo.logs.process({}, dummy_emit)
assert(count == 0 and err == nil, "Empty table logs must return 0, nil")

count, err = vo.traces.process({}, dummy_emit)
assert(count == 0 and err == nil, "Empty table traces must return 0, nil")

count, err = vo.metrics.process({}, dummy_emit)
assert(count == 0 and err == nil, "Empty table metrics must return 0, nil")

-- Test 6: Protected emit callback error returns failure tuple without crashing
local failing_emit = function(ev)
    error("downstream backpressure: buffer full")
end
local log_sample = [[{"resourceLogs": [{"scopeLogs": [{"logRecords": [{"body": {"stringValue": "hi"}}]}]}]}]]
count, err, emitted = vo.logs.process(log_sample, failing_emit)
assert(count == nil, "failing emit must return nil count")
assert(type(err) == "string" and err:find("downstream backpressure"), "error must capture emit error message, got: " .. tostring(err))
assert(emitted == 0, "emitted count must be 0")

-- Test 7: Re-entrant process invocation inside emit callback is rejected safely
local inner_count, inner_err, inner_emitted
local outer_events = {}
local reentrant_payload = [[{"resourceLogs": [{"scopeLogs": [{"logRecords": [{"body": {"stringValue": "first"}}, {"body": {"stringValue": "second"}}]}]}]}]]
local reentrant_emit = function(ev)
    table.insert(outer_events, ev)
    if #outer_events == 1 then
        inner_count, inner_err, inner_emitted = vo.logs.process(log_sample, dummy_emit)
    end
end
count, err = vo.logs.process(reentrant_payload, reentrant_emit)
assert(inner_count == nil, "Re-entrant call must return nil count, got: " .. tostring(inner_count))
assert(inner_err == "re-entrant vector_otel.process() invocation is not permitted",
    "Unexpected re-entrant error message: " .. tostring(inner_err))
assert(inner_emitted == 0, "Re-entrant call must return 0 emitted_count, got: " .. tostring(inner_emitted))
assert(count == 2 and err == nil, "Outer call must succeed with 2 records, got: " .. tostring(count) .. ", " .. tostring(err))
assert(#outer_events == 2 and outer_events[1].log.Body == "first" and outer_events[2].log.Body == "second",
    "Outer call records must be intact without memory corruption")

print("   Error contracts passed.")

--------------------------------------------------------------------------------
-- Logs Extraction & ClickHouse Schema Validation
--------------------------------------------------------------------------------
print(">> Testing logs extraction and ClickHouse schema...")

local log_events = {}
local function log_emit(ev)
    table.insert(log_events, ev)
end

-- Comprehensive edge fixture
local log_edge_payload = [[{
  "resourceLogs": [
    {
      "resource": {
        "attributes": [
          {"key": "service.name", "value": {"stringValue": "edge-svc"}},
          {"key": "env", "value": {"stringValue": "prod"}}
        ]
      },
      "schemaUrl": "https://opentelemetry.io/schemas/1.26.0",
      "scopeLogs": [
        {
          "scope": {
            "name": "edge-scope",
            "version": "1.0.0",
            "attributes": [{"key": "scope_tag", "value": {"stringValue": "v1"}}]
          },
          "schemaUrl": "https://opentelemetry.io/schemas/scope/1.0.0",
          "logRecords": [
            {
              "timeUnixNano": "1293840000123456789",
              "severityNumber": 9,
              "severityText": "INFO",
              "body": {"kvlistValue": {"values": [{"key": "message", "value": {"stringValue": "processed successfully"}}]}},
              "attributes": [
                {"key": "str_attr", "value": {"stringValue": "val"}},
                {"key": "int_max", "value": {"intValue": "9223372036854775807"}},
                {"key": "int_min", "value": {"intValue": "-9223372036854775808"}},
                {"key": "dbl_val", "value": {"doubleValue": 3.14159}},
                {"key": "bool_true", "value": {"boolValue": true}},
                {"key": "bool_false", "value": {"boolValue": false}},
                {"key": "arr_val", "value": {"arrayValue": {"values": [{"stringValue": "item1"}, {"intValue": "2"}]}}},
                {"key": "map_val", "value": {"kvlistValue": {"values": [{"key": "sub_k", "value": {"intValue": "1"}}]}}},
                {"key": "bytes_val", "value": {"bytesValue": "AAEC"}},
                {"key": "unicode_val", "value": {"stringValue": "h\u00e9llo \u2713 \"quotes\" \\ \n\t"}}
              ],
              "traceId": "5b8efff798038103d269b633813fc60c",
              "spanId": "eee19b7ec3c1b174",
              "flags": 1
            }
          ]
        }
      ]
    },
    {
      "resource": {"attributes": []},
      "scopeLogs": [
        {
          "logRecords": [
            {
              "observedTimeUnixNano": "1293840000123456794",
              "body": {"stringValue": "fallback-record"}
            }
          ]
        }
      ]
    }
  ]
}]]

log_events = {}
local n_logs, l_err = vo.logs.process(log_edge_payload, log_emit)
assert(n_logs == 2, "Expected 2 log records emitted, got: " .. tostring(n_logs))
assert(l_err == nil, "Expected no error, got: " .. tostring(l_err))
assert(#log_events == 2, "Expected 2 events in table, got: " .. #log_events)

-- Verify Record 1
local r1 = log_events[1].log
assert(r1 ~= nil, "log_events[1].log must exist")
assert_all_columns_primitive(r1, "logs.r1")
assert(r1.Timestamp:match(ISO_8601_NANO_PATTERN), "Record 1 Timestamp must match ISO-8601 nanosecond pattern: " .. tostring(r1.Timestamp))
assert(r1.Timestamp == "2011-01-01T00:00:00.123456789Z", "Timestamp exact value mismatch")
assert(r1.TraceId == "5b8efff798038103d269b633813fc60c", "TraceId mismatch")
assert(r1.SpanId == "eee19b7ec3c1b174", "SpanId mismatch")
assert(r1.TraceFlags == 1, "TraceFlags mismatch")
assert(r1.SeverityText == "INFO", "SeverityText mismatch")
assert(r1.SeverityNumber == 9, "SeverityNumber mismatch")
assert(r1.ServiceName == "edge-svc", "ServiceName mismatch")
assert(r1.Body == '{"message":"processed successfully"}', "Body mismatch: " .. tostring(r1.Body))
assert(r1.ScopeName == "edge-scope", "ScopeName mismatch")
assert(r1.ScopeVersion == "1.0.0", "ScopeVersion mismatch")
assert(r1.ResourceSchemaUrl == "https://opentelemetry.io/schemas/1.26.0", "ResourceSchemaUrl mismatch")
assert(r1.ScopeSchemaUrl == "https://opentelemetry.io/schemas/scope/1.0.0", "ScopeSchemaUrl mismatch")
assert(type(r1.processed_at) == "string" and r1.processed_at:match(ISO_8601_GENERAL_PATTERN), "processed_at must be ISO-8601 UTC")

-- Attributes checks (JSON strings)
assert(type(r1.ResourceAttributes) == "string", "ResourceAttributes must be string")
assert(r1.ResourceAttributes == '{"service.name":"edge-svc","env":"prod"}', "ResourceAttributes mismatch: " .. tostring(r1.ResourceAttributes))

assert(type(r1.ScopeAttributes) == "string", "ScopeAttributes must be string")
assert(r1.ScopeAttributes == '{"scope_tag":"v1"}', "ScopeAttributes mismatch: " .. tostring(r1.ScopeAttributes))

assert(type(r1.LogAttributes) == "string", "LogAttributes must be string")
assert(r1.LogAttributes:find('"str_attr":"val"', 1, true) ~= nil, "LogAttributes.str_attr mismatch")
assert(r1.LogAttributes:find('"int_max":9223372036854775807', 1, true) ~= nil, "int64 max not preserved")
assert(r1.LogAttributes:find('"int_min":-9223372036854775808', 1, true) ~= nil, "int64 min not preserved")
assert(r1.LogAttributes:find('"dbl_val":3.14159', 1, true) ~= nil, "dbl_val mismatch")
assert(r1.LogAttributes:find('"bool_true":true', 1, true) ~= nil, "bool_true mismatch")
assert(r1.LogAttributes:find('"bool_false":false', 1, true) ~= nil, "bool_false mismatch")
assert(r1.LogAttributes:find('"arr_val":["item1",2]', 1, true) ~= nil, "arr_val mismatch")
assert(r1.LogAttributes:find('"map_val":{"sub_k":1}', 1, true) ~= nil, "map_val mismatch")
assert(r1.LogAttributes:find('"bytes_val":"AAEC"', 1, true) ~= nil, "bytes_val mismatch")
assert(r1.LogAttributes:find('\\"quotes\\"', 1, true) ~= nil, "unicode_val mismatch")

-- Verify Record 2 (Defaults & Fallbacks)
local r2 = log_events[2].log
assert(r2 ~= nil, "log_events[2].log must exist")
assert_all_columns_primitive(r2, "logs.r2")
assert(r2.ServiceName == "unknown_service", "Missing service.name must fallback to unknown_service")
assert(r2.Timestamp == "2011-01-01T00:00:00.123456794Z", "Missing timeUnixNano must fallback to observedTimeUnixNano")
assert(r2.TraceId == "", "Default TraceId must be empty string")
assert(r2.SpanId == "", "Default SpanId must be empty string")
assert(r2.TraceFlags == 0, "Default TraceFlags must be 0")
assert(r2.SeverityText == "", "Default SeverityText must be empty string")
assert(r2.SeverityNumber == 0, "Default SeverityNumber must be 0")
assert(r2.Body == "fallback-record", "Body mismatch")
assert(r2.ScopeName == "", "Default ScopeName must be empty string")
assert(r2.ScopeVersion == "", "Default ScopeVersion must be empty string")
assert(r2.ResourceAttributes == "{}", "Default ResourceAttributes must be '{}'")
assert(r2.ScopeAttributes == "{}", "Default ScopeAttributes must be '{}'")
assert(r2.LogAttributes == "{}", "Default LogAttributes must be '{}'")

print("   Logs ClickHouse schema validation passed.")

--------------------------------------------------------------------------------
-- Traces Extraction & ClickHouse Schema Validation
--------------------------------------------------------------------------------
print(">> Testing traces extraction and ClickHouse schema...")

local trace_sample_json = read_file("traces_sample.json")
local trace_events = {}
local function trace_emit(ev)
    table.insert(trace_events, ev)
end

local n_traces, t_err = vo.traces.process(trace_sample_json, trace_emit)
assert(n_traces == 2, "Expected 2 traces emitted, got: " .. tostring(n_traces))
assert(t_err == nil, "Expected no error, got: " .. tostring(t_err))
assert(#trace_events == 2, "Expected 2 trace events in table")

-- Trace Span 1 (Parent Server Span)
local s1 = trace_events[1].log
assert(s1 ~= nil, "trace_events[1].log must exist")
assert_all_columns_primitive(s1, "traces.s1")
assert(s1.Timestamp:match(ISO_8601_NANO_PATTERN), "Timestamp must match ISO-8601 nano: " .. tostring(s1.Timestamp))
assert(s1.Timestamp == "2024-03-09T16:00:00.000000000Z", "Timestamp value mismatch")
assert(s1.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736", "TraceId mismatch")
assert(s1.SpanId == "00f067aa0ba902b7", "SpanId mismatch")
assert(s1.ParentSpanId == "", "Root span ParentSpanId must be empty string")
assert(s1.TraceState == "rojo=1,congo=2", "TraceState mismatch")
assert(s1.SpanName == "HTTP GET /checkout", "SpanName mismatch")
assert(s1.SpanKind == "SPAN_KIND_SERVER", "SpanKind mismatch")
assert(s1.ServiceName == "cart-service", "ServiceName mismatch")
assert(s1.ScopeName == "opentelemetry.instrumentation.http", "ScopeName mismatch")
assert(s1.ScopeVersion == "1.25.0", "ScopeVersion mismatch")
assert(s1.StatusCode == "STATUS_CODE_OK", "StatusCode mismatch")
assert(s1.StatusMessage == "OK", "StatusMessage mismatch")
assert(s1.Duration == 500000000, "Duration mismatch: expected 500000000, got: " .. tostring(s1.Duration))
assert(type(s1.ResourceAttributes) == "string", "ResourceAttributes must be string")
assert(s1.ResourceAttributes == '{"service.name":"cart-service","deployment.environment":"production","host.id":12345}',
    "ResourceAttributes mismatch: " .. tostring(s1.ResourceAttributes))
assert(type(s1.SpanAttributes) == "string", "SpanAttributes must be string")
assert(s1.SpanAttributes == '{"http.method":"GET","http.status_code":200,"http.route":"/checkout"}',
    "SpanAttributes mismatch: " .. tostring(s1.SpanAttributes))
assert(type(s1.Events) == "string" and s1.Events:find("cart_loaded") ~= nil, "Events array string missing cart_loaded")
assert(s1.Events:find("2024%-03%-09T16:00:00.100000000Z") ~= nil, "Events must contain formatted ISO-8601 timestamp")
assert(type(s1.Links) == "string" and s1.Links:find("7bf92f3577b34da6a3ce929d0e0e4736") ~= nil, "Links array string missing traceId")
assert(type(s1.processed_at) == "string" and s1.processed_at:match(ISO_8601_GENERAL_PATTERN), "processed_at must be ISO-8601 UTC")

-- Trace Span 2 (Child Client Span)
local s2 = trace_events[2].log
assert(s2 ~= nil, "trace_events[2].log must exist")
assert_all_columns_primitive(s2, "traces.s2")
assert(s2.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736", "Child TraceId mismatch")
assert(s2.SpanId == "5fb397be34d23b0f", "Child SpanId mismatch")
assert(s2.ParentSpanId == "00f067aa0ba902b7", "Child ParentSpanId mismatch: " .. tostring(s2.ParentSpanId))
assert(s2.SpanName == "SELECT * FROM cart_items", "SpanName mismatch")
assert(s2.SpanKind == "SPAN_KIND_CLIENT", "SpanKind mismatch")
assert(s2.StatusCode == "STATUS_CODE_ERROR", "StatusCode mismatch")
assert(s2.StatusMessage == "Connection timeout", "StatusMessage mismatch")
assert(s2.Duration == 100000000, "Child Duration mismatch")
assert(s2.Events == "[]", "Child Events should be empty array string '[]'")
assert(s2.Links == "[]", "Child Links should be empty array string '[]'")

print("   Traces ClickHouse schema validation passed.")

--------------------------------------------------------------------------------
-- Metrics Extraction & ClickHouse Schema Validation
--------------------------------------------------------------------------------
print(">> Testing metrics extraction and ClickHouse schema...")

local metric_sample_json = read_file("metrics_sample.json")
local metric_events = {}
local function metric_emit(ev)
    table.insert(metric_events, ev)
end

local n_metrics, m_err = vo.metrics.process(metric_sample_json, metric_emit)
assert(n_metrics == 5, "Expected 5 metrics emitted, got: " .. tostring(n_metrics))
assert(m_err == nil, "Expected no error, got: " .. tostring(m_err))
assert(#metric_events == 5, "Expected 5 metric events in table")

-- Metric 1: Gauge (asDouble)
local m1 = metric_events[1].log
assert(m1 ~= nil, "metric_events[1].log must exist")
assert_all_columns_primitive(m1, "metrics.m1")
assert(m1.MetricName == "memory_usage", "MetricName mismatch")
assert(m1.MetricDescription == "Process memory usage", "MetricDescription mismatch")
assert(m1.MetricUnit == "By", "MetricUnit mismatch")
assert(m1.MetricType == "gauge", "MetricType mismatch")
assert(m1.ServiceName == "payment-service", "ServiceName mismatch")
assert(m1.ScopeName == "com.acme.metrics", "ScopeName mismatch")
assert(m1.ScopeVersion == "1.2.3", "ScopeVersion mismatch")
assert(type(m1.ResourceAttributes) == "string" and m1.ResourceAttributes == '{"service.name":"payment-service","deployment.environment":"production"}')
assert(type(m1.ScopeAttributes) == "string" and m1.ScopeAttributes == '{"scope.key":"scope.val"}')
assert(type(m1.Attributes) == "string" and m1.Attributes == '{"host":"node1"}')
assert(m1.StartTimeUnix:match(ISO_8601_NANO_PATTERN), "StartTimeUnix must be ISO-8601 nano")
assert(m1.TimeUnix:match(ISO_8601_NANO_PATTERN), "TimeUnix must be ISO-8601 nano")
assert(m1.StartTimeUnix == "2021-08-26T17:45:00.000000000Z")
assert(m1.TimeUnix == "2021-08-26T17:46:40.000000000Z")
assert(math.abs(m1.Value - 1048576.5) < 0.001, "Value mismatch")
assert(m1.Flags == 0, "Flags mismatch")
assert(m1.Histogram == nil, "Gauge Histogram must be nil")
assert(m1.ExponentialHistogram == nil, "Gauge ExponentialHistogram must be nil")
assert(m1.Summary == nil, "Gauge Summary must be nil")
assert(m1.Exemplars == "[]", "Gauge Exemplars must be '[]'")
assert(type(m1.processed_at) == "string" and m1.processed_at:match(ISO_8601_GENERAL_PATTERN), "processed_at must be ISO-8601 UTC")

-- Metric 2: Gauge (asInt)
local m2 = metric_events[2].log
assert_all_columns_primitive(m2, "metrics.m2")
assert(m2.MetricName == "memory_usage")
assert(m2.Attributes == '{"host":"node2"}')
assert(m2.Value == 2097152, "Gauge asInt Value mismatch: " .. tostring(m2.Value))
assert(m2.Flags == 1, "Flags mismatch")
assert(m2.Exemplars == "[]")

-- Metric 3: Sum with Exemplar
local m3 = metric_events[3].log
assert_all_columns_primitive(m3, "metrics.m3")
assert(m3.MetricName == "http_requests_total")
assert(m3.MetricType == "sum")
assert(m3.Value == 5000, "Sum Value mismatch: " .. tostring(m3.Value))
assert(m3.Attributes == '{"status":200}')
assert(type(m3.Exemplars) == "string", "Exemplars must be JSON string")
assert(m3.Exemplars == '[{"FilteredAttributes":{"user_id":"usr_99"},"TimeUnix":"2021-08-26T17:46:41.500000000Z","Value":1.25,"SpanId":"0123456789abcdef","TraceId":"0123456789abcdef0123456789abcdef"}]',
    "Exemplars mismatch: " .. tostring(m3.Exemplars))

-- Metric 4: Histogram
local m4 = metric_events[4].log
assert_all_columns_primitive(m4, "metrics.m4")
assert(m4.MetricName == "request_duration_seconds")
assert(m4.MetricType == "histogram")
assert(m4.Value == nil, "Histogram Value must be nil")
assert(m4.Attributes == '{"method":"POST"}')
assert(type(m4.Histogram) == "string", "Histogram must be JSON string")
assert(m4.Histogram == '{"aggregationTemporality":0,"count":100,"sum":25.5,"min":0.05,"max":5.2,"bucketCounts":[10,40,30,15,5],"explicitBounds":[0.1,0.5,1,2.5]}',
    "Histogram mismatch: " .. tostring(m4.Histogram))
assert(type(m4.Exemplars) == "string")
assert(m4.Exemplars == '[{"FilteredAttributes":{"client_ip":"192.168.1.1"},"TimeUnix":"2021-08-26T17:46:42.800000000Z","Value":2.45,"SpanId":"fedcba9876543210","TraceId":"fedcba9876543210fedcba9876543210"}]',
    "Histogram Exemplars mismatch: " .. tostring(m4.Exemplars))

-- Metric 5: Summary
local m5 = metric_events[5].log
assert_all_columns_primitive(m5, "metrics.m5")
assert(m5.MetricName == "rpc_latency")
assert(m5.MetricType == "summary")
assert(m5.Value == nil, "Summary Value must be nil")
assert(m5.Attributes == '{"service":"auth"}')
assert(type(m5.Summary) == "string", "Summary must be JSON string")
assert(m5.Summary == '{"count":50,"sum":1234.5,"quantileValues":[{"quantile":0.5,"value":12},{"quantile":0.99,"value":85}]}',
    "Summary mismatch: " .. tostring(m5.Summary))
assert(m5.Exemplars == "[]")

print("   Metrics ClickHouse schema validation passed.")

--------------------------------------------------------------------------------
-- Table-Mode Extraction & Schema Validation (Native Lua Tables)
--------------------------------------------------------------------------------
print(">> Testing table-mode extraction for logs, traces, and metrics...")

-- 1. Table-mode Logs
local table_logs_payload = {
    resourceLogs = {
        {
            resource = {
                attributes = {
                    { key = "service.name", value = { stringValue = "tbl-log-svc" } },
                    { key = "deployment.env", value = { stringValue = "staging" } },
                }
            },
            schemaUrl = "https://opentelemetry.io/schemas/1.26.0",
            scopeLogs = {
                {
                    scope = {
                        name = "tbl-scope",
                        version = "2.1.0",
                        attributes = { { key = "scope_attr", value = { intValue = 42 } } }
                    },
                    logRecords = {
                        {
                            timeUnixNano = "1700000000123456789",
                            severityNumber = 9,
                            severityText = "INFO",
                            body = "Table log message body",
                            attributes = {
                                { key = "user.id", value = { intValue = 1001 } },
                                { key = "req.success", value = { boolValue = true } },
                                { key = "req.lat", value = { doubleValue = 12.34 } },
                                { key = "raw.bytes", value = { bytesValue = "c25ha2U=" } },
                                { key = "nested.kv", value = { kvlistValue = { values = { { key = "sub_k", value = { stringValue = "nested_val" } } } } } },
                            },
                            traceId = "0123456789abcdef0123456789abcdef",
                            spanId = "0123456789abcdef",
                            flags = 1,
                        }
                    }
                }
            }
        }
    }
}

local tbl_log_events = {}
local n_tbl_logs, tbl_l_err = vo.logs.process(table_logs_payload, function(ev) table.insert(tbl_log_events, ev) end)
assert(n_tbl_logs == 1, "Expected 1 table log record, got: " .. tostring(n_tbl_logs))
assert(tbl_l_err == nil, "Expected no error, got: " .. tostring(tbl_l_err))
assert(#tbl_log_events == 1, "Expected 1 event in table")
local tbl_r1 = tbl_log_events[1].log
assert_all_columns_primitive(tbl_r1, "table_logs.tbl_r1")
assert(tbl_r1.Timestamp == "2023-11-14T22:13:20.123456789Z", "Timestamp mismatch: " .. tostring(tbl_r1.Timestamp))
assert(tbl_r1.ServiceName == "tbl-log-svc", "ServiceName mismatch")
assert(tbl_r1.Body == "Table log message body", "Body mismatch")
assert(tbl_r1.SeverityText == "INFO", "SeverityText mismatch")
assert(tbl_r1.SeverityNumber == 9, "SeverityNumber mismatch")
assert(tbl_r1.TraceId == "0123456789abcdef0123456789abcdef", "TraceId mismatch")
assert(tbl_r1.SpanId == "0123456789abcdef", "SpanId mismatch")
assert(tbl_r1.TraceFlags == 1, "TraceFlags mismatch")
assert(type(tbl_r1.ResourceAttributes) == "string")
assert(tbl_r1.ResourceAttributes == '{"service.name":"tbl-log-svc","deployment.env":"staging"}', "Resource attribute mismatch: " .. tostring(tbl_r1.ResourceAttributes))
assert(tbl_r1.ScopeAttributes == '{"scope_attr":42}', "Scope attribute mismatch: " .. tostring(tbl_r1.ScopeAttributes))
assert(type(tbl_r1.LogAttributes) == "string")
assert(tbl_r1.LogAttributes == '{"user.id":1001,"req.success":true,"req.lat":12.34,"raw.bytes":"c25ha2U=","nested.kv":{"sub_k":"nested_val"}}', "Log attribute mismatch: " .. tostring(tbl_r1.LogAttributes))

-- 2. Table-mode Traces
local table_traces_payload = {
    resourceSpans = {
        {
            resource = {
                attributes = {
                    { key = "service.name", value = { stringValue = "tbl-trace-svc" } }
                }
            },
            scopeSpans = {
                {
                    scope = { name = "tracer" },
                    spans = {
                        {
                            traceId = "4bf92f3577b34da6a3ce929d0e0e4736",
                            spanId = "00f067aa0ba902b7",
                            parentSpanId = "5fb397be34d23b0f",
                            name = "GET /api/v1/orders",
                            kind = 2, -- SPAN_KIND_SERVER
                            startTimeUnixNano = "1700000000000000000",
                            endTimeUnixNano = "1700000000500000000",
                            status = { code = 1, message = "OK" },
                            attributes = {
                                { key = "http.status_code", value = { intValue = 200 } }
                            }
                        }
                    }
                }
            }
        }
    }
}

local tbl_trace_events = {}
local n_tbl_traces, tbl_t_err = vo.traces.process(table_traces_payload, function(ev) table.insert(tbl_trace_events, ev) end)
assert(n_tbl_traces == 1, "Expected 1 table trace span, got: " .. tostring(n_tbl_traces))
assert(tbl_t_err == nil, "Expected no error, got: " .. tostring(tbl_t_err))
assert(#tbl_trace_events == 1, "Expected 1 trace event in table")
local tbl_s1 = tbl_trace_events[1].log
assert_all_columns_primitive(tbl_s1, "table_traces.tbl_s1")
assert(tbl_s1.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736", "TraceId mismatch")
assert(tbl_s1.SpanId == "00f067aa0ba902b7", "SpanId mismatch")
assert(tbl_s1.ParentSpanId == "5fb397be34d23b0f", "ParentSpanId mismatch")
assert(tbl_s1.SpanName == "GET /api/v1/orders", "SpanName mismatch")
assert(tbl_s1.SpanKind == "SPAN_KIND_SERVER", "SpanKind mismatch")
assert(tbl_s1.StatusCode == "STATUS_CODE_OK", "StatusCode mismatch")
assert(tbl_s1.Duration == 500000000, "Duration mismatch: " .. tostring(tbl_s1.Duration))
assert(tbl_s1.ResourceAttributes == '{"service.name":"tbl-trace-svc"}', "Resource attribute mismatch")
assert(tbl_s1.SpanAttributes == '{"http.status_code":200}', "Span attribute mismatch: " .. tostring(tbl_s1.SpanAttributes))

-- 3. Table-mode Metrics
local table_metrics_payload = {
    resourceMetrics = {
        {
            resource = {
                attributes = {
                    { key = "service.name", value = { stringValue = "tbl-metric-svc" } }
                }
            },
            scopeMetrics = {
                {
                    scope = { name = "meter" },
                    metrics = {
                        {
                            name = "system.cpu.load",
                            description = "CPU load gauge",
                            unit = "1",
                            gauge = {
                                dataPoints = {
                                    {
                                        timeUnixNano = "1700000000000000000",
                                        asDouble = 0.75,
                                        attributes = {
                                            { key = "core", value = { stringValue = "0" } }
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

local tbl_metric_events = {}
local n_tbl_metrics, tbl_m_err = vo.metrics.process(table_metrics_payload, function(ev) table.insert(tbl_metric_events, ev) end)
assert(n_tbl_metrics == 1, "Expected 1 table metric, got: " .. tostring(n_tbl_metrics))
assert(tbl_m_err == nil, "Expected no error, got: " .. tostring(tbl_m_err))
assert(#tbl_metric_events == 1, "Expected 1 metric event in table")
local tbl_m1 = tbl_metric_events[1].log
assert_all_columns_primitive(tbl_m1, "table_metrics.tbl_m1")
assert(tbl_m1.MetricName == "system.cpu.load", "MetricName mismatch")
assert(tbl_m1.MetricType == "gauge", "MetricType mismatch")
assert(math.abs(tbl_m1.Value - 0.75) < 0.001, "Metric Value mismatch")
assert(tbl_m1.ResourceAttributes == '{"service.name":"tbl-metric-svc"}', "Resource attribute mismatch")
assert(tbl_m1.Attributes == '{"core":"0"}', "Metric attribute mismatch: " .. tostring(tbl_m1.Attributes))
assert(tbl_m1.Exemplars == "[]", "Exemplars default mismatch")

print("   Table-mode schema validation passed.")

--------------------------------------------------------------------------------
-- Issue #8 Correlation ID Hex Normalization & Binary Decoding Regression Suite
--------------------------------------------------------------------------------
print(">> Testing Issue #8 correlation ID normalization and binary decoding...")

local bin_trace_id_16 = string.rep(string.char(0xaa), 16)
local bin_span_id_8 = string.rep(string.char(0xbb), 8)
local bin_parent_span_id_8 = string.rep(string.char(0xcc), 8)
local bin_link_trace_id_16 = string.rep(string.char(0x11), 16)
local bin_link_span_id_8 = string.rep(string.char(0x22), 8)

-- 1. Traces table with 16B traceId, 8B spanId, 8B parentSpanId, and binary links
local binary_table_traces = {
    resourceSpans = {
        {
            resource = {
                attributes = {
                    { key = "service.name", value = { stringValue = "bin-trace-svc" } },
                    { key = "res.upper_hex", value = { stringValue = "4BF92F3577B34DA6A3CE929D0E0E4736" } },
                }
            },
            scopeSpans = {
                {
                    scope = { name = "bin-tracer" },
                    spans = {
                        {
                            traceId = bin_trace_id_16,
                            spanId = bin_span_id_8,
                            parentSpanId = bin_parent_span_id_8,
                            name = "BINARY_SPAN_NAME_4BF9",
                            startTimeUnixNano = "1700000000000000000",
                            endTimeUnixNano = "1700000000100000000",
                            attributes = {
                                { key = "span.raw_hex", value = { stringValue = "UPPERCASE_AND_HEX_4BF9" } },
                                { key = "span.exact_16", value = { stringValue = "1234567812345678" } },
                            },
                            links = {
                                {
                                    traceId = bin_link_trace_id_16,
                                    spanId = bin_link_span_id_8,
                                    traceState = "vendor=active",
                                    attributes = {
                                        { key = "link.key", value = { stringValue = "link.val" } }
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

local bin_trace_events = {}
local n_bin_traces, bin_t_err = vo.traces.process(binary_table_traces, function(ev)
    table.insert(bin_trace_events, ev)
end)
assert(n_bin_traces == 1, "Expected 1 binary trace span emitted, got: " .. tostring(n_bin_traces))
assert(bin_t_err == nil, "Expected no error, got: " .. tostring(bin_t_err))
assert(#bin_trace_events == 1, "Expected 1 trace event in table")

local b_span = bin_trace_events[1].log
assert(b_span.TraceId == string.rep("aa", 16),
    "TraceId must be 32 lowercase hex chars, got: " .. tostring(b_span.TraceId))
assert(b_span.SpanId == string.rep("bb", 8),
    "SpanId must be 16 lowercase hex chars, got: " .. tostring(b_span.SpanId))
assert(b_span.ParentSpanId == string.rep("cc", 8),
    "ParentSpanId must be 16 lowercase hex chars, got: " .. tostring(b_span.ParentSpanId))
assert(b_span.SpanName == "BINARY_SPAN_NAME_4BF9", "SpanName must not be modified")

-- Span links verification
assert(type(b_span.Links) == "string", "Links must be a JSON string")
assert(b_span.Links:find('"TraceId":"' .. string.rep("11", 16) .. '"', 1, true) ~= nil,
    "Links must contain normalized 32-char hex TraceId: " .. b_span.Links)
assert(b_span.Links:find('"SpanId":"' .. string.rep("22", 8) .. '"', 1, true) ~= nil,
    "Links must contain normalized 16-char hex SpanId: " .. b_span.Links)
assert(b_span.Links:find('"TraceState":"vendor=active"', 1, true) ~= nil,
    "Links must contain TraceState: " .. b_span.Links)
assert(b_span.Links:find('"link.key":"link.val"', 1, true) ~= nil,
    "Links must contain link attribute: " .. b_span.Links)
assert(not b_span.Links:find("[\x00-\x1f]"), "Links JSON must not contain raw control bytes")

-- Attribute preservation verification
assert_all_columns_primitive(b_span, "b_span")
assert(type(b_span.ResourceAttributes) == "string", "ResourceAttributes must be a JSON string")
assert(b_span.ResourceAttributes:find('"res.upper_hex":"4BF92F3577B34DA6A3CE929D0E0E4736"', 1, true) ~= nil,
    "ResourceAttributes uppercase hex string must not be altered")
assert(type(b_span.SpanAttributes) == "string", "SpanAttributes must be a JSON string")
assert(b_span.SpanAttributes:find('"span.raw_hex":"UPPERCASE_AND_HEX_4BF9"', 1, true) ~= nil,
    "SpanAttributes string must not be altered")
assert(b_span.SpanAttributes:find('"span.exact_16":"1234567812345678"', 1, true) ~= nil,
    "SpanAttributes exact 16-byte string must not be altered")

-- 2. Logs table with 16B/8B binary IDs, embedded NUL bytes, and attribute preservation
local binary_table_logs = {
    resourceLogs = {
        {
            resource = {
                attributes = {
                    { key = "service.name", value = { stringValue = "bin-log-svc" } },
                    { key = "res.upper_hex", value = { stringValue = "4BF92F3577B34DA6A3CE929D0E0E4736" } },
                }
            },
            scopeLogs = {
                {
                    scope = { name = "bin-log-scope" },
                    logRecords = {
                        {
                            timeUnixNano = "1700000000000000000",
                            traceId = bin_trace_id_16,
                            spanId = bin_span_id_8,
                            body = "BODY_WITH_UPPER_HEX_4BF9",
                            attributes = {
                                { key = "log.raw_hex", value = { stringValue = "UPPERCASE_AND_HEX_4BF9" } },
                                { key = "log.exact_16", value = { stringValue = "1234567812345678" } },
                            }
                        },
                        {
                            timeUnixNano = "1700000000000000001",
                            traceId = string.rep(string.char(0x00), 16),
                            spanId = string.rep(string.char(0x00), 8),
                            body = "all-zero-binary-ids",
                        },
                        {
                            timeUnixNano = "1700000000000000002",
                            traceId = "\x4b\xf9\x2f\x35\x77\xb3\x4d\xa6\xa3\xce\x92\x9d\x0e\x0e\x47\x36",
                            spanId = "\x00\xf0\x67\xaa\x0b\xa9\x02\xb7",
                            body = "mixed-bytes-ids",
                        }
                    }
                }
            }
        }
    }
}

local bin_log_events = {}
local n_bin_logs, bin_l_err = vo.logs.process(binary_table_logs, function(ev)
    table.insert(bin_log_events, ev)
end)
assert(n_bin_logs == 3, "Expected 3 binary log records emitted, got: " .. tostring(n_bin_logs))
assert(bin_l_err == nil, "Expected no error, got: " .. tostring(bin_l_err))
assert(#bin_log_events == 3, "Expected 3 log events in table")

local b_log1 = bin_log_events[1].log
assert_all_columns_primitive(b_log1, "b_log1")
assert(b_log1.TraceId == string.rep("aa", 16), "Log TraceId mismatch: " .. tostring(b_log1.TraceId))
assert(b_log1.SpanId == string.rep("bb", 8), "Log SpanId mismatch: " .. tostring(b_log1.SpanId))
assert(b_log1.Body == "BODY_WITH_UPPER_HEX_4BF9", "Log Body must not be modified")
assert(type(b_log1.ResourceAttributes) == "string", "ResourceAttributes must be a JSON string")
assert(b_log1.ResourceAttributes:find('"res.upper_hex":"4BF92F3577B34DA6A3CE929D0E0E4736"', 1, true) ~= nil,
    "ResourceAttributes uppercase hex string must not be altered")
assert(type(b_log1.LogAttributes) == "string", "LogAttributes must be a JSON string")
assert(b_log1.LogAttributes:find('"log.raw_hex":"UPPERCASE_AND_HEX_4BF9"', 1, true) ~= nil,
    "LogAttributes raw string must not be altered")
assert(b_log1.LogAttributes:find('"log.exact_16":"1234567812345678"', 1, true) ~= nil,
    "LogAttributes exact 16 string must not be altered")

local b_log2 = bin_log_events[2].log
assert(b_log2.TraceId == string.rep("0", 32),
    "16 NUL bytes TraceId must emit 32 '0's, got: " .. tostring(b_log2.TraceId))
assert(b_log2.SpanId == string.rep("0", 16),
    "8 NUL bytes SpanId must emit 16 '0's, got: " .. tostring(b_log2.SpanId))

local b_log3 = bin_log_events[3].log
assert(b_log3.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736", "Mixed binary TraceId mismatch: " .. tostring(b_log3.TraceId))
assert(b_log3.SpanId == "00f067aa0ba902b7", "Mixed binary SpanId mismatch: " .. tostring(b_log3.SpanId))

-- 3. Metrics table with 16B/8B binary IDs in Exemplars (Gauge, Sum, Histogram)
local binary_table_metrics = {
    resourceMetrics = {
        {
            resource = {
                attributes = {
                    { key = "service.name", value = { stringValue = "bin-metric-svc" } },
                    { key = "res.upper_hex", value = { stringValue = "4BF92F3577B34DA6A3CE929D0E0E4736" } },
                }
            },
            scopeMetrics = {
                {
                    scope = { name = "bin-meter" },
                    metrics = {
                        {
                            name = "bin_gauge",
                            gauge = {
                                dataPoints = {
                                    {
                                        timeUnixNano = "1700000000000000000",
                                        asDouble = 12.5,
                                        attributes = {
                                            { key = "dp.raw_hex", value = { stringValue = "UPPERCASE_4BF9" } },
                                        },
                                        exemplars = {
                                            {
                                                timeUnixNano = "1700000000123456789",
                                                traceId = bin_trace_id_16,
                                                spanId = bin_span_id_8,
                                                asDouble = 12.5,
                                                filteredAttributes = {
                                                    { key = "user.id", value = { stringValue = "usr-1" } },
                                                    { key = "filter.raw_hex", value = { stringValue = "UPPERCASE_4BF9" } },
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        },
                        {
                            name = "bin_sum",
                            sum = {
                                dataPoints = {
                                    {
                                        timeUnixNano = "1700000000000000000",
                                        asInt = 100,
                                        exemplars = {
                                            {
                                                timeUnixNano = "1700000000987654321",
                                                traceId = "\x4b\xf9\x2f\x35\x77\xb3\x4d\xa6\xa3\xce\x92\x9d\x0e\x0e\x47\x36",
                                                spanId = "\x00\xf0\x67\xaa\x0b\xa9\x02\xb7",
                                                asInt = 42,
                                                filteredAttributes = {
                                                    { key = "tenant", value = { stringValue = "acme" } }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        },
                        {
                            name = "bin_histogram",
                            histogram = {
                                dataPoints = {
                                    {
                                        timeUnixNano = "1700000000000000000",
                                        count = 1,
                                        sum = 0.5,
                                        bucketCounts = { 1 },
                                        explicitBounds = { 1.0 },
                                        exemplars = {
                                            {
                                                timeUnixNano = "1700000000555555555",
                                                traceId = string.rep(string.char(0x00), 16),
                                                spanId = string.rep(string.char(0x00), 8),
                                                value = 0.5,
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

local bin_metric_events = {}
local n_bin_metrics, bin_m_err = vo.metrics.process(binary_table_metrics, function(ev)
    table.insert(bin_metric_events, ev)
end)
assert(n_bin_metrics == 3, "Expected 3 binary metrics emitted, got: " .. tostring(n_bin_metrics))
assert(bin_m_err == nil, "Expected no error, got: " .. tostring(bin_m_err))
assert(#bin_metric_events == 3, "Expected 3 metric events in table")

-- Gauge Exemplar
local b_m1 = bin_metric_events[1].log
assert_all_columns_primitive(b_m1, "b_m1")
assert(b_m1.MetricName == "bin_gauge")
assert(type(b_m1.ResourceAttributes) == "string")
assert(b_m1.ResourceAttributes:find('"res.upper_hex":"4BF92F3577B34DA6A3CE929D0E0E4736"', 1, true) ~= nil)
assert(type(b_m1.Attributes) == "string")
assert(b_m1.Attributes:find('"dp.raw_hex":"UPPERCASE_4BF9"', 1, true) ~= nil)
assert(type(b_m1.Exemplars) == "string", "Exemplars must be a JSON string")
assert(b_m1.Exemplars == '[{"FilteredAttributes":{"user.id":"usr-1","filter.raw_hex":"UPPERCASE_4BF9"},"TimeUnix":"2023-11-14T22:13:20.123456789Z","Value":12.5,"SpanId":"' .. string.rep("bb", 8) .. '","TraceId":"' .. string.rep("aa", 16) .. '"}]',
    "Exemplar 1 JSON mismatch: " .. tostring(b_m1.Exemplars))

-- Sum Exemplar
local b_m2 = bin_metric_events[2].log
assert_all_columns_primitive(b_m2, "b_m2")
assert(b_m2.MetricName == "bin_sum")
assert(type(b_m2.Exemplars) == "string")
assert(b_m2.Exemplars == '[{"FilteredAttributes":{"tenant":"acme"},"TimeUnix":"2023-11-14T22:13:20.987654321Z","Value":42,"SpanId":"00f067aa0ba902b7","TraceId":"4bf92f3577b34da6a3ce929d0e0e4736"}]',
    "Exemplar 2 JSON mismatch: " .. tostring(b_m2.Exemplars))

-- Histogram Exemplar (NUL bytes)
local b_m3 = bin_metric_events[3].log
assert_all_columns_primitive(b_m3, "b_m3")
assert(b_m3.MetricName == "bin_histogram")
assert(type(b_m3.Exemplars) == "string")
assert(b_m3.Exemplars == '[{"FilteredAttributes":{},"TimeUnix":"2023-11-14T22:13:20.555555555Z","Value":0.5,"SpanId":"' .. string.rep("0", 16) .. '","TraceId":"' .. string.rep("0", 32) .. '"}]',
    "Exemplar 3 JSON mismatch: " .. tostring(b_m3.Exemplars))

-- 4. Uppercase Hex Normalization Across All Modes (JSON and Table)
-- JSON Mode: Traces, Logs, Metrics Exemplars
local upper_json_traces = [[{
  "resourceSpans": [{
    "resource": {"attributes": [{"key": "res.hex", "value": {"stringValue": "4BF92F3577B34DA6A3CE929D0E0E4736"}}]},
    "scopeSpans": [{
      "spans": [{
        "traceId": "4BF92F3577B34DA6A3CE929D0E0E4736",
        "spanId": "00F067AA0BA902B7",
        "parentSpanId": "AABBCCDDEEFF0011",
        "name": "UPPER_SPAN",
        "startTimeUnixNano": "1000",
        "endTimeUnixNano": "2000",
        "links": [{
          "traceId": "DEADBEEF0123456789ABCDEF00112233",
          "spanId": "CAFEDEAD00112233"
        }]
      }]
    }]
  }]
}]]

local upper_trace_events = {}
local n_u_traces, u_t_err = vo.traces.process(upper_json_traces, function(ev) table.insert(upper_trace_events, ev) end)
assert(n_u_traces == 1 and u_t_err == nil)
local u_s = upper_trace_events[1].log
assert_all_columns_primitive(u_s, "u_s")
assert(u_s.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736", "JSON TraceId must be lowercase hex")
assert(u_s.SpanId == "00f067aa0ba902b7", "JSON SpanId must be lowercase hex")
assert(u_s.ParentSpanId == "aabbccddeeff0011", "JSON ParentSpanId must be lowercase hex")
assert(u_s.ResourceAttributes == '{"res.hex":"4BF92F3577B34DA6A3CE929D0E0E4736"}', "ResourceAttributes must remain untouched")
assert(u_s.Links:find('"TraceId":"deadbeef0123456789abcdef00112233"', 1, true) ~= nil, "JSON Links TraceId must be lowercase hex")
assert(u_s.Links:find('"SpanId":"cafedead00112233"', 1, true) ~= nil, "JSON Links SpanId must be lowercase hex")

-- JSON Mode: Logs
local upper_json_logs = [[{
  "resourceLogs": [{
    "scopeLogs": [{
      "logRecords": [{
        "timeUnixNano": "1000",
        "traceId": "4BF92F3577B34DA6A3CE929D0E0E4736",
        "spanId": "00F067AA0BA902B7",
        "body": "UPPER_BODY_4BF9"
      }]
    }]
  }]
}]]
local upper_log_events = {}
local n_u_logs, u_l_err = vo.logs.process(upper_json_logs, function(ev) table.insert(upper_log_events, ev) end)
assert(n_u_logs == 1 and u_l_err == nil)
local u_l = upper_log_events[1].log
assert(u_l.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736")
assert(u_l.SpanId == "00f067aa0ba902b7")
assert(u_l.Body == "UPPER_BODY_4BF9")

-- JSON Mode: Metrics Exemplars
local upper_json_metrics = [[{
  "resourceMetrics": [{
    "scopeMetrics": [{
      "metrics": [{
        "name": "u_gauge",
        "gauge": {
          "dataPoints": [{
            "timeUnixNano": "1000",
            "asDouble": 1.0,
            "exemplars": [{
              "timeUnixNano": "1000",
              "traceId": "4BF92F3577B34DA6A3CE929D0E0E4736",
              "spanId": "00F067AA0BA902B7",
              "asDouble": 1.0
            }]
          }]
        }
      }]
    }]
  }]
}]]
local upper_metric_events = {}
local n_u_metrics, u_m_err = vo.metrics.process(upper_json_metrics, function(ev) table.insert(upper_metric_events, ev) end)
assert(n_u_metrics == 1 and u_m_err == nil)
local u_m = upper_metric_events[1].log
assert_all_columns_primitive(u_m, "u_m")
assert(type(u_m.Exemplars) == "string")
assert(u_m.Exemplars:find('"TraceId":"4bf92f3577b34da6a3ce929d0e0e4736"', 1, true) ~= nil)
assert(u_m.Exemplars:find('"SpanId":"00f067aa0ba902b7"', 1, true) ~= nil)

-- Table Mode: Uppercase Hex in Logs, Traces, Metrics
local upper_table_traces = {
    resourceSpans = {
        {
            scopeSpans = {
                {
                    spans = {
                        {
                            trace_id = "4BF92F3577B34DA6A3CE929D0E0E4736",
                            span_id = "00F067AA0BA902B7",
                            parent_span_id = "AABBCCDDEEFF0011",
                            name = "tbl-upper",
                            links = {
                                {
                                    trace_id = "DEADBEEF0123456789ABCDEF00112233",
                                    span_id = "CAFEDEAD00112233"
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
local u_tbl_traces = {}
local n_ut_t, ut_t_err = vo.traces.process(upper_table_traces, function(ev) table.insert(u_tbl_traces, ev) end)
assert(n_ut_t == 1 and ut_t_err == nil)
assert(u_tbl_traces[1].log.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736")
assert(u_tbl_traces[1].log.SpanId == "00f067aa0ba902b7")
assert(u_tbl_traces[1].log.ParentSpanId == "aabbccddeeff0011")
assert(u_tbl_traces[1].log.Links:find('"TraceId":"deadbeef0123456789abcdef00112233"', 1, true) ~= nil)
assert(u_tbl_traces[1].log.Links:find('"SpanId":"cafedead00112233"', 1, true) ~= nil)

local upper_table_logs = {
    resourceLogs = {
        {
            scopeLogs = {
                {
                    logRecords = {
                        {
                            trace_id = "4BF92F3577B34DA6A3CE929D0E0E4736",
                            span_id = "00F067AA0BA902B7",
                            body = "tbl-upper-body"
                        }
                    }
                }
            }
        }
    }
}
local u_tbl_logs = {}
local n_ut_l, ut_l_err = vo.logs.process(upper_table_logs, function(ev) table.insert(u_tbl_logs, ev) end)
assert(n_ut_l == 1 and ut_l_err == nil)
assert(u_tbl_logs[1].log.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736")
assert(u_tbl_logs[1].log.SpanId == "00f067aa0ba902b7")

local upper_table_metrics = {
    resourceMetrics = {
        {
            scopeMetrics = {
                {
                    metrics = {
                        {
                            name = "tbl_upper_gauge",
                            gauge = {
                                dataPoints = {
                                    {
                                        asDouble = 1.0,
                                        exemplars = {
                                            {
                                                trace_id = "4BF92F3577B34DA6A3CE929D0E0E4736",
                                                span_id = "00F067AA0BA902B7",
                                                asDouble = 1.0
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
local u_tbl_metrics = {}
local n_ut_m, ut_m_err = vo.metrics.process(upper_table_metrics, function(ev) table.insert(u_tbl_metrics, ev) end)
assert(n_ut_m == 1 and ut_m_err == nil)
assert_all_columns_primitive(u_tbl_metrics[1].log, "u_tbl_metrics[1].log")
assert(type(u_tbl_metrics[1].log.Exemplars) == "string")
assert(u_tbl_metrics[1].log.Exemplars:find('"TraceId":"4bf92f3577b34da6a3ce929d0e0e4736"', 1, true) ~= nil)
assert(u_tbl_metrics[1].log.Exemplars:find('"SpanId":"00f067aa0ba902b7"', 1, true) ~= nil)

print("   Issue #8 correlation ID normalization regression tests passed.")

--------------------------------------------------------------------------------
-- Issue #9 Reproduction: ExponentialHistogram & Summary (Table & JSON Modes)
--------------------------------------------------------------------------------
print(">> Testing Issue #9 ExponentialHistogram and Summary fidelity (Table & JSON)...")

-- 1. Issue #9 Lua Table mode reproduction (exponentialHistogram + summary)
local issue9_table_payload = {
    resourceMetrics = {
        {
            resource = {
                attributes = {
                    { key = "service.name", value = { stringValue = "issue9-svc" } }
                }
            },
            scopeMetrics = {
                {
                    scope = { name = "issue9-meter", version = "1.0.0" },
                    metrics = {
                        {
                            name = "http.server.duration.exp",
                            description = "Exponential histogram duration",
                            unit = "ms",
                            exponentialHistogram = {
                                aggregationTemporality = 2,
                                dataPoints = {
                                    {
                                        startTimeUnixNano = "1700000000000000000",
                                        timeUnixNano = "1700000001000000000",
                                        count = 42,
                                        sum = 105.5,
                                        min = 0.5,
                                        max = 12.0,
                                        scale = 3,
                                        zeroCount = 2,
                                        zeroThreshold = 0.0,
                                        positive = {
                                            offset = -1,
                                            bucketCounts = { 5, 15, 20 }
                                        },
                                        negative = {
                                            offset = 0,
                                            bucketCounts = { 0 }
                                        },
                                        attributes = {
                                            { key = "http.route", value = { stringValue = "/api/v1/pay" } }
                                        }
                                    }
                                }
                            }
                        },
                        {
                            name = "rpc.server.latency.summary",
                            description = "Summary RPC latency",
                            unit = "ms",
                            summary = {
                                dataPoints = {
                                    {
                                        startTimeUnixNano = "1700000000000000000",
                                        timeUnixNano = "1700000002000000000",
                                        count = 50,
                                        sum = 1234.5,
                                        quantileValues = {
                                            { quantile = 0.5, value = 12.0 },
                                            { quantile = 0.99, value = 85.0 }
                                        },
                                        attributes = {
                                            { key = "rpc.method", value = { stringValue = "Charge" } }
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

local issue9_tbl_events = {}
local n_i9_tbl, i9_tbl_err = vo.metrics.process(issue9_table_payload, function(ev) table.insert(issue9_tbl_events, ev) end)
assert(n_i9_tbl == 2, "Expected 2 table metrics for Issue #9, got: " .. tostring(n_i9_tbl))
assert(i9_tbl_err == nil, "Expected nil error for Issue #9 table mode, got: " .. tostring(i9_tbl_err))
assert(#issue9_tbl_events == 2, "Expected 2 emitted events for Issue #9 table mode")

local i9_tbl_eh = issue9_tbl_events[1].log
assert_all_columns_primitive(i9_tbl_eh, "i9_tbl_eh")
assert(i9_tbl_eh.MetricName == "http.server.duration.exp")
assert(i9_tbl_eh.MetricType == "exponential_histogram", "Expected exponential_histogram, got: " .. tostring(i9_tbl_eh.MetricType))
assert(i9_tbl_eh.AggregationTemporality == 2, "Expected AggregationTemporality 2, got: " .. tostring(i9_tbl_eh.AggregationTemporality))
assert(i9_tbl_eh.Value == nil, "ExponentialHistogram Value must be nil")
assert(type(i9_tbl_eh.ExponentialHistogram) == "string", "ExponentialHistogram must be a JSON string")
assert(i9_tbl_eh.ExponentialHistogram == '{"aggregationTemporality":2,"count":42,"sum":105.5,"min":0.5,"max":12,"scale":3,"zeroCount":2,"zeroThreshold":0,"positive":{"offset":-1,"bucketCounts":[5,15,20]},"negative":{"offset":0,"bucketCounts":[0]}}',
    "ExponentialHistogram table-mode JSON mismatch: " .. tostring(i9_tbl_eh.ExponentialHistogram))
assert(i9_tbl_eh.Attributes == '{"http.route":"/api/v1/pay"}')
assert(i9_tbl_eh.Exemplars == "[]")

local i9_tbl_sum = issue9_tbl_events[2].log
assert_all_columns_primitive(i9_tbl_sum, "i9_tbl_sum")
assert(i9_tbl_sum.MetricName == "rpc.server.latency.summary")
assert(i9_tbl_sum.MetricType == "summary")
assert(i9_tbl_sum.Value == nil, "Summary Value must be nil")
assert(type(i9_tbl_sum.Summary) == "string", "Summary must be a JSON string")
assert(i9_tbl_sum.Summary == '{"count":50,"sum":1234.5,"quantileValues":[{"quantile":0.5,"value":12},{"quantile":0.99,"value":85}]}',
    "Summary table-mode JSON mismatch: " .. tostring(i9_tbl_sum.Summary))
assert(i9_tbl_sum.Attributes == '{"rpc.method":"Charge"}')
assert(i9_tbl_sum.Exemplars == "[]")

-- 2. Issue #9 JSON string mode reproduction (mixed_json)
local mixed_json = [[{
  "resourceMetrics": [{
    "resource": {"attributes": [{"key": "service.name", "value": {"stringValue": "issue9-json-svc"}}]},
    "scopeMetrics": [{
      "scope": {"name": "issue9-json-scope", "version": "1.0.0"},
      "metrics": [
        {
          "name": "http.server.duration.exp",
          "description": "Exponential histogram duration",
          "unit": "ms",
          "exponentialHistogram": {
            "aggregationTemporality": 1,
            "dataPoints": [{
              "startTimeUnixNano": "1700000000000000000",
              "timeUnixNano": "1700000001000000000",
              "count": "42",
              "sum": 105.5,
              "min": 0.5,
              "max": 12.0,
              "scale": -1,
              "zeroCount": "2",
              "zeroThreshold": 0.0,
              "positive": {"offset": 1, "bucketCounts": ["5", "15", "20"]},
              "negative": {"offset": -2, "bucketCounts": ["0"]}
            }]
          }
        },
        {
          "name": "rpc.server.latency.summary",
          "description": "Summary RPC latency",
          "unit": "ms",
          "summary": {
            "dataPoints": [{
              "startTimeUnixNano": "1700000000000000000",
              "timeUnixNano": "1700000002000000000",
              "count": "50",
              "sum": 1234.5,
              "quantileValues": [
                {"quantile": 0.5, "value": 12.0},
                {"quantile": 0.99, "value": 85.0}
              ]
            }]
          }
        }
      ]
    }]
  }]
}]]

local issue9_json_events = {}
local n_i9_json, i9_json_err = vo.metrics.process(mixed_json, function(ev) table.insert(issue9_json_events, ev) end)
assert(n_i9_json == 2, "Expected 2 JSON metrics for Issue #9, got: " .. tostring(n_i9_json))
assert(i9_json_err == nil, "Expected nil error for Issue #9 JSON mode, got: " .. tostring(i9_json_err))
assert(#issue9_json_events == 2, "Expected 2 emitted events for Issue #9 JSON mode")

local i9_json_eh = issue9_json_events[1].log
assert_all_columns_primitive(i9_json_eh, "i9_json_eh")
assert(i9_json_eh.MetricType == "exponential_histogram")
assert(i9_json_eh.AggregationTemporality == 1)
assert(i9_json_eh.Value == nil)
assert(type(i9_json_eh.ExponentialHistogram) == "string")
assert(i9_json_eh.ExponentialHistogram == '{"aggregationTemporality":1,"count":42,"sum":105.5,"min":0.5,"max":12,"scale":-1,"zeroCount":2,"zeroThreshold":0,"positive":{"offset":1,"bucketCounts":[5,15,20]},"negative":{"offset":-2,"bucketCounts":[0]}}',
    "ExponentialHistogram JSON-mode mismatch: " .. tostring(i9_json_eh.ExponentialHistogram))
assert(i9_json_eh.Exemplars == "[]")

local i9_json_sum = issue9_json_events[2].log
assert_all_columns_primitive(i9_json_sum, "i9_json_sum")
assert(i9_json_sum.MetricType == "summary")
assert(i9_json_sum.Value == nil)
assert(type(i9_json_sum.Summary) == "string")
assert(i9_json_sum.Summary == '{"count":50,"sum":1234.5,"quantileValues":[{"quantile":0.5,"value":12},{"quantile":0.99,"value":85}]}',
    "Summary JSON-mode mismatch: " .. tostring(i9_json_sum.Summary))
assert(i9_json_sum.Exemplars == "[]")

print("   Issue #9 ExponentialHistogram and Summary validation passed.")

--------------------------------------------------------------------------------
-- 50,000-Event Stress Loop & GC Stability Check (Dual-Mode: String & Table)
--------------------------------------------------------------------------------
print(">> Running 50,000-event stress loop and checking memory stability...")

local stress_count = 0
local function discard_emit(ev)
    stress_count = stress_count + 1
end

-- Compact JSON payloads for stress test
local stress_log = [[{"resourceLogs":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"stress"}}]},"scopeLogs":[{"scope":{"name":"s"},"logRecords":[{"timeUnixNano":"1293840000000000000","severityNumber":9,"severityText":"INFO","body":{"stringValue":"stress-msg"},"attributes":[{"key":"iter","value":{"intValue":"1"}}]}]}]}]}]]
local stress_trace = [[{"resourceSpans":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"stress"}}]},"scopeSpans":[{"scope":{"name":"s"},"spans":[{"traceId":"5b8efff798038103d269b633813fc60c","spanId":"eee19b7ec3c1b174","name":"stress-op","startTimeUnixNano":"1293840000000000000","endTimeUnixNano":"1293840000100000000"}]}]}]}]]
local stress_metric = [[{"resourceMetrics":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"stress"}}]},"scopeMetrics":[{"scope":{"name":"s"},"metrics":[{"name":"stress_m","gauge":{"dataPoints":[{"timeUnixNano":"1293840000000000000","asInt":"42"}]}}]}]}]}]]

-- Single-event binary table payloads for stress test
local stress_bin_log = {
    resourceLogs = {
        {
            resource = { attributes = { { key = "service.name", value = { stringValue = "stress-bin" } } } },
            scopeLogs = {
                {
                    scope = { name = "s" },
                    logRecords = {
                        {
                            timeUnixNano = "1293840000000000000",
                            severityNumber = 9,
                            severityText = "INFO",
                            body = "stress-bin-msg",
                            traceId = bin_trace_id_16,
                            spanId = bin_span_id_8,
                            attributes = { { key = "iter", value = { intValue = 1 } } }
                        }
                    }
                }
            }
        }
    }
}

local stress_bin_trace = {
    resourceSpans = {
        {
            resource = { attributes = { { key = "service.name", value = { stringValue = "stress-bin" } } } },
            scopeSpans = {
                {
                    scope = { name = "s" },
                    spans = {
                        {
                            traceId = bin_trace_id_16,
                            spanId = bin_span_id_8,
                            parentSpanId = bin_parent_span_id_8,
                            name = "stress-bin-op",
                            startTimeUnixNano = "1293840000000000000",
                            endTimeUnixNano = "1293840000100000000",
                            links = {
                                {
                                    traceId = bin_link_trace_id_16,
                                    spanId = bin_link_span_id_8,
                                    traceState = "v=1",
                                    attributes = { { key = "lk", value = { stringValue = "lv" } } }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

local stress_bin_metric = {
    resourceMetrics = {
        {
            resource = { attributes = { { key = "service.name", value = { stringValue = "stress-bin" } } } },
            scopeMetrics = {
                {
                    scope = { name = "s" },
                    metrics = {
                        {
                            name = "stress_bin_m",
                            gauge = {
                                dataPoints = {
                                    {
                                        timeUnixNano = "1293840000000000000",
                                        asDouble = 42.0,
                                        exemplars = {
                                            {
                                                timeUnixNano = "1293840000000000000",
                                                traceId = bin_trace_id_16,
                                                spanId = bin_span_id_8,
                                                asDouble = 42.0
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

-- 1. Warm up Lua VM internal string pools, upvalues, and C++ static allocations
for i = 1, 1000 do
    vo.logs.process(stress_log, discard_emit)
    vo.traces.process(stress_trace, discard_emit)
    vo.metrics.process(stress_metric, discard_emit)
    vo.logs.process(table_logs_payload, discard_emit)
    vo.traces.process(table_traces_payload, discard_emit)
    vo.metrics.process(table_metrics_payload, discard_emit)
    vo.logs.process(stress_bin_log, discard_emit)
    vo.traces.process(stress_bin_trace, discard_emit)
    vo.metrics.process(stress_bin_metric, discard_emit)
end

-- Reset counter for actual 50,000 stress loop
stress_count = 0

-- Full GC collection to establish baseline
collectgarbage("collect")
collectgarbage("collect")
local mem_start = collectgarbage("count")

local target_events = 50000

-- Run 6,000 string logs, 6,000 standard table logs, 6,000 binary table logs (18,000 logs)
for i = 1, 6000 do
    vo.logs.process(stress_log, discard_emit)
end
for i = 1, 6000 do
    vo.logs.process(table_logs_payload, discard_emit)
end
for i = 1, 6000 do
    vo.logs.process(stress_bin_log, discard_emit)
end
local current_mem = collectgarbage("count")
assert(current_mem < mem_start + 1024, "Unbounded memory growth during logs: " .. current_mem .. " KB")

-- Run 6,000 string traces, 6,000 standard table traces, 6,000 binary table traces (18,000 traces)
for i = 1, 6000 do
    vo.traces.process(stress_trace, discard_emit)
end
for i = 1, 6000 do
    vo.traces.process(table_traces_payload, discard_emit)
end
for i = 1, 6000 do
    vo.traces.process(stress_bin_trace, discard_emit)
end
current_mem = collectgarbage("count")
assert(current_mem < mem_start + 1024, "Unbounded memory growth during traces: " .. current_mem .. " KB")

-- Run 5,000 string metrics, 5,000 standard table metrics, 4,000 binary table metrics (14,000 metrics)
for i = 1, 5000 do
    vo.metrics.process(stress_metric, discard_emit)
end
for i = 1, 5000 do
    vo.metrics.process(table_metrics_payload, discard_emit)
end
for i = 1, 4000 do
    vo.metrics.process(stress_bin_metric, discard_emit)
end
current_mem = collectgarbage("count")
assert(current_mem < mem_start + 1024, "Unbounded memory growth during metrics: " .. current_mem .. " KB")

assert(stress_count == target_events, string.format("Expected %d emitted events, got %d", target_events, stress_count))

-- Collect garbage and measure final memory
collectgarbage("collect")
collectgarbage("collect")
local mem_end = collectgarbage("count")
local delta = mem_end - mem_start

print(string.format("   Stress Loop: Processed %d events across logs, traces, and metrics (String, Table & Binary modes).", stress_count))
print(string.format("   Memory Baseline: %.2f KB, Memory Final: %.2f KB, Memory Delta: %.2f KB", mem_start, mem_end, delta))

-- Requirement: memory stays bounded with zero leaks (delta between start and end after collectgarbage("collect") is <= 10 KB)
assert(delta <= 10.0, string.format("Memory leak detected! Delta is %.2f KB (must be <= 10 KB)", delta))

print("ALL LUA TESTS PASSED (0 leaks)")
