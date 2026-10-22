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

namespace vector_otel::core {
namespace {

using namespace fixtures;
using test::CppFaultMode;
using test::CppFaultScope;

constexpr int kN = static_cast<int>(kEmitBatchSize);

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
};

const Sig kLogsSig{
    "logs",
    LogsJson,
    LogsLua,
    true,
    {"\"Body\":\"msg-0\"", "\"ServiceName\":\"svc-0\"", "\"LogAttributes\":{\"k\":\"v0\"}"},
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
    {"\"SpanName\":\"span-0\"", "\"ServiceName\":\"svc-0\"", "\"SpanAttributes\":{\"k\":\"v0\"}",
     "\"SpanKind\":\"SPAN_KIND_SERVER\"", "\"StatusCode\":\"STATUS_CODE_OK\"", "\"Duration\":998"},
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
     "\"MetricUnit\":\"1\"", "\"Attributes\":{\"k\":\"v0\"}",
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
  return has("\"Exemplars\":[");
}

const Sig kAnyMetricsSig{
    "metrics",
    AnyMetricsJson,
    AnyMetricsLua,
    true,
    {"\"MetricName\":\"m0\"", "\"MetricType\":\"gauge\"", "\"ServiceName\":\"svc-0\"",
     "\"Attributes\":{\"k\":\"v0\"}", "\"FilteredAttributes\":{\"fk\":\"f0\"}"},
    {"{}", "{\"resourceMetrics\":[]}"},
    {"return {}", "return {resourceMetrics={}}"},
    extractors::ProcessMetrics,
    extractors::ProcessMetricsTable,
    AnyMetricRowComplete};

const Sig* g_sig = &kLogsSig;  // the signal under test (set by the fixture)

class FailureSafetyOtelTest : public testing::TestWithParam<const Sig*> {
 protected:
  void SetUp() override { g_sig = GetParam(); }
};

Payload PayloadFor(bool table, int resources, int records) {
  return table ? TablePayload(g_sig->lua(resources, records))
               : JsonPayload(g_sig->json(resources, records));
}

void ExpectAnchorsReleased(lua_State* L) {
  for (int i = 0; i < kInvocationSlotCount; ++i) {
    EXPECT_EQ(AnchorSlotType(L, ModuleKind::Otel, static_cast<AnchorSlot>(i)), LUA_TBOOLEAN)
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

TEST_P(FailureSafetyOtelTest, ShareOneResourceAndScopeTablePerResource) {
  for (bool table : {false, true}) {
    SCOPED_TRACE(table ? "table input" : "json input");
    RawState s;
    ProcRecorder rec;
    rec.dump = true;
    const ProcResult r = CallProcess(s, false, g_sig->name, PayloadFor(table, 2, 3), rec);
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.nresults, 2);
    EXPECT_TRUE(r.first_nil == false);
    EXPECT_EQ(r.count, 6);
    EXPECT_EQ(rec.rows, 6);
    EXPECT_EQ(rec.calls, CeilDiv(6, kN) <= 6 ? rec.calls : -1);
    ASSERT_EQ(rec.resource_ptrs.size(), 6u);
    for (int i = 0; i < 6; ++i) {
      ASSERT_NE(rec.resource_ptrs[i], nullptr);
      if (g_sig->scope_attrs) ASSERT_NE(rec.scope_ptrs[i], nullptr);
    }
    for (int i = 1; i < 3; ++i) {
      EXPECT_EQ(rec.resource_ptrs[0], rec.resource_ptrs[i]) << "ResourceAttributes copied per row";
      if (g_sig->scope_attrs) {
        EXPECT_EQ(rec.scope_ptrs[0], rec.scope_ptrs[i]) << "ScopeAttributes copied per row";
      }
      EXPECT_EQ(rec.resource_ptrs[3], rec.resource_ptrs[3 + i]);
    }
    EXPECT_NE(rec.resource_ptrs[0], rec.resource_ptrs[3]) << "distinct resources must not share";
    if (g_sig->scope_attrs) EXPECT_NE(rec.scope_ptrs[0], rec.scope_ptrs[3]);
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
  ASSERT_EQ(CallProcess(a, false, g_sig->name, PayloadFor(false, 2, 2), ja).count, 4);
  ASSERT_EQ(CallProcess(b, false, g_sig->name, PayloadFor(true, 2, 2), tb).count, 4);
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
    const ProcResult r = CallProcess(s, false, g_sig->name, JsonPayload(j), rec);
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.nresults, 2);
    EXPECT_FALSE(r.first_nil);
    EXPECT_EQ(r.count, 0);
    EXPECT_EQ(rec.calls, 0);
  }
  for (const char* t : g_sig->empty_lua) {
    SCOPED_TRACE(t);
    ProcRecorder rec;
    const ProcResult r = CallProcess(s, false, g_sig->name, TablePayload(t), rec);
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
    const ProcResult r = CallProcess(s, false, g_sig->name, p, rec);
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.nresults, 3);
    EXPECT_TRUE(r.first_nil);
    EXPECT_EQ(r.error, "payload must be a string or table");
    EXPECT_EQ(r.count, 0);
  }
  const ProcResult r = CallProcess(
      s, false, g_sig->name, JsonPayload("{}"), rec, [](auto&& run) { run(); },
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
  const ProcResult r = CallProcess(s, false, g_sig->name, JsonPayload("not json"), rec);
  ASSERT_EQ(r.status, LUA_OK);
  ASSERT_EQ(r.nresults, 3);
  EXPECT_TRUE(r.first_nil);
  EXPECT_FALSE(r.error.empty());
  EXPECT_EQ(r.count, 0);
  EXPECT_EQ(rec.calls, 0);
  ExpectAnchorsReleased(s.L());
}

TEST_P(FailureSafetyOtelTest, RecordLimitIsEnforcedBeforeAnyEmission) {
  RawState s;
  ProcRecorder rec;
  const ProcResult ok = CallProcess(s, false, g_sig->name, PayloadFor(false, 1, 2048), rec);
  ASSERT_EQ(ok.nresults, 2);
  EXPECT_EQ(ok.count, 2048);
  ProcRecorder over;
  const ProcResult r = CallProcess(s, false, g_sig->name, PayloadFor(false, 1, 2049), over);
  ASSERT_EQ(r.status, LUA_OK);
  ASSERT_EQ(r.nresults, 3);
  EXPECT_TRUE(r.first_nil);
  EXPECT_EQ(r.error, "event exceeds 2048-record limit");
  EXPECT_EQ(r.count, 0);
  EXPECT_EQ(over.calls, 0);
  ProcRecorder over_t;
  const ProcResult rt = CallProcess(s, false, g_sig->name, PayloadFor(true, 1, 2049), over_t);
  ASSERT_EQ(rt.nresults, 3);
  EXPECT_EQ(rt.error, "event exceeds 2048-record limit");
  EXPECT_EQ(over_t.calls, 0);
}

TEST_P(FailureSafetyOtelTest, CallbackFailureReportsRowsFromCompletedCallbacks) {
  for (bool table : {false, true}) {
    SCOPED_TRACE(table ? "table input" : "json input");
    RawState s;
    ProcRecorder rec;
    rec.fail_at_call = 1;  // the second delivery raises
    const ProcResult r = CallProcess(s, false, g_sig->name, PayloadFor(table, 1, 5), rec);
    ASSERT_EQ(r.status, LUA_OK);
    if (kN >= 5) {
      // a single delivery for all five rows: call index 1 never happens
      ASSERT_EQ(r.nresults, 2);
      EXPECT_EQ(r.count, 5);
    } else {
      ASSERT_EQ(r.nresults, 3);
      EXPECT_TRUE(r.first_nil);
      EXPECT_NE(r.error.find("boom"), std::string::npos) << r.error;
      EXPECT_EQ(r.count, kN) << "only rows of completed callbacks are counted";
      EXPECT_EQ(rec.rows, kN);
    }
    ExpectAnchorsReleased(s.L());
  }
}

TEST_P(FailureSafetyOtelTest, ReentrantProcessIsRejectedWithoutDisturbingTheOuterCall) {
  RawState s;
  ProcRecorder rec;
  rec.reenter = true;
  const ProcResult r = CallProcess(s, false, g_sig->name, PayloadFor(false, 1, 3), rec);
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
    ASSERT_TRUE(EnsureModuleState(s.L(), ModuleKind::Otel));
    lua_pop(s.L(), 1);
    ProcRecorder rec;
    rec.dump = true;
    const Payload payload = PayloadFor(table, resources, records);
    const ProcResult r = CallProcess(s, false, g_sig->name, payload, rec, [&](auto&& run) {
      s.alloc().SetPlan(LuaFaultMode::kPersistent, skip);
      run();
      s.alloc().ClearPlan();
    });
    ASSERT_EQ(r.status, LUA_OK) << "an error escaped the dispatcher frame";
    ExpectAnchorsReleased(s.L());
    ExpectRowsComplete(rec);
    for (const void* p : rec.resource_ptrs) {
      EXPECT_NE(p, nullptr) << "a delivered row lost its shared ResourceAttributes table";
    }
    for (const void* p : rec.scope_ptrs) {
      if (!g_sig->scope_attrs) break;
      EXPECT_NE(p, nullptr) << "a delivered row lost its shared ScopeAttributes table";
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
    const ProcResult ok = CallProcess(s, false, g_sig->name, payload, again);
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
    ASSERT_TRUE(EnsureModuleState(s.L(), ModuleKind::Otel));
    lua_pop(s.L(), 1);
    ProcRecorder rec;
    rec.dump = true;
    const Payload payload = PayloadFor(table, resources, records);
    std::size_t attempts = 0;
    const ProcResult r = CallProcess(s, false, g_sig->name, payload, rec, [&](auto&& run) {
      s.alloc().SetPlan(LuaFaultMode::kWindow, skip, 2);
      run();
      attempts = s.alloc().stats().attempts;
      s.alloc().ClearPlan();
    });
    ASSERT_EQ(r.status, LUA_OK) << "an error escaped the dispatcher frame";
    ExpectAnchorsReleased(s.L());
    ExpectRowsComplete(rec);
    for (const void* p : rec.resource_ptrs) {
      EXPECT_NE(p, nullptr) << "a delivered row lost its shared ResourceAttributes table";
    }
    for (const void* p : rec.scope_ptrs) {
      if (!g_sig->scope_attrs) break;
      EXPECT_NE(p, nullptr) << "a delivered row lost its shared ScopeAttributes table";
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
  ASSERT_EQ(luaopen_vector_otel(L), 1);
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
    if (!EnsureModuleState(s.L(), ModuleKind::Otel)) return;
    lua_pop(s.L(), 1);
    ProcRecorder rec;
    const Payload payload = PayloadFor(table, resources, records);
    const ProcResult r = CallProcess(s, false, g_sig->name, payload, rec, [&](auto&& run) {
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
    ASSERT_EQ(CallProcess(s, false, g_sig->name, payload, warm).count, 6);
    ProcRecorder rec;
    std::size_t attempts = 0;
    const ProcResult r = CallProcess(s, false, g_sig->name, payload, rec, [&](auto&& run) {
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

// The 2048 limit counts data points: not metrics, not bucket or quantile entries.
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
  struct Case {
    std::vector<std::vector<int>> split;
    bool ok;
  };
  const Case cases[] = {
      {{{1024, 1024}}, true},           // 2048 in one resource, two scopes
      {{{1024, 1023}, {1, 0}}, true},   // 2048 over two resources
      {{{1024, 1024}, {1, 0}}, false},  // 2049 over two resources
      {{{2047, 0}}, true},              // 2047
      {{{2048, 1}}, false},             // 2049 over two scopes
  };
  for (const Case& c : cases) {
    RawState s;
    ProcRecorder rec;
    const ProcResult r =
        CallProcess(s, false, "metrics", JsonPayload(MetricsSplitJson(c.split)), rec);
    ASSERT_EQ(r.status, LUA_OK);
    if (c.ok) {
      ASSERT_EQ(r.nresults, 2) << r.error;
      EXPECT_GE(r.count, 2047);
    } else {
      ASSERT_EQ(r.nresults, 3);
      EXPECT_TRUE(r.first_nil);
      EXPECT_EQ(r.error, "event exceeds 2048-record limit");
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
    EXPECT_TRUE(has(0, "\"Attributes\":{\"k\":\"v\"}")) << rows[0];
    EXPECT_TRUE(has(2, "\"bucketCounts\":[7,8]")) << rows[2];
    EXPECT_TRUE(has(4, "\"quantileValues\":[{")) << rows[4];
    // Second points have none of them, and an empty Attributes map.
    EXPECT_FALSE(has(1, "[4,5,6]")) << rows[1];
    EXPECT_FALSE(has(1, "1.500000f")) << "stale explicitBounds: " << rows[1];
    EXPECT_FALSE(has(1, "\"k\":\"v\"")) << "stale attributes: " << rows[1];
    EXPECT_TRUE(has(1, "\"Attributes\":{}")) << rows[1];
    EXPECT_FALSE(has(3, "[7,8]")) << rows[3];
    EXPECT_FALSE(has(3, "[9]")) << rows[3];
    EXPECT_TRUE(has(3, "\"Attributes\":{}")) << rows[3];
    EXPECT_FALSE(has(5, "3.500000f")) << "stale quantiles: " << rows[5];
    EXPECT_TRUE(has(5, "\"Attributes\":{}")) << rows[5];
  }
}

INSTANTIATE_TEST_SUITE_P(OtelSignals, FailureSafetyOtelTest,
                         testing::Values(&kLogsSig, &kTracesSig, &kMetricsSig, &kAnyMetricsSig),
                         [](const testing::TestParamInfo<const Sig*>& info) {
                           // `name` is the process() signal; the all-types variant shares it.
                           return info.param == &kAnyMetricsSig ? std::string("metrics_all_types")
                                                                : std::string(info.param->name);
                         });

}  // namespace
}  // namespace vector_otel::core
