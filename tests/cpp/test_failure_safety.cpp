// Process-level failure-safety tests: the real module, loaded into a state whose allocator is
// under test control, driven through process() exactly as Vector would.

#include <gtest/gtest.h>

#include <lua.hpp>
#include <string>
#include <thread>

#include "fault_new.hpp"
#include "process_fixtures.hpp"
#include "vector_otel/core/process_runner.hpp"
#include "vector_otel/extractors/logs_extractor.hpp"
#include "vector_otel/extractors/metrics_extractor.hpp"
#include "vector_otel/extractors/traces_extractor.hpp"
#include "vector_otel/native_extractors/native_logs_extractor.hpp"
#include "vector_otel/native_extractors/native_metrics_extractor.hpp"
#include "vector_otel/native_extractors/native_traces_extractor.hpp"

namespace vector_otel::core {
namespace {

using namespace fixtures;
using test::CppFaultMode;
using test::CppFaultScope;

// ---- payload factories ------------------------------------------------------------------------

std::string LogRecordJson(int i) {
  const std::string n = std::to_string(i);
  return "{\"timeUnixNano\":\"17000000000000000" + std::string(i < 10 ? "0" : "") + n +
         "\",\"severityText\":\"INFO\",\"body\":{\"stringValue\":\"msg-" + n +
         "\"},\"attributes\":[{\"key\":\"k\",\"value\":{\"stringValue\":\"v" + n + "\"}}]}";
}

std::string LogRecordLua(int i) {
  const std::string n = std::to_string(i);
  return "{timeUnixNano=\"17000000000000000" + std::string(i < 10 ? "0" : "") + n +
         "\",severityText=\"INFO\",body={stringValue=\"msg-" + n +
         "\"},attributes={{key=\"k\",value={stringValue=\"v" + n + "\"}}}}";
}

// `resources` resources, each with one scope holding `records` records.
std::string LogsJson(int resources, int records) {
  std::string out = "{\"resourceLogs\":[";
  for (int r = 0; r < resources; ++r) {
    if (r > 0) out += ',';
    out +=
        "{\"resource\":{\"attributes\":[{\"key\":\"service.name\",\"value\":{\"stringValue\":"
        "\"svc-" +
        std::to_string(r) + "\"}}]},\"scopeLogs\":[{\"scope\":{\"name\":\"scope-" +
        std::to_string(r) + "\"},\"logRecords\":[";
    for (int i = 0; i < records; ++i) {
      if (i > 0) out += ',';
      out += LogRecordJson(i);
    }
    out += "]}]}";
  }
  return out + "]}";
}

std::string LogsLua(int resources, int records) {
  std::string out = "return {resourceLogs={";
  for (int r = 0; r < resources; ++r) {
    out += "{resource={attributes={{key=\"service.name\",value={stringValue=\"svc-" +
           std::to_string(r) + "\"}}}},scopeLogs={{scope={name=\"scope-" + std::to_string(r) +
           "\"},logRecords={";
    for (int i = 0; i < records; ++i) {
      if (i > 0) out += ',';
      out += LogRecordLua(i);
    }
    out += "}}}},";
  }
  return out + "}}";
}

// ---- traces factories -------------------------------------------------------------------------

std::string SpanJson(int i) {
  const std::string n = std::to_string(i);
  return "{\"traceId\":\"0102030405060708090a0b0c0d0e0f10\",\"spanId\":\"0102030405060708\","
         "\"name\":\"span-" +
         n +
         "\",\"kind\":2,\"startTimeUnixNano\":\"1700000000000000001\","
         "\"endTimeUnixNano\":\"1700000000000000999\",\"status\":{\"code\":1,\"message\":\"ok\"},"
         "\"attributes\":[{\"key\":\"k\",\"value\":{\"stringValue\":\"v" +
         n +
         "\"}}],"
         "\"events\":[{\"timeUnixNano\":\"1700000000000000002\",\"name\":\"ev\",\"attributes\":"
         "[{\"key\":\"ek\",\"value\":{\"stringValue\":\"ev" +
         n +
         "\"}}]}],"
         "\"links\":[{\"traceId\":\"0102030405060708090a0b0c0d0e0f10\",\"spanId\":"
         "\"0102030405060708\",\"traceState\":\"ts\",\"attributes\":[{\"key\":\"lk\",\"value\":"
         "{\"stringValue\":\"l" +
         n + "\"}}]}]}";
}

std::string SpanLua(int i) {
  const std::string n = std::to_string(i);
  return "{traceId=\"0102030405060708090a0b0c0d0e0f10\",spanId=\"0102030405060708\",name=\"span-" +
         n +
         "\",kind=2,startTimeUnixNano=\"1700000000000000001\","
         "endTimeUnixNano=\"1700000000000000999\",status={code=1,message=\"ok\"},"
         "attributes={{key=\"k\",value={stringValue=\"v" +
         n +
         "\"}}},"
         "events={{timeUnixNano=\"1700000000000000002\",name=\"ev\",attributes="
         "{{key=\"ek\",value={stringValue=\"ev" +
         n +
         "\"}}}}},"
         "links={{traceId=\"0102030405060708090a0b0c0d0e0f10\",spanId=\"0102030405060708\","
         "traceState=\"ts\",attributes={{key=\"lk\",value={stringValue=\"l" +
         n + "\"}}}}}}";
}

std::string TracesJson(int resources, int records) {
  std::string out = "{\"resourceSpans\":[";
  for (int r = 0; r < resources; ++r) {
    if (r > 0) out += ',';
    out +=
        "{\"resource\":{\"attributes\":[{\"key\":\"service.name\",\"value\":{\"stringValue\":"
        "\"svc-" +
        std::to_string(r) + "\"}}]},\"scopeSpans\":[{\"scope\":{\"name\":\"scope-" +
        std::to_string(r) + "\"},\"spans\":[";
    for (int i = 0; i < records; ++i) {
      if (i > 0) out += ',';
      out += SpanJson(i);
    }
    out += "]}]}";
  }
  return out + "]}";
}

std::string TracesLua(int resources, int records) {
  std::string out = "return {resourceSpans={";
  for (int r = 0; r < resources; ++r) {
    out += "{resource={attributes={{key=\"service.name\",value={stringValue=\"svc-" +
           std::to_string(r) + "\"}}}},scopeSpans={{scope={name=\"scope-" + std::to_string(r) +
           "\"},spans={";
    for (int i = 0; i < records; ++i) {
      if (i > 0) out += ',';
      out += SpanLua(i);
    }
    out += "}}}},";
  }
  return out + "}}";
}

// ---- metrics factories ------------------------------------------------------------------------

// One gauge data point (a record) carrying attributes and an exemplar with filtered attributes.
std::string DataPointJson(int i) {
  const std::string n = std::to_string(i);
  return "{\"timeUnixNano\":\"1700000000000000" + std::string(i < 10 ? "00" : "0") + n +
         "\",\"asInt\":\"" + n +
         "\",\"attributes\":[{\"key\":\"k\",\"value\":{\"stringValue\":\"v" + n +
         "\"}}],\"exemplars\":[{\"timeUnixNano\":\"1700000000000000001\",\"asDouble\":2.5,"
         "\"traceId\":\"0102030405060708090a0b0c0d0e0f10\",\"spanId\":\"0102030405060708\","
         "\"filteredAttributes\":[{\"key\":\"fk\",\"value\":{\"stringValue\":\"f" +
         n + "\"}}]}]}";
}

std::string DataPointLua(int i) {
  const std::string n = std::to_string(i);
  return "{timeUnixNano=\"1700000000000000" + std::string(i < 10 ? "00" : "0") + n + "\",asInt=\"" +
         n + "\",attributes={{key=\"k\",value={stringValue=\"v" + n +
         "\"}}},exemplars={{timeUnixNano=\"1700000000000000001\",asDouble=2.5,"
         "traceId=\"0102030405060708090a0b0c0d0e0f10\",spanId=\"0102030405060708\","
         "filteredAttributes={{key=\"fk\",value={stringValue=\"f" +
         n + "\"}}}}}}";
}

// `resources` resources, each with one scope and one gauge metric of `records` data points.
std::string MetricsJson(int resources, int records) {
  std::string out = "{\"resourceMetrics\":[";
  for (int r = 0; r < resources; ++r) {
    if (r > 0) out += ',';
    out +=
        "{\"resource\":{\"attributes\":[{\"key\":\"service.name\",\"value\":{\"stringValue\":"
        "\"svc-" +
        std::to_string(r) + "\"}}]},\"scopeMetrics\":[{\"scope\":{\"name\":\"scope-" +
        std::to_string(r) +
        "\"},\"metrics\":[{\"name\":\"m\",\"unit\":\"1\",\"gauge\":"
        "{\"dataPoints\":[";
    for (int i = 0; i < records; ++i) {
      if (i > 0) out += ',';
      out += DataPointJson(i);
    }
    out += "]}}]}]}";
  }
  return out + "]}";
}

std::string MetricsLua(int resources, int records) {
  std::string out = "return {resourceMetrics={";
  for (int r = 0; r < resources; ++r) {
    out += "{resource={attributes={{key=\"service.name\",value={stringValue=\"svc-" +
           std::to_string(r) + "\"}}}},scopeMetrics={{scope={name=\"scope-" + std::to_string(r) +
           "\"},metrics={{name=\"m\",unit=\"1\",gauge={dataPoints={";
    for (int i = 0; i < records; ++i) {
      if (i > 0) out += ',';
      out += DataPointLua(i);
    }
    out += "}}}}}}},";
  }
  return out + "}}";
}

// ---- metrics, every data point type -----------------------------------------------------------

// Mechanical JSON -> Lua table literal: `"key":` -> `key=`, `[`/`]` -> `{`/`}`. The fixtures hold
// no escaped quotes or brackets inside strings.
std::string JsonToLua(const std::string& json) {
  std::string out = "return ";
  for (std::size_t i = 0; i < json.size(); ++i) {
    const char c = json[i];
    if (c == '"') {
      const std::size_t end = json.find('"', i + 1);
      const bool is_key = end + 1 < json.size() && json[end + 1] == ':';
      if (is_key) {
        out.append(json, i + 1, end - i - 1);
        out += '=';
        i = end + 1;
      } else {
        out.append(json, i, end - i + 1);
        i = end;
      }
    } else if (c == '[') {
      out += '{';
    } else if (c == ']') {
      out += '}';
    } else {
      out += c;
    }
  }
  return out;
}

constexpr int kMetricTypeCount = 5;

// The data point fields shared by every metric type, exemplar included (the JSON path omits an
// absent exemplar array while table mode writes an empty one).
std::string CommonPoint(int i) {
  const std::string n = std::to_string(i);
  return "\"timeUnixNano\":\"1700000000000000" + std::string(i < 10 ? "00" : "0") + n +
         "\",\"attributes\":[{\"key\":\"k\",\"value\":{\"stringValue\":\"v" + n +
         "\"}}],\"exemplars\":[{\"timeUnixNano\":\"1700000000000000001\",\"asInt\":\"7\","
         "\"traceId\":\"0102030405060708090a0b0c0d0e0f10\",\"spanId\":\"0102030405060708\","
         "\"filteredAttributes\":[{\"key\":\"fk\",\"value\":{\"stringValue\":\"f" +
         n + "\"}}]}]";
}

// Type 0..4: gauge, sum, histogram, exponential histogram, summary.
std::string AnyMetricJson(int i, int type) {
  const std::string common = CommonPoint(i);
  switch (type % kMetricTypeCount) {
    case 0:
      return "{\"name\":\"m" + std::to_string(i) + "\",\"gauge\":{\"dataPoints\":[{" + common +
             ",\"asDouble\":1.5}]}}";
    case 1:
      return "{\"name\":\"m" + std::to_string(i) +
             "\",\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,\"dataPoints\":[{" +
             common + ",\"asInt\":\"9\"}]}}";
    case 2:
      return "{\"name\":\"m" + std::to_string(i) +
             "\",\"histogram\":{\"aggregationTemporality\":1,\"dataPoints\":[{" + common +
             ",\"count\":\"6\",\"sum\":10.5,\"min\":1.0,\"max\":5.0,\"bucketCounts\":[1,2,3],"
             "\"explicitBounds\":[1.0,5.0]}]}}";
    case 3:
      return "{\"name\":\"m" + std::to_string(i) +
             "\",\"exponentialHistogram\":{\"aggregationTemporality\":2,\"dataPoints\":[{" +
             common +
             ",\"count\":\"9\",\"sum\":20.5,\"min\":0.5,\"max\":8.0,\"scale\":2,"
             "\"zeroCount\":\"1\",\"positive\":{\"offset\":3,\"bucketCounts\":[1,2,3]},"
             "\"negative\":{\"offset\":-1,\"bucketCounts\":[4,5]}}]}}";
    default:
      return "{\"name\":\"m" + std::to_string(i) + "\",\"summary\":{\"dataPoints\":[{" + common +
             ",\"count\":\"4\",\"sum\":12.5,\"quantileValues\":[{\"quantile\":0.5,\"value\":2.0},"
             "{\"quantile\":0.99,\"value\":4.0}]}]}}";
  }
}

// `resources` resources, each with one scope and `records` metrics of cycling type, one data
// point each. Resource r starts the cycle at type 2r, so even a 2 x 3 payload holds all five.
std::string AnyMetricsJson(int resources, int records) {
  std::string out = "{\"resourceMetrics\":[";
  for (int r = 0; r < resources; ++r) {
    if (r > 0) out += ',';
    out +=
        "{\"resource\":{\"attributes\":[{\"key\":\"service.name\",\"value\":{\"stringValue\":"
        "\"svc-" +
        std::to_string(r) + "\"}}]},\"scopeMetrics\":[{\"scope\":{\"name\":\"scope-" +
        std::to_string(r) + "\"},\"metrics\":[";
    for (int i = 0; i < records; ++i) {
      if (i > 0) out += ',';
      out += AnyMetricJson(i, i + 2 * r);
    }
    out += "]}]}";
  }
  return out + "]}";
}

std::string AnyMetricsLua(int resources, int records) {
  return JsonToLua(AnyMetricsJson(resources, records));
}

// ---- per-signal description -------------------------------------------------------------------

struct Sig {
  const char* name;
  std::string (*json)(int resources, int records);
  std::string (*lua)(int resources, int records);
  bool scope_attrs;                          // rows carry a shared ScopeAttributes table
  std::vector<std::string> expected_fields;  // normalised fragments of the first staged row
  std::vector<const char*> empty_json;
  std::vector<const char*> empty_lua;
  extractors::ExtractionResult (*compat_json)(lua_State*, std::string_view, int);
  extractors::ExtractionResult (*compat_table)(lua_State*, int, int);
  // Optional: true when a delivered row's dump holds everything its type requires.
  bool (*row_complete)(const std::string& dump);
  bool native = false;        // driven through vector_native instead of vector_otel
  bool shared_tables = true;  // rows of one resource/scope share their attribute tables
};

const Sig kLogsSig{
    "logs",
    LogsJson,
    LogsLua,
    true,
    {"\"Body\":\"msg-0\"", "\"ServiceName\":\"svc-0\"", "\"LogAttributes\":\"{\"k\":\"v0\"}\""},
    {"{}", "{\"resourceLogs\":[]}", "{\"resourceLogs\":[{}]}",
     "{\"resourceLogs\":[{\"scopeLogs\":[]}]}", "{\"resourceLogs\":[{\"scopeLogs\":[{}]}]}",
     "{\"resourceLogs\":[{\"scopeLogs\":[{\"logRecords\":[]}]}]}"},
    {"return {}", "return {resourceLogs={}}", "return {resourceLogs={{}}}",
     "return {resourceLogs={{scopeLogs={{}}}}}",
     "return {resourceLogs={{scopeLogs={{logRecords={}}}}}}"},
    extractors::ProcessLogs,
    extractors::ProcessLogsTable,
    nullptr};

const Sig kTracesSig{
    "traces",
    TracesJson,
    TracesLua,
    false,
    {"\"SpanName\":\"span-0\"", "\"ServiceName\":\"svc-0\"",
     "\"SpanAttributes\":\"{\"k\":\"v0\"}\"", "\"SpanKind\":\"SPAN_KIND_SERVER\"",
     "\"StatusCode\":\"STATUS_CODE_OK\"", "\"Duration\":998"},
    {"{}", "{\"resourceSpans\":[]}", "{\"resourceSpans\":[{}]}",
     "{\"resourceSpans\":[{\"scopeSpans\":[]}]}", "{\"resourceSpans\":[{\"scopeSpans\":[{}]}]}",
     "{\"resourceSpans\":[{\"scopeSpans\":[{\"spans\":[]}]}]}"},
    {"return {}", "return {resourceSpans={}}", "return {resourceSpans={{}}}",
     "return {resourceSpans={{scopeSpans={{}}}}}",
     "return {resourceSpans={{scopeSpans={{spans={}}}}}}"},
    extractors::ProcessTraces,
    extractors::ProcessTracesTable,
    nullptr};

const Sig kMetricsSig{
    "metrics",
    MetricsJson,
    MetricsLua,
    true,
    {"\"MetricName\":\"m\"", "\"MetricType\":\"gauge\"", "\"ServiceName\":\"svc-0\"",
     "\"MetricUnit\":\"1\"", "\"Attributes\":\"{\"k\":\"v0\"}\"",
     "\"FilteredAttributes\":{\"fk\":\"f0\"}"},
    {"{}", "{\"resourceMetrics\":[]}", "{\"resourceMetrics\":[{}]}",
     "{\"resourceMetrics\":[{\"scopeMetrics\":[]}]}",
     "{\"resourceMetrics\":[{\"scopeMetrics\":[{}]}]}",
     "{\"resourceMetrics\":[{\"scopeMetrics\":[{\"metrics\":[]}]}]}",
     "{\"resourceMetrics\":[{\"scopeMetrics\":[{\"metrics\":[{\"name\":\"m\"}]}]}]}",
     "{\"resourceMetrics\":[{\"scopeMetrics\":[{\"metrics\":[{\"gauge\":{\"dataPoints\":[]}}]}]}]"
     "}"},
    {"return {}", "return {resourceMetrics={}}", "return {resourceMetrics={{}}}",
     "return {resourceMetrics={{scopeMetrics={{}}}}}",
     "return {resourceMetrics={{scopeMetrics={{metrics={}}}}}}",
     "return {resourceMetrics={{scopeMetrics={{metrics={{name=\"m\"}}}}}}}",
     "return {resourceMetrics={{scopeMetrics={{metrics={{gauge={dataPoints={}}}}}}}}}"},
    extractors::ProcessMetrics,
    extractors::ProcessMetricsTable,
    nullptr};

// A delivered row must carry every shared array its type needs (an unanchored array would be a
// silently damaged row).
bool AnyMetricRowComplete(const std::string& dump) {
  auto has = [&](const char* needle) { return dump.find(needle) != std::string::npos; };
  if (has("\"MetricType\":\"histogram\"")) {
    return has("\"bucketCounts\":[1,2,3]") && has("\"explicitBounds\":[");
  }
  if (has("\"MetricType\":\"exponential_histogram\"")) {
    return has("\"bucketCounts\":[1,2,3]") && has("\"bucketCounts\":[4,5]");
  }
  if (has("\"MetricType\":\"summary\"")) return has("\"quantileValues\":[");
  return has("\"Exemplars\":\"[");
}

const Sig kAnyMetricsSig{
    "metrics",
    AnyMetricsJson,
    AnyMetricsLua,
    true,
    {"\"MetricName\":\"m0\"", "\"MetricType\":\"gauge\"", "\"ServiceName\":\"svc-0\"",
     "\"Attributes\":\"{\"k\":\"v0\"}\"", "\"FilteredAttributes\":{\"fk\":\"f0\"}"},
    {"{}", "{\"resourceMetrics\":[]}"},
    {"return {}", "return {resourceMetrics={}}"},
    extractors::ProcessMetrics,
    extractors::ProcessMetricsTable,
    AnyMetricRowComplete};

// ---- native logs ------------------------------------------------------------------------------

// Vector native log events are flat: `resources * records` independent records, each carrying its
// own resource, scope and attributes. The JSON form is an array of events, the table form a Lua
// array of the same.
std::string NativeLogRecordJson(int r, int i) {
  const std::string n = std::to_string(i);
  return "{\"timestamp\":\"2023-11-14T22:13:20.0000000" + std::string(i < 10 ? "0" : "") + n +
         "Z\",\"message\":\"msg-" + n +
         "\",\"severity_text\":\"INFO\",\"resources\":{"
         "\"service.name\":\"svc-" +
         std::to_string(r) + "\"},\"scope\":{\"name\":\"scope-" + std::to_string(r) +
         "\"},\"attributes\":{\"k\":\"v" + n + "\"}}";
}

std::string NativeLogRecordLua(int r, int i) {
  const std::string n = std::to_string(i);
  return "{timestamp=\"2023-11-14T22:13:20.0000000" + std::string(i < 10 ? "0" : "") + n +
         "Z\",message=\"msg-" + n +
         "\",severity_text=\"INFO\",resources={[\"service.name\"]="
         "\"svc-" +
         std::to_string(r) + "\"},scope={name=\"scope-" + std::to_string(r) +
         "\"},attributes={k=\"v" + n + "\"}}";
}

std::string NativeLogsJson(int resources, int records) {
  std::string out = "[";
  for (int r = 0; r < resources; ++r) {
    for (int i = 0; i < records; ++i) {
      if (r > 0 || i > 0) out += ',';
      out += NativeLogRecordJson(r, i);
    }
  }
  return out + "]";
}

std::string NativeLogsLua(int resources, int records) {
  std::string out = "return {";
  for (int r = 0; r < resources; ++r) {
    for (int i = 0; i < records; ++i) {
      out += NativeLogRecordLua(r, i) + ",";
    }
  }
  return out + "}";
}

const Sig kNativeLogsSig{
    "logs",
    NativeLogsJson,
    NativeLogsLua,
    true,
    {"\"Body\":\"msg-0\"", "\"ServiceName\":\"svc-0\"", "\"LogAttributes\":\"{\"k\":\"v0\"}\"",
     "\"ScopeName\":\"scope-0\"", "\"SeverityText\":\"INFO\""},
    {"{}", "[]", "[{}]", "{\"log\":{}}", "{\"log\":[]}", "{\"log\":[{}]}", "[{\"log\":{}}]"},
    {"return {}", "return {{}}", "return {log={}}", "return {log={{}}}", "return {{log={}}}",
     "return {{unrelated=1}}"},
    native_extractors::ProcessNativeLogs,
    native_extractors::ProcessNativeLogsTable,
    nullptr,
    /*native=*/true,
    /*shared_tables=*/false};

// ---- native traces ----------------------------------------------------------------------------

std::string NativeSpanJson(int r, int i) {
  const std::string n = std::to_string(i);
  return "{\"start_time_unix_nano\":\"2023-11-14T22:13:20.000000001Z\","
         "\"end_time_unix_nano\":\"2023-11-14T22:13:20.000000999Z\","
         "\"trace_id\":\"0102030405060708090a0b0c0d0e0f10\",\"span_id\":\"0102030405060708\","
         "\"name\":\"span-" +
         n +
         "\",\"kind\":2,\"status\":{\"code\":1,\"message\":\"fine\"},"
         "\"resources\":{\"service.name\":\"svc-" +
         std::to_string(r) + "\"},\"scope\":{\"name\":\"scope-" + std::to_string(r) +
         "\",\"version\":\"1.0\"},"
         "\"attributes\":{\"k\":\"v" +
         n +
         "\"},\"events\":[{\"time_unix_nano\":"
         "\"2023-11-14T22:13:20.000000500Z\",\"name\":\"ev\",\"attributes\":{\"ea\":\"eb\"}}],"
         "\"links\":[{\"trace_id\":\"aa\",\"span_id\":\"bb\",\"attributes\":{\"la\":\"lb\"}}]}";
}

std::string NativeSpanLua(int r, int i) {
  const std::string n = std::to_string(i);
  return "{start_time_unix_nano=\"2023-11-14T22:13:20.000000001Z\","
         "end_time_unix_nano=\"2023-11-14T22:13:20.000000999Z\","
         "trace_id=\"0102030405060708090a0b0c0d0e0f10\",span_id=\"0102030405060708\","
         "name=\"span-" +
         n +
         "\",kind=2,status={code=1,message=\"fine\"},"
         "resources={[\"service.name\"]=\"svc-" +
         std::to_string(r) + "\"},scope={name=\"scope-" + std::to_string(r) +
         "\",version=\"1.0\"},"
         "attributes={k=\"v" +
         n +
         "\"},events={{time_unix_nano="
         "\"2023-11-14T22:13:20.000000500Z\",name=\"ev\",attributes={ea=\"eb\"}}},"
         "links={{trace_id=\"aa\",span_id=\"bb\",attributes={la=\"lb\"}}}}";
}

std::string NativeTracesJson(int resources, int records) {
  std::string out = "[";
  for (int r = 0; r < resources; ++r) {
    for (int i = 0; i < records; ++i) {
      if (r > 0 || i > 0) out += ',';
      out += NativeSpanJson(r, i);
    }
  }
  return out + "]";
}

std::string NativeTracesLua(int resources, int records) {
  std::string out = "return {";
  for (int r = 0; r < resources; ++r) {
    for (int i = 0; i < records; ++i) out += NativeSpanLua(r, i) + ",";
  }
  return out + "}";
}

const Sig kNativeTracesSig{
    "traces",
    NativeTracesJson,
    NativeTracesLua,
    false,
    {"\"SpanName\":\"span-0\"", "\"ServiceName\":\"svc-0\"",
     "\"SpanAttributes\":\"{\"k\":\"v0\"}\"", "\"SpanKind\":\"SPAN_KIND_SERVER\"",
     "\"StatusCode\":\"STATUS_CODE_OK\"", "\"Duration\":998", "\"ScopeName\":\"scope-0\"",
     "\"StatusMessage\":\"fine\"", "\"Name\":\"ev\"", "\"SpanId\":\"bb\""},
    {"{}", "[]", "[{}]", "{\"log\":{}}", "{\"trace\":[]}", "{\"log\":[{}]}", "[{\"trace\":{}}]"},
    {"return {}", "return {{}}", "return {log={}}", "return {trace={{}}}", "return {{trace={}}}",
     "return {{unrelated=1}}"},
    native_extractors::ProcessNativeTraces,
    native_extractors::ProcessNativeTracesTable,
    nullptr,
    /*native=*/true,
    /*shared_tables=*/false};

// ---- native metrics ---------------------------------------------------------------------------

// Records cycle through gauge, counter, aggregated_histogram and aggregated_summary so every body
// type (and every shared array it anchors) is exercised by each sweep.
std::string NativeMetricBodyJson(int i) {
  switch (i % 4) {
    case 0:
      return "\"gauge\":{\"value\":1.5}";
    case 1:
      return "\"counter\":{\"value\":2}";
    case 2:
      return "\"aggregated_histogram\":{\"count\":6,\"sum\":10.5,\"explicitBounds\":[1.0,5.0],"
             "\"counts\":[1,2,3]}";
    default:
      return "\"aggregated_summary\":{\"count\":4,\"sum\":2.5,\"quantiles\":[{\"quantile\":0.5,"
             "\"value\":1.0},{\"quantile\":0.99,\"value\":2.0}]}";
  }
}

std::string NativeMetricBodyLua(int i) {
  switch (i % 4) {
    case 0:
      return "gauge={value=1.5}";
    case 1:
      return "counter={value=2}";
    case 2:
      return "aggregated_histogram={count=6,sum=10.5,explicitBounds={1.0,5.0},counts={1,2,3}}";
    default:
      return "aggregated_summary={count=4,sum=2.5,quantiles={{quantile=0.5,value=1.0},"
             "{quantile=0.99,value=2.0}}}";
  }
}

std::string NativeMetricRecordJson(int r, int i) {
  const std::string n = std::to_string(i);
  return "{\"name\":\"m-" + n +
         "\",\"namespace\":\"ns\",\"description\":\"d\",\"unit\":\"1\",\"kind\":\"incremental\","
         "\"timestamp\":\"2023-11-14T22:13:20.000000001Z\",\"tags\":{\"resource.service.name\":"
         "\"svc-" +
         std::to_string(r) + "\",\"scope.name\":\"scope-" + std::to_string(r) + "\",\"host\":\"h" +
         n + "\"}," + NativeMetricBodyJson(i) + "}";
}

std::string NativeMetricRecordLua(int r, int i) {
  const std::string n = std::to_string(i);
  return "{name=\"m-" + n +
         "\",namespace=\"ns\",description=\"d\",unit=\"1\",kind=\"incremental\","
         "timestamp=\"2023-11-14T22:13:20.000000001Z\",tags={[\"resource.service.name\"]=\"svc-" +
         std::to_string(r) + "\",[\"scope.name\"]=\"scope-" + std::to_string(r) + "\",host=\"h" +
         n + "\"}," + NativeMetricBodyLua(i) + "}";
}

std::string NativeMetricsJson(int resources, int records) {
  std::string out = "[";
  for (int r = 0; r < resources; ++r) {
    for (int i = 0; i < records; ++i) {
      if (r > 0 || i > 0) out += ',';
      out += NativeMetricRecordJson(r, i);
    }
  }
  return out + "]";
}

std::string NativeMetricsLua(int resources, int records) {
  std::string out = "return {";
  for (int r = 0; r < resources; ++r) {
    for (int i = 0; i < records; ++i) out += NativeMetricRecordLua(r, i) + ",";
  }
  return out + "}";
}

bool NativeMetricRowComplete(const std::string& dump) {
  auto has = [&](const char* needle) { return dump.find(needle) != std::string::npos; };
  if (!has("\"Exemplars\":") || !has("\"ScopeAttributes\":\"{") ||
      !has("\"ResourceAttributes\":\"{") || !has("\"Attributes\":\"{")) {
    return false;
  }
  if (has("\"MetricType\":\"histogram\"")) {
    return has("\"bucketCounts\":[1,2,3]") && has("\"explicitBounds\":[");
  }
  if (has("\"MetricType\":\"summary\"")) return has("\"quantileValues\":[");
  return true;
}

const Sig kNativeMetricsSig{
    "metrics",
    NativeMetricsJson,
    NativeMetricsLua,
    true,
    {"\"MetricName\":\"ns.m-0\"", "\"MetricType\":\"gauge\"", "\"ServiceName\":\"svc-0\"",
     "\"ScopeName\":\"scope-0\"", "\"Attributes\":\"{\"host\":\"h0\"}\"",
     "\"ResourceAttributes\":\"{\"service.name\":\"svc-0\"}\"", "\"MetricUnit\":\"1\""},
    {"{}", "[]", "[{}]", "{\"metric\":{}}", "{\"log\":[]}", "{\"log\":[{}]}", "[{\"metric\":{}}]",
     "{\"name\":\"m\"}"},
    {"return {}", "return {{}}", "return {metric={}}", "return {log={{}}}", "return {{metric={}}}",
     "return {{unrelated=1}}", "return {name=\"m\"}"},
    native_extractors::ProcessNativeMetrics,
    native_extractors::ProcessNativeMetricsTable,
    NativeMetricRowComplete,
    /*native=*/true,
    /*shared_tables=*/false};

const Sig* g_sig = &kLogsSig;  // the signal under test (set by the fixture)

class FailureSafetyOtelTest : public testing::TestWithParam<const Sig*> {
 protected:
  void SetUp() override { g_sig = GetParam(); }
};

Payload PayloadFor(bool table, int resources, int records) {
  return table ? TablePayload(g_sig->lua(resources, records))
               : JsonPayload(g_sig->json(resources, records));
}

ModuleKind SigModule() { return g_sig->native ? ModuleKind::Native : ModuleKind::Otel; }

void ExpectAnchorsReleased(lua_State* L) {
  for (int i = 0; i < kInvocationSlotCount; ++i) {
    EXPECT_EQ(AnchorSlotType(L, SigModule(), static_cast<AnchorSlot>(i)), LUA_TBOOLEAN)
        << "anchor slot " << i << " still holds a value";
  }
}

void ExpectRowsComplete(const ProcRecorder& rec) {
  if (g_sig->row_complete == nullptr) return;
  for (const std::string& event : rec.events) {
    EXPECT_TRUE(g_sig->row_complete(event)) << "a delivered row is missing data: " << event;
  }
}

int CeilDiv(int a, int b) { return (a + b - 1) / b; }

// ---- behaviour --------------------------------------------------------------------------------

TEST_P(FailureSafetyOtelTest, RowsOfOneResourceCarryIdenticalAttributeJson) {
  if (!g_sig->shared_tables) GTEST_SKIP() << "native rows derive attributes per record";
  for (bool table : {false, true}) {
    SCOPED_TRACE(table ? "table input" : "json input");
    RawState s;
    ProcRecorder rec;
    rec.dump = true;
    const ProcResult r = CallProcess(s, g_sig->native, g_sig->name, PayloadFor(table, 2, 3), rec);
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.nresults, 2);
    EXPECT_TRUE(r.first_nil == false);
    EXPECT_EQ(r.count, 6);
    EXPECT_EQ(rec.rows, 6);
    ASSERT_EQ(rec.resource_json.size(), 6u);
    for (int i = 0; i < 6; ++i) {
      EXPECT_FALSE(rec.resource_json[i].empty());
      if (g_sig->scope_attrs) EXPECT_FALSE(rec.scope_json[i].empty());
    }
    for (int i = 1; i < 3; ++i) {
      EXPECT_EQ(rec.resource_json[0], rec.resource_json[i]);
      if (g_sig->scope_attrs) EXPECT_EQ(rec.scope_json[0], rec.scope_json[i]);
      EXPECT_EQ(rec.resource_json[3], rec.resource_json[3 + i]);
    }
    EXPECT_NE(rec.resource_json[0], rec.resource_json[3]) << "distinct resources must differ";
    ExpectAnchorsReleased(s.L());
  }
}

// processed_at is the wall clock; legacy table mode omits an absent EventName while JSON mode
// writes "" (behaviour preserved from before the staging rewrite). The same applies to an absent
// traceState on spans.
std::string Normalized(std::string text) {
  for (std::size_t p; (p = text.find(",\"processed_at\":\"")) != std::string::npos;) {
    text.erase(p, text.find('"', p + 17) + 1 - p);
  }
  const std::string empty_name = "\"EventName\":\"\",";
  for (std::size_t e; (e = text.find(empty_name)) != std::string::npos;) {
    text.erase(e, empty_name.size());
  }
  const std::string empty_state = ",\"TraceState\":\"\"";
  for (std::size_t e; (e = text.find(empty_state)) != std::string::npos;) {
    text.erase(e, empty_state.size());
  }
  return text;
}

TEST_P(FailureSafetyOtelTest, JsonAndTableInputsStageIdenticalRows) {
  RawState a;
  RawState b;
  ProcRecorder ja;
  ProcRecorder tb;
  ja.dump = tb.dump = true;
  ASSERT_EQ(CallProcess(a, g_sig->native, g_sig->name, PayloadFor(false, 2, 2), ja).count, 4);
  ASSERT_EQ(CallProcess(b, g_sig->native, g_sig->name, PayloadFor(true, 2, 2), tb).count, 4);
  ASSERT_EQ(ja.events.size(), tb.events.size());
  for (std::size_t i = 0; i < ja.events.size(); ++i) {
    EXPECT_EQ(Normalized(ja.events[i]), Normalized(tb.events[i])) << "event " << i;
  }
  ASSERT_FALSE(ja.events.empty());
  for (const std::string& fragment : g_sig->expected_fields) {
    EXPECT_NE(ja.events[0].find(fragment), std::string::npos) << fragment << " in " << ja.events[0];
  }
}

TEST_P(FailureSafetyOtelTest, EmptyAndSparseInputsAreNotErrors) {
  RawState s;
  for (const char* j : g_sig->empty_json) {
    SCOPED_TRACE(j);
    ProcRecorder rec;
    const ProcResult r = CallProcess(s, g_sig->native, g_sig->name, JsonPayload(j), rec);
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.nresults, 2);
    EXPECT_FALSE(r.first_nil);
    EXPECT_EQ(r.count, 0);
    EXPECT_EQ(rec.calls, 0);
  }
  for (const char* t : g_sig->empty_lua) {
    SCOPED_TRACE(t);
    ProcRecorder rec;
    const ProcResult r = CallProcess(s, g_sig->native, g_sig->name, TablePayload(t), rec);
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.nresults, 2);
    EXPECT_FALSE(r.first_nil);
    EXPECT_EQ(r.count, 0);
    EXPECT_EQ(rec.calls, 0);
  }
  ExpectAnchorsReleased(s.L());
}

