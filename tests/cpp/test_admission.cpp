// Admission: configured byte limit and whole-event record count (fault target only).

#include <gtest/gtest.h>

#include <cstddef>
#include <lua.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "contract_fixtures.hpp"
#include "fault_new.hpp"
#include "operation_fixtures.hpp"
#include "vector_otel/core/admission.hpp"
#include "vector_otel/core/config.hpp"
#include "vector_otel/core/parser.hpp"

namespace vector_otel::core {
namespace {

using namespace fixtures;
using test::CppFaultMode;
using test::CppFaultScope;

OtelParser* g_parser = nullptr;  // POD, set by the test before each scenario

// ---- scenario bodies (POD only: they run under lua_pcall) -------------------------------------

void RecordOutcome(OperationContext& ctx, Scenario& sc, bool ok) {
  sc.flag = ok;
  sc.code = ctx.error.code;
  sc.number = ctx.incoming_records;
  CopyMessage(sc, ctx);
}

void JsonBody(OperationContext& ctx, Scenario& sc) {
  simdjson::padded_string_view admitted;
  bool ok = false;
  if (sc.flag2) {  // C++ allocation failure while the workspace is prepared
    CppFaultScope scope(CppFaultMode::kPersistent, 0);
    ok = AdmitJson(ctx, *g_parser, std::string_view(sc.text, sc.text_len), admitted);
    sc.denied = scope.stats().denied;
  } else if (sc.text_capacity != 0) {  // caller-owned padded input
    ok = AdmitJson(ctx, *g_parser,
                   simdjson::padded_string_view(sc.text, sc.text_len, sc.text_capacity), admitted);
  } else {
    ok = AdmitJson(ctx, *g_parser, std::string_view(sc.text, sc.text_len), admitted);
  }
  sc.admitted_size = admitted.size();
  sc.admitted_data = admitted.data();
  RecordOutcome(ctx, sc, ok);
}

bool LoadTableInput(lua_State* L, Scenario& sc) {
  // The raw state has no standard library; the metatable fixtures need setmetatable/error.
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  lua_pop(L, 1);
  if (luaL_loadstring(L, sc.chunk) != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
    sc.fixture_error = true;
    return false;
  }
  return true;
}

void TableBody(OperationContext& ctx, Scenario& sc) {
  if (!LoadTableInput(ctx.L, sc)) {
    return;
  }
  const int idx = lua_gettop(ctx.L);
  RecordOutcome(ctx, sc, AdmitTable(ctx, idx));
  sc.pending_type = AnchorType(ctx.L, ctx.anchor_index, AnchorSlot::kInput);
}

// ---- runners ----------------------------------------------------------------------------------

struct Outcome {
  bool ok = false;
  ErrorCode code = ErrorCode::None;
  std::size_t count = 0;
  std::string message;
  Observed observed;
  std::size_t admitted_size = 0;
  const char* admitted_data = nullptr;
  std::size_t denied = 0;
};

Outcome Collect(const Scenario& sc, const Observed& o) {
  Outcome r;
  r.ok = sc.flag;
  r.code = sc.code;
  r.count = sc.number;
  r.message = sc.message;
  r.observed = o;
  r.admitted_size = sc.admitted_size;
  r.admitted_data = sc.admitted_data;
  r.denied = sc.denied;
  EXPECT_FALSE(sc.fixture_error) << "table fixture failed to build";
  EXPECT_EQ(o.status, LUA_OK) << "the process closure itself must never raise";
  return r;
}

Outcome RunJson(ModuleKind kind, Signal signal, const std::string& payload,
                OtelParser* parser = nullptr) {
  g_parser = parser != nullptr ? parser : &OtelParser::Instance();
  RawState s;
  Scenario sc;
  sc.body = JsonBody;
  sc.kind = kind;
  sc.signal = signal;
  sc.text = payload.data();
  sc.text_len = payload.size();
  const Observed o = RunScenario(s, sc, [] {});
  return Collect(sc, o);
}

Outcome RunTable(ModuleKind kind, Signal signal, const std::string& chunk) {
  RawState s;
  Scenario sc;
  sc.body = TableBody;
  sc.kind = kind;
  sc.signal = signal;
  sc.chunk = chunk.c_str();
  const Observed o = RunScenario(s, sc, [] {});
  return Collect(sc, o);
}

const char* const kRecordLimitMessage = "event exceeds 2048-record limit";

void ExpectAccepted(const Outcome& r, std::size_t count, const std::string& what) {
  EXPECT_TRUE(r.ok) << what;
  EXPECT_EQ(r.code, ErrorCode::None) << what;
  EXPECT_EQ(r.count, count) << what;
  EXPECT_EQ(r.observed.nresults, 2) << what;
  EXPECT_FALSE(r.observed.first_nil) << what;
}

void ExpectRecordLimit(const Outcome& r, const std::string& what) {
  EXPECT_FALSE(r.ok) << what;
  EXPECT_EQ(r.code, ErrorCode::RecordLimit) << what;
  EXPECT_EQ(r.count, kMaxIncomingRecords + 1) << what << ": counting stops at record 2049";
  EXPECT_EQ(r.message, kRecordLimitMessage) << what;
  ASSERT_EQ(r.observed.nresults, 3) << what;
  EXPECT_TRUE(r.observed.first_nil) << what;
  EXPECT_EQ(r.observed.text, kRecordLimitMessage) << what;
  EXPECT_EQ(r.observed.last, 0) << what << ": nothing was delivered";
}

// ---- OTLP fixtures ----------------------------------------------------------------------------

struct OtlpNames {
  const char* resource;
  const char* scope;
  const char* leaf;
  bool metrics;
};

OtlpNames NamesFor(Signal signal) {
  switch (signal) {
    case Signal::Logs:
      return {"resourceLogs", "scopeLogs", "logRecords", false};
    case Signal::Traces:
      return {"resourceSpans", "scopeSpans", "spans", false};
    case Signal::Metrics:
      break;
  }
  return {"resourceMetrics", "scopeMetrics", "metrics", true};
}

// resources -> scopes -> number of records (metrics: data points of one gauge metric).
using Layout = std::vector<std::vector<std::size_t>>;

// 3 resources x 2 scopes with `total` records spread unevenly (remainder in the last scope).
Layout Spread(std::size_t total) {
  Layout layout(3, std::vector<std::size_t>(2, total / 6));
  layout.back().back() += total - 6 * (total / 6);
  layout[0][0] += 0;
  return layout;
}

std::string Repeat(const std::string& item, std::size_t n) {
  std::string out = "[";
  for (std::size_t i = 0; i < n; ++i) {
    if (i != 0) out += ',';
    out += item;
  }
  return out + "]";
}

std::string OtlpJson(Signal signal, const Layout& layout, const char* item = "{}") {
  const OtlpNames nm = NamesFor(signal);
  std::string out = std::string("{\"") + nm.resource + "\":[";
  for (std::size_t r = 0; r < layout.size(); ++r) {
    if (r != 0) out += ',';
    out += std::string("{\"") + nm.scope + "\":[";
    for (std::size_t s = 0; s < layout[r].size(); ++s) {
      if (s != 0) out += ',';
      if (nm.metrics) {
        out += "{\"metrics\":[{\"name\":\"m\",\"gauge\":{\"dataPoints\":" +
               Repeat(item, layout[r][s]) + "}}]}";
      } else {
        out += std::string("{\"") + nm.leaf + "\":" + Repeat(item, layout[r][s]) + "}";
      }
    }
    out += "]}";
  }
  return out + "]}";
}

const char* const kLuaHelpers =
    "local function r(n, item) local t = {} for i = 1, n do t[i] = item and item() or {} end "
    "return t end\n";

std::string OtlpChunk(Signal signal, const Layout& layout, const std::string& item = "") {
  const OtlpNames nm = NamesFor(signal);
  const std::string item_arg = item.empty() ? "" : ", function() return " + item + " end";
  std::string out = std::string(kLuaHelpers) + "return {" + nm.resource + " = {";
  for (const auto& scopes : layout) {
    out += std::string("{") + nm.scope + " = {";
    for (std::size_t n : scopes) {
      if (nm.metrics) {
        out += "{metrics = {{name = 'm', gauge = {dataPoints = r(" + std::to_string(n) + item_arg +
               ")}}}},";
      } else {
        out += std::string("{") + nm.leaf + " = r(" + std::to_string(n) + item_arg + ")},";
      }
    }
    out += "}},";
  }
  return out + "}}";
}

void CheckWholeEvent(Signal signal) {
  for (const std::size_t n :
       {std::size_t{0}, std::size_t{1}, std::size_t{2047}, std::size_t{2048}, std::size_t{2049}}) {
    const Layout layout = Spread(n);
    for (int form = 0; form < 2; ++form) {
      const std::string what = std::string(form == 0 ? "json " : "table ") + std::to_string(n);
      const Outcome r = form == 0 ? RunJson(ModuleKind::Otel, signal, OtlpJson(signal, layout))
                                  : RunTable(ModuleKind::Otel, signal, OtlpChunk(signal, layout));
      if (n <= kMaxIncomingRecords) {
        ExpectAccepted(r, n, what);
      } else {
        ExpectRecordLimit(r, what);
      }
    }
  }
}

TEST(AdmissionTest, LogsCountsWholeEvent) { CheckWholeEvent(Signal::Logs); }
TEST(AdmissionTest, TracesCountsWholeEvent) { CheckWholeEvent(Signal::Traces); }
TEST(AdmissionTest, MetricsCountsWholeEvent) { CheckWholeEvent(Signal::Metrics); }

TEST(AdmissionTest, MetricsOneDescriptorWith2049DataPointsIsRejected) {
  const Layout one = {{2049}};
  ExpectRecordLimit(RunJson(ModuleKind::Otel, Signal::Metrics, OtlpJson(Signal::Metrics, one)),
                    "json");
  ExpectRecordLimit(RunTable(ModuleKind::Otel, Signal::Metrics, OtlpChunk(Signal::Metrics, one)),
                    "table");
  // Bulk additions saturate at the limit + 1 too (the reported count is exactly 2049).
  const Layout two = {{1500, 1500}};
  ExpectRecordLimit(RunJson(ModuleKind::Otel, Signal::Metrics, OtlpJson(Signal::Metrics, two)),
                    "json two descriptors");
  const Layout ok = {{2048}};
  ExpectAccepted(RunJson(ModuleKind::Otel, Signal::Metrics, OtlpJson(Signal::Metrics, ok)), 2048,
                 "json");
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Metrics, OtlpChunk(Signal::Metrics, ok)), 2048,
                 "table");
}

