-- test_vector_native.lua
-- Standalone Lua 5.4 Test Suite & GC Stability for vector_native

local build_dir = os.getenv("BUILD_DIR")
if build_dir and #build_dir > 0 then
  package.cpath = build_dir .. "/?.so;" .. package.cpath
elseif not package.cpath:find("build/%?%.so") then
  package.cpath = "build/?.so;../build/?.so;../../build/?.so;" .. package.cpath
end

local ISO_8601_NANO_PATTERN = "^%d%d%d%d%-%d%d%-%d%dT%d%d:%d%d:%d%d%.%d%d%d%d%d%d%d%d%dZ$"

print("=== Running vector_native Standalone Test Suite ===")

--------------------------------------------------------------------------------
-- 1. Module Loading & Submodule Registration
--------------------------------------------------------------------------------
print(">> Testing module loading and exports...")

local vn = require("vector_native")
assert(type(vn) == "table", "vector_native must export a table")
assert(vn._VERSION == "0.3.0", "Expected _VERSION 0.3.0, got: " .. tostring(vn._VERSION))

assert(type(vn.logs) == "table", "vn.logs must be a table")
assert(type(vn.logs.process) == "function", "vn.logs.process must be a function")

assert(type(vn.traces) == "table", "vn.traces must be a table")
assert(type(vn.traces.process) == "function", "vn.traces.process must be a function")

assert(type(vn.metrics) == "table", "vn.metrics must be a table")
assert(type(vn.metrics.process) == "function", "vn.metrics.process must be a function")

local logs = require("vector_native.logs")
assert(logs == vn.logs, "require('vector_native.logs') must return vn.logs")

local traces = require("vector_native.traces")
assert(traces == vn.traces, "require('vector_native.traces') must return vn.traces")

local metrics = require("vector_native.metrics")
assert(metrics == vn.metrics, "require('vector_native.metrics') must return vn.metrics")

local vo = require("vector_otel")

print("   Module and submodule loading passed.")

--------------------------------------------------------------------------------
-- 2. Error Contracts, Falsy Semantics & Non-OTLP Metric Exclusions
--------------------------------------------------------------------------------
print(">> Testing error contracts and non-OTLP metric variant exclusions...")

local dummy_emit = function(event) end