TEST_P(FailureSafetyOtelTest, ArgumentErrorsUseTheExactMessages) {
  RawState s;
  ProcRecorder rec;
  Payload none;
  none.kind = PayloadKind::kRaw;
  Payload number = none;
  number.raw_type = LUA_TNUMBER;
  for (const Payload& p : {none, number}) {
    const ProcResult r = CallProcess(s, g_sig->native, g_sig->name, p, rec);
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.nresults, 3);
    EXPECT_TRUE(r.first_nil);
    EXPECT_EQ(r.error, "payload must be a string or table");
    EXPECT_EQ(r.count, 0);
  }
  const ProcResult r = CallProcess(
      s, g_sig->native, g_sig->name, JsonPayload("{}"), rec, [](auto&& run) { run(); },
      /*pass_emit=*/false);
  ASSERT_EQ(r.status, LUA_OK);
  ASSERT_EQ(r.nresults, 3);
  EXPECT_TRUE(r.first_nil);
  EXPECT_EQ(r.error, "emit callback must be a function");
  EXPECT_EQ(r.count, 0);
  EXPECT_EQ(rec.calls, 0);
}

TEST_P(FailureSafetyOtelTest, InvalidJsonIsAnErrorWithNothingEmitted) {
  RawState s;
  ProcRecorder rec;
  const ProcResult r = CallProcess(s, g_sig->native, g_sig->name, JsonPayload("not json"), rec);
  ASSERT_EQ(r.status, LUA_OK);
  ASSERT_EQ(r.nresults, 3);
  EXPECT_TRUE(r.first_nil);
  EXPECT_FALSE(r.error.empty());
  EXPECT_EQ(r.count, 0);
  EXPECT_EQ(rec.calls, 0);
  ExpectAnchorsReleased(s.L());
}