TEST(AdmissionTest, MetricsCountOnlyTheSelectedTypeRegardlessOfKeyOrder) {
  // gauge outranks sum whatever the source order; a non-object candidate is not selectable.
  const std::string gauge_first =
      R"({"resourceMetrics":[{"scopeMetrics":[{"metrics":[{"gauge":{"dataPoints":[{}]},)"
      R"("sum":{"dataPoints":[{},{},{}]}}]}]}]})";
  const std::string sum_first =
      R"({"resourceMetrics":[{"scopeMetrics":[{"metrics":[{"sum":{"dataPoints":[{},{},{}]},)"
      R"("gauge":{"dataPoints":[{}]}}]}]}]})";
  const std::string scalar_gauge = R"({"resourceMetrics":[{"scopeMetrics":[{"metrics":[{"gauge":5,)"
                                   R"("sum":{"dataPoints":[{},{}]}}]}]}]})";
  const std::string snake =
      R"({"resource_metrics":[{"scope_metrics":[{"metrics":[{"exponential_histogram":)"
      R"({"data_points":[{},{}]}}]}]}]})";
  ExpectAccepted(RunJson(ModuleKind::Otel, Signal::Metrics, gauge_first), 1, "gauge first");
  ExpectAccepted(RunJson(ModuleKind::Otel, Signal::Metrics, sum_first), 1, "sum first");
  ExpectAccepted(RunJson(ModuleKind::Otel, Signal::Metrics, scalar_gauge), 2, "scalar gauge");
  ExpectAccepted(RunJson(ModuleKind::Otel, Signal::Metrics, snake), 2, "snake case");

  const std::string table_both =
      "return {resourceMetrics = {{scopeMetrics = {{metrics = {{gauge = {dataPoints = {{}}}, "
      "sum = {dataPoints = {{},{},{}}}}}}}}}}";
  const std::string table_scalar =
      "return {resourceMetrics = {{scopeMetrics = {{metrics = {{gauge = 5, "
      "sum = {dataPoints = {{},{}}}}}}}}}}";
  const std::string table_exp =
      "return {resource_metrics = {{scope_metrics = {{metrics = {{exponentialHistogram = "
      "{data_points = {{},{}}}}}}}}}}";
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Metrics, table_both), 1, "table gauge");
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Metrics, table_scalar), 2, "table scalar");
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Metrics, table_exp), 2, "table exp");
}