-- Malformed JSON
local count, err, emitted = vn.logs.process("{invalid json", dummy_emit)
assert(count == nil and type(err) == "string" and #err > 0 and emitted == 0)

count, err, emitted = vn.traces.process("{invalid json", dummy_emit)
assert(count == nil and type(err) == "string" and #err > 0 and emitted == 0)

count, err, emitted = vn.metrics.process("{invalid json", dummy_emit)
assert(count == nil and type(err) == "string" and #err > 0 and emitted == 0)

-- Invalid argument types
count, err, emitted = vn.logs.process(12345, dummy_emit)
assert(count == nil and err == "payload must be a string or table" and emitted == 0)

count, err, emitted = vn.logs.process({}, "not_a_function")
assert(count == nil and err == "emit callback must be a function" and emitted == 0)

count, err, emitted = vn.traces.process(false, dummy_emit)
assert(count == nil and err == "payload must be a string or table" and emitted == 0)

count, err, emitted = vn.traces.process({}, nil)
assert(count == nil and err == "emit callback must be a function" and emitted == 0)

count, err, emitted = vn.metrics.process(true, dummy_emit)
assert(count == nil and err == "payload must be a string or table" and emitted == 0)

count, err, emitted = vn.metrics.process({}, 42)
assert(count == nil and err == "emit callback must be a function" and emitted == 0)

-- Empty/sparse payloads return (0, nil)
count, err = vn.logs.process({}, dummy_emit)
assert(count == 0 and err == nil)
count, err = vn.logs.process({ log = {} }, dummy_emit)
assert(count == 0 and err == nil)

count, err = vn.traces.process({}, dummy_emit)
assert(count == 0 and err == nil)
count, err = vn.traces.process({ trace = {} }, dummy_emit)
assert(count == 0 and err == nil)

count, err = vn.metrics.process({}, dummy_emit)
assert(count == 0 and err == nil)
count, err = vn.metrics.process({ metric = {} }, dummy_emit)
assert(count == 0 and err == nil)

-- Protected emit callback failure
local failing_emit = function(ev)
  error("sink buffer full")
end
count, err, emitted = vn.logs.process({ message = "hello" }, failing_emit)
assert(count == nil and type(err) == "string" and err:find("sink buffer full") and emitted == 0)

-- Re-entrant invocation rejection
local inner_count, inner_err, inner_emitted
local outer_events = {}
count, err = vn.logs.process({ { message = "first" }, { message = "second" } }, function(ev)
  table.insert(outer_events, ev)
  if #outer_events == 1 then
    inner_count, inner_err, inner_emitted = vn.logs.process({ message = "inner" }, dummy_emit)
  end
end)
assert(count == 2 and err == nil and #outer_events == 2)
assert(inner_count == nil)
assert(inner_err == "re-entrant vector_otel.process() invocation is not permitted")
assert(inner_emitted == 0)

-- Non-OTLP metric variants (distribution, set, sketch) skipped & warned in Table & String modes
for _, variant in ipairs({ "distribution", "set", "sketch" }) do
  local tbl_payload = { name = "bad_" .. variant }
  tbl_payload[variant] = { values = { 1, 2 } }
  count, err = vn.metrics.process(tbl_payload, dummy_emit)
  assert(count == 0 and err == nil, "Expected count == 0 and err == nil for table variant " .. variant .. ", got count=" .. tostring(count) .. " err=" .. tostring(err))

  local json_payload = string.format('{"name":"bad_%s","%s":{"values":[1,2]}}', variant, variant)
  count, err = vn.metrics.process(json_payload, dummy_emit)
  assert(count == 0 and err == nil, "Expected count == 0 and err == nil for JSON variant " .. variant .. ", got count=" .. tostring(count) .. " err=" .. tostring(err))
end

-- Mixed batch: valid item emitted, unsupported item skipped without failing batch
local mixed_emitted_records = {}
count, err = vn.metrics.process({
  { name = "ok_m", counter = { value = 5 } },
  { name = "bad_m", sketch = {} },
}, function(ev) table.insert(mixed_emitted_records, ev) end)
assert(count == 1 and err == nil, "Expected mixed batch success count == 1, got count=" .. tostring(count) .. " err=" .. tostring(err))
assert(#mixed_emitted_records == 1 and mixed_emitted_records[1].log.MetricName == "ok_m")

print("   Error contracts and non-OTLP metric variant exclusions passed.")

--------------------------------------------------------------------------------
-- Helper: Assert Exact Key Set Parity Between Two Tables
--------------------------------------------------------------------------------
local function assert_same_keys(t1, t2, context)
  for k, _ in pairs(t1) do
    assert(t2[k] ~= nil, string.format("[%s] Key '%s' present in first table but missing in second", context, tostring(k)))
  end
  for k, _ in pairs(t2) do
    assert(t1[k] ~= nil, string.format("[%s] Key '%s' present in second table but missing in first", context, tostring(k)))
  end
end

local function assert_all_columns_primitive(rec, label)
  assert(type(rec) == "table", label .. " must be a table")
  for k, v in pairs(rec) do
    local t = type(v)
    assert(t == "string" or t == "number" or t == "boolean",
      string.format("%s.%s must be a primitive type (string/number/boolean), got %s", label, tostring(k), t))
  end
end

--------------------------------------------------------------------------------
-- 3. Native Logs Extraction & Schema Parity with vector_otel.logs
--------------------------------------------------------------------------------
print(">> Testing native logs extraction and schema parity...")

local native_log_table = {
  log = {
    timestamp = {
      year = 2026, month = 10, day = 22,
      hour = 10, min = 0, sec = 1, nanosec = 123456789,
    },
    observed_timestamp = {
      year = 2026, month = 10, day = 22,
      hour = 10, min = 0, sec = 2, nanosec = 0,
    },
    resources = {
      ["service.name"] = "checkout-svc",
      ["host.name"] = "node-1",
    },
    resource_schema_url = "https://opentelemetry.io/schemas/1.21.0",
    scope = {
      name = "my.logger",
      version = "1.2.0",
      schema_url = "https://opentelemetry.io/schemas/scope/1.0",
      attributes = { sc_k = "sc_v" },
    },
    attributes = {
      ["http.status_code"] = 200,
      max_i = 9223372036854775807,
      nested = { k = "v" },
    },
    trace_id = "5b8efff798038103d269b633813fc60c",
    span_id = "eee19b7ec3c1b174",
    severity_text = "INFO",
    severity_number = 9,
    flags = 4,
    event_name = "checkout.completed",
    message = "hello native world",
  },
}

local native_log_events = {}
local n_logs, l_err = vn.logs.process(native_log_table, function(ev) table.insert(native_log_events, ev) end)
assert(n_logs == 1 and l_err == nil and #native_log_events == 1)

local nl = native_log_events[1].log
assert_all_columns_primitive(nl, "nl")
assert(nl.Timestamp == "2026-10-22T10:00:01.123456789Z")
assert(nl.TraceId == "5b8efff798038103d269b633813fc60c")
assert(nl.SpanId == "eee19b7ec3c1b174")
assert(nl.TraceFlags == 4)
assert(nl.SeverityText == "INFO")
assert(nl.SeverityNumber == 9)
assert(nl.ServiceName == "checkout-svc")
assert(nl.Body == "hello native world")
assert(nl.ResourceSchemaUrl == "https://opentelemetry.io/schemas/1.21.0")
assert(type(nl.ResourceAttributes) == "string")
assert(nl.ResourceAttributes:find('"service.name":"checkout-svc"', 1, true) ~= nil)
assert(nl.ResourceAttributes:find('"host.name":"node-1"', 1, true) ~= nil)
assert(nl.ScopeSchemaUrl == "https://opentelemetry.io/schemas/scope/1.0")
assert(nl.ScopeName == "my.logger")
assert(nl.ScopeVersion == "1.2.0")
assert(nl.ScopeAttributes == '{"sc_k":"sc_v"}')
assert(type(nl.LogAttributes) == "string")
assert(nl.LogAttributes:find('"http.status_code":200', 1, true) ~= nil)
assert(nl.LogAttributes:find('"max_i":9223372036854775807', 1, true) ~= nil)
assert(nl.LogAttributes:find('"nested":{"k":"v"}', 1, true) ~= nil)
assert(nl.EventName == "checkout.completed")
assert(nl.processed_at:match(ISO_8601_NANO_PATTERN))

-- Compare schema keys against vector_otel.logs
local otel_log_events = {}
vo.logs.process([[{"resourceLogs":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"checkout-svc"}}]},"scopeLogs":[{"scope":{"name":"my.logger"},"logRecords":[{"timeUnixNano":"1792663201123456789","body":{"stringValue":"hello"}}]}]}]}]], function(ev)
  table.insert(otel_log_events, ev)
end)
assert_same_keys(nl, otel_log_events[1].log, "Logs ClickHouse Schema Parity")