TEST_P(FailureSafetyOtelTest, RecordLimitIsEnforcedBeforeAnyEmission) {
  char limit_buf[96];
  const std::size_t limit_len = FormatRecordLimitText(limit_buf, sizeof(limit_buf));
  const std::string_view expected_limit(limit_buf, limit_len);

  RawState s;
  ProcRecorder rec;
  const ProcResult ok =
      CallProcess(s, g_sig->native, g_sig->name, PayloadFor(false, 1, kMaxIncomingRecords), rec);
  ASSERT_EQ(ok.nresults, 2);
  EXPECT_EQ(ok.count, kMaxIncomingRecords);
  ProcRecorder over;
  const ProcResult r = CallProcess(s, g_sig->native, g_sig->name,
                                   PayloadFor(false, 1, kMaxIncomingRecords + 1), over);
  ASSERT_EQ(r.status, LUA_OK);
  ASSERT_EQ(r.nresults, 3);
  EXPECT_TRUE(r.first_nil);
  EXPECT_EQ(r.error, expected_limit);
  EXPECT_EQ(r.count, 0);
  EXPECT_EQ(over.calls, 0);
  ProcRecorder over_t;
  const ProcResult rt = CallProcess(s, g_sig->native, g_sig->name,
                                    PayloadFor(true, 1, kMaxIncomingRecords + 1), over_t);
  ASSERT_EQ(rt.nresults, 3);
  EXPECT_EQ(rt.error, expected_limit);
  EXPECT_EQ(over_t.calls, 0);
}

TEST_P(FailureSafetyOtelTest, CallbackFailureReportsRowsFromCompletedCallbacks) {
  for (bool table : {false, true}) {
    SCOPED_TRACE(table ? "table input" : "json input");
    RawState s;
    ProcRecorder rec;
    rec.fail_at_call = 1;  // the second delivery raises
    const ProcResult r = CallProcess(s, g_sig->native, g_sig->name, PayloadFor(table, 1, 5), rec);
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.nresults, 3);
    EXPECT_TRUE(r.first_nil);
    EXPECT_NE(r.error.find("boom"), std::string::npos) << r.error;
    EXPECT_EQ(r.count, 1) << "only rows of completed callbacks are counted";
    EXPECT_EQ(rec.rows, 1);
    ExpectAnchorsReleased(s.L());
  }
}

// A failed delivery - including one raising a non-string value - reports exactly the rows of
// completed callbacks, and the same state then serves empty, over-limit and ordinary calls
// correctly.
TEST_P(FailureSafetyOtelTest, StateRecoversAfterDeliveryFailures) {
  for (bool table : {false, true}) {
    for (bool non_string : {false, true}) {
      SCOPED_TRACE(std::string(table ? "table" : "json") +
                   (non_string ? " non-string error" : " string error"));
      RawState s;
      ProcRecorder bad;
      bad.fail_at_call = 1;  // the second delivery raises
      bad.fail_with_table = non_string;
      const ProcResult r = CallProcess(s, g_sig->native, g_sig->name, PayloadFor(table, 1, 2), bad);
      ASSERT_EQ(r.status, LUA_OK);
      ASSERT_EQ(r.nresults, 3);
      EXPECT_TRUE(r.first_nil);
      EXPECT_FALSE(r.error.empty());
      EXPECT_EQ(r.count, 1);
      EXPECT_EQ(bad.rows, 1);
      ExpectAnchorsReleased(s.L());

      ProcRecorder empty;
      const std::vector<const char*>& empties = table ? g_sig->empty_lua : g_sig->empty_json;
      const ProcResult re =
          CallProcess(s, g_sig->native, g_sig->name,
                      table ? TablePayload(empties.front()) : JsonPayload(empties.front()), empty);
      ASSERT_EQ(re.nresults, 2) << re.error;
      EXPECT_EQ(re.count, 0);
      EXPECT_EQ(empty.calls, 0);

      char limit_buf[96];
      const std::size_t limit_len = FormatRecordLimitText(limit_buf, sizeof(limit_buf));
      const std::string_view expected_limit(limit_buf, limit_len);

      ProcRecorder over;
      const ProcResult ro = CallProcess(s, g_sig->native, g_sig->name,
                                        PayloadFor(table, 1, kMaxIncomingRecords + 1), over);
      ASSERT_EQ(ro.nresults, 3);
      EXPECT_EQ(ro.error, expected_limit);
      EXPECT_EQ(ro.count, 0);
      EXPECT_EQ(over.calls, 0);

      ProcRecorder good;
      const ProcResult rg =
          CallProcess(s, g_sig->native, g_sig->name, PayloadFor(table, 2, 3), good);
      ASSERT_EQ(rg.nresults, 2) << rg.error;
      EXPECT_EQ(rg.count, 6);
      EXPECT_EQ(good.rows, 6);
      lua_gc(s.L(), LUA_GCCOLLECT);
      ExpectAnchorsReleased(s.L());
    }
  }
}

TEST_P(FailureSafetyOtelTest, ReentrantProcessIsRejectedWithoutDisturbingTheOuterCall) {
  RawState s;
  ProcRecorder rec;
  rec.reenter = true;
  const ProcResult r = CallProcess(s, g_sig->native, g_sig->name, PayloadFor(false, 1, 3), rec);
  ASSERT_EQ(r.status, LUA_OK);
  ASSERT_EQ(r.nresults, 2);
  EXPECT_EQ(r.count, 3);
  EXPECT_EQ(rec.reenter_result, 1) << "inner call must return nil first";
  EXPECT_EQ(rec.reenter_message, "re-entrant vector_otel.process() invocation is not permitted");
  ExpectAnchorsReleased(s.L());
}