TEST(AdmissionTest, NestedCollectionsAreFieldsNotRecords) {
  // 3000-element sub-collections inside ONE record must not inflate the count.
  const std::string big = Repeat("{}", 3000);
  const Layout one = {{1}};
  const std::string log_item = "{\"attributes\":" + big + ",\"body\":" + big + "}";
  const std::string span_item = "{\"events\":" + big + ",\"links\":" + big + "}";
  const std::string dp_item = "{\"exemplars\":" + big + ",\"bucketCounts\":" + big + "}";
  ExpectAccepted(
      RunJson(ModuleKind::Otel, Signal::Logs, OtlpJson(Signal::Logs, one, log_item.c_str())), 1,
      "log");
  ExpectAccepted(
      RunJson(ModuleKind::Otel, Signal::Traces, OtlpJson(Signal::Traces, one, span_item.c_str())),
      1, "span");
  ExpectAccepted(
      RunJson(ModuleKind::Otel, Signal::Metrics, OtlpJson(Signal::Metrics, one, dp_item.c_str())),
      1, "datapoint");
  const std::string tbl_big = "r(3000)";
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Traces,
                          OtlpChunk(Signal::Traces, one,
                                    "{events = " + tbl_big + ", links = " + tbl_big + "}")),
                 1, "table span");
  ExpectAccepted(
      RunTable(ModuleKind::Otel, Signal::Metrics,
               OtlpChunk(Signal::Metrics, one,
                         "{exemplars = " + tbl_big + ", bucketCounts = " + tbl_big + "}")),
      1, "table datapoint");
}

