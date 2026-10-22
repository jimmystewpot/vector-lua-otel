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

Payload PayloadFor(bool table, int resources, int records) {
  return table ? TablePayload(LogsLua(resources, records))
               : JsonPayload(LogsJson(resources, records));
}

void ExpectAnchorsReleased(lua_State* L) {
  for (int i = 0; i < kInvocationSlotCount; ++i) {
    EXPECT_EQ(AnchorSlotType(L, ModuleKind::Otel, static_cast<AnchorSlot>(i)), LUA_TBOOLEAN)
        << "anchor slot " << i << " still holds a value";
  }
}

int CeilDiv(int a, int b) { return (a + b - 1) / b; }

// ---- behaviour --------------------------------------------------------------------------------

TEST(FailureSafetyTest, OtelLogsShareOneResourceAndScopeTablePerResource) {
  for (bool table : {false, true}) {
    SCOPED_TRACE(table ? "table input" : "json input");
    RawState s;
    ProcRecorder rec;
    rec.dump = true;
    const ProcResult r = CallProcess(s, false, "logs", PayloadFor(table, 2, 3), rec);
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.nresults, 2);
    EXPECT_TRUE(r.first_nil == false);
    EXPECT_EQ(r.count, 6);
    EXPECT_EQ(rec.rows, 6);
    EXPECT_EQ(rec.calls, CeilDiv(6, kN) <= 6 ? rec.calls : -1);
    ASSERT_EQ(rec.resource_ptrs.size(), 6u);
    for (int i = 0; i < 6; ++i) {
      ASSERT_NE(rec.resource_ptrs[i], nullptr);
      ASSERT_NE(rec.scope_ptrs[i], nullptr);
    }
    for (int i = 1; i < 3; ++i) {
      EXPECT_EQ(rec.resource_ptrs[0], rec.resource_ptrs[i]) << "ResourceAttributes copied per row";
      EXPECT_EQ(rec.scope_ptrs[0], rec.scope_ptrs[i]) << "ScopeAttributes copied per row";
      EXPECT_EQ(rec.resource_ptrs[3], rec.resource_ptrs[3 + i]);
    }
    EXPECT_NE(rec.resource_ptrs[0], rec.resource_ptrs[3]) << "distinct resources must not share";
    EXPECT_NE(rec.scope_ptrs[0], rec.scope_ptrs[3]);
    ExpectAnchorsReleased(s.L());
  }
}

// processed_at is the wall clock; legacy table mode omits an absent EventName while JSON mode
// writes "" (behaviour preserved from before the staging rewrite).
std::string Normalized(std::string text) {
  for (std::size_t p; (p = text.find(",\"processed_at\":\"")) != std::string::npos;) {
    text.erase(p, text.find('"', p + 17) + 1 - p);
  }
  const std::string empty_name = "\"EventName\":\"\",";
  for (std::size_t e; (e = text.find(empty_name)) != std::string::npos;) {
    text.erase(e, empty_name.size());
  }
  return text;
}

TEST(FailureSafetyTest, OtelLogsJsonAndTableInputsStageIdenticalRows) {
  RawState a;
  RawState b;
  ProcRecorder ja;
  ProcRecorder tb;
  ja.dump = tb.dump = true;
  ASSERT_EQ(CallProcess(a, false, "logs", PayloadFor(false, 2, 2), ja).count, 4);
  ASSERT_EQ(CallProcess(b, false, "logs", PayloadFor(true, 2, 2), tb).count, 4);
  ASSERT_EQ(ja.events.size(), tb.events.size());
  for (std::size_t i = 0; i < ja.events.size(); ++i) {
    EXPECT_EQ(Normalized(ja.events[i]), Normalized(tb.events[i])) << "event " << i;
  }
  ASSERT_FALSE(ja.events.empty());
  EXPECT_NE(ja.events[0].find("\"Body\":\"msg-0\""), std::string::npos) << ja.events[0];
  EXPECT_NE(ja.events[0].find("\"ServiceName\":\"svc-0\""), std::string::npos) << ja.events[0];
  EXPECT_NE(ja.events[0].find("\"LogAttributes\":{\"k\":\"v0\"}"), std::string::npos)
      << ja.events[0];
}