TEST_P(FailureSafetyOtelTest, DryRunCountsWithoutEmitting) {
  const std::string json = g_sig->json(2, 3);
  const extractors::ExtractionResult r = g_sig->compat_json(nullptr, json, 0);
  EXPECT_TRUE(r.success) << r.error;
  EXPECT_EQ(r.emitted_count, 6);
}

// ---- allocator sweeps -------------------------------------------------------------------------

// Lua denies every growth request from index `skip` on. Whatever happens: nothing escapes the
// closure, the returned count equals rows in completed callbacks, every anchor (resource, scope,
// row, event, batch, input, emit) is released, and the same state serves a correct call after a
// recovery collection.
void SweepLuaAllocations(bool table, int resources, int records) {
  const int total = resources * records;
  bool saw_failure = false;
  bool saw_success = false;
  for (std::size_t skip = 0; skip < 1500 && !saw_success; ++skip) {
    SCOPED_TRACE("persistent Lua denial from growth request " + std::to_string(skip));
    RawState s;
    ASSERT_TRUE(EnsureModuleState(s.L(), SigModule()));
    lua_pop(s.L(), 1);
    ProcRecorder rec;
    rec.dump = true;
    const Payload payload = PayloadFor(table, resources, records);
    const ProcResult r = CallProcess(s, g_sig->native, g_sig->name, payload, rec, [&](auto&& run) {
      s.alloc().SetPlan(LuaFaultMode::kPersistent, skip);
      run();
      s.alloc().ClearPlan();
    });
    ASSERT_EQ(r.status, LUA_OK) << "an error escaped the dispatcher frame";
    ExpectAnchorsReleased(s.L());
    ExpectRowsComplete(rec);
    for (const std::string& json : rec.resource_json) {
      EXPECT_FALSE(json.empty()) << "a delivered row lost its ResourceAttributes JSON string";
    }
    for (const std::string& json : rec.scope_json) {
      if (!g_sig->scope_attrs) break;
      EXPECT_FALSE(json.empty()) << "a delivered row lost its ScopeAttributes JSON string";
    }
    if (r.nresults == 3) {
      saw_failure = true;
      EXPECT_TRUE(r.first_nil);
      EXPECT_EQ(r.count, rec.rows) << "count must equal rows in completed callbacks";
      EXPECT_FALSE(r.error.empty());
    } else {
      ASSERT_EQ(r.nresults, 2);
      EXPECT_EQ(r.count, total);
      EXPECT_EQ(rec.rows, total);
      saw_success = true;
    }
    lua_gc(s.L(), LUA_GCCOLLECT);
    ProcRecorder again;
    const ProcResult ok = CallProcess(s, g_sig->native, g_sig->name, payload, again);
    ASSERT_EQ(ok.status, LUA_OK);
    ASSERT_EQ(ok.nresults, 2);
    EXPECT_EQ(ok.count, total);
    EXPECT_EQ(again.rows, total);
  }
  EXPECT_TRUE(saw_failure);
  EXPECT_TRUE(saw_success);
}

TEST_P(FailureSafetyOtelTest, SameStateOomReleasesResourceScopeAndRowJson) {
  SweepLuaAllocations(false, 2, 3);
}

TEST_P(FailureSafetyOtelTest, SameStateOomReleasesResourceScopeAndRowTable) {
  SweepLuaAllocations(true, 2, 3);
}

// Transient denial: exactly two consecutive growth requests (so Lua's emergency-GC retry cannot
// absorb it) fail, then the allocator recovers. An extractor that ignores a failed step would keep
// going and deliver a damaged row, which a persistent denial can never reveal.
void SweepLuaWindows(bool table) {
  const int resources = 2;
  const int records = 3;
  const int total = resources * records;
  bool saw_failure = false;
  for (std::size_t skip = 0; skip < 3000; ++skip) {
    SCOPED_TRACE("two-request Lua denial at growth request " + std::to_string(skip));
    RawState s;
    ASSERT_TRUE(EnsureModuleState(s.L(), SigModule()));
    lua_pop(s.L(), 1);
    ProcRecorder rec;
    rec.dump = true;
    const Payload payload = PayloadFor(table, resources, records);
    std::size_t attempts = 0;
    const ProcResult r = CallProcess(s, g_sig->native, g_sig->name, payload, rec, [&](auto&& run) {
      s.alloc().SetPlan(LuaFaultMode::kWindow, skip, 2);
      run();
      attempts = s.alloc().stats().attempts;
      s.alloc().ClearPlan();
    });
    ASSERT_EQ(r.status, LUA_OK) << "an error escaped the dispatcher frame";
    ExpectAnchorsReleased(s.L());
    ExpectRowsComplete(rec);
    for (const std::string& json : rec.resource_json) {
      EXPECT_FALSE(json.empty()) << "a delivered row lost its ResourceAttributes JSON string";
    }
    for (const std::string& json : rec.scope_json) {
      if (!g_sig->scope_attrs) break;
      EXPECT_FALSE(json.empty()) << "a delivered row lost its ScopeAttributes JSON string";
    }
    if (r.nresults == 3) {
      saw_failure = true;
      EXPECT_TRUE(r.first_nil);
      EXPECT_EQ(r.count, rec.rows);
    } else {
      ASSERT_EQ(r.nresults, 2);
      EXPECT_EQ(r.count, total) << "a transient failure must never be reported as success";
      EXPECT_EQ(rec.rows, total);
    }
    if (attempts <= skip) break;  // the window lies beyond the end of the call
  }
  EXPECT_TRUE(saw_failure);
}

TEST_P(FailureSafetyOtelTest, TransientOomNeverDeliversADamagedRowJson) { SweepLuaWindows(false); }

TEST_P(FailureSafetyOtelTest, TransientOomNeverDeliversADamagedRowTable) { SweepLuaWindows(true); }

// Embedders and the compatibility wrappers run extractors on an arbitrary stack: whatever the
// outcome, the caller's stack depth must be exactly what it was.
int RaisingEmit(lua_State* L) {
  lua_pushliteral(L, "boom");
  return lua_error(L);
}

TEST_P(FailureSafetyOtelTest, CompatCallsRestoreTheCallersStackOnFailure) {
  RawState s;
  lua_State* L = s.L();
  ASSERT_EQ(g_sig->native ? luaopen_vector_native(L) : luaopen_vector_otel(L), 1);
  lua_pop(L, 1);
  for (bool table : {false, true}) {
    SCOPED_TRACE(table ? "table input" : "json input");
    if (table) {
      const std::string chunk = g_sig->lua(2, 3);
      ASSERT_EQ(luaL_loadstring(L, chunk.c_str()), LUA_OK);
      ASSERT_EQ(lua_pcall(L, 0, 1, 0), LUA_OK);
    } else {
      lua_pushstring(L, g_sig->json(2, 3).c_str());
    }
    lua_pushcfunction(L, RaisingEmit);
    const int top = lua_gettop(L);
    const extractors::ExtractionResult r =
        table ? g_sig->compat_table(L, top - 1, top)
              : g_sig->compat_json(L, lua_tostring(L, top - 1), top);
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.error.find("boom"), std::string::npos) << r.error;
    EXPECT_EQ(lua_gettop(L), top) << "the extractor leaked or dropped stack slots";
    lua_settop(L, 0);
  }
}

// Runs on a thread whose thread_local scratch is cold, so the extraction really allocates from
// the C++ heap; every C++ allocation from index `skip` on is then refused.
struct CppSweepResult {
  bool ran = false;
  int status = LUA_OK;
  int nresults = 0;
  bool first_nil = false;
  lua_Integer count = -1;
  std::string error;
  int rows = 0;
  int calls = 0;
  int total = 0;
};

CppSweepResult RunColdThread(bool table, std::size_t skip, int resources, int records) {
  CppSweepResult out;
  std::thread worker([&] {
    RawState s;
    if (!EnsureModuleState(s.L(), SigModule())) return;
    lua_pop(s.L(), 1);
    ProcRecorder rec;
    const Payload payload = PayloadFor(table, resources, records);
    const ProcResult r = CallProcess(s, g_sig->native, g_sig->name, payload, rec, [&](auto&& run) {
      CppFaultScope scope(CppFaultMode::kPersistent, skip);
      run();
    });
    out.ran = true;
    out.status = r.status;
    out.nresults = r.nresults;
    out.first_nil = r.first_nil;
    out.count = r.count;
    out.error = r.error;
    out.rows = rec.rows;
    out.calls = rec.calls;
    out.total = resources * records;
  });
  worker.join();
  return out;
}

void SweepCppAllocations(bool table) {
  bool saw_failure = false;
  bool saw_success = false;
  for (std::size_t skip = 0; skip < 400 && !saw_success; ++skip) {
    SCOPED_TRACE("persistent C++ denial from allocation " + std::to_string(skip));
    const CppSweepResult r = RunColdThread(table, skip, 2, 3);
    ASSERT_TRUE(r.ran);
    ASSERT_EQ(r.status, LUA_OK) << "an error escaped the dispatcher frame";
    if (r.nresults == 3) {
      saw_failure = true;
      EXPECT_TRUE(r.first_nil);
      EXPECT_EQ(r.count, r.rows) << "count must equal rows in completed callbacks";
      EXPECT_FALSE(r.error.empty());
    } else {
      ASSERT_EQ(r.nresults, 2);
      EXPECT_EQ(r.count, r.total);
      saw_success = true;
    }
  }
  EXPECT_TRUE(saw_failure);
  EXPECT_TRUE(saw_success);
}

TEST_P(FailureSafetyOtelTest, PersistentCppOomPreservesCompletedRowsJson) {
  SweepCppAllocations(false);
}

TEST_P(FailureSafetyOtelTest, PersistentCppOomPreservesCompletedRowsTable) {
  SweepCppAllocations(true);
}

// After warm-up the extraction loop must not touch the C++ heap at all: refuse every allocation
// from the first one and the whole call still succeeds.
TEST_P(FailureSafetyOtelTest, WarmExtractionNeverAllocatesFromTheCppHeap) {
  for (bool table : {false, true}) {
    SCOPED_TRACE(table ? "table input" : "json input");
    RawState s;
    const Payload payload = PayloadFor(table, 2, 3);
    ProcRecorder warm;
    ASSERT_EQ(CallProcess(s, g_sig->native, g_sig->name, payload, warm).count, 6);
    ProcRecorder rec;
    std::size_t attempts = 0;
    const ProcResult r = CallProcess(s, g_sig->native, g_sig->name, payload, rec, [&](auto&& run) {
      CppFaultScope scope(CppFaultMode::kPersistent, 0);
      run();
      attempts = scope.stats().attempts;
    });
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.nresults, 2) << r.error;
    EXPECT_EQ(r.count, 6);
    EXPECT_EQ(attempts, 0u) << "C++ heap allocations on the warm path";
  }
}

// Spans without events or links still carry the JSON array strings the schema expects.
TEST(FailureSafetyTraces, SpansWithoutEventsOrLinksEmitEmptyArrays) {
  const char* kJson =
      R"({"resourceSpans":[{"scopeSpans":[{"spans":[{"traceId":"0102030405060708090a0b0c0d0e0f10",)"
      R"("spanId":"0102030405060708","name":"bare","startTimeUnixNano":"1","endTimeUnixNano":"2"}]}]}]})";
  const char* kLua =
      "return {resourceSpans={{scopeSpans={{spans={{traceId=\"0102030405060708090a0b0c0d0e0f10\","
      "spanId=\"0102030405060708\",name=\"bare\",startTimeUnixNano=\"1\","
      "endTimeUnixNano=\"2\"}}}}}}}";
  for (const bool table : {false, true}) {
    SCOPED_TRACE(table ? "table" : "json");
    RawState s;
    ProcRecorder rec;
    rec.dump = true;
    const ProcResult r =
        CallProcess(s, false, "traces", table ? TablePayload(kLua) : JsonPayload(kJson), rec);
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.count, 1) << r.error;
    ASSERT_EQ(rec.events.size(), 1u);
    EXPECT_NE(rec.events[0].find("\"Events\":\"[]\""), std::string::npos) << rec.events[0];
    EXPECT_NE(rec.events[0].find("\"Links\":\"[]\""), std::string::npos) << rec.events[0];
  }
}

// The record limit counts data points: not metrics, not bucket or quantile entries.
std::string GaugeMetricJson(int points) {
  std::string out = "{\"name\":\"g\",\"gauge\":{\"dataPoints\":[";
  for (int i = 0; i < points; ++i) {
    if (i > 0) out += ',';
    out += "{\"asInt\":\"1\"}";
  }
  return out + "]}}";
}

std::string MetricsSplitJson(const std::vector<std::vector<int>>& resources) {
  std::string out = "{\"resourceMetrics\":[";
  for (std::size_t r = 0; r < resources.size(); ++r) {
    if (r > 0) out += ',';
    out += "{\"scopeMetrics\":[";
    for (std::size_t sc = 0; sc < 2; ++sc) {
      if (sc > 0) out += ',';
      out += "{\"metrics\":[" + GaugeMetricJson(resources[r][sc]) + "]}";
    }
    out += "]}";
  }
  return out + "]}";
}

TEST(FailureSafetyMetrics, DataPointLimitCountsAcrossResourcesScopesAndMetrics) {
  char limit_buf[96];
  const std::size_t limit_len = FormatRecordLimitText(limit_buf, sizeof(limit_buf));
  const std::string_view expected_limit(limit_buf, limit_len);

  const int half = static_cast<int>(kMaxIncomingRecords / 2);
  const int other_half = static_cast<int>(kMaxIncomingRecords - half);
  const int prev = static_cast<int>(kMaxIncomingRecords > 1 ? kMaxIncomingRecords - 1 : 0);
  const int max_rec = static_cast<int>(kMaxIncomingRecords);

  struct Case {
    std::vector<std::vector<int>> split;
    bool ok;
  };
  const Case cases[] = {
      {{{half, other_half}}, true},              // limit in one resource, two scopes
      {{{half, other_half - 1}, {1, 0}}, true},  // limit over two resources
      {{{half, other_half}, {1, 0}}, false},     // limit + 1 over two resources
      {{{prev, 0}}, true},                       // limit - 1
      {{{max_rec, 1}}, false},                   // limit + 1 over two scopes
  };
  for (const Case& c : cases) {
    RawState s;
    ProcRecorder rec;
    const ProcResult r =
        CallProcess(s, false, "metrics", JsonPayload(MetricsSplitJson(c.split)), rec);
    ASSERT_EQ(r.status, LUA_OK);
    if (c.ok) {
      ASSERT_EQ(r.nresults, 2) << r.error;
      EXPECT_GE(r.count, prev);
    } else {
      ASSERT_EQ(r.nresults, 3);
      EXPECT_TRUE(r.first_nil);
      EXPECT_EQ(r.error, expected_limit);
      EXPECT_EQ(rec.calls, 0) << "nothing may be emitted once the limit is exceeded";
    }
  }
}

