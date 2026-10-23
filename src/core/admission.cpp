#include "vector_otel/core/admission.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "vector_otel/core/config.hpp"
#include "vector_otel/core/module_state.hpp"
#include "vector_otel/core/protected_lua.hpp"
#include "vector_otel/core/table_reader.hpp"

namespace vector_otel::core {

namespace {

namespace od = simdjson::ondemand;

// Running candidate count. It saturates at kMaxIncomingRecords + 1 and then stops the walk, so
// admission work is bounded by the limit rather than by the size of the event.
struct Walk {
  std::size_t n = 0;
  bool over = false;

  void Add(std::size_t k) noexcept {
    n += k;
    if (n > kMaxIncomingRecords) {
      n = kMaxIncomingRecords + 1;
      over = true;
    }
  }
};

// Field names of the shapes the extractors read. Camel case is tried before snake case.
struct Layout {
  ReadKey resource[2];
  ReadKey scope[2];
  ReadKey leaf[2];             // logRecords / spans / metrics
  ReadKey table_wrappers[2];   // OTLP table input: wrapper that may hold the resource array
  ReadKey native_wrappers[2];  // native input: wrapper that may hold the event(s)
  bool metrics;
};

constexpr Layout kLogsLayout{{"resourceLogs", "resource_logs"},
                             {"scopeLogs", "scope_logs"},
                             {"logRecords", "log_records"},
                             {"log", nullptr},
                             {"log", nullptr},
                             false};
constexpr Layout kTracesLayout{{"resourceSpans", "resource_spans"},
                               {"scopeSpans", "scope_spans"},
                               {"spans", nullptr},
                               {"trace", "log"},
                               {"log", "trace"},
                               false};
constexpr Layout kMetricsLayout{{"resourceMetrics", "resource_metrics"},
                                {"scopeMetrics", "scope_metrics"},
                                {"metrics", nullptr},
                                {"metric", "log"},
                                {"metric", "log"},
                                true};

const Layout& LayoutFor(Signal signal) noexcept {
  switch (signal) {
    case Signal::Traces:
      return kTracesLayout;
    case Signal::Metrics:
      return kMetricsLayout;
    case Signal::Logs:
      break;
  }
  return kLogsLayout;
}

// The extractor reads exactly one metric type per descriptor: the lowest rank that is present as
// an object. Counting only that type is what keeps a descriptor's alternatives from being summed.
struct MetricType {
  ReadKey name;
  ReadKey alt;
  int rank;  // 1-based; lower wins
};
constexpr MetricType kMetricTypes[] = {
    {"gauge", nullptr, 1},     {"sum", nullptr, 2},
    {"histogram", nullptr, 3}, {"exponentialHistogram", "exponential_histogram", 4},
    {"summary", nullptr, 5},
};
constexpr int kMetricRankCount = 5;

int MetricRank(std::string_view key) noexcept {
  for (const MetricType& t : kMetricTypes) {
    if (key == t.name.c_str() || (t.alt.present() && key == t.alt.c_str())) {
      return t.rank;
    }
  }
  return 0;
}

// ---- JSON (simdjson on-demand) ------------------------------------------------------------
//
// Every walker is error tolerant: any simdjson error ends that branch and counting continues with
// what was already seen. The extraction pass reports the error itself, with its own wording.

bool FieldOf(od::object& obj, const char* name, const char* alt, od::value& out) noexcept {
  auto found = obj.find_field_unordered(name);
  if (found.error() == simdjson::NO_SUCH_FIELD && alt != nullptr) {
    found = obj.find_field_unordered(alt);
  }
  return found.get(out) == simdjson::SUCCESS;
}

void CountObjectsInArray(od::array& arr, Walk& w) noexcept {
  for (auto element : arr) {
    if (w.over || element.error() != simdjson::SUCCESS) {
      return;
    }
    od::json_type type{};
    if (element.type().get(type) != simdjson::SUCCESS) {
      return;
    }
    if (type == od::json_type::object) {
      w.Add(1);
    }
  }
}

void CountObjects(od::value& v, Walk& w) noexcept {
  od::array arr;
  if (v.get_array().get(arr) != simdjson::SUCCESS) {
    return;
  }
  CountObjectsInArray(arr, w);
}

// Calls f(object) for every object element of the array `v`; other elements are skipped.
template <class F>
void EachObject(od::value& v, Walk& w, F&& f) noexcept {
  od::array arr;
  if (v.get_array().get(arr) != simdjson::SUCCESS) {
    return;
  }
  for (auto element : arr) {
    if (w.over || element.error() != simdjson::SUCCESS) {
      return;
    }
    od::object obj;
    if (element.get_object().get(obj) != simdjson::SUCCESS) {
      continue;
    }
    f(obj);
  }
}

void CountMetricJson(od::object& metric, Walk& w) noexcept {
  std::size_t counts[kMetricRankCount + 1] = {};
  bool seen[kMetricRankCount + 1] = {};
  for (auto field_result : metric) {
    od::field field;
    if (std::move(field_result).get(field) != simdjson::SUCCESS) {
      break;
    }
    std::string_view key;
    if (field.unescaped_key().get(key) != simdjson::SUCCESS) {
      break;
    }
    const int rank = MetricRank(key);
    if (rank == 0 || seen[rank]) {
      continue;
    }
    od::value value = std::move(field.value());
    od::json_type type{};
    if (value.type().get(type) != simdjson::SUCCESS) {
      break;
    }
    if (type != od::json_type::object) {
      continue;
    }
    seen[rank] = true;
    od::object body;
    if (value.get_object().get(body) != simdjson::SUCCESS) {
      break;
    }
    od::value points;
    if (!FieldOf(body, "dataPoints", "data_points", points)) {
      continue;
    }
    Walk sub;
    CountObjects(points, sub);
    counts[rank] = sub.n;
  }
  for (int rank = 1; rank <= kMetricRankCount; ++rank) {
    if (seen[rank]) {
      w.Add(counts[rank]);
      return;
    }
  }
}

void CountOtlpJson(od::document& doc, const Layout& layout, Walk& w) noexcept {
  od::object root;
  if (doc.get_object().get(root) != simdjson::SUCCESS) {
    return;
  }
  od::value resources;
  if (!FieldOf(root, layout.resource[0].c_str(), layout.resource[1].c_str(), resources)) {
    return;
  }
  EachObject(resources, w, [&](od::object& resource) noexcept {
    od::value scopes;
    if (!FieldOf(resource, layout.scope[0].c_str(), layout.scope[1].c_str(), scopes)) {
      return;
    }
    EachObject(scopes, w, [&](od::object& scope) noexcept {
      od::value leaves;
      if (!FieldOf(scope, layout.leaf[0].c_str(), layout.leaf[1].c_str(), leaves)) {
        return;
      }
      if (!layout.metrics) {
        CountObjects(leaves, w);
        return;
      }
      EachObject(leaves, w, [&](od::object& metric) noexcept { CountMetricJson(metric, w); });
    });
  });
}

// Count candidate records in a single native JSON object: iterate every field once,
// checking every wrapper occurrence and taking the maximum of their record counts.
// Wrapper objects count as 1; wrapper arrays count their object elements.
// Does not recursively expand nested wrappers (depth-one only).
std::size_t CountNativeObjectJson(od::object& obj, const Layout& layout) noexcept {
  std::size_t max_candidates = 1;  // single event fallback

  for (auto field_result : obj) {
    od::field field;
    if (std::move(field_result).get(field) != simdjson::SUCCESS) {
      break;
    }
    std::string_view key;
    if (field.unescaped_key().get(key) != simdjson::SUCCESS) {
      break;
    }

    // Check if this key matches any native wrapper for the current signal.
    bool is_wrapper = false;
    for (const ReadKey& wrapper : layout.native_wrappers) {
      if (wrapper.present() && key == wrapper.c_str()) {
        is_wrapper = true;
        break;
      }
    }

    if (!is_wrapper) {
      continue;
    }

    od::value value = std::move(field.value());
    od::json_type type{};
    if (value.type().get(type) != simdjson::SUCCESS) {
      break;
    }

    std::size_t wrapper_candidates = 0;
    if (type == od::json_type::array) {
      Walk sub;
      CountObjects(value, sub);
      wrapper_candidates = sub.n;
    } else if (type == od::json_type::object) {
      // Wrapper object itself counts as one candidate.
      wrapper_candidates = 1;
    }
    // Non-array, non-object wrappers contribute zero.

    max_candidates = std::max<std::size_t>(max_candidates, wrapper_candidates);
    if (max_candidates > kMaxIncomingRecords) return max_candidates;
  }

  return max_candidates;
}

void CountNativeJson(od::document& doc, const Layout& layout, Walk& w) noexcept {
  od::json_type type{};
  if (doc.type().get(type) != simdjson::SUCCESS) {
    return;
  }
  if (type == od::json_type::array) {
    od::array arr;
    if (doc.get_array().get(arr) != simdjson::SUCCESS) {
      return;
    }
    // For root arrays, apply the per-object logic to each element and sum the maxima.
    for (auto element : arr) {
      if (w.over) {
        return;
      }
      if (element.error() != simdjson::SUCCESS) {
        return;
      }
      od::json_type elem_type{};
      if (element.type().get(elem_type) != simdjson::SUCCESS) {
        return;
      }
      if (elem_type != od::json_type::object) {
        continue;  // skip non-object elements
      }
      od::object elem_obj;
      if (element.get_object().get(elem_obj) != simdjson::SUCCESS) {
        continue;  // skip elements that fail to parse as objects
      }
      std::size_t obj_candidates = CountNativeObjectJson(elem_obj, layout);
      w.Add(obj_candidates);
    }
    return;
  }
  if (type != od::json_type::object) {
    return;
  }
  od::object root;
  if (doc.get_object().get(root) != simdjson::SUCCESS) {
    return;
  }
  std::size_t candidates = CountNativeObjectJson(root, layout);
  w.Add(candidates);
}

void CountJson(OtelParser& parser, ModuleKind module, Signal signal,
               simdjson::padded_string_view view, Walk& w) noexcept {
  od::document doc;
  if (parser.IteratePrepared(view).get(doc) != simdjson::SUCCESS) {
    return;  // extraction reports the parse error
  }
  const Layout& layout = LayoutFor(signal);
  if (module == ModuleKind::Otel) {
    CountOtlpJson(doc, layout, w);
  } else {
    CountNativeJson(doc, layout, w);
  }
}

// ---- Lua tables (raw access, one protected leaf) -----------------------------------------------
//
// Only lua_rawget / lua_rawgeti / lua_rawlen / lua_next are used: no metamethod, no serialisation,
// no user code. Every nested level restores the stack top, and the walk has fixed depth, so a
// cyclic table terminates.

// Calls f(index of the element) for every table element of the array at `arr`.
template <class F>
void EachTable(lua_State* L, int arr, Walk& w, F&& f) {
  if (!lua_istable(L, arr)) {
    return;
  }
  const int top = lua_gettop(L);
  const lua_Unsigned length = lua_rawlen(L, arr);
  for (lua_Unsigned i = 1; i <= length && !w.over; ++i) {
    lua_rawgeti(L, arr, static_cast<lua_Integer>(i));
    if (lua_istable(L, -1)) {
      f(lua_gettop(L));
    }
    lua_settop(L, top);
  }
}

void CountTables(lua_State* L, int arr, Walk& w) {
  EachTable(L, arr, w, [&](int) { w.Add(1); });
}

void CountNonEmptyTables(lua_State* L, int arr, Walk& w) {
  EachTable(L, arr, w, [&](int element) {
    if (!IsLuaTableEmpty(L, element)) {
      w.Add(1);
    }
  });
}

void CountMetricTable(lua_State* L, int metric, Walk& w) {
  const int top = lua_gettop(L);
  for (const MetricType& type : kMetricTypes) {
    for (const ReadKey& name : {type.name, type.alt}) {
      if (!name.present() || !GetTableFieldRaw(L, metric, name)) {
        continue;
      }
      if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        continue;
      }
      const int body = lua_gettop(L);
      if (GetTableFieldRaw(L, body, "dataPoints", "data_points")) {
        CountTables(L, lua_gettop(L), w);
      }
      lua_settop(L, top);
      return;
    }
  }
  lua_settop(L, top);
}

// Leaves the resource array on top of the stack and returns its index (or the root itself).
int ResolveResources(lua_State* L, int root, const Layout& layout) {
  if (GetTableFieldRaw(L, root, layout.resource[0], layout.resource[1])) {
    return lua_gettop(L);
  }
  for (const ReadKey& wrapper : layout.table_wrappers) {
    if (!wrapper.present()) {
      continue;
    }
    const int top = lua_gettop(L);
    if (!GetTableFieldRaw(L, root, wrapper)) {
      continue;
    }
    // Only the first wrapper that is present is consulted, as the extractors do.
    if (lua_istable(L, -1) &&
        GetTableFieldRaw(L, lua_gettop(L), layout.resource[0], layout.resource[1])) {
      return lua_gettop(L);
    }
    lua_settop(L, top);
    break;
  }
  return root;
}

void CountOtlpTable(lua_State* L, int root, const Layout& layout, Walk& w) {
  const int top = lua_gettop(L);
  const int resources = ResolveResources(L, root, layout);
  EachTable(L, resources, w, [&](int resource) {
    if (!GetTableFieldRaw(L, resource, layout.scope[0], layout.scope[1])) {
      return;
    }
    EachTable(L, lua_gettop(L), w, [&](int scope) {
      if (!GetTableFieldRaw(L, scope, layout.leaf[0], layout.leaf[1])) {
        return;
      }
      const int leaves = lua_gettop(L);
      if (!layout.metrics) {
        CountTables(L, leaves, w);
        return;
      }
      EachTable(L, leaves, w, [&](int metric) { CountMetricTable(L, metric, w); });
    });
  });
  lua_settop(L, top);
}

void CountNativeTable(lua_State* L, int root, const Layout& layout, Walk& w) {
  if (lua_rawlen(L, root) > 0) {
    CountNonEmptyTables(L, root, w);
    return;
  }
  const int top = lua_gettop(L);
  // Match extraction's first-non-nil wrapper selection without retrying a
  // secondary wrapper after an empty or scalar primary.
  if (GetTableFieldRaw(L, root, layout.native_wrappers[0], layout.native_wrappers[1])) {
    const int value = lua_gettop(L);
    if (lua_istable(L, value)) {
      if (lua_rawlen(L, value) > 0) {
        CountNonEmptyTables(L, value, w);
        lua_settop(L, top);
        return;
      }
      if (!IsLuaTableEmpty(L, value)) {
        w.Add(1);
        lua_settop(L, top);
        return;
      }
    }
    lua_settop(L, top);
  }
  if (!IsLuaTableEmpty(L, root)) {
    w.Add(1);
  }
}

// Protected leaf: only trivial automatic objects; every call is a raw Lua operation.
int CountLeaf(lua_State* L) {
  OperationContext& ctx = LeafContext(L);
  if (!lua_checkstack(L, 32)) {
    ctx.error.Set(ErrorCode::Stack, kStackText);
    return 0;
  }
  PushAnchor(L, kLeafAnchorIndex, AnchorSlot::kInput);
  const int root = lua_gettop(L);
  Walk w;
  if (lua_istable(L, root)) {
    const Layout& layout = LayoutFor(ctx.signal);
    if (ctx.module == ModuleKind::Otel) {
      CountOtlpTable(L, root, layout, w);
    } else {
      CountNativeTable(L, root, layout, w);
    }
  }
  ctx.incoming_records = w.n;
  return 0;
}

// ---- shared admission steps ----------------------------------------------------------------

bool RejectPayload(OperationContext& ctx) noexcept {
  char text[96];
  const std::size_t length = FormatPayloadLimitText(text, sizeof(text));
  ctx.error.Set(ErrorCode::PayloadLimit, std::string_view(text, length));
  return false;
}

bool RejectRecordLimit(OperationContext& ctx) noexcept {
  char text[96];
  const std::size_t length = FormatRecordLimitText(text, sizeof(text));
  ctx.error.Set(ErrorCode::RecordLimit, std::string_view(text, length));
  return false;
}

bool RejectStaging(OperationContext& ctx, simdjson::error_code error) noexcept {
  if (error == simdjson::CAPACITY) {
    return RejectPayload(ctx);
  }
  if (error == simdjson::MEMALLOC) {
    ctx.error.Set(ErrorCode::Memory, kMemoryText);
  } else {
    ctx.error.Set(ErrorCode::Cpp, kExtractionText);
  }
  return false;
}

bool CountAndDecide(OperationContext& ctx, OtelParser& parser, simdjson::padded_string_view staged,
                    simdjson::padded_string_view& admitted) noexcept {
  // The borrowed (already padded) path does not prepare the workspace by itself.
  if (const simdjson::error_code error = parser.Prepare(); error != simdjson::SUCCESS) {
    return RejectStaging(ctx, error);
  }
  Walk w;
  CountJson(parser, ctx.module, ctx.signal, staged, w);
  ctx.incoming_records = w.n;
  if (w.over) {
    return RejectRecordLimit(ctx);
  }
  admitted = staged;
  return true;
}

}  // namespace