TEST(AdmissionTest, OtlpMalformedStructuresCountOnlyWhatExtractionWouldVisit) {
  // Non-object members, non-array nodes and a wrong root are skipped here; extraction keeps
  // its own diagnostics for them.
  ExpectAccepted(
      RunJson(
          ModuleKind::Otel, Signal::Logs,
          R"({"resourceLogs":[1,"x",null,[],{"scopeLogs":[7,{"logRecords":[{},3,[],{},"s"]}]}]})"),
      2, "mixed members");
  ExpectAccepted(RunJson(ModuleKind::Otel, Signal::Logs, R"({"resourceLogs":{"scopeLogs":[{}]}})"),
                 0, "object instead of array");
  ExpectAccepted(RunJson(ModuleKind::Otel, Signal::Logs, R"({"resourceLogs":5})"), 0, "scalar");
  ExpectAccepted(RunJson(ModuleKind::Otel, Signal::Logs, "[1,2,3]"), 0, "array root");
  ExpectAccepted(RunJson(ModuleKind::Otel, Signal::Logs, "{}"), 0, "empty object");
  ExpectAccepted(
      RunTable(ModuleKind::Otel, Signal::Logs,
               "return {resourceLogs = {1, 'x', {scopeLogs = {7, {logRecords = {{}, 3, {}}}}}}}"),
      2, "table mixed members");
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Logs, "return {resourceLogs = 5}"), 0,
                 "table scalar");
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Logs, "return {}"), 0, "empty table");
}

TEST(AdmissionTest, OtlpTableWrappersAndRootArrayMatchExtractionNormalization) {
  struct Case {
    Signal signal;
    const char* wrapper;
    const char* resource;
    const char* scope;
    const char* leaf;
  } const cases[] = {
      {Signal::Logs, "log", "resourceLogs", "scopeLogs", "logRecords"},
      {Signal::Traces, "trace", "resourceSpans", "scopeSpans", "spans"},
      {Signal::Traces, "log", "resourceSpans", "scopeSpans", "spans"},
      {Signal::Metrics, "log", "resourceMetrics", "scopeMetrics", "metrics"},
  };
  for (const Case& c : cases) {
    const std::string what = std::string(c.wrapper) + "/" + c.resource;
    const std::string body = std::string("{") + c.scope + " = {{" + c.leaf + " = r(3)}}}";
    if (c.signal == Signal::Metrics) {
      continue;  // metrics leaves need the type structure; covered below
    }
    ExpectAccepted(RunTable(ModuleKind::Otel, c.signal,
                            std::string(kLuaHelpers) + "return {" + c.wrapper + " = {" +
                                c.resource + " = {" + body + "}}}"),
                   3, what);
    // the table itself is the resource array
    ExpectAccepted(
        RunTable(ModuleKind::Otel, c.signal, std::string(kLuaHelpers) + "return {" + body + "}"), 3,
        what + " root array");
  }
  // Only the first wrapper that is present is consulted (else-if chain in the extractors): a
  // `trace` wrapper without resources hides a later `log` wrapper and the root array is read.
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Traces,
                          std::string(kLuaHelpers) +
                              "return {trace = {}, log = {resourceSpans = {{scopeSpans = {{spans = "
                              "r(3)}}}}}}"),
                 0, "first present wrapper only");
  const std::string metric_body = "{scopeMetrics = {{metrics = {{gauge = {dataPoints = r(3)}}}}}}";
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Metrics,
                          std::string(kLuaHelpers) + "return {metric = {resourceMetrics = {" +
                              metric_body + "}}}"),
                 3, "metric wrapper");
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Metrics,
                          std::string(kLuaHelpers) + "return {log = {resourceMetrics = {" +
                              metric_body + "}}}"),
                 3, "log wrapper");
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Metrics,
                          std::string(kLuaHelpers) + "return {" + metric_body + "}"),
                 3, "metrics root array");
  // snake_case aliases
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Logs,
                          std::string(kLuaHelpers) +
                              "return {resource_logs = {{scope_logs = {{log_records = r(4)}}}}}"),
                 4, "snake logs");
  ExpectAccepted(RunJson(ModuleKind::Otel, Signal::Logs,
                         R"({"resource_logs":[{"scope_logs":[{"log_records":[{},{},{},{}]}]}]})"),
                 4, "snake logs json");
  // a signal never reads another signal's keys
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Logs,
                          std::string(kLuaHelpers) +
                              "return {resourceSpans = {{scopeSpans = {{spans = r(5)}}}}}"),
                 0, "wrong signal");
}

