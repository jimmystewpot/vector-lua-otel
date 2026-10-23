// Extraction latency benchmark (opt-in: -DBUILD_EXTRACTION_BENCHMARKS=ON).
//
// Drives `<module>.<signal>.process(payload, emit)` exactly as a Vector Lua transform would, in
// one standard Lua 5.4 state, with a counting callback that buffers nothing. The fixture comes
// from the same deterministic builders the tests use (tests/support/signal_payloads.hpp), so a
// baseline checkout and the final tree can be compared with identical inputs.
//
// Timed region: the lua_pcall of process() only. Payload construction, table-input loading,
// module loading and (with --warm) all preparation happen before timing starts. With --cold the
// very first call is timed separately and reported as `cold_first_call_ns`.
//
// Output: one JSON object (or CSV header+row with --format csv) on stdout. Allocation domains
// are NOT measured by this tool: allocator instrumentation perturbs CPU timings, so use an
// allocation-counting run of the fault/test harness for that and report it separately.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <lua.hpp>
#include <string>
#include <vector>

#include "signal_payloads.hpp"
// A pre-limit baseline checkout has neither header; the same driver then reports the limit and
// batch size as 0 ("not applicable") so baseline and final share one fixture/driver contract.
#if __has_include("vector_otel/core/process_runner.hpp")
#include "vector_otel/core/config.hpp"
#include "vector_otel/core/process_runner.hpp"
namespace bench_cfg {
constexpr std::size_t kMaxPayload = vector_otel::core::kMaxPayloadBytes;
constexpr std::size_t kMaxRecords = vector_otel::core::kMaxIncomingRecords;
constexpr int kBatch = 1;
}  // namespace bench_cfg
#else
namespace bench_cfg {
constexpr std::size_t kMaxPayload = 0;
constexpr std::size_t kMaxRecords = 2048;
constexpr int kBatch = 0;
}  // namespace bench_cfg
#endif

extern "C" {
int luaopen_vector_otel(lua_State* L);
int luaopen_vector_native(lua_State* L);
}

namespace {

namespace fx = vector_otel::core::fixtures;

struct Options {
  bool native = false;
  int signal = 0;
  bool table = false;
  int records = 64;
  int iterations = 1000;
  int warmup = 100;
  std::uint64_t seed = 1;
  std::size_t want_bytes = 0;  // 0 = natural size; else pad the tag to reach about this size
  bool cold = false;
  bool csv = false;
  bool allow_reject = false;
  std::string sha = "unknown";
};

void Usage(std::FILE* out) {
  std::fprintf(
      out,
      "usage: vector_otel_latency_bench [options]\n"
      "  --module otel|native      module under test (default otel)\n"
      "  --signal logs|traces|metrics\n"
      "  --mode json|table         input form (default json)\n"
      "  --records N               records per call, 1..%zu (default 64)\n"
      "  --bytes B                 approximate payload size in bytes (pads the tag; default: "
      "none)\n"
      "  --iterations N            timed calls (default 1000)\n"
      "  --warmup N                untimed calls before timing (default 100)\n"
      "  --cold                    time the first call separately (skips warm-up before it)\n"
      "  --seed S                  fixture seed (default 1)\n"
      "  --sha GIT_SHA             recorded in the output\n"
      "  --format json|csv         output format (default json)\n"
      "  --allow-reject            allow payloads above the configured limit (measures rejection)\n"
      "  --help\n",
      bench_cfg::kMaxRecords);
}

[[noreturn]] void Fail(const char* what, const char* detail) {
  std::fprintf(stderr, "vector_otel_latency_bench: %s: %s\n", what, detail);
  std::exit(2);
}

long ParseInt(const char* flag, const char* text, long lo, long hi) {
  char* end = nullptr;
  const long v = std::strtol(text, &end, 10);
  if (text[0] == '\0' || *end != '\0' || v < lo || v > hi) {
    std::fprintf(stderr,
                 "vector_otel_latency_bench: %s must be an integer in %ld..%ld (got '%s')\n", flag,
                 lo, hi, text);
    std::exit(2);
  }
  return v;
}

Options Parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) Fail("missing value for", a.c_str());
      return argv[++i];
    };
    if (a == "--help" || a == "-h") {
      Usage(stdout);
      std::exit(0);
    } else if (a == "--module") {
      const std::string v = value();
      if (v != "otel" && v != "native") Fail("--module must be otel or native", v.c_str());
      o.native = v == "native";
    } else if (a == "--signal") {
      const std::string v = value();
      if (v == "logs")
        o.signal = 0;
      else if (v == "traces")
        o.signal = 1;
      else if (v == "metrics")
        o.signal = 2;
      else
        Fail("--signal must be logs, traces or metrics", v.c_str());
    } else if (a == "--mode") {
      const std::string v = value();
      if (v != "json" && v != "table") Fail("--mode must be json or table", v.c_str());
      o.table = v == "table";
    } else if (a == "--records") {
      o.records = static_cast<int>(
          ParseInt("--records", value(), 1, static_cast<long>(bench_cfg::kMaxRecords)));
    } else if (a == "--bytes") {
      o.want_bytes = static_cast<std::size_t>(ParseInt("--bytes", value(), 1, 1L << 30));
    } else if (a == "--iterations") {
      o.iterations = static_cast<int>(ParseInt("--iterations", value(), 1, 100000000));
    } else if (a == "--warmup") {
      o.warmup = static_cast<int>(ParseInt("--warmup", value(), 0, 100000000));
    } else if (a == "--seed") {
      o.seed = static_cast<std::uint64_t>(ParseInt("--seed", value(), 0, 2147483647L));
    } else if (a == "--sha") {
      o.sha = value();
    } else if (a == "--format") {
      const std::string v = value();
      if (v != "json" && v != "csv") Fail("--format must be json or csv", v.c_str());
      o.csv = v == "csv";
    } else if (a == "--cold") {
      o.cold = true;
    } else if (a == "--allow-reject") {
      o.allow_reject = true;
    } else {
      Usage(stderr);
      Fail("unknown option", a.c_str());
    }
  }
  return o;
}