TEST(FailureSafetyTest, OtelLogsEmptyAndSparseInputsAreNotErrors) {
  const char* json[] = {"{}",
                        "{\"resourceLogs\":[]}",
                        "{\"resourceLogs\":[{}]}",
                        "{\"resourceLogs\":[{\"scopeLogs\":[]}]}",
                        "{\"resourceLogs\":[{\"scopeLogs\":[{}]}]}",
                        "{\"resourceLogs\":[{\"scopeLogs\":[{\"logRecords\":[]}]}]}"};
  const char* lua[] = {"return {}", "return {resourceLogs={}}", "return {resourceLogs={{}}}",
                       "return {resourceLogs={{scopeLogs={{}}}}}",
                       "return {resourceLogs={{scopeLogs={{logRecords={}}}}}}"};
  RawState s;
  for (const char* j : json) {
    SCOPED_TRACE(j);
    ProcRecorder rec;
    const ProcResult r = CallProcess(s, false, "logs", JsonPayload(j), rec);
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.nresults, 2);
    EXPECT_FALSE(r.first_nil);
    EXPECT_EQ(r.count, 0);
    EXPECT_EQ(rec.calls, 0);
  }
  for (const char* t : lua) {
    SCOPED_TRACE(t);
    ProcRecorder rec;
    const ProcResult r = CallProcess(s, false, "logs", TablePayload(t), rec);
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.nresults, 2);
    EXPECT_FALSE(r.first_nil);
    EXPECT_EQ(r.count, 0);
    EXPECT_EQ(rec.calls, 0);
  }
  ExpectAnchorsReleased(s.L());
}

TEST(FailureSafetyTest, OtelLogsArgumentErrorsUseTheExactMessages) {
  RawState s;
  ProcRecorder rec;
  Payload none;
  none.kind = PayloadKind::kRaw;
  Payload number = none;
  number.raw_type = LUA_TNUMBER;
  for (const Payload& p : {none, number}) {
    const ProcResult r = CallProcess(s, false, "logs", p, rec);
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.nresults, 3);
    EXPECT_TRUE(r.first_nil);
    EXPECT_EQ(r.error, "payload must be a string or table");
    EXPECT_EQ(r.count, 0);
  }
  const ProcResult r = CallProcess(
      s, false, "logs", JsonPayload("{}"), rec, [](auto&& run) { run(); }, /*pass_emit=*/false);
  ASSERT_EQ(r.status, LUA_OK);
  ASSERT_EQ(r.nresults, 3);
  EXPECT_TRUE(r.first_nil);
  EXPECT_EQ(r.error, "emit callback must be a function");
  EXPECT_EQ(r.count, 0);
  EXPECT_EQ(rec.calls, 0);
}

TEST(FailureSafetyTest, OtelLogsInvalidJsonIsAnErrorWithNothingEmitted) {
  RawState s;
  ProcRecorder rec;
  const ProcResult r = CallProcess(s, false, "logs", JsonPayload("not json"), rec);
  ASSERT_EQ(r.status, LUA_OK);
  ASSERT_EQ(r.nresults, 3);
  EXPECT_TRUE(r.first_nil);
  EXPECT_FALSE(r.error.empty());
  EXPECT_EQ(r.count, 0);
  EXPECT_EQ(rec.calls, 0);
  ExpectAnchorsReleased(s.L());
}

TEST(FailureSafetyTest, OtelLogsRecordLimitIsEnforcedBeforeAnyEmission) {
  RawState s;
  ProcRecorder rec;
  const ProcResult ok = CallProcess(s, false, "logs", PayloadFor(false, 1, 2048), rec);
  ASSERT_EQ(ok.nresults, 2);
  EXPECT_EQ(ok.count, 2048);
  ProcRecorder over;
  const ProcResult r = CallProcess(s, false, "logs", PayloadFor(false, 1, 2049), over);
  ASSERT_EQ(r.status, LUA_OK);
  ASSERT_EQ(r.nresults, 3);
  EXPECT_TRUE(r.first_nil);
  EXPECT_EQ(r.error, "event exceeds 2048-record limit");
  EXPECT_EQ(r.count, 0);
  EXPECT_EQ(over.calls, 0);
  ProcRecorder over_t;
  const ProcResult rt = CallProcess(s, false, "logs", PayloadFor(true, 1, 2049), over_t);
  ASSERT_EQ(rt.nresults, 3);
  EXPECT_EQ(rt.error, "event exceeds 2048-record limit");
  EXPECT_EQ(over_t.calls, 0);
}