// ---- native -----------------------------------------------------------------------------------

struct NativeNames {
  Signal signal;
  const char* wrappers[2];
};

const NativeNames kNative[] = {
    {Signal::Logs, {"log", nullptr}},
    {Signal::Traces, {"trace", "log"}},
    {Signal::Metrics, {"metric", "log"}},
};

std::string NativeTableArray(std::size_t n) {
  return "local function r(n) local t = {} for i = 1, n do t[i] = {message = 'x'} end return t "
         "end\n"
         "local N = " +
         std::to_string(n) + "\n";
}

TEST(AdmissionTest, NativeJsonSingleArrayAndWrapperNormalization) {
  for (const NativeNames& nn : kNative) {
    const Signal sig = nn.signal;
    const std::string tag = "signal " + std::to_string(static_cast<int>(sig));
    ExpectAccepted(RunJson(ModuleKind::Native, sig, R"({"message":"x"})"), 1, tag + " single");
    ExpectAccepted(RunJson(ModuleKind::Native, sig, "[]"), 0, tag + " empty array");
    ExpectAccepted(RunJson(ModuleKind::Native, sig, Repeat(R"({"message":"x"})", 2048)), 2048,
                   tag + " array 2048");
    ExpectRecordLimit(RunJson(ModuleKind::Native, sig, Repeat(R"({"message":"x"})", 2049)),
                      tag + " array 2049");
    // non-object members are left to extraction; only objects are candidates
    ExpectAccepted(RunJson(ModuleKind::Native, sig, R"([{"message":"x"},5,null,{"message":"y"}])"),
                   2, tag + " malformed member");
    for (const char* wrapper : nn.wrappers) {
      if (wrapper == nullptr) continue;
      const std::string w = std::string("\"") + wrapper + "\":";
      ExpectAccepted(RunJson(ModuleKind::Native, sig, "{" + w + R"({"message":"x"}})"), 1,
                     tag + " wrapped object");
      ExpectAccepted(
          RunJson(ModuleKind::Native, sig, "{" + w + Repeat(R"({"message":"x"})", 2048) + "}"),
          2048, tag + " wrapped array 2048");
      ExpectRecordLimit(
          RunJson(ModuleKind::Native, sig, "{" + w + Repeat(R"({"message":"x"})", 2049) + "}"),
          tag + " wrapped array 2049");
      // source-field order does not change the decision
      ExpectRecordLimit(
          RunJson(ModuleKind::Native, sig, R"({"message":"x",)" + w + Repeat("{}", 2049) + "}"),
          tag + " wrapper after field");
      ExpectRecordLimit(
          RunJson(ModuleKind::Native, sig, "{" + w + Repeat("{}", 2049) + R"(,"message":"x"})"),
          tag + " wrapper before field");
    }
  }
  // wrappers belong to their own signal only
  ExpectAccepted(
      RunJson(ModuleKind::Native, Signal::Logs, R"({"trace":)" + Repeat("{}", 2049) + "}"), 1,
      "logs ignores trace wrapper");
}