struct Counters {
  long callbacks = 0;
};

int CountingEmit(lua_State* L) {
  auto* c = static_cast<Counters*>(lua_touserdata(L, lua_upvalueindex(1)));
  ++c->callbacks;
  return 0;
}

struct Sample {
  bool ok = false;
  long long count = -1;
};

double Quantile(const std::vector<std::int64_t>& sorted, double q) {
  if (sorted.empty()) return 0;
  const double pos = q * static_cast<double>(sorted.size() - 1);
  const std::size_t lo = static_cast<std::size_t>(pos);
  const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
  const double frac = pos - static_cast<double>(lo);
  return static_cast<double>(sorted[lo]) * (1.0 - frac) + static_cast<double>(sorted[hi]) * frac;
}

}  // namespace

int main(int argc, char** argv) {
  Options o = Parse(argc, argv);

  std::string tag = "sd" + std::to_string(o.seed);
  std::string json = fx::BuildJson(o.native, o.signal, tag, o.records);
  if (o.want_bytes != 0) {
    const std::size_t want = o.want_bytes;
    const std::size_t base = json.size();
    if (want > base) {
      // Grow by padding the tag; every record carries it at least once.
      const std::size_t extra = (want - base) / static_cast<std::size_t>(o.records) + 1;
      tag.append(extra, 'x');
      json = fx::BuildJson(o.native, o.signal, tag, o.records);
    }
  }
  const std::size_t payload_bytes = json.size();
  if (bench_cfg::kMaxPayload != 0 && payload_bytes > bench_cfg::kMaxPayload && !o.allow_reject) {
    std::fprintf(stderr,
                 "vector_otel_latency_bench: fixture is %zu bytes, above the configured %zu-byte "
                 "limit; it would be rejected (use --allow-reject to measure rejection)\n",
                 payload_bytes, bench_cfg::kMaxPayload);
    return 2;
  }

  lua_State* L = luaL_newstate();
  if (L == nullptr) Fail("cannot create Lua state", "out of memory");
  luaL_openlibs(L);
  if (o.native) {
    luaopen_vector_native(L);
  } else {
    luaopen_vector_otel(L);
  }
  lua_getfield(L, -1, fx::kSignals[o.signal]);
  lua_getfield(L, -1, "process");
  const int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_settop(L, 0);

  Counters counters;
  lua_pushlightuserdata(L, &counters);
  lua_pushcclosure(L, &CountingEmit, 1);
  const int emit_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  int input_ref = LUA_NOREF;
  if (o.table) {
    const std::string chunk = fx::JsonToLuaChunk(json);
    if (luaL_loadstring(L, chunk.c_str()) != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
      Fail("cannot build table fixture", lua_tostring(L, -1));
    }
  } else {
    lua_pushlstring(L, json.data(), json.size());
  }
  input_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  long errors = 0;
  long long delivered = 0;
  auto call = [&]() -> std::int64_t {
    lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, input_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, emit_ref);
    const auto t0 = std::chrono::steady_clock::now();
    const int status = lua_pcall(L, 2, LUA_MULTRET, 0);
    const auto t1 = std::chrono::steady_clock::now();
    if (status != LUA_OK) {
      ++errors;
    } else if (lua_isnil(L, 1)) {
      ++errors;  // (nil, message, partial)
    } else {
      delivered += lua_tointeger(L, 1);
    }
    lua_settop(L, 0);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  };

  std::int64_t cold_ns = -1;
  if (o.cold) {
    cold_ns = call();
  }
  for (int i = 0; i < o.warmup; ++i) call();
  // Counters describe the timed region only.
  const long warm_errors = errors;
  const long long warm_delivered = delivered;
  const long warm_callbacks = counters.callbacks;
  std::vector<std::int64_t> samples;
  samples.reserve(static_cast<std::size_t>(o.iterations));
  for (int i = 0; i < o.iterations; ++i) samples.push_back(call());
  std::sort(samples.begin(), samples.end());
  const long timed_errors = errors - warm_errors;
  const long long timed_delivered = delivered - warm_delivered;
  const long timed_callbacks = counters.callbacks - warm_callbacks;

  double total_ns = 0;
  for (std::int64_t s : samples) total_ns += static_cast<double>(s);
  const double mean_ns = total_ns / static_cast<double>(samples.size());
  const double rows_per_sec = static_cast<double>(timed_delivered) / (total_ns / 1e9);
  const double mb_per_sec =
      static_cast<double>(payload_bytes) * o.iterations / (total_ns / 1e9) / (1024.0 * 1024.0);