TEST(FailureSafetyTest, OtelLogsCallbackFailureReportsRowsFromCompletedCallbacks) {
  for (bool table : {false, true}) {
    SCOPED_TRACE(table ? "table input" : "json input");
    RawState s;
    ProcRecorder rec;
    rec.fail_at_call = 1;  // the second delivery raises
    const ProcResult r = CallProcess(s, false, "logs", PayloadFor(table, 1, 5), rec);
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

TEST(FailureSafetyTest, OtelLogsReentrantProcessIsRejectedWithoutDisturbingTheOuterCall) {
  RawState s;
  ProcRecorder rec;
  rec.reenter = true;
  const ProcResult r = CallProcess(s, false, "logs", PayloadFor(false, 1, 3), rec);
  ASSERT_EQ(r.status, LUA_OK);
  ASSERT_EQ(r.nresults, 2);
  EXPECT_EQ(r.count, 3);
  EXPECT_EQ(rec.reenter_result, 1) << "inner call must return nil first";
  EXPECT_EQ(rec.reenter_message, "re-entrant vector_otel.process() invocation is not permitted");
  ExpectAnchorsReleased(s.L());
}

TEST(FailureSafetyTest, OtelLogsDryRunCountsWithoutEmitting) {
  const std::string json = LogsJson(2, 3);
  const extractors::ExtractionResult r = extractors::ProcessLogs(nullptr, json, 0);
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
    const ProcResult r = CallProcess(s, false, "logs", payload, rec, [&](auto&& run) {
      s.alloc().SetPlan(LuaFaultMode::kPersistent, skip);
      run();
      s.alloc().ClearPlan();
    });
    ASSERT_EQ(r.status, LUA_OK) << "an error escaped the dispatcher frame";
    ExpectAnchorsReleased(s.L());
    for (const void* p : rec.resource_ptrs) {
      EXPECT_NE(p, nullptr) << "a delivered row lost its shared ResourceAttributes table";
    }
    for (const void* p : rec.scope_ptrs) {
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
    const ProcResult ok = CallProcess(s, false, "logs", payload, again);
    ASSERT_EQ(ok.status, LUA_OK);
    ASSERT_EQ(ok.nresults, 2);
    EXPECT_EQ(ok.count, total);
    EXPECT_EQ(again.rows, total);
  }
  EXPECT_TRUE(saw_failure);
  EXPECT_TRUE(saw_success);
}

TEST(FailureSafetyTest, OtelLogsSameStateOomReleasesResourceScopeAndRowJson) {
  SweepLuaAllocations(false, 2, 3);
}

TEST(FailureSafetyTest, OtelLogsSameStateOomReleasesResourceScopeAndRowTable) {
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
    const ProcResult r = CallProcess(s, false, "logs", payload, rec, [&](auto&& run) {
      s.alloc().SetPlan(LuaFaultMode::kWindow, skip, 2);
      run();
      attempts = s.alloc().stats().attempts;
      s.alloc().ClearPlan();
    });
    ASSERT_EQ(r.status, LUA_OK) << "an error escaped the dispatcher frame";
    ExpectAnchorsReleased(s.L());
    for (const void* p : rec.resource_ptrs) {
      EXPECT_NE(p, nullptr) << "a delivered row lost its shared ResourceAttributes table";
    }
    for (const void* p : rec.scope_ptrs) {
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

TEST(FailureSafetyTest, OtelLogsTransientOomNeverDeliversADamagedRowJson) {
  SweepLuaWindows(false);
}

TEST(FailureSafetyTest, OtelLogsTransientOomNeverDeliversADamagedRowTable) {
  SweepLuaWindows(true);
}

// Embedders and the compatibility wrappers run extractors on an arbitrary stack: whatever the
// outcome, the caller's stack depth must be exactly what it was.
int RaisingEmit(lua_State* L) {
  lua_pushliteral(L, "boom");
  return lua_error(L);
}

TEST(FailureSafetyTest, OtelLogsCompatCallsRestoreTheCallersStackOnFailure) {
  RawState s;
  lua_State* L = s.L();
  ASSERT_EQ(luaopen_vector_otel(L), 1);
  lua_pop(L, 1);
  for (bool table : {false, true}) {
    SCOPED_TRACE(table ? "table input" : "json input");
    if (table) {
      const std::string chunk = LogsLua(2, 3);
      ASSERT_EQ(luaL_loadstring(L, chunk.c_str()), LUA_OK);
      ASSERT_EQ(lua_pcall(L, 0, 1, 0), LUA_OK);
    } else {
      lua_pushstring(L, LogsJson(2, 3).c_str());
    }
    lua_pushcfunction(L, RaisingEmit);
    const int top = lua_gettop(L);
    const extractors::ExtractionResult r =
        table ? extractors::ProcessLogsTable(L, top - 1, top)
              : extractors::ProcessLogs(L, lua_tostring(L, top - 1), top);
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
    const ProcResult r = CallProcess(s, false, "logs", payload, rec, [&](auto&& run) {
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

TEST(FailureSafetyTest, OtelLogsPersistentCppOomPreservesCompletedRowsJson) {
  SweepCppAllocations(false);
}

TEST(FailureSafetyTest, OtelLogsPersistentCppOomPreservesCompletedRowsTable) {
  SweepCppAllocations(true);
}

// After warm-up the extraction loop must not touch the C++ heap at all: refuse every allocation
// from the first one and the whole call still succeeds.
TEST(FailureSafetyTest, OtelLogsWarmExtractionNeverAllocatesFromTheCppHeap) {
  for (bool table : {false, true}) {
    SCOPED_TRACE(table ? "table input" : "json input");
    RawState s;
    const Payload payload = PayloadFor(table, 2, 3);
    ProcRecorder warm;
    ASSERT_EQ(CallProcess(s, false, "logs", payload, warm).count, 6);
    ProcRecorder rec;
    std::size_t attempts = 0;
    const ProcResult r = CallProcess(s, false, "logs", payload, rec, [&](auto&& run) {
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

}  // namespace
}  // namespace vector_otel::core