TEST(AdmissionTest, NativeTableSingleArrayAndWrapperNormalization) {
  for (const NativeNames& nn : kNative) {
    const Signal sig = nn.signal;
    const std::string tag = "signal " + std::to_string(static_cast<int>(sig));
    ExpectAccepted(RunTable(ModuleKind::Native, sig, "return {message = 'x'}"), 1, tag + " single");
    ExpectAccepted(RunTable(ModuleKind::Native, sig, "return {}"), 0, tag + " empty");
    ExpectAccepted(RunTable(ModuleKind::Native, sig, NativeTableArray(2048) + "return r(N)"), 2048,
                   tag + " array 2048");
    ExpectRecordLimit(RunTable(ModuleKind::Native, sig, NativeTableArray(2049) + "return r(N)"),
                      tag + " array 2049");
    ExpectAccepted(
        RunTable(ModuleKind::Native, sig, "return {{message = 'x'}, 5, 'y', {}, {message = 'z'}}"),
        2, tag + " malformed members (empty tables never emit)");
    for (const char* wrapper : nn.wrappers) {
      if (wrapper == nullptr) continue;
      const std::string w = std::string(wrapper) + " = ";
      ExpectAccepted(RunTable(ModuleKind::Native, sig, "return {" + w + "{message = 'x'}}"), 1,
                     tag + " wrapped single");
      ExpectAccepted(
          RunTable(ModuleKind::Native, sig, NativeTableArray(2048) + "return {" + w + "r(N)}"),
          2048, tag + " wrapped array 2048");
      ExpectRecordLimit(
          RunTable(ModuleKind::Native, sig, NativeTableArray(2049) + "return {" + w + "r(N)}"),
          tag + " wrapped array 2049");
    }
  }
}

TEST(AdmissionTest, NativeMetricEventIsOneRecordWhateverItsBucketCount) {
  const std::string buckets = Repeat("1", 2049);
  ExpectAccepted(RunJson(ModuleKind::Native, Signal::Metrics,
                         R"({"name":"h","aggregated_histogram":{"buckets":)" + buckets + "}}"),
                 1, "json");
  ExpectAccepted(RunTable(ModuleKind::Native, Signal::Metrics,
                          "local b = {} for i = 1, 2049 do b[i] = {upper_limit = i, count = 1} end "
                          "return {name = 'h', aggregated_histogram = {buckets = b}}"),
                 1, "table");
}

// ---- table traversal safety -------------------------------------------------------------------

TEST(AdmissionTest, TableCountingUsesRawAccessOnly) {
  const std::string chunk =
      "local trap = {__index = function() error('index touched') end, "
      "__len = function() error('len touched') end, "
      "__pairs = function() error('pairs touched') end}\n"
      "local function t(x) return setmetatable(x, trap) end\n"
      "return t({log = t({resourceLogs = t({t({scopeLogs = t({t({logRecords = t({t({}), nil, "
      "t({})})})})})})})})";
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Logs, chunk), 2, "otlp");
  ExpectAccepted(
      RunTable(ModuleKind::Native, Signal::Logs,
               "local trap = {__index = function() error('i') end, __len = function() "
               "error('l') end}\nreturn setmetatable({log = setmetatable({message = 'x'}, "
               "trap)}, trap)"),
      1, "native");
}

TEST(AdmissionTest, CyclicTablesTerminate) {
  ExpectAccepted(RunTable(ModuleKind::Otel, Signal::Logs,
                          "local t = {} t.resourceLogs = {t} t.scopeLogs = {t} t.logRecords = {t} "
                          "t.log = t return t"),
                 1, "otlp cycle (the table itself is the one record element)");
  ExpectAccepted(RunTable(ModuleKind::Native, Signal::Logs,
                          "local t = {message = 'x'} t.log = t t[1] = t return t"),
                 1, "native cycle");
}

TEST(AdmissionTest, TableInputIsAnchoredForTheFollowingLeaves) {
  RawState s;
  Scenario sc;
  const std::string chunk =
      std::string(kLuaHelpers) + "return {resourceLogs = {{scopeLogs = {{logRecords = r(2)}}}}}";
  sc.body = TableBody;
  sc.chunk = chunk.c_str();
  const Observed o = RunScenario(s, sc, [] {});
  ASSERT_EQ(o.status, LUA_OK);
  EXPECT_TRUE(sc.flag);
  EXPECT_EQ(sc.pending_type, LUA_TTABLE) << "the input must sit in AnchorSlot::kInput";
  EXPECT_EQ(AnchorSlotType(s.L(), ModuleKind::Otel, AnchorSlot::kInput), LUA_TBOOLEAN)
      << "and be released by invocation cleanup";
}

// A persistent Lua OOM during the protected traversal is a Memory error, never a raise.
void OomTableBody(OperationContext& ctx, Scenario& sc) {
  if (!LoadTableInput(ctx.L, sc)) {
    return;
  }
  const int idx = lua_gettop(ctx.L);
  // Arming happens inside the call so that building the fixture is not affected.
  sc.alloc->SetPlan(LuaFaultMode::kPersistent, sc.skip);
  const bool ok = AdmitTable(ctx, idx);
  sc.denied = sc.alloc->stats().denied;
  RecordOutcome(ctx, sc, ok);
}