-- Native Logs JSON mode & timestamp fallback to processed_at
local json_log_events = {}
n_logs, l_err = vn.logs.process('{"message":"no_ts_msg","resources":{"service.name":"json-log-svc"}}', function(ev)
  table.insert(json_log_events, ev)
end)
assert(n_logs == 1 and l_err == nil)
assert_all_columns_primitive(json_log_events[1].log, "json_log_events[1].log")
assert(json_log_events[1].log.Body == "no_ts_msg")
assert(json_log_events[1].log.ServiceName == "json-log-svc")
assert(json_log_events[1].log.ResourceAttributes == '{"service.name":"json-log-svc"}')
assert(json_log_events[1].log.ScopeAttributes == "{}")
assert(json_log_events[1].log.LogAttributes == "{}")
assert(json_log_events[1].log.Timestamp == json_log_events[1].log.processed_at)

-- Array-wrapped logs in JSON
local arr_json_events = {}
local n_arr, a_err = vn.logs.process('{"log":[{"message":"arr1","service_name":"s1"},{"message":"arr2","service_name":"s2"}]}', function(ev)
  table.insert(arr_json_events, ev)
end)
assert(n_arr == 2 and a_err == nil and #arr_json_events == 2)
assert(arr_json_events[1].log.Body == "arr1")
assert(arr_json_events[2].log.Body == "arr2")

-- Array-wrapped logs in Table
local arr_tbl_events = {}
n_arr, a_err = vn.logs.process({ { message = "t1" }, { message = "t2" } }, function(ev)
  table.insert(arr_tbl_events, ev)
end)
assert(n_arr == 2 and a_err == nil and #arr_tbl_events == 2)
assert(arr_tbl_events[1].log.Body == "t1")
assert(arr_tbl_events[2].log.Body == "t2")

print("   Native logs extraction and schema parity passed.")

--------------------------------------------------------------------------------
-- 4. Native Traces Extraction & Schema Parity with vector_otel.traces
--------------------------------------------------------------------------------
print(">> Testing native traces extraction and schema parity...")

local native_trace_table = {
  source_type = "opentelemetry",
  log = {
    trace_id = "4bf92f3577b34da6a3ce929d0e0e4736",
    span_id = "00f067aa0ba902b7",
    parent_span_id = "b9c7c989f97918e1",
    trace_state = "congo=t61rcWkgMzE",
    name = "HTTP POST /orders",
    kind = 2,
    start_time_unix_nano = {
      year = 2026, month = 10, day = 22,
      hour = 10, min = 0, sec = 1, nanosec = 0,
    },
    end_time_unix_nano = {
      year = 2026, month = 10, day = 22,
      hour = 10, min = 0, sec = 1, nanosec = 50000000,
    },
    status = { code = 1, message = "ok" },
    resources = { ["service.name"] = "trace-svc" },
    scope = { name = "otel.tracer", version = "2.1.0" },
    attributes = { ["http.method"] = "POST", ["http.status_code"] = 201 },
    events = {
      {
        name = "ev1",
        time_unix_nano = "2026-10-22T10:00:01.005000000Z",
        attributes = { ek = "ev_val" },
      },
    },
    links = {
      {
        trace_id = "t2",
        span_id = "s2",
        trace_state = "ts",
        attributes = { lk = 1 },
      },
    },
  },
}

local native_trace_events = {}
local n_traces, t_err = vn.traces.process(native_trace_table, function(ev) table.insert(native_trace_events, ev) end)
assert(n_traces == 1 and t_err == nil and #native_trace_events == 1)

local nt = native_trace_events[1].log
assert_all_columns_primitive(nt, "nt")
assert(nt.Timestamp == "2026-10-22T10:00:01.000000000Z")
assert(nt.TraceId == "4bf92f3577b34da6a3ce929d0e0e4736")
assert(nt.SpanId == "00f067aa0ba902b7")
assert(nt.ParentSpanId == "b9c7c989f97918e1")
assert(nt.TraceState == "congo=t61rcWkgMzE")
assert(nt.SpanName == "HTTP POST /orders")
assert(nt.SpanKind == "SPAN_KIND_SERVER")
assert(nt.ServiceName == "trace-svc")
assert(nt.ResourceAttributes == '{"service.name":"trace-svc"}')
assert(nt.ScopeName == "otel.tracer")
assert(nt.ScopeVersion == "2.1.0")
assert(type(nt.SpanAttributes) == "string")
assert(nt.SpanAttributes:find('"http.method":"POST"', 1, true) ~= nil)
assert(nt.SpanAttributes:find('"http.status_code":201', 1, true) ~= nil)
assert(nt.Duration == 50000000)
assert(nt.StatusCode == "STATUS_CODE_OK")
assert(nt.StatusMessage == "ok")
assert(type(nt.Events) == "string" and nt.Events:find('"Name":"ev1"', 1, true) ~= nil)
assert(type(nt.Links) == "string" and nt.Links:find('"TraceId":"t2"', 1, true) ~= nil)
assert(nt.processed_at:match(ISO_8601_NANO_PATTERN))

-- Compare schema keys against vector_otel.traces
local otel_trace_events = {}
vo.traces.process([[{"resourceSpans":[{"resource":{},"scopeSpans":[{"scope":{},"spans":[{"traceId":"4bf92f3577b34da6a3ce929d0e0e4736","spanId":"00f067aa0ba902b7","name":"s","startTimeUnixNano":"1792663201000000000","endTimeUnixNano":"1792663201050000000"}]}]}]}]], function(ev)
  table.insert(otel_trace_events, ev)
end)
assert_same_keys(nt, otel_trace_events[1].log, "Traces ClickHouse Schema Parity")

-- Explicit error status code & message
local err_trace_events = {}
local n_et, et_err = vn.traces.process({
  log = {
    name = "rpc.call",
    trace_id = "5b8efff798038103d269b633813fc60c",
    span_id = "eee19b7ec3c1b174",
    status = { code = "STATUS_CODE_ERROR", message = "connection refused" },
  },
}, function(ev) table.insert(err_trace_events, ev) end)
assert(n_et == 1 and et_err == nil and #err_trace_events == 1)
assert(err_trace_events[1].log.StatusCode == "STATUS_CODE_ERROR")
assert(err_trace_events[1].log.StatusMessage == "connection refused")

-- Array-wrapped traces in JSON
local arr_trace_events = {}
local n_at, at_err = vn.traces.process('{"trace":[{"name":"s1","trace_id":"5b8efff798038103d269b633813fc60c","span_id":"eee19b7ec3c1b174"},{"name":"s2","trace_id":"5b8efff798038103d269b633813fc60c","span_id":"eee19b7ec3c1b175"}]}', function(ev)
  table.insert(arr_trace_events, ev)
end)
assert(n_at == 2 and at_err == nil and #arr_trace_events == 2)

print("   Native traces extraction and schema parity passed.")

--------------------------------------------------------------------------------
-- 5. Native Metrics Extraction & Schema Parity with vector_otel.metrics
--------------------------------------------------------------------------------
print(">> Testing native metrics extraction and schema parity...")

local native_metrics_batch = {
  {
    name = "req_total",
    namespace = "http",
    kind = "incremental",
    description = "HTTP requests counter",
    unit = "1",
    flags = 2,
    timestamp = "2026-10-22T10:00:15.500000000Z",
    interval_ms = 10000,
    tags = {
      ["resource.service.name"] = "pay-svc",
      ["resource.host.name"] = "h1",
      ["scope.name"] = "meter.http",
      ["scope.version"] = "2.0",
      ["scope.attr"] = "sval",
      ["http.method"] = "GET",
    },
    counter = { value = 42.5 },
  },
  {
    name = "req_latency",
    namespace = "rpc",
    timestamp = "2026-10-22T10:01:00.000000000Z",
    tags = {
      ["resource.service.name"] = { "old-svc", "hist-svc" },
      endpoint = { "/v1/pay", "/v2/pay" },
    },
    aggregated_histogram = {
      buckets = { 10.0, 50.0, 100.0, math.huge },
      counts = { 1, 3, 5, 2 },
      count = 11,
      sum = 450.5,
    },
  },
  {
    name = "exp_latency",
    namespace = "rpc",
    kind = "cumulative",
    timestamp = "2026-10-22T10:02:00.000000000Z",
    tags = {
      ["resource.service.name"] = "exp-svc",
      ["http.route"] = "/api/v1/exp",
    },
    exponential_histogram = {
      count = 42,
      sum = 105.5,
      min = 0.5,
      max = 12.0,
      scale = 3,
      zero_count = 2,
      zero_threshold = 0.0,
      positive = {
        offset = -1,
        bucket_counts = { 5, 15, 20 },
      },
      negative = {
        offset = 0,
        bucket_counts = { 0 },
      },
    },
  },
  {
    name = "sum_latency",
    namespace = "rpc",
    timestamp = "2026-10-22T10:03:00.000000000Z",
    tags = {
      ["resource.service.name"] = "sum-svc",
      ["rpc.method"] = "Charge",
    },
    aggregated_summary = {
      count = 50,
      sum = 1234.5,
      quantiles = { 0.5, 0.99 },
      values = { 12.0, 85.0 },
    },
  },
}

local native_metric_events = {}
local n_metrics, m_err = vn.metrics.process(native_metrics_batch, function(ev) table.insert(native_metric_events, ev) end)
assert(n_metrics == 4 and m_err == nil and #native_metric_events == 4)

local nm1 = native_metric_events[1].log
assert_all_columns_primitive(nm1, "nm1")
assert(nm1.MetricName == "http.req_total")
assert(nm1.MetricType == "sum")
assert(nm1.ServiceName == "pay-svc")
assert(type(nm1.ResourceAttributes) == "string")
assert(nm1.ResourceAttributes:find('"service.name":"pay-svc"', 1, true) ~= nil)
assert(nm1.ResourceAttributes:find('"host.name":"h1"', 1, true) ~= nil)
assert(nm1.ScopeName == "meter.http")
assert(nm1.ScopeVersion == "2.0")
assert(nm1.ScopeAttributes == '{"attr":"sval"}')
assert(nm1.Attributes == '{"http.method":"GET"}')
assert(nm1.StartTimeUnix == "2026-10-22T10:00:05.500000000Z")
assert(nm1.TimeUnix == "2026-10-22T10:00:15.500000000Z")
assert(math.abs(nm1.Value - 42.5) < 1e-6)
assert(nm1.Histogram == nil)
assert(nm1.Exemplars == "[]")

local nm2 = native_metric_events[2].log
assert_all_columns_primitive(nm2, "nm2")
assert(nm2.MetricName == "rpc.req_latency")
assert(nm2.MetricType == "histogram")
assert(nm2.ServiceName == "hist-svc")
assert(nm2.Attributes == '{"endpoint":"/v2/pay"}')
assert(nm2.Value == nil)
assert(type(nm2.Histogram) == "string")
assert(nm2.Histogram == '{"aggregationTemporality":0,"count":11,"sum":450.5,"bucketCounts":[1,3,5,2],"explicitBounds":[10,50,100]}')
assert(nm2.Exemplars == "[]")

local nm3 = native_metric_events[3].log
assert_all_columns_primitive(nm3, "nm3")
assert(nm3.MetricName == "rpc.exp_latency")
assert(nm3.MetricType == "exponential_histogram")
assert(nm3.ServiceName == "exp-svc")
assert(nm3.AggregationTemporality == 2)
assert(nm3.Attributes == '{"http.route":"/api/v1/exp"}')
assert(nm3.Value == nil)
assert(type(nm3.ExponentialHistogram) == "string")
assert(nm3.ExponentialHistogram == '{"aggregationTemporality":2,"count":42,"sum":105.5,"min":0.5,"max":12,"scale":3,"zeroCount":2,"zeroThreshold":0,"positive":{"offset":-1,"bucketCounts":[5,15,20]},"negative":{"offset":0,"bucketCounts":[0]}}')
assert(nm3.Exemplars == "[]")

local nm4 = native_metric_events[4].log
assert_all_columns_primitive(nm4, "nm4")
assert(nm4.MetricName == "rpc.sum_latency")
assert(nm4.MetricType == "summary")
assert(nm4.ServiceName == "sum-svc")
assert(nm4.Attributes == '{"rpc.method":"Charge"}')
assert(nm4.Value == nil)
assert(type(nm4.Summary) == "string")
assert(nm4.Summary == '{"count":50,"sum":1234.5,"quantileValues":[{"quantile":0.5,"value":12},{"quantile":0.99,"value":85}]}')
assert(nm4.Exemplars == "[]")

-- Also verify native exponential_histogram & summary in JSON mode
local native_exp_json = [[{"name":"exp_latency_json","kind":"delta","timestamp":"2026-10-22T10:02:00.000000000Z","tags":{"resource.service.name":"exp-json-svc"},"exponential_histogram":{"count":"42","sum":105.5,"min":0.5,"max":12.0,"scale":-1,"zero_count":"2","zero_threshold":0.0,"positive":{"offset":1,"bucket_counts":["5","15","20"]},"negative":{"offset":-2,"bucket_counts":["0"]}}}]]
local native_exp_json_events = {}
local n_ej, ej_err = vn.metrics.process(native_exp_json, function(ev) table.insert(native_exp_json_events, ev) end)
assert(n_ej == 1 and ej_err == nil)
assert_all_columns_primitive(native_exp_json_events[1].log, "native_exp_json_events[1].log")
assert(native_exp_json_events[1].log.MetricType == "exponential_histogram")
assert(native_exp_json_events[1].log.AggregationTemporality == 1)
assert(native_exp_json_events[1].log.ExponentialHistogram == '{"aggregationTemporality":1,"count":42,"sum":105.5,"min":0.5,"max":12,"scale":-1,"zeroCount":2,"zeroThreshold":0,"positive":{"offset":1,"bucketCounts":[5,15,20]},"negative":{"offset":-2,"bucketCounts":[0]}}')

-- Compare schema keys against vector_otel.metrics (sum, histogram, exponential_histogram, summary)
local otel_metric_events = {}
vo.metrics.process([[{"resourceMetrics":[{"resource":{},"scopeMetrics":[{"scope":{},"metrics":[
  {"name":"m1","sum":{"dataPoints":[{"timeUnixNano":"1792663215500000000","asDouble":42.5,"exemplars":[]}]}},
  {"name":"m2","histogram":{"dataPoints":[{"timeUnixNano":"1792663260000000000","count":"11","sum":450.5,"explicitBounds":[10,50,100],"bucketCounts":["1","3","5","2"],"exemplars":[]}]}},
  {"name":"m3","exponentialHistogram":{"aggregationTemporality":2,"dataPoints":[{"timeUnixNano":"1792663320000000000","count":"42","sum":105.5,"scale":3,"zeroCount":"2","positive":{"offset":-1,"bucketCounts":["5","15","20"]},"negative":{"offset":0,"bucketCounts":["0"]}}]}},
  {"name":"m4","summary":{"dataPoints":[{"timeUnixNano":"1792663380000000000","count":"50","sum":1234.5,"quantileValues":[{"quantile":0.5,"value":12},{"quantile":0.99,"value":85}]}]}}
]}]}]}]], function(ev)
  table.insert(otel_metric_events, ev)
end)
assert_same_keys(nm1, otel_metric_events[1].log, "Metrics Sum ClickHouse Schema Parity")
assert_same_keys(nm2, otel_metric_events[2].log, "Metrics Histogram ClickHouse Schema Parity")
assert_same_keys(nm3, otel_metric_events[3].log, "Metrics ExponentialHistogram ClickHouse Schema Parity")
assert_same_keys(nm4, otel_metric_events[4].log, "Metrics Summary ClickHouse Schema Parity")

-- Summary metric with structured quantiles
local summary_metric_events = {}
local n_sum, s_err = vn.metrics.process({
  metric = {
    name = "http_req_summary",
    timestamp = "2026-10-22T10:02:00.000000000Z",
    tags = { ["resource.service.name"] = "pay-svc" },
    summary = {
      count = 100,
      sum = 450.5,
      quantiles = {
        { quantile = 0.5, value = 1.5 },
        { quantile = 0.9, value = 4.2 },
        { quantile = 0.99, value = 9.8 },
      },
    },
  },
}, function(ev) table.insert(summary_metric_events, ev) end)
assert(n_sum == 1 and s_err == nil and #summary_metric_events == 1)
local sm = summary_metric_events[1].log
assert_all_columns_primitive(sm, "sm")
assert(sm.MetricType == "summary")
assert(type(sm.Summary) == "string")
assert(sm.Summary == '{"count":100,"sum":450.5,"quantileValues":[{"quantile":0.5,"value":1.5},{"quantile":0.9,"value":4.2},{"quantile":0.99,"value":9.8}]}')

print("   Native metrics extraction and schema parity passed.")

--------------------------------------------------------------------------------
-- 6. 50,000-Event Stress Loop & GC Stability Check (Dual-Mode: String & Table)
--------------------------------------------------------------------------------
print(">> Running 50,000-event stress loop and checking memory stability...")

local stress_count = 0
local function discard_emit(ev)
  stress_count = stress_count + 1
end

local stress_log_json = [[{"message":"stress-msg","timestamp":"2026-10-22T10:00:01.123456789Z","resources":{"service.name":"stress"},"attributes":{"iter":1}}]]
local stress_trace_json = [[{"trace_id":"5b8efff798038103d269b633813fc60c","span_id":"eee19b7ec3c1b174","name":"stress-op","kind":"SERVER","start_time_unix_nano":"2026-10-22T10:00:01.000000000Z","end_time_unix_nano":"2026-10-22T10:00:01.050000000Z","resources":{"service.name":"stress"}}]]
local stress_metric_json = [[{"name":"stress_m","timestamp":"2026-10-22T10:00:01.000000000Z","tags":{"resource.service.name":"stress","host":"h1"},"gauge":{"value":42}}]]

local stress_metric_table = {
  metric = {
    name = "stress_m",
    timestamp = "2026-10-22T10:00:01.000000000Z",
    tags = {
      ["resource.service.name"] = "stress",
      host = "h1",
    },
    gauge = { value = 42 },
  },
}

local stress_summary_table = {
  metric = {
    name = "stress_sum",
    timestamp = "2026-10-22T10:00:01.000000000Z",
    tags = { ["resource.service.name"] = "stress" },
    summary = {
      count = 10,
      sum = 25.0,
      quantiles = { { quantile = 0.5, value = 2.0 }, { quantile = 0.99, value = 5.0 } },
    },
  },
}

-- 1. Warm up Lua VM internal string pools, upvalues, and C++ thread-local buffers
for i = 1, 1000 do
  vn.logs.process(stress_log_json, discard_emit)
  vn.traces.process(stress_trace_json, discard_emit)
  vn.metrics.process(stress_metric_json, discard_emit)
  vn.logs.process(native_log_table, discard_emit)
  vn.traces.process(native_trace_table, discard_emit)
  vn.metrics.process(stress_metric_table, discard_emit)
  vn.metrics.process(stress_summary_table, discard_emit)
end

stress_count = 0

collectgarbage("collect")
collectgarbage("collect")
local mem_start = collectgarbage("count")

local target_events = 50000
local checkpoint_interval = 10000

-- 10,000 string logs + 10,000 table logs
for i = 1, 10000 do
  vn.logs.process(stress_log_json, discard_emit)
  if i % checkpoint_interval == 0 then
    local current_mem = collectgarbage("count")
    assert(current_mem < mem_start + 1024, "Unbounded memory growth during string logs: " .. current_mem .. " KB")
  end
end
for i = 1, 10000 do
  vn.logs.process(native_log_table, discard_emit)
  if i % checkpoint_interval == 0 then
    local current_mem = collectgarbage("count")
    assert(current_mem < mem_start + 1024, "Unbounded memory growth during table logs: " .. current_mem .. " KB")
  end
end

-- 10,000 string traces + 10,000 table traces
for i = 1, 10000 do
  vn.traces.process(stress_trace_json, discard_emit)
  if i % checkpoint_interval == 0 then
    local current_mem = collectgarbage("count")
    assert(current_mem < mem_start + 1024, "Unbounded memory growth during string traces: " .. current_mem .. " KB")
  end
end
for i = 1, 10000 do
  vn.traces.process(native_trace_table, discard_emit)
  if i % checkpoint_interval == 0 then
    local current_mem = collectgarbage("count")
    assert(current_mem < mem_start + 1024, "Unbounded memory growth during table traces: " .. current_mem .. " KB")
  end
end

-- 5,000 string metrics + 5,000 table metrics
for i = 1, 5000 do
  vn.metrics.process(stress_metric_json, discard_emit)
  if i % checkpoint_interval == 0 then
    local current_mem = collectgarbage("count")
    assert(current_mem < mem_start + 1024, "Unbounded memory growth during string metrics: " .. current_mem .. " KB")
  end
end
for i = 1, 2500 do
  vn.metrics.process(stress_metric_table, discard_emit)
end
for i = 1, 2500 do
  vn.metrics.process(stress_summary_table, discard_emit)
end

assert(stress_count == target_events, string.format("Expected %d emitted events, got %d", target_events, stress_count))

collectgarbage("collect")
collectgarbage("collect")
local mem_end = collectgarbage("count")
local mem_delta_kb = mem_end - mem_start

print(string.format("   Stress Loop: Processed %d events across native logs, traces, and metrics (String & Table modes).", stress_count))
print(string.format("   Memory Baseline: %.2f KB, Memory Final: %.2f KB, Memory Delta: %.2f KB", mem_start, mem_end, mem_delta_kb))

assert(mem_delta_kb <= 16.0, string.format("Memory leak detected! Delta is %.2f KB (must be <= 16 KB)", mem_delta_kb))

print("ALL NATIVE LUA TESTS PASSED (0 leaks)")