TEST(FailureSafetyMetrics, LargeBucketAndQuantileArraysAreNotRecords) {
  std::string counts;
  std::string bounds;
  std::string quantiles;
  for (int i = 0; i < 5000; ++i) {
    if (i > 0) {
      counts += ',';
      bounds += ',';
      quantiles += ',';
    }
    counts += "1";
    bounds += std::to_string(i) + ".5";
    quantiles += "{\"quantile\":0.5,\"value\":1.0}";
  }
  const std::string json =
      "{\"resourceMetrics\":[{\"scopeMetrics\":[{\"metrics\":[{\"name\":\"h\",\"histogram\":{"
      "\"dataPoints\":[{\"count\":\"5000\",\"bucketCounts\":[" +
      counts + "],\"explicitBounds\":[" + bounds +
      "]}]}},{\"name\":\"s\",\"summary\":{\"dataPoints\":[{\"count\":\"1\",\"quantileValues\":[" +
      quantiles + "]}]}}]}]}]}";
  RawState s;
  ProcRecorder rec;
  const ProcResult r = CallProcess(s, false, "metrics", JsonPayload(json), rec);
  ASSERT_EQ(r.status, LUA_OK);
  ASSERT_EQ(r.nresults, 2) << r.error;
  EXPECT_EQ(r.count, 2);
  EXPECT_EQ(rec.rows, 2);
}

// One dump per delivered row: a batched event ({"log":{"records":[row,...]}}) is split into its
// rows (the fixtures hold no braces inside strings), a single-row event is its own row.
std::vector<std::string> RowDumps(const std::vector<std::string>& events) {
  std::vector<std::string> rows;
  for (const std::string& event : events) {
    const std::size_t at = event.find("\"records\":[");
    if (at == std::string::npos) {
      rows.push_back(event);
      continue;
    }
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = at + 11; i < event.size() && !(depth == 0 && event[i] == ']'); ++i) {
      if (event[i] == '{') {
        if (depth++ == 0) start = i;
      } else if (event[i] == '}') {
        if (--depth == 0) rows.push_back(event.substr(start, i - start + 1));
      }
    }
  }
  return rows;
}

// Per-data-point state never leaks into the next data point: a point without arrays or attributes
// gets empty ones, not its predecessor's.
TEST(FailureSafetyMetrics, ArraysAndAttributesNeverLeakAcrossDataPoints) {
  const std::string json =
      "{\"resourceMetrics\":[{\"scopeMetrics\":[{\"metrics\":["
      "{\"name\":\"h\",\"histogram\":{\"dataPoints\":[{\"count\":\"3\",\"bucketCounts\":[4,5,6],"
      "\"explicitBounds\":[1.5,2.5],\"attributes\":[{\"key\":\"k\",\"value\":{\"stringValue\":"
      "\"v\"}}]},{\"count\":\"1\"}]}},"
      "{\"name\":\"e\",\"exponentialHistogram\":{\"dataPoints\":[{\"count\":\"3\",\"positive\":"
      "{\"offset\":2,\"bucketCounts\":[7,8]},\"negative\":{\"offset\":1,\"bucketCounts\":[9]}},"
      "{\"count\":\"1\"}]}},"
      "{\"name\":\"s\",\"summary\":{\"dataPoints\":[{\"count\":\"2\",\"quantileValues\":"
      "[{\"quantile\":0.5,\"value\":3.5}]},{\"count\":\"1\"}]}}]}]}]}";
  for (const bool table : {false, true}) {
    SCOPED_TRACE(table ? "table" : "json");
    RawState s;
    ProcRecorder rec;
    rec.dump = true;
    const ProcResult r = CallProcess(
        s, false, "metrics", table ? TablePayload(JsonToLua(json)) : JsonPayload(json), rec);
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.count, 6) << r.error;
    const std::vector<std::string> rows = RowDumps(rec.events);
    ASSERT_EQ(rows.size(), 6u);
    auto has = [&](int event, const char* needle) {
      return rows[event].find(needle) != std::string::npos;
    };
    // First points carry their arrays and attributes.
    EXPECT_TRUE(has(0, "\"bucketCounts\":[4,5,6]")) << rows[0];
    EXPECT_TRUE(has(0, "\"Attributes\":\"{\"k\":\"v\"}\"")) << rows[0];
    EXPECT_TRUE(has(2, "\"bucketCounts\":[7,8]")) << rows[2];
    EXPECT_TRUE(has(4, "\"quantileValues\":[{")) << rows[4];
    // Second points have none of them, and an empty Attributes map.
    EXPECT_FALSE(has(1, "[4,5,6]")) << rows[1];
    EXPECT_FALSE(has(1, "1.500000f")) << "stale explicitBounds: " << rows[1];
    EXPECT_FALSE(has(1, "\"k\":\"v\"")) << "stale attributes: " << rows[1];
    EXPECT_TRUE(has(1, "\"Attributes\":\"{}\"")) << rows[1];
    EXPECT_FALSE(has(3, "[7,8]")) << rows[3];
    EXPECT_FALSE(has(3, "[9]")) << rows[3];
    EXPECT_TRUE(has(3, "\"Attributes\":\"{}\"")) << rows[3];
    EXPECT_FALSE(has(5, "3.500000f")) << "stale quantiles: " << rows[5];
    EXPECT_TRUE(has(5, "\"Attributes\":\"{}\"")) << rows[5];
  }
}

// ---- native logs: accepted shapes, ordering, malformed entries -------------------------------

struct NativeShape {
  const char* name;
  const char* json;
  int json_count;
  const char* lua;
  int lua_count;
  const char* first_body;           // Body of the first delivered row (nullptr: none expected)
  std::vector<std::string> extras;  // further fragments the first row must contain
};

TEST(FailureSafetyNativeLogs, AcceptedShapesAndSourceFieldOrdering) {
  const NativeShape shapes[] = {
      {"single event", R"({"message":"a"})", 1, R"(return {message="a"})", 1, "a"},
      {"root array", R"([{"message":"a"},{"message":"b"}])", 2,
       R"(return {{message="a"},{message="b"}})", 2, "a"},
      {"log wrapper object", R"({"log":{"message":"a"}})", 1, R"(return {log={message="a"}})", 1,
       "a"},
      {"log wrapper array", R"({"log":[{"message":"a"},{"message":"b"}]})", 2,
       R"(return {log={{message="a"},{message="b"}}})", 2, "a"},
      // Table mode only recognises entries that themselves carry log fields.
      {"array of wrappers", R"([{"log":{"message":"a"}},{"message":"b"}])", 2,
       R"(return {{log={message="a"}},{message="b"}})", 1, nullptr},
      {"unrecognised wrapper falls back to the enclosing event",
       R"({"log":{"zzz":1},"message":"a"})", 1, R"(return {log={zzz=1},message="a"})", 1, "a"},
      {"message before body", R"({"message":"m","body":"b"})", 1,
       R"(return {message="m",body="b"})", 1, "m"},
      {"body before message", R"({"body":"b","message":"m"})", 1,
       R"(return {body="b",message="m"})", 1, "m"},
      {"unrecognised wrapper keeps the enclosing event's staged fields",
       R"({"resources":{"service.name":"keep"},"log":{"zzz":1},"message":"a"})",
       1,
       R"(return {resources={["service.name"]="keep"},log={zzz=1},message="a"})",
       1,
       "a",
       {R"("ServiceName":"keep")", R"("ResourceAttributes":"{"service.name":"keep"}")"}},
      {"a matching wrapper replaces the enclosing event",
       R"({"log":{"message":"inner"},"message":"outer"})", 1,
       R"(return {log={message="inner"},message="outer"})", 1, "inner"},
      {"absent maps default to empty tables",
       R"({"message":"a"})",
       1,
       R"(return {message="a"})",
       1,
       "a",
       {R"("LogAttributes":"{}")", R"("ResourceAttributes":"{}")", R"("ScopeAttributes":"{}")"}},
      {"top-level service name",
       R"({"service_name":"top","message":"a"})",
       1,
       R"(return {service_name="top",message="a"})",
       1,
       "a",
       {R"("ServiceName":"top")"}},
      {"resource service name wins",
       R"({"service_name":"top","resources":{"service.name":"res"}})",
       1,
       R"(return {service_name="top",resources={["service.name"]="res"}})",
       1,
       "",
       {R"("ServiceName":"res")"}},
      {"body only", R"({"body":"b"})", 1, R"(return {body="b"})", 1, "b"},
      {"structured body", R"({"message":{"stringValue":"s"}})", 1,
       R"(return {message={stringValue="s"}})", 1, "s"},
  };
  for (const NativeShape& shape : shapes) {
    for (bool table : {false, true}) {
      SCOPED_TRACE(std::string(shape.name) + (table ? " (table)" : " (json)"));
      RawState s;
      ProcRecorder rec;
      rec.dump = true;
      const ProcResult r = CallProcess(
          s, true, "logs", table ? TablePayload(shape.lua) : JsonPayload(shape.json), rec);
      ASSERT_EQ(r.status, LUA_OK);
      ASSERT_EQ(r.nresults, 2) << r.error;
      const int expected = table ? shape.lua_count : shape.json_count;
      EXPECT_EQ(r.count, expected);
      EXPECT_EQ(rec.rows, expected);
      if (shape.first_body != nullptr && !rec.events.empty()) {
        EXPECT_NE(rec.events[0].find(std::string("\"Body\":\"") + shape.first_body + "\""),
                  std::string::npos)
            << rec.events[0];
      }
      for (const std::string& fragment : shape.extras) {
        ASSERT_FALSE(rec.events.empty());
        EXPECT_NE(rec.events[0].find(fragment), std::string::npos)
            << fragment << " in " << rec.events[0];
      }
      for (int i = 0; i < kInvocationSlotCount; ++i) {
        EXPECT_EQ(AnchorSlotType(s.L(), ModuleKind::Native, static_cast<AnchorSlot>(i)),
                  LUA_TBOOLEAN);
      }
    }
  }
}

TEST(FailureSafetyNativeLogs, MalformedJsonEntryFailsAfterDeliveredRows) {
  RawState s;
  ProcRecorder rec;
  const ProcResult r = CallProcess(s, true, "logs", JsonPayload(R"([{"message":"a"},5])"), rec);
  ASSERT_EQ(r.status, LUA_OK);
  ASSERT_EQ(r.nresults, 3);
  EXPECT_TRUE(r.first_nil);
  EXPECT_FALSE(r.error.empty());
  EXPECT_EQ(r.count, rec.rows) << "count must equal rows in completed callbacks";
  EXPECT_EQ(r.count, 1);

  ProcRecorder scalar;
  const ProcResult rs = CallProcess(s, true, "logs", JsonPayload("5"), scalar);
  ASSERT_EQ(rs.nresults, 3);
  EXPECT_TRUE(rs.first_nil);
  EXPECT_EQ(rs.error, "root JSON value must be an object or array");
  EXPECT_EQ(rs.count, 0);
  EXPECT_EQ(scalar.calls, 0);
}

TEST(FailureSafetyNativeLogs, NonTableEntriesInATableArrayAreSkipped) {
  RawState s;
  ProcRecorder rec;
  const ProcResult r =
      CallProcess(s, true, "logs",
                  TablePayload(R"(return {{message="a"},5,"x",{unrelated=1},{message="b"}})"), rec);
  ASSERT_EQ(r.status, LUA_OK);
  ASSERT_EQ(r.nresults, 2) << r.error;
  EXPECT_EQ(r.count, 2);
  EXPECT_EQ(rec.rows, 2);
}

TEST(FailureSafetyNativeLogs, TimestampFallsBackToObservedThenProcessedAt) {
  for (bool table : {false, true}) {
    SCOPED_TRACE(table ? "table input" : "json input");
    RawState s;
    ProcRecorder rec;
    rec.dump = true;
    const Payload payload =
        table ? TablePayload(R"(return {{message="a",observed_timestamp="2023-11-14T22:13:20Z"},
                                         {message="b"}})")
              : JsonPayload(R"([{"message":"a","observed_timestamp":"2023-11-14T22:13:20Z"},
                                {"message":"b"}])");
    const ProcResult r = CallProcess(s, true, "logs", payload, rec);
    ASSERT_EQ(r.nresults, 2) << r.error;
    ASSERT_EQ(r.count, 2);
    std::string all;
    for (const std::string& event : rec.events) all += event;
    EXPECT_NE(all.find("\"Timestamp\":\"2023-11-14T22:13:20.000000000Z\""), std::string::npos)
        << all;
    // The second record has neither timestamp: it falls back to processed_at (now()).
    const std::size_t second = all.rfind("\"Timestamp\":\"");
    const std::size_t proc = all.rfind("\"processed_at\":\"");
    ASSERT_NE(second, std::string::npos);
    ASSERT_NE(proc, std::string::npos);
    EXPECT_EQ(all.substr(second + 13, 30), all.substr(proc + 16, 30)) << all;
  }
}

TEST(FailureSafetyNativeLogs, AdmissionLimitEnforcedBeforeEmission) {
  // Root array with kMaxIncomingRecords valid records: admitted and all delivered.
  {
    SCOPED_TRACE("root array " + std::to_string(kMaxIncomingRecords));
    RawState s;
    ProcRecorder rec;
    std::string payload = "[";
    for (std::size_t i = 0; i < kMaxIncomingRecords; ++i) {
      if (i > 0) payload += ",";
      payload += NativeLogRecordJson(0, static_cast<int>(i));
    }
    payload += "]";
    const ProcResult r = CallProcess(s, true, "logs", JsonPayload(payload), rec);
    ASSERT_EQ(r.nresults, 2) << r.error;
    EXPECT_FALSE(r.first_nil);
    EXPECT_EQ(r.count, kMaxIncomingRecords);
    EXPECT_EQ(rec.rows, kMaxIncomingRecords);
  }

  // Same with kMaxIncomingRecords + 1: record-limit error, zero callbacks, zero partial.
  {
    SCOPED_TRACE("root array " + std::to_string(kMaxIncomingRecords + 1) + " rejected");
    RawState s;
    ProcRecorder rec;
    std::string payload = "[";
    for (std::size_t i = 0; i < kMaxIncomingRecords + 1; ++i) {
      if (i > 0) payload += ",";
      payload += NativeLogRecordJson(0, static_cast<int>(i));
    }
    payload += "]";
    const ProcResult r = CallProcess(s, true, "logs", JsonPayload(payload), rec);
    ASSERT_EQ(r.nresults, 3) << "Expected (nil, error, partial)";
    EXPECT_TRUE(r.first_nil);
    char limit_buf[96];
    const std::size_t limit_len = FormatRecordLimitText(limit_buf, sizeof(limit_buf));
    EXPECT_NE(r.error.find(std::string_view(limit_buf, limit_len)), std::string_view::npos)
        << r.error;
    EXPECT_EQ(r.count, 0) << "Expected zero delivered_count on reject";
    EXPECT_EQ(rec.rows, 0) << "Expected no rows emitted on admission reject";
  }
}