#if defined(__aarch64__) || defined(_M_ARM64)
  const char* arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  const char* arch = "amd64";
#else
  const char* arch = "other";
#endif
  static const char* const kSignalNames[3] = {"logs", "traces", "metrics"};
  const char* compiler =
#if defined(__VERSION__)
      __VERSION__;
#else
      "unknown";
#endif

  if (o.csv) {
    std::printf(
        "module,signal,mode,records,payload_bytes,seed,sha,arch,max_payload_bytes,"
        "max_batch_records,emit_batch_size,"
        "iterations,errors,callbacks,rows,cold_first_call_ns,mean_ns,p50_ns,p95_ns,p99_ns,"
        "p99_9_ns,max_ns,rows_per_sec,payload_mib_per_sec\n");
    std::printf(
        "%s,%s,%s,%d,%zu,%llu,%s,%s,%zu,%zu,%d,%d,%ld,%ld,%lld,%lld,%.0f,%.0f,%.0f,%.0f,%.0f,%lld,"
        "%.0f,%.3f\n",
        o.native ? "native" : "otel", kSignalNames[o.signal], o.table ? "table" : "json", o.records,
        payload_bytes, static_cast<unsigned long long>(o.seed), o.sha.c_str(), arch,
        bench_cfg::kMaxPayload, bench_cfg::kMaxRecords, bench_cfg::kBatch, o.iterations,
        timed_errors, timed_callbacks, timed_delivered, static_cast<long long>(cold_ns), mean_ns,
        Quantile(samples, 0.50), Quantile(samples, 0.95), Quantile(samples, 0.99),
        Quantile(samples, 0.999), static_cast<long long>(samples.back()), rows_per_sec, mb_per_sec);
  } else {
    std::printf(
        "{\"module\":\"%s\",\"signal\":\"%s\",\"mode\":\"%s\",\"records\":%d,"
        "\"payload_bytes\":%zu,\"seed\":%llu,\"sha\":\"%s\",\"compiler\":\"%s\",\"arch\":\"%s\","
        "\"max_payload_bytes\":%zu,\"max_batch_records\":%zu,\"emit_batch_size\":%d,"
        "\"iterations\":%d,\"errors\":%ld,\"callbacks\":%ld,\"rows\":%lld,"
        "\"cold_first_call_ns\":%lld,"
        "\"latency_ns\":{\"mean\":%.0f,\"p50\":%.0f,\"p95\":%.0f,\"p99\":%.0f,\"p99_9\":%.0f,"
        "\"max\":%lld},\"rows_per_sec\":%.0f,\"payload_mib_per_sec\":%.3f,"
        "\"allocations\":null}\n",
        o.native ? "native" : "otel", kSignalNames[o.signal], o.table ? "table" : "json", o.records,
        payload_bytes, static_cast<unsigned long long>(o.seed), o.sha.c_str(), compiler, arch,
        bench_cfg::kMaxPayload, bench_cfg::kMaxRecords, bench_cfg::kBatch, o.iterations,
        timed_errors, timed_callbacks, timed_delivered, static_cast<long long>(cold_ns), mean_ns,
        Quantile(samples, 0.50), Quantile(samples, 0.95), Quantile(samples, 0.99),
        Quantile(samples, 0.999), static_cast<long long>(samples.back()), rows_per_sec, mb_per_sec);
  }
  lua_close(L);
  return (timed_errors == 0 && timed_delivered == static_cast<long long>(o.records) * o.iterations)
             ? 0
             : 1;
}