TEST(AdmissionTest, PersistentOomDuringTableCountingIsAMemoryError) {
  RawState s;
  const std::string chunk = std::string(kLuaHelpers) +
                            "return {resource_logs = {{scope_logs = {{log_records = r(40)}}}}}";
  bool saw_failure = false;
  bool saw_success = false;
  for (std::size_t skip = 0; skip < 48 && !saw_success; ++skip) {
    Scenario sc;
    sc.body = OomTableBody;
    sc.chunk = chunk.c_str();
    sc.alloc = &s.alloc();
    sc.skip = skip;
    const Observed o = RunScenario(s, sc, [] {});
    s.alloc().ClearPlan();
    ASSERT_EQ(o.status, LUA_OK) << "skip " << skip;
    ASSERT_FALSE(sc.fixture_error);
    if (sc.flag) {
      EXPECT_EQ(sc.number, 40u);
      saw_success = true;
    } else {
      saw_failure = true;
      EXPECT_EQ(sc.code, ErrorCode::Memory) << "skip " << skip;
      EXPECT_GT(sc.denied, 0u);
      ASSERT_EQ(o.nresults, 3);
      EXPECT_TRUE(o.first_nil);
      EXPECT_EQ(o.last, 0);
    }
  }
  EXPECT_TRUE(saw_failure) << "no fault ever hit the traversal";
  EXPECT_TRUE(saw_success);
}

// ---- byte limit -------------------------------------------------------------------------------

// Valid JSON of exactly `size` bytes for the module kind.
std::string SizedPayload(ModuleKind kind, std::size_t size) {
  const std::string prefix =
      kind == ModuleKind::Otel ? R"({"resourceLogs":[],"pad":")" : R"({"message":")";
  const std::string suffix = R"("})";
  std::string out;
  out.reserve(size);
  out = prefix;
  out.append(size - prefix.size() - suffix.size(), 'x');
  out += suffix;
  return out;
}

std::string LimitMessage() {
  return "payload exceeds " + std::to_string(kMaxPayloadBytes) + "-byte uncompressed JSON limit";
}

TEST(AdmissionTest, ByteLimitBoundariesForBothModules) {
  if (kMaxPayloadBytes < 64) {
    GTEST_SKIP() << "configured limit too small for a meaningful boundary payload";
  }
  for (ModuleKind kind : {ModuleKind::Otel, ModuleKind::Native}) {
    for (const std::size_t size : {kMaxPayloadBytes - 1, kMaxPayloadBytes}) {
      const std::string payload = SizedPayload(kind, size);
      OtelParser parser;
      const Outcome r = RunJson(kind, Signal::Logs, payload, &parser);
      EXPECT_TRUE(r.ok) << size;
      EXPECT_EQ(r.admitted_size, size);
      EXPECT_EQ(r.admitted_data, parser.InputData()) << "staged once into the workspace";
      EXPECT_EQ(r.observed.nresults, 2);
      // the staged view is what the extraction pass iterates (no second copy)
      auto doc = parser.IteratePrepared(simdjson::padded_string_view(
          r.admitted_data, r.admitted_size, r.admitted_size + simdjson::SIMDJSON_PADDING));
      EXPECT_EQ(doc.error(), simdjson::SUCCESS);
    }
    const std::string over = SizedPayload(kind, kMaxPayloadBytes + 1);
    OtelParser fresh;
    const Outcome r = RunJson(kind, Signal::Logs, over, &fresh);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, ErrorCode::PayloadLimit);
    EXPECT_EQ(r.message, LimitMessage());
    ASSERT_EQ(r.observed.nresults, 3);
    EXPECT_TRUE(r.observed.first_nil);
    EXPECT_EQ(r.observed.text, LimitMessage());
    EXPECT_EQ(r.observed.last, 0);
    EXPECT_FALSE(fresh.IsPrepared()) << "rejected before preparation";
    EXPECT_EQ(fresh.InputData(), nullptr) << "and before any copy";
  }
}