bool AdmitJson(OperationContext& ctx, OtelParser& parser, std::string_view raw,
               simdjson::padded_string_view& admitted) noexcept {
  ctx.incoming_records = 0;
  if (raw.size() > kMaxPayloadBytes) {
    return RejectPayload(ctx);
  }
  auto staged = parser.StageInput(raw);
  if (staged.error() != simdjson::SUCCESS) {
    return RejectStaging(ctx, staged.error());
  }
  return CountAndDecide(ctx, parser, staged.value_unsafe(), admitted);
}

bool AdmitJson(OperationContext& ctx, OtelParser& parser, simdjson::padded_string_view raw,
               simdjson::padded_string_view& admitted) noexcept {
  ctx.incoming_records = 0;
  if (raw.size() > kMaxPayloadBytes) {
    return RejectPayload(ctx);
  }
  auto staged = parser.StageInput(raw);
  if (staged.error() != simdjson::SUCCESS) {
    return RejectStaging(ctx, staged.error());
  }
  return CountAndDecide(ctx, parser, staged.value_unsafe(), admitted);
}

bool AdmitTable(OperationContext& ctx, int input_index) noexcept {
  ctx.incoming_records = 0;
  lua_pushvalue(ctx.L, input_index);
  SetAnchor(ctx.L, ctx.anchor_index, AnchorSlot::kInput);
  if (!RunLuaLeaf(ctx, CountLeaf, nullptr)) {
    return false;
  }
  if (ctx.error.code != ErrorCode::None) {
    return false;
  }
  if (ctx.incoming_records > kMaxIncomingRecords) {
    return RejectRecordLimit(ctx);
  }
  return true;
}

}  // namespace vector_otel::core
