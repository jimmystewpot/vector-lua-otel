// Worker-isolation tests. Vector runs one Lua state per transform worker thread. These tests prove
// that independent states never observe each other (no shared mutable module state), that a
// failure injected into one state's allocator cannot leak into another, and that a state can be
// handed to a different thread between calls (never used by two threads at once).
//
// This target deliberately does NOT replace global operator new (unlike the fault target), so it
// can also be built with ThreadSanitizer.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <lua.hpp>
#include <string>
#include <thread>
#include <vector>

#include "process_fixtures.hpp"
#include "signal_payloads.hpp"

namespace vector_otel::core {
namespace {

using namespace fixtures;
using test::LuaFaultMode;

// ---- payload helper ------------------------------------------------------------------------

Payload BuildPayload(bool native, int signal, bool table, const std::string& tag, int n) {
  const std::string json = BuildJson(native, signal, tag, n);
  return table ? TablePayload(JsonToLuaChunk(json)) : JsonPayload(json);
}

// ---- worker driver -----------------------------------------------------------------------------

constexpr int kWorkers = 8;
constexpr int kRounds = 24;

struct CallOutcome {
  bool ok = false;           // the call returned (count, nil) with the expected count
  bool clean_error = false;  // the call returned (nil, msg, partial)
  bool bad = false;          // anything else, or emitted rows from a foreign worker
  int expected = 0;
  int rows = 0;
};

struct WorkerResult {
  int calls = 0;
  int succeeded = 0;
  int failed_cleanly = 0;
  int recoveries = 0;
  int bad = 0;
  std::string first_bad;
};

// Runs one call and classifies the outcome. `foreign_tags` are tags of other workers that must
// never appear in what this state's callback receives.
CallOutcome RunCall(RawState& s, bool native, int signal, bool table, int worker, int records,
                    bool inject_fault, std::size_t fault_skip) {
  const std::string tag = "wk" + std::to_string(worker) + "x";
  ProcRecorder rec;
  rec.dump = true;
  const Payload payload = BuildPayload(native, signal, table, tag, records);
  CallOutcome out;
  out.expected = records;
  if (inject_fault) {
    // Allocation counts differ between Lua builds, so measure how many growth requests this very
    // call makes and place the denied request somewhere inside that range.
    ProcRecorder probe;
    std::size_t attempts = 0;
    CallProcess(s, native, kSignals[signal], payload, probe, [&](auto&& run) {
      s.alloc().SetPlan(LuaFaultMode::kWindow, static_cast<std::size_t>(-1), 1);
      run();
      attempts = s.alloc().stats().attempts;
      s.alloc().ClearPlan();
    });
    fault_skip %= (attempts > 0 ? attempts : 1);
  }
  // The fault window covers only the process() call itself, not payload preparation.
  const ProcResult r = CallProcess(s, native, kSignals[signal], payload, rec, [&](auto&& run) {
    if (inject_fault) {
      // Two denials: Lua retries a refused request once after an emergency collection.
      s.alloc().SetPlan(LuaFaultMode::kWindow, fault_skip, 2);
    }
    run();
    s.alloc().ClearPlan();
  });
  out.rows = rec.rows;
  if (r.status != LUA_OK) {
    out.bad = true;
    return out;
  }
  if (!r.first_nil && r.count == records && rec.rows == records) {
    out.ok = true;
  } else if (r.first_nil && !r.error.empty() && rec.rows <= records) {
    out.clean_error = true;
  } else {
    out.bad = true;
  }
  // Every event the callback saw belongs to this worker.
  for (const std::string& ev : rec.events) {
    if (ev.find(tag) == std::string::npos) {
      out.bad = true;
    }
    for (int other = 0; other < kWorkers; ++other) {
      if (other != worker && ev.find("wk" + std::to_string(other) + "x") != std::string::npos) {
        out.bad = true;
      }
    }
  }
  return out;
}

void RunWorker(int worker, std::atomic<int>& ready, std::atomic<bool>& go, bool with_faults,
               WorkerResult& result) {
  RawState state;  // own Lua state, own allocator, own fault budget
  ready.fetch_add(1);
  while (!go.load()) {
    std::this_thread::yield();
  }
  for (int round = 0; round < kRounds; ++round) {
    const bool native = ((round + worker) % 2) == 1;
    const int signal = (round + worker / 2) % 3;
    const bool table = ((round / 2 + worker) % 2) == 1;
    const int records = 5 + worker;  // distinct per worker
    const bool inject = with_faults && (round % 4 == 3);
    const std::size_t skip = static_cast<std::size_t>(round % 7);

    CallOutcome o = RunCall(state, native, signal, table, worker, records, inject, skip);
    ++result.calls;
    if (o.ok) ++result.succeeded;
    if (o.clean_error) ++result.failed_cleanly;
    if (o.bad) {
      ++result.bad;
      if (result.first_bad.empty()) {
        result.first_bad = "round " + std::to_string(round) + " native=" + std::to_string(native) +
                           " signal=" + kSignals[signal] + " table=" + std::to_string(table);
      }
    }
    if (inject) {
      // The failed (or surviving) call must leave the SAME state fully usable.
      CallOutcome again = RunCall(state, native, signal, table, worker, records, false, 0);
      ++result.calls;
      if (again.ok) {
        ++result.recoveries;
      } else {
        ++result.bad;
        if (result.first_bad.empty())
          result.first_bad = "no recovery in round " + std::to_string(round);
      }
    }
  }
}

void RunAll(bool with_faults) {
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::vector<WorkerResult> results(kWorkers);
  std::vector<std::thread> threads;
  for (int w = 0; w < kWorkers; ++w) {
    threads.emplace_back(RunWorker, w, std::ref(ready), std::ref(go), with_faults,
                         std::ref(results[w]));
  }
  while (ready.load() < kWorkers) {
    std::this_thread::yield();
  }
  go.store(true);
  for (std::thread& t : threads) t.join();

  int total_failed = 0;
  int total_recovered = 0;
  for (const WorkerResult& r : results) {
    total_failed += r.failed_cleanly;
    total_recovered += r.recoveries;
  }
  if (with_faults) {
    // The injection must actually bite somewhere, and every bite must have been recovered.
    EXPECT_GT(total_failed, 0);
    EXPECT_GE(total_recovered, total_failed);
  }

  for (int w = 0; w < kWorkers; ++w) {
    EXPECT_EQ(results[w].bad, 0) << "worker " << w << ": " << results[w].first_bad;
    EXPECT_GE(results[w].calls, kRounds);
    EXPECT_EQ(results[w].succeeded + results[w].failed_cleanly + results[w].recoveries,
              results[w].calls)
        << "worker " << w;
    if (!with_faults) {
      EXPECT_EQ(results[w].succeeded, kRounds) << "worker " << w;
      EXPECT_EQ(results[w].failed_cleanly, 0) << "worker " << w;
    }
  }
}

TEST(WorkerIsolationTest, EightIndependentStatesMixedSignalsAndModules) { RunAll(false); }

TEST(WorkerIsolationTest, InjectedLuaFaultsStayInTheFailingStateAndAreRecoverable) { RunAll(true); }

TEST(WorkerIsolationTest, OneStateMigratesBetweenThreadsSequentially) {
  RawState state;
  int observed_bad = 0;
  int calls = 0;
  for (int hop = 0; hop < 12; ++hop) {
    // Each hop runs on a fresh thread; the main thread waits (join) before the next hop, so the
    // state is never used by two threads at the same time.
    std::thread t([&, hop] {
      const bool native = (hop % 2) == 1;
      const int signal = hop % 3;
      const bool table = ((hop / 3) % 2) == 1;
      CallOutcome o = RunCall(state, native, signal, table, 0, 3 + hop, false, 0);
      ++calls;
      if (!o.ok) ++observed_bad;
    });
    t.join();
  }
  // And back on the creating thread.
  CallOutcome o = RunCall(state, false, 0, false, 0, 4, false, 0);
  ++calls;
  if (!o.ok) ++observed_bad;
  EXPECT_EQ(observed_bad, 0);
  EXPECT_EQ(calls, 13);
}

TEST(WorkerIsolationTest, ConcurrentStatesDoNotShareParserOrScratchBuffers) {
  // Two states process large, distinct payloads at the same time; any shared scratch would
  // corrupt one side's tag/rows.
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  WorkerResult a, b;
  auto body = [&](int worker, WorkerResult& res) {
    RawState s;
    ready.fetch_add(1);
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < 40; ++i) {
      CallOutcome o = RunCall(s, false, 0, (i % 2) == 1, worker, 1000, false, 0);
      ++res.calls;
      if (o.ok) {
        ++res.succeeded;
      } else {
        ++res.bad;
      }
    }
  };
  std::thread ta(body, 0, std::ref(a));
  std::thread tb(body, 1, std::ref(b));
  while (ready.load() < 2) std::this_thread::yield();
  go.store(true);
  ta.join();
  tb.join();
  EXPECT_EQ(a.bad, 0);
  EXPECT_EQ(b.bad, 0);
  EXPECT_EQ(a.succeeded, 40);
  EXPECT_EQ(b.succeeded, 40);
}

}  // namespace
}  // namespace vector_otel::core