// ---- native traces: accepted shapes, mappings, malformed entries -----------------------------

struct NativeTraceCase {
  const char* name;
  const char* json;
  int json_count;
  const char* lua;
  int lua_count;
  std::vector<std::string> fragments;  // each must appear in the first delivered row
};

TEST(FailureSafetyNativeTraces, AcceptedShapesAndMappings) {
  const NativeTraceCase cases[] = {
      {"single span",
       R"({"name":"a"})",
       1,
       R"(return {name="a"})",
       1,
       {R"("SpanName":"a")", R"("SpanKind":"SPAN_KIND_UNSPECIFIED")",
        R"("StatusCode":"STATUS_CODE_UNSET")", R"("Events":"[]")", R"("Links":"[]")",
        R"("ResourceAttributes":"{}")", R"("SpanAttributes":"{}")", R"("Duration":0)",
        R"("Timestamp":"20)"}},
      {"root array",
       R"([{"name":"a"},{"name":"b"}])",
       2,
       R"(return {{name="a"},{name="b"}})",
       2,
       {R"("SpanName":"a")"}},
      {"log wrapper",
       R"({"log":{"name":"a"}})",
       1,
       R"(return {log={name="a"}})",
       1,
       {R"("SpanName":"a")"}},
      {"trace wrapper array",
       R"({"trace":[{"name":"a"},{"name":"b"}]})",
       2,
       R"(return {trace={{name="a"},{name="b"}}})",
       2,
       {R"("SpanName":"a")"}},
      {"matching wrapper replaces the enclosing span",
       R"({"log":{"name":"inner"},"name":"outer"})",
       1,
       R"(return {log={name="inner"},name="outer"})",
       1,
       {R"("SpanName":"inner")"}},
      {"unrecognised wrapper keeps the enclosing span's staged fields",
       R"({"resources":{"service.name":"keep"},"events":[{"name":"e"}],"log":{"zzz":1},"name":"a"})",
       1,
       R"(return {resources={["service.name"]="keep"},events={{name="e"}},log={zzz=1},name="a"})",
       1,
       {R"("ServiceName":"keep")", R"("ResourceAttributes":"{"service.name":"keep"}")",
        R"("Name":"e")"}},
      {"kind and status as strings",
       R"({"kind":"SPAN_KIND_CLIENT","status":{"code":"STATUS_CODE_ERROR","message":"bad"}})",
       1,
       R"(return {kind="SPAN_KIND_CLIENT",status={code="STATUS_CODE_ERROR",message="bad"}})",
       1,
       {R"("SpanKind":"SPAN_KIND_CLIENT")", R"("StatusCode":"STATUS_CODE_ERROR")",
        R"("StatusMessage":"bad")"}},
      {"kind and status as integers",
       R"({"kind":3,"status":{"code":2}})",
       1,
       R"(return {kind=3,status={code=2}})",
       1,
       {R"("SpanKind":"SPAN_KIND_CLIENT")", R"("StatusCode":"STATUS_CODE_ERROR")"}},
      {"flat status fields",
       R"({"status_code":1,"status_message":"m"})",
       1,
       R"(return {status_code=1,status_message="m"})",
       1,
       {R"("StatusCode":"STATUS_CODE_OK")", R"("StatusMessage":"m")"}},
      {"duration is end minus start",
       R"({"start_timestamp":"2023-11-14T22:13:20Z","end_timestamp":"2023-11-14T22:13:21Z"})",
       1,
       R"(return {start_timestamp="2023-11-14T22:13:20Z",end_timestamp="2023-11-14T22:13:21Z"})",
       1,
       {R"("Duration":1000000000)"}},
      {"end before start gives zero duration",
       R"({"start_timestamp":"2023-11-14T22:13:21Z","end_timestamp":"2023-11-14T22:13:20Z"})",
       1,
       R"(return {start_timestamp="2023-11-14T22:13:21Z",end_timestamp="2023-11-14T22:13:20Z"})",
       1,
       {R"("Duration":0)"}},
      {"ingest timestamp is the fallback",
       R"({"name":"a","ingest_timestamp":"2023-11-14T22:13:20Z"})",
       1,
       R"(return {name="a",ingest_timestamp="2023-11-14T22:13:20Z"})",
       1,
       {R"("Timestamp":"2023-11-14T22:13:20.000000000Z")"}},
      {"top-level service name",
       R"({"service_name":"top","name":"a"})",
       1,
       R"(return {service_name="top",name="a"})",
       1,
       {R"("ServiceName":"top")"}},
      {"resource service name wins",
       R"({"service_name":"top","resources":{"service.name":"res"}})",
       1,
       R"(return {service_name="top",resources={["service.name"]="res"}})",
       1,
       {R"("ServiceName":"res")"}},
      {"events and links serialise to JSON arrays",
       R"({"events":[{"name":"e","attributes":{"x":"y"}}],"links":[{"trace_id":"t","span_id":"s"}]})",
       1,
       R"(return {events={{name="e",attributes={x="y"}}},links={{trace_id="t",span_id="s"}}})",
       1,
       {R"("Name":"e")", R"("x":"y")", R"("TraceId":"t")"}},
  };
  for (const NativeTraceCase& c : cases) {
    for (bool table : {false, true}) {
      SCOPED_TRACE(std::string(c.name) + (table ? " (table)" : " (json)"));
      RawState s;
      ProcRecorder rec;
      rec.dump = true;
      const ProcResult r =
          CallProcess(s, true, "traces", table ? TablePayload(c.lua) : JsonPayload(c.json), rec);
      ASSERT_EQ(r.status, LUA_OK);
      ASSERT_EQ(r.nresults, 2) << r.error;
      const int expected = table ? c.lua_count : c.json_count;
      EXPECT_EQ(r.count, expected);
      EXPECT_EQ(rec.rows, expected);
      ASSERT_FALSE(rec.events.empty());
      for (const std::string& fragment : c.fragments) {
        EXPECT_NE(rec.events[0].find(fragment), std::string::npos)
            << fragment << " in " << rec.events[0];
      }
      for (int i = 0; i < kInvocationSlotCount; ++i) {
        EXPECT_EQ(AnchorSlotType(s.L(), ModuleKind::Native, static_cast<AnchorSlot>(i)),
                  LUA_TBOOLEAN);
      }
    }
  }
}

TEST(FailureSafetyNativeTraces, MalformedEntriesAndRoots) {
  RawState s;
  ProcRecorder rec;
  const ProcResult r = CallProcess(s, true, "traces", JsonPayload(R"([{"name":"a"},5])"), rec);
  ASSERT_EQ(r.nresults, 3);
  EXPECT_TRUE(r.first_nil);
  EXPECT_EQ(r.count, rec.rows) << "count must equal rows in completed callbacks";
  EXPECT_EQ(r.count, 1);

  ProcRecorder scalar;
  const ProcResult rs = CallProcess(s, true, "traces", JsonPayload("5"), scalar);
  ASSERT_EQ(rs.nresults, 3);
  EXPECT_EQ(rs.error, "root JSON value must be an object or array");
  EXPECT_EQ(rs.count, 0);

  ProcRecorder skipped;
  const ProcResult rt =
      CallProcess(s, true, "traces",
                  TablePayload(R"(return {{name="a"},5,"x",{unrelated=1},{name="b"}})"), skipped);
  ASSERT_EQ(rt.nresults, 2) << rt.error;
  EXPECT_EQ(rt.count, 2);
}

// Numbers are read as text without converting the Lua value (lua_tolstring would allocate a Lua
// string, which can raise in the extractor's outer frame, and would rewrite a lua_next key).
TEST(FailureSafetyNativeTraces, NumericSpanNamesAndAttributeKeysAreFormattedWithoutConversion) {
  RawState s;
  ProcRecorder rec;
  rec.dump = true;
  const ProcResult r = CallProcess(s, true, "traces", TablePayload(R"(return {name=42,span_id="ab",
        events={{name="e",time_unix_nano=1,attributes={[1]="a",[2.5]="b",k="c"}}}})"),
                                   rec);
  ASSERT_EQ(r.nresults, 2) << r.error;
  ASSERT_EQ(rec.rows, 1);
  EXPECT_NE(rec.events[0].find(R"("SpanName":"42")"), std::string::npos) << rec.events[0];
  EXPECT_NE(rec.events[0].find(R"("1":"a")"), std::string::npos) << rec.events[0];
  EXPECT_NE(rec.events[0].find(R"("2.5":"b")"), std::string::npos) << rec.events[0];
  EXPECT_NE(rec.events[0].find(R"("k":"c")"), std::string::npos) << rec.events[0];
  EXPECT_EQ(lua_gettop(s.L()), 0);
}

TEST(FailureSafetyNativeTraces, AdmissionLimitEnforcedBeforeEmission) {
  // Root array with kMaxIncomingRecords valid spans: admitted and all delivered.
  {
    SCOPED_TRACE("root array " + std::to_string(kMaxIncomingRecords));
    RawState s;
    ProcRecorder rec;
    std::string payload = "[";
    for (std::size_t i = 0; i < kMaxIncomingRecords; ++i) {
      if (i > 0) payload += ",";
      payload += NativeSpanJson(0, static_cast<int>(i));
    }
    payload += "]";
    const ProcResult r = CallProcess(s, true, "traces", JsonPayload(payload), rec);
    ASSERT_EQ(r.nresults, 2) << r.error;
    EXPECT_FALSE(r.first_nil);
    EXPECT_EQ(r.count, kMaxIncomingRecords);
    EXPECT_EQ(rec.rows, kMaxIncomingRecords);
  }

  // Same with kMaxIncomingRecords + 1: record-limit error, zero callbacks.
  {
    SCOPED_TRACE("root array " + std::to_string(kMaxIncomingRecords + 1) + " rejected");
    RawState s;
    ProcRecorder rec;
    std::string payload = "[";
    for (std::size_t i = 0; i < kMaxIncomingRecords + 1; ++i) {
      if (i > 0) payload += ",";
      payload += NativeSpanJson(0, static_cast<int>(i));
    }
    payload += "]";
    const ProcResult r = CallProcess(s, true, "traces", JsonPayload(payload), rec);
    ASSERT_EQ(r.nresults, 3);
    EXPECT_TRUE(r.first_nil);
    char limit_buf[96];
    const std::size_t limit_len = FormatRecordLimitText(limit_buf, sizeof(limit_buf));
    EXPECT_NE(r.error.find(std::string_view(limit_buf, limit_len)), std::string_view::npos)
        << r.error;
    EXPECT_EQ(r.count, 0);
    EXPECT_EQ(rec.rows, 0);
  }
}

// ---- native metrics: accepted shapes, mappings, unsupported variants, limits ------------------

struct NativeMetricCase {
  const char* name;
  const char* json;
  const char* lua;
  std::vector<std::string> fragments;  // each must appear in the first delivered row (both inputs)
};