TEST(AdmissionTest, PaddedCallerInputIsBorrowedAndBoundedByTheSameLimit) {
  if (kMaxPayloadBytes < 64) {
    GTEST_SKIP() << "configured limit too small";
  }
  for (const std::size_t size : {kMaxPayloadBytes, kMaxPayloadBytes + 1}) {
    const std::string payload = SizedPayload(ModuleKind::Otel, size);
    const simdjson::padded_string padded(payload);
    OtelParser parser;
    g_parser = &parser;
    RawState s;
    Scenario sc;
    sc.body = JsonBody;
    sc.text = padded.data();
    sc.text_len = padded.size();
    sc.text_capacity = padded.size() + simdjson::SIMDJSON_PADDING;
    const Observed o = RunScenario(s, sc, [] {});
    const Outcome r = Collect(sc, o);
    if (size <= kMaxPayloadBytes) {
      EXPECT_TRUE(r.ok);
      EXPECT_EQ(r.admitted_data, padded.data()) << "borrowed, not copied";
      EXPECT_EQ(r.admitted_size, size);
      EXPECT_TRUE(parser.IsPrepared()) << "borrowing must not skip workspace preparation";
    } else {
      EXPECT_FALSE(r.ok);
      EXPECT_EQ(r.code, ErrorCode::PayloadLimit);
      EXPECT_EQ(r.message, LimitMessage());
      EXPECT_FALSE(parser.IsPrepared());
    }
  }

  // Borrowed input is counted like any other: 2049 records on a fresh parser are rejected.
  const Layout over = {{2049}};
  const std::string records = OtlpJson(Signal::Logs, over);
  const simdjson::padded_string padded(records);
  OtelParser parser;
  g_parser = &parser;
  RawState s;
  Scenario sc;
  sc.body = JsonBody;
  sc.text = padded.data();
  sc.text_len = padded.size();
  sc.text_capacity = padded.size() + simdjson::SIMDJSON_PADDING;
  const Outcome r = Collect(sc, RunScenario(s, sc, [] {}));
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.code, ErrorCode::RecordLimit);
  EXPECT_EQ(r.count, kMaxIncomingRecords + 1);
}

TEST(AdmissionTest, EmptyAndNulInputsStayParseErrorsForExtraction) {
  const std::string nul_only(1, '\0');
  const std::string nul_in_object = std::string("{") + '\0' + "}";
  for (const std::string& input : {std::string(), nul_only, nul_in_object}) {
    OtelParser parser;
    const Outcome r = RunJson(ModuleKind::Otel, Signal::Logs, input, &parser);
    EXPECT_TRUE(r.ok) << "admission does not validate syntax";
    EXPECT_EQ(r.count, 0u);
    auto doc = parser.IteratePrepared(simdjson::padded_string_view(
        r.admitted_data, r.admitted_size, r.admitted_size + simdjson::SIMDJSON_PADDING));
    bool parses_as_object = false;
    if (doc.error() == simdjson::SUCCESS) {
      auto obj = doc.get_object();
      if (obj.error() == simdjson::SUCCESS) {
        parses_as_object = true;
        for (auto field : obj.value_unsafe()) {
          if (field.error() != simdjson::SUCCESS) {
            parses_as_object = false;
            break;
          }
        }
      }
    }
    EXPECT_FALSE(parses_as_object) << "must remain an error for extraction";
  }
}

TEST(AdmissionTest, CountAdmissionIsNotTransactionalValidation) {
  // Admission only counts: a malformed later field is still extraction's to report, but if the
  // 2049th record is seen first the event is rejected before anything can be emitted.
  const std::string malformed_later =
      R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{},{"bad":}]}]}]})";
  const Outcome r = RunJson(ModuleKind::Otel, Signal::Logs, malformed_later);
  EXPECT_TRUE(r.ok);
  EXPECT_LE(r.count, 2u);

  std::string over = R"({"resourceLogs":[{"scopeLogs":[{"logRecords":)" + Repeat("{}", 2049);
  over.resize(over.size() - 1);  // drop the closing bracket: truncated document
  over += R"(,{"bad":}]}]}]})";
  ExpectRecordLimit(RunJson(ModuleKind::Otel, Signal::Logs, over),
                    "limit reached before the error");
}

TEST(AdmissionTest, PreparationFailureIsAMemoryErrorAndRetryable) {
  OtelParser parser;
  g_parser = &parser;
  RawState s;
  Scenario sc;
  const std::string payload = R"({"resourceLogs":[]})";
  sc.body = JsonBody;
  sc.text = payload.data();
  sc.text_len = payload.size();
  sc.flag2 = true;
  const Observed o = RunScenario(s, sc, [] {});
  const Outcome r = Collect(sc, o);
  EXPECT_FALSE(r.ok);
  EXPECT_GT(r.denied, 0u);
  EXPECT_EQ(r.code, ErrorCode::Memory);
  ASSERT_EQ(o.nresults, 3);
  EXPECT_EQ(o.text, "memory allocation failed");
  EXPECT_EQ(o.last, 0);
  EXPECT_FALSE(parser.IsPrepared());

  const Outcome again = RunJson(ModuleKind::Otel, Signal::Logs, payload, &parser);
  EXPECT_TRUE(again.ok) << "a later call prepares and succeeds";
  EXPECT_TRUE(parser.IsPrepared());
}

}  // namespace
}  // namespace vector_otel::core