TEST(FailureSafetyNativeMetrics, AcceptedShapesAndMappings) {
  const NativeMetricCase cases[] = {
      {"counter with temporality and monotonicity",
       R"({"name":"c","counter":{"value":2},"kind":"incremental","is_monotonic":true})",
       R"(return {name="c",counter={value=2},kind="incremental",is_monotonic=true})",
       {R"("MetricName":"c")", R"("MetricType":"sum")", R"("Value":2)", R"("IsMonotonic":true)",
        R"("AggregationTemporality":1)", R"("ResourceAttributes":"{}")",
        R"("ScopeAttributes":"{}")", R"("Attributes":"{}")", R"("Exemplars":"[]")",
        R"("ServiceName":"unknown_service")", R"("Flags":0)"}},
      {"gauge keeps a double value",
       R"({"name":"g","gauge":{"value":99.25}})",
       R"(return {name="g",gauge={value=99.25}})",
       {R"("MetricType":"gauge")", R"("Value":99.25)"}},
      {"metric without a value has no Value column",
       R"({"name":"g","gauge":{}})",
       R"(return {name="g",gauge={}})",
       {R"("MetricType":"gauge")"}},
      {"namespace prefixes the name",
       R"({"name":"n","namespace":"ns","gauge":{"value":1}})",
       R"(return {name="n",namespace="ns",gauge={value=1}})",
       {R"("MetricName":"ns.n")"}},
      {"metric wrapper",
       R"({"metric":{"name":"w","gauge":{"value":1}}})",
       R"(return {metric={name="w",gauge={value=1}}})",
       {R"("MetricName":"w")"}},
      {"histogram with bucket objects strips infinity",
       R"({"name":"h","aggregated_histogram":{"count":3,"sum":4.5,"min":1,"max":3,"buckets":[{"count":1,"upper_limit":1.0},{"count":2,"upper_limit":"+Inf"}]}})",
       R"(return {name="h",aggregated_histogram={count=3,sum=4.5,min=1,max=3,buckets={{count=1,upper_limit=1.0},{count=2,upper_limit=1e999}}}})",
       {R"("MetricType":"histogram")", R"("bucketCounts":[1,2])", R"("explicitBounds":[1])",
        R"("count":3)", R"("min":1)", R"("max":3)"}},
      {"histogram with parallel arrays",
       R"({"name":"h","aggregated_histogram":{"count":6,"sum":10.5,"explicitBounds":[1.0,5.0],"counts":[1,2,3]}})",
       R"(return {name="h",aggregated_histogram={count=6,sum=10.5,explicitBounds={1.0,5.0},counts={1,2,3}}})",
       {R"("bucketCounts":[1,2,3])", R"("explicitBounds":[1,5])"}},
      {"summary with quantile objects",
       R"({"name":"s","aggregated_summary":{"count":2,"sum":3.5,"quantiles":[{"quantile":0.5,"value":1.0},{"quantile":0.99,"value":2.0}]}})",
       R"(return {name="s",aggregated_summary={count=2,sum=3.5,quantiles={{quantile=0.5,value=1.0},{quantile=0.99,value=2.0}}}})",
       {R"("MetricType":"summary")", R"("quantileValues":[{)", R"("quantile":0.99)",
        R"("value":2)"}},
      {"summary with parallel quantile and value arrays",
       R"({"name":"s","aggregated_summary":{"count":2,"sum":3.5,"quantiles":[0.5,0.99],"values":[1.0,2.0]}})",
       R"(return {name="s",aggregated_summary={count=2,sum=3.5,quantiles={0.5,0.99},values={1.0,2.0}}})",
       {R"("quantile":0.5)", R"("value":2)"}},
      {"tags split into resource, scope, unit, description and attributes",
       R"({"name":"t","gauge":{"value":1},"tags":{"resource.service.name":"svc","resource.region":"eu","scope.name":"sc","scope.version":"2","scope.attr":"sv","otel.unit":"ms","otel.description":"desc","host":"h1","count":7,"ratio":1.5,"flag":true}})",
       R"(return {name="t",gauge={value=1},tags={["resource.service.name"]="svc",["resource.region"]="eu",["scope.name"]="sc",["scope.version"]="2",["scope.attr"]="sv",["otel.unit"]="ms",["otel.description"]="desc",host="h1",count=7,ratio=1.5,flag=true}})",
       {R"("ServiceName":"svc")", R"("region":"eu")", R"("service.name":"svc")",
        R"("ScopeName":"sc")", R"("ScopeVersion":"2")", R"("ScopeAttributes":"{"attr":"sv"}")",
        R"("MetricUnit":"ms")", R"("MetricDescription":"desc")", R"("host":"h1")", R"("count":7)",
        R"("ratio":1.5)", R"("flag":true)"}},
      {"multi-value tags keep the last value",
       R"({"name":"m","gauge":{"value":1},"tags":{"zone":["a","b"]}})",
       R"(return {name="m",gauge={value=1},tags={zone={"a","b"}}})",
       {R"("Attributes":"{"zone":"b"}")"}},
      {"top-level service name is the fallback",
       R"({"name":"m","gauge":{"value":1},"service_name":"top"})",
       R"(return {name="m",gauge={value=1},service_name="top"})",
       {R"("ServiceName":"top")"}},
      {"interval derives the start time",
       R"({"name":"m","gauge":{"value":1},"timestamp":"2023-11-14T22:13:21Z","interval_ms":1000})",
       R"(return {name="m",gauge={value=1},timestamp="2023-11-14T22:13:21Z",interval_ms=1000})",
       {R"("StartTimeUnix":"2023-11-14T22:13:20.000000000Z")",
        R"("TimeUnix":"2023-11-14T22:13:21.000000000Z")"}},
      {"resources, scope attributes and attributes",
       R"({"name":"m","gauge":{"value":1},"resources":{"service.name":"r","k":"v"},"scope":{"name":"sn","attributes":{"sa":"sb"}},"attributes":{"a":"b"}})",
       R"(return {name="m",gauge={value=1},resources={["service.name"]="r",k="v"},scope={name="sn",attributes={sa="sb"}},attributes={a="b"}})",
       {R"("ServiceName":"r")", R"("k":"v")", R"("service.name":"r")", R"("ScopeName":"sn")",
        R"("ScopeAttributes":"{"sa":"sb"}")", R"("Attributes":"{"a":"b"}")"}},
  };
  for (const NativeMetricCase& c : cases) {
    for (bool table : {false, true}) {
      SCOPED_TRACE(std::string(c.name) + (table ? " (table)" : " (json)"));
      RawState s;
      ProcRecorder rec;
      rec.dump = true;
      const ProcResult r =
          CallProcess(s, true, "metrics", table ? TablePayload(c.lua) : JsonPayload(c.json), rec);
      ASSERT_EQ(r.status, LUA_OK);
      ASSERT_EQ(r.nresults, 2) << r.error;
      EXPECT_EQ(r.count, 1);
      EXPECT_EQ(rec.rows, 1);
      ASSERT_FALSE(rec.events.empty());
      for (const std::string& fragment : c.fragments) {
        EXPECT_NE(rec.events[0].find(fragment), std::string::npos)
            << fragment << " in " << rec.events[0];
      }
      for (int i = 0; i < kInvocationSlotCount; ++i) {
        EXPECT_EQ(AnchorSlotType(s.L(), ModuleKind::Native, static_cast<AnchorSlot>(i)),
                  LUA_TBOOLEAN);
      }
    }
  }
}

// Tags are applied after the resources / attributes sources and win on a conflicting key,
// whatever the document order (key order within a map is not asserted: table input follows Lua's
// iteration order).
TEST(FailureSafetyNativeMetrics, AttributeSourcesMergeLastWriteWins) {
  RawState a;
  ProcRecorder json_first;
  json_first.dump = true;
  ASSERT_EQ(CallProcess(a, true, "metrics", JsonPayload(R"({"name":"m","gauge":{"value":1},
                          "tags":{"resource.k":"tag","resource.t":"1","plain":"tag"},
                          "resources":{"k":"res","r":"2"},"attributes":{"plain":"attr"}})"),
                        json_first)
                .count,
            1);
  for (const char* pair : {R"("k":"tag")", R"("r":"2")", R"("t":"1")"}) {
    EXPECT_NE(json_first.events[0].find(pair), std::string::npos) << json_first.events[0];
  }
  EXPECT_EQ(json_first.events[0].find(R"("k":"res")"), std::string::npos) << json_first.events[0];
  EXPECT_NE(json_first.events[0].find(R"("Attributes":"{"plain":"tag"}")"), std::string::npos)
      << json_first.events[0];

  RawState b;
  ProcRecorder json_last;
  json_last.dump = true;
  ASSERT_EQ(CallProcess(b, true, "metrics", JsonPayload(R"({"name":"m","gauge":{"value":1},
                          "resources":{"k":"res"},"attributes":{"plain":"attr"},
                          "tags":{"resource.k":"tag","plain":"tag"}})"),
                        json_last)
                .count,
            1);
  EXPECT_NE(json_last.events[0].find(R"("ResourceAttributes":"{"k":"tag"}")"), std::string::npos)
      << json_last.events[0];
  EXPECT_NE(json_last.events[0].find(R"("Attributes":"{"plain":"tag"}")"), std::string::npos);

  RawState c;
  ProcRecorder tbl;
  tbl.dump = true;
  ASSERT_EQ(CallProcess(c, true, "metrics", TablePayload(R"(return {name="m",gauge={value=1},
                          tags={["resource.k"]="tag",plain="tag"},
                          resources={k="res"},attributes={plain="attr"}})"),
                        tbl)
                .count,
            1);
  EXPECT_NE(tbl.events[0].find(R"("ResourceAttributes":"{"k":"tag"}")"), std::string::npos)
      << tbl.events[0];
  EXPECT_NE(tbl.events[0].find(R"("Attributes":"{"plain":"tag"}")"), std::string::npos);
}

// Table-mode exemplars are normalised into the row (the legacy path passed the caller's table
// through by reference).
TEST(FailureSafetyNativeMetrics, TableExemplarsAreCopiedNotAliased) {
  RawState s;
  ProcRecorder rec;
  rec.dump = true;
  const ProcResult r = CallProcess(
      s, true, "metrics",
      TablePayload(R"(return {name="m",gauge={value=1},exemplars={{value=1.5,span_id="ab"},5}})"),
      rec);
  ASSERT_EQ(r.nresults, 2) << r.error;
  ASSERT_EQ(rec.rows, 1);
  EXPECT_NE(rec.events[0].find(R"("Exemplars":"[{)"), std::string::npos) << rec.events[0];
  EXPECT_NE(rec.events[0].find(R"("SpanId":"ab")"), std::string::npos) << rec.events[0];
}

TEST(FailureSafetyNativeMetrics, UnsupportedVariantsSkippedWithWarningAndValidItemsEmitted) {
  for (const char* variant : {"distribution", "set", "sketch"}) {
    for (bool table : {false, true}) {
      SCOPED_TRACE(std::string(variant) + (table ? " (table)" : " (json)"));
      RawState s;
      ProcRecorder rec;
      const std::string json =
          std::string(R"([{"name":"ok","gauge":{"value":1}},{"name":"bad",")") + variant +
          R"(":{"x":1}},{"name":"never","gauge":{"value":2}}])";
      const std::string lua = std::string(R"(return {{name="ok",gauge={value=1}},{name="bad",)") +
                              variant + R"(={x=1}},{name="never",gauge={value=2}}})";
      testing::internal::CaptureStdout();
      const ProcResult r =
          CallProcess(s, true, "metrics", table ? TablePayload(lua) : JsonPayload(json), rec);
      std::string captured_out = testing::internal::GetCapturedStdout();
      ASSERT_EQ(r.status, LUA_OK);
      ASSERT_EQ(r.nresults, 2);
      EXPECT_FALSE(r.first_nil);
      EXPECT_EQ(r.count, 2);
      EXPECT_EQ(rec.rows, 2);
      EXPECT_NE(captured_out.find("[WARN] Dropped 1 unsupported metric item(s) in batch"),
                std::string::npos);
      EXPECT_NE(captured_out.find(variant), std::string::npos);
      for (int i = 0; i < kInvocationSlotCount; ++i) {
        EXPECT_EQ(AnchorSlotType(s.L(), ModuleKind::Native, static_cast<AnchorSlot>(i)),
                  LUA_TBOOLEAN);
      }
    }
  }
  // A null variant is ignored in JSON input.
  RawState s;
  ProcRecorder rec;
  const ProcResult r = CallProcess(
      s, true, "metrics", JsonPayload(R"({"name":"ok","gauge":{"value":1},"set":null})"), rec);
  ASSERT_EQ(r.nresults, 2) << r.error;
  EXPECT_EQ(r.count, 1);
}

// Root shapes, wrapper handling and error propagation that the shape table cannot express.
TEST(FailureSafetyNativeMetrics, RootShapesWrappersAndErrors) {
  // A scalar root, or a non-object array element, is a JSON error (not a silent skip).
  for (const char* bad : {"5", R"([{"name":"a","gauge":{"value":1}},5])"}) {
    SCOPED_TRACE(bad);
    RawState s;
    ProcRecorder rec;
    const ProcResult r = CallProcess(s, true, "metrics", JsonPayload(bad), rec);
    ASSERT_EQ(r.nresults, 3);
    EXPECT_TRUE(r.first_nil);
    EXPECT_EQ(r.count, rec.rows);
  }

  // The "log" wrapper key is accepted in both inputs, like "metric".
  for (bool table : {false, true}) {
    SCOPED_TRACE(table ? "table log wrapper" : "json log wrapper");
    RawState s;
    ProcRecorder rec;
    rec.dump = true;
    const ProcResult r =
        CallProcess(s, true, "metrics",
                    table ? TablePayload(R"(return {log={name="w",gauge={value=1}}})")
                          : JsonPayload(R"({"log":{"name":"w","gauge":{"value":1}}})"),
                    rec);
    ASSERT_EQ(r.nresults, 2) << r.error;
    EXPECT_EQ(r.count, 1);
    ASSERT_EQ(rec.rows, 1);
    EXPECT_NE(rec.events[0].find(R"("MetricName":"w")"), std::string::npos);
  }

  // An unmatched wrapper must not clobber the staging of its enclosing object.
  {
    RawState s;
    ProcRecorder rec;
    rec.dump = true;
    const ProcResult r = CallProcess(
        s, true, "metrics",
        JsonPayload(
            R"({"name":"outer","aggregated_histogram":{"count":2,"sum":1,"counts":[1,1],"explicitBounds":[1]},)"
            R"("tags":{"resource.k":"v","scope.s":"t","h":"x"},"metric":{"unrelated":1}})"),
        rec);
    ASSERT_EQ(r.nresults, 2) << r.error;
    ASSERT_EQ(rec.rows, 1);
    EXPECT_NE(rec.events[0].find(R"("MetricName":"outer")"), std::string::npos) << rec.events[0];
    EXPECT_NE(rec.events[0].find(R"("ResourceAttributes":"{"k":"v"}")"), std::string::npos)
        << rec.events[0];
    EXPECT_NE(rec.events[0].find(R"("ScopeAttributes":"{"s":"t"}")"), std::string::npos);
    EXPECT_NE(rec.events[0].find(R"("Attributes":"{"h":"x"}")"), std::string::npos);
    EXPECT_NE(rec.events[0].find(R"("bucketCounts":[1,1])"), std::string::npos) << rec.events[0];
  }

  // The derived start time clamps at the epoch instead of wrapping.
  {
    RawState s;
    ProcRecorder rec;
    rec.dump = true;
    const ProcResult r = CallProcess(
        s, true, "metrics",
        JsonPayload(R"({"name":"m","gauge":{"value":1},"timestamp":"1970-01-01T00:00:00.000001Z",)"
                    R"("interval_ms":5})"),
        rec);
    ASSERT_EQ(r.nresults, 2) << r.error;
    ASSERT_EQ(rec.rows, 1);
    EXPECT_NE(rec.events[0].find(R"("StartTimeUnix":"1970-01-01T00:00:00.000000000Z")"),
              std::string::npos)
        << rec.events[0];
  }

  // An unsupported variant inside a single table record or a table wrapper is skipped with warning.
  for (const char* lua : {R"(return {name="x",gauge={value=1},set={x=1}})",
                          R"(return {metric={name="x",gauge={value=1},set={x=1}}})"}) {
    SCOPED_TRACE(lua);
    RawState s;
    ProcRecorder rec;
    testing::internal::CaptureStdout();
    const ProcResult r = CallProcess(s, true, "metrics", TablePayload(lua), rec);
    std::string captured_out = testing::internal::GetCapturedStdout();
    ASSERT_EQ(r.nresults, 2);
    EXPECT_FALSE(r.first_nil);
    EXPECT_EQ(r.count, 0);
    EXPECT_EQ(rec.rows, 0);
    EXPECT_NE(captured_out.find("[WARN] Dropped 1 unsupported metric item(s) in batch"),
              std::string::npos);
    EXPECT_NE(captured_out.find("set: 1"), std::string::npos);
  }
}

TEST(FailureSafetyNativeMetrics, AdmissionLimitEnforcedBeforeEmission) {
  // Root array with kMaxIncomingRecords valid metrics: admitted and all delivered.
  {
    SCOPED_TRACE("root array " + std::to_string(kMaxIncomingRecords));
    RawState s;
    ProcRecorder rec;
    std::string payload = "[";
    for (std::size_t i = 0; i < kMaxIncomingRecords; ++i) {
      if (i > 0) payload += ",";
      payload += NativeMetricRecordJson(0, static_cast<int>(i));
    }
    payload += "]";
    const ProcResult r = CallProcess(s, true, "metrics", JsonPayload(payload), rec);
    ASSERT_EQ(r.nresults, 2) << r.error;
    EXPECT_FALSE(r.first_nil);
    EXPECT_EQ(r.count, kMaxIncomingRecords);
    EXPECT_EQ(rec.rows, kMaxIncomingRecords);
  }

  // Same with kMaxIncomingRecords + 1: record-limit error, zero callbacks.
  {
    SCOPED_TRACE("root array " + std::to_string(kMaxIncomingRecords + 1) + " rejected");
    RawState s;
    ProcRecorder rec;
    std::string payload = "[";
    for (std::size_t i = 0; i < kMaxIncomingRecords + 1; ++i) {
      if (i > 0) payload += ",";
      payload += NativeMetricRecordJson(0, static_cast<int>(i));
    }
    payload += "]";
    const ProcResult r = CallProcess(s, true, "metrics", JsonPayload(payload), rec);
    ASSERT_EQ(r.nresults, 3);
    EXPECT_TRUE(r.first_nil);
    char limit_buf[96];
    const std::size_t limit_len = FormatRecordLimitText(limit_buf, sizeof(limit_buf));
    EXPECT_NE(r.error.find(std::string_view(limit_buf, limit_len)), std::string_view::npos)
        << r.error;
    EXPECT_EQ(r.count, 0);
    EXPECT_EQ(rec.rows, 0);
  }

  // Two legal alternative wrappers of kMaxIncomingRecords each: admit by maximum, not summed.
  // Extraction selects source-order first match; admission bounds any alternative.
  {
    SCOPED_TRACE("two " + std::to_string(kMaxIncomingRecords) +
                 " wrappers, metric first (maximum not sum)");
    RawState s;
    ProcRecorder rec;
    rec.dump = true;
    // Build: {"metric": [kMaxIncomingRecords records], "log": [kMaxIncomingRecords records]}.
    // Admission sees max(1, kMaxIncomingRecords, kMaxIncomingRecords) = kMaxIncomingRecords;
    // extraction uses "metric" first.
    std::string metric_array = "[";
    for (std::size_t i = 0; i < kMaxIncomingRecords; ++i) {
      if (i > 0) metric_array += ",";
      metric_array += NativeMetricRecordJson(0, static_cast<int>(i));
    }
    metric_array += "]";
    std::string log_array = "[";
    for (std::size_t i = 0; i < kMaxIncomingRecords; ++i) {
      if (i > 0) log_array += ",";
      log_array += NativeMetricRecordJson(0, static_cast<int>(i));
    }
    log_array += "]";
    std::string payload = R"({"metric":)" + metric_array + R"(,"log":)" + log_array + "}";
    const ProcResult r = CallProcess(s, true, "metrics", JsonPayload(payload), rec);
    ASSERT_EQ(r.nresults, 2) << r.error;
    EXPECT_FALSE(r.first_nil);
    EXPECT_EQ(r.count, kMaxIncomingRecords)
        << "Admission uses maximum, extraction uses first match";
    EXPECT_EQ(rec.rows, kMaxIncomingRecords);
  }

  // Same but reversed order: "log" first, "metric" second (source order unchanged in admission).
  {
    SCOPED_TRACE("two " + std::to_string(kMaxIncomingRecords) +
                 " wrappers, log first (order preserved)");
    RawState s;
    ProcRecorder rec;
    rec.dump = true;
    std::string metric_array = "[";
    for (std::size_t i = 0; i < kMaxIncomingRecords; ++i) {
      if (i > 0) metric_array += ",";
      metric_array += NativeMetricRecordJson(0, static_cast<int>(i));
    }
    metric_array += "]";
    std::string log_array = "[";
    for (std::size_t i = 0; i < kMaxIncomingRecords; ++i) {
      if (i > 0) log_array += ",";
      log_array += NativeMetricRecordJson(0, static_cast<int>(i));
    }
    log_array += "]";
    std::string payload = R"({"log":)" + log_array + R"(,"metric":)" + metric_array + "}";
    const ProcResult r = CallProcess(s, true, "metrics", JsonPayload(payload), rec);
    ASSERT_EQ(r.nresults, 2) << r.error;
    EXPECT_FALSE(r.first_nil);
    EXPECT_EQ(r.count, kMaxIncomingRecords);
    EXPECT_EQ(rec.rows, kMaxIncomingRecords);
  }
}

TEST(FailureSafetyNativeMetrics, OversizedCandidateBoundRejectsEvenIfExtractionWouldSelectSmaller) {
  // Empty primary wrapper "metric" followed by large "log": admission sees
  // max(1, 0, kMaxIncomingRecords + 1) = kMaxIncomingRecords + 1, and rejects.
  // Extraction would skip empty "metric" and use "log", but never gets there.
  {
    SCOPED_TRACE("empty metric, large log wrapper");
    RawState s;
    ProcRecorder rec;
    std::string log_array = "[";
    for (std::size_t i = 0; i < kMaxIncomingRecords + 1; ++i) {
      if (i > 0) log_array += ",";
      log_array += NativeMetricRecordJson(0, static_cast<int>(i));
    }
    log_array += "]";
    std::string payload = R"({"metric":[],"log":)" + log_array + "}";
    const ProcResult r = CallProcess(s, true, "metrics", JsonPayload(payload), rec);
    ASSERT_EQ(r.nresults, 3);
    EXPECT_TRUE(r.first_nil);
    char limit_buf[96];
    const std::size_t limit_len = FormatRecordLimitText(limit_buf, sizeof(limit_buf));
    EXPECT_NE(r.error.find(std::string_view(limit_buf, limit_len)), std::string_view::npos)
        << r.error;
    EXPECT_EQ(r.count, 0);
    EXPECT_EQ(rec.rows, 0);
  }

  // Reverse: large "log", then empty "metric"; same rejection.
  {
    SCOPED_TRACE("large log, empty metric wrapper");
    RawState s;
    ProcRecorder rec;
    std::string log_array = "[";
    for (std::size_t i = 0; i < kMaxIncomingRecords + 1; ++i) {
      if (i > 0) log_array += ",";
      log_array += NativeMetricRecordJson(0, static_cast<int>(i));
    }
    log_array += "]";
    std::string payload = R"({"log":)" + log_array + R"(,"metric":[]})";
    const ProcResult r = CallProcess(s, true, "metrics", JsonPayload(payload), rec);
    ASSERT_EQ(r.nresults, 3);
    EXPECT_TRUE(r.first_nil);
    char limit_buf[96];
    const std::size_t limit_len = FormatRecordLimitText(limit_buf, sizeof(limit_buf));
    EXPECT_NE(r.error.find(std::string_view(limit_buf, limit_len)), std::string_view::npos)
        << r.error;
    EXPECT_EQ(r.count, 0);
    EXPECT_EQ(rec.rows, 0);
  }
}

// The record limit counts events, never the buckets or quantiles inside one.
TEST(FailureSafetyNativeMetrics, LimitCountsEventsNotBuckets) {
  std::string counts_json;
  std::string counts_lua;
  for (int i = 0; i < 3000; ++i) {
    counts_json += (i ? ",0" : "0");
    counts_lua += (i ? ",0" : "0");
  }
  const std::string json =
      R"({"name":"h","aggregated_histogram":{"count":0,"sum":0,"counts":[)" + counts_json + "]}}";
  const std::string lua =
      R"(return {name="h",aggregated_histogram={count=0,sum=0,counts={)" + counts_lua + "}}}";
  for (bool table : {false, true}) {
    SCOPED_TRACE(table ? "table input" : "json input");
    RawState s;
    ProcRecorder rec;
    const ProcResult r =
        CallProcess(s, true, "metrics", table ? TablePayload(lua) : JsonPayload(json), rec);
    ASSERT_EQ(r.nresults, 2) << r.error;
    EXPECT_EQ(r.count, 1);
    EXPECT_EQ(rec.rows, 1);
  }
}

INSTANTIATE_TEST_SUITE_P(OtelSignals, FailureSafetyOtelTest,
                         testing::Values(&kLogsSig, &kTracesSig, &kMetricsSig, &kAnyMetricsSig,
                                         &kNativeLogsSig, &kNativeTracesSig, &kNativeMetricsSig),
                         [](const testing::TestParamInfo<const Sig*>& info) {
                           // `name` is the process() signal; the all-types variant shares it.
                           if (info.param == &kAnyMetricsSig)
                             return std::string("metrics_all_types");
                           return std::string(info.param->native ? "native_" : "") +
                                  info.param->name;
                         });

TEST(NativeTableAdmissionRegression, NonNilPrimaryHasSameProcessResultWithIgnoredSecondary) {
  char limit_buf[96];
  const std::size_t limit_len = FormatRecordLimitText(limit_buf, sizeof(limit_buf));
  const std::string_view limit_text(limit_buf, limit_len);
  for (const char* signal : {"traces", "metrics"}) {
    const std::string primary = std::string(signal) == "metrics" ? "metric" : "log";
    const std::string secondary = std::string(signal) == "metrics" ? "log" : "trace";
    for (const char* value : {"{}", "false", "42", "'text'"}) {
      SCOPED_TRACE(std::string(signal) + " primary=" + value);
      const std::string only = "return {" + primary + "=" + value + "}";
      const std::string with_secondary = "local rows={} for i=1," +
                                         std::to_string(kMaxIncomingRecords + 1) +
                                         " do rows[i]={name='m',gauge={value=1}} end return {" +
                                         primary + "=" + value + "," + secondary + "=rows}";
      RawState first;
      RawState second;
      ProcRecorder a;
      ProcRecorder b;
      const auto expected = CallProcess(first, true, signal, TablePayload(only), a);
      const auto actual = CallProcess(second, true, signal, TablePayload(with_secondary), b);
      EXPECT_EQ(actual.status, expected.status);
      EXPECT_EQ(actual.nresults, expected.nresults);
      EXPECT_EQ(actual.first_nil, expected.first_nil);
      EXPECT_EQ(actual.count, expected.count);
      EXPECT_EQ(actual.error, expected.error);
      EXPECT_EQ(b.rows, a.rows);
      EXPECT_EQ(actual.error.find(limit_text), std::string::npos);
    }
  }
}

TEST(NativeTableAdmissionRegression, AbsentPrimarySecondaryRespectsRecordBoundary) {
  char limit_buf[96];
  const std::size_t limit_len = FormatRecordLimitText(limit_buf, sizeof(limit_buf));
  const std::string expected_limit_err(limit_buf, limit_len);
  for (const char* signal : {"traces", "metrics"}) {
    for (std::size_t count : {kMaxIncomingRecords, kMaxIncomingRecords + 1}) {
      SCOPED_TRACE(std::string(signal) + " rows=" + std::to_string(count));
      const bool metric = std::string(signal) == "metrics";
      const std::string row = metric ? "{name='m',gauge={value=1}}" : NativeSpanLua(0, 0);
      const std::string chunk = "local rows={} for i=1," + std::to_string(count) +
                                " do rows[i]=" + row + " end return {" +
                                (metric ? "log" : "trace") + "=rows}";
      RawState state;
      ProcRecorder rec;
      const auto result = CallProcess(state, true, signal, TablePayload(chunk), rec);
      ASSERT_EQ(result.status, LUA_OK);
      if (count == kMaxIncomingRecords) {
        EXPECT_EQ(result.nresults, 2) << result.error;
        EXPECT_EQ(result.count, count);
        EXPECT_EQ(rec.rows, count);
      } else {
        EXPECT_TRUE(result.first_nil);
        EXPECT_EQ(result.error, expected_limit_err);
        EXPECT_EQ(result.count, 0);
        EXPECT_EQ(rec.rows, 0);
      }
    }
  }
}

TEST(NativeJsonAdmissionRegression, ExpandedWrappersRejectBeforeAnyCallback) {
  char limit_buf[96];
  const std::size_t limit_len = FormatRecordLimitText(limit_buf, sizeof(limit_buf));
  const std::string expected_limit_err(limit_buf, limit_len);

  for (const char* signal : {"logs", "traces", "metrics"}) {
    const std::string primary = std::string(signal) == "metrics" ? "metric" : "log";
    const std::string secondary = std::string(signal) == "traces" ? "trace" : "log";
    auto rows = [&](std::size_t n) {
      std::string result = "[";
      for (std::size_t i = 0; i < n; ++i) {
        if (i) result += ',';
        result += std::string(signal) == "logs"     ? NativeLogRecordJson(0, 0)
                  : std::string(signal) == "traces" ? NativeSpanJson(0, 0)
                                                    : NativeMetricRecordJson(0, 0);
      }
      return result + "]";
    };
    const std::string legal = rows(kMaxIncomingRecords);
    const std::string too_many = rows(kMaxIncomingRecords + 1);
    // Outer array contains one wrapper object, not kMaxIncomingRecords plain root objects.
    for (bool over : {false, true}) {
      SCOPED_TRACE(std::string(signal) + (over ? " nested" + std::to_string(kMaxIncomingRecords + 1)
                                               : " nested" + std::to_string(kMaxIncomingRecords)));
      RawState state;
      ProcRecorder rec;
      const auto result = CallProcess(
          state, true, signal,
          JsonPayload("[{\"" + primary + "\":" + (over ? too_many : legal) + "}]"), rec);
      ASSERT_EQ(result.status, LUA_OK);
      EXPECT_EQ(result.count, over ? 0 : kMaxIncomingRecords) << result.error;
      EXPECT_EQ(rec.rows, over ? 0 : kMaxIncomingRecords);
      EXPECT_EQ(result.first_nil, over);
      if (over) EXPECT_EQ(result.error, expected_limit_err);
    }
    const std::string a = "{\"" + primary + "\":";
    const std::string b = "\"" + secondary + "\":";
    const std::size_t half = kMaxIncomingRecords / 2;
    const std::size_t other_half_plus_one = kMaxIncomingRecords + 1 - half;
    const std::string payloads[] = {
        a + "[]," + b + too_many + "}",
        "{" + b + too_many + ",\"" + primary + "\":[]}",
        a + "[],\"" + primary + "\":" + too_many + "}",
        "[" + a + rows(half) + "}," + a + rows(other_half_plus_one) + "}]",
    };
    for (const auto& payload : payloads) {
      RawState state;
      ProcRecorder rec;
      const auto result = CallProcess(state, true, signal, JsonPayload(payload), rec);
      EXPECT_EQ(result.status, LUA_OK);
      EXPECT_TRUE(result.first_nil);
      EXPECT_EQ(result.error, expected_limit_err);
      EXPECT_EQ(result.count, 0);
      EXPECT_EQ(rec.calls, 0);
    }
  }
}

}  // namespace
}  // namespace vector_otel::core
