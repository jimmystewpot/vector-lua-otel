#include "vector_otel/extractors/logs_extractor.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "vector_otel/core/fast_json_writer.hpp"
#include "vector_otel/core/lua_bridge.hpp"
#include "vector_otel/core/otlp_common.hpp"
#include "vector_otel/core/parser.hpp"
#include "vector_otel/core/scratch.hpp"
#include "vector_otel/core/table_reader.hpp"
#include "vector_otel/core/timestamp.hpp"

namespace vector_otel::extractors {

namespace {

using core::ScopedLuaRef;

std::string_view ExtractBodyView(simdjson::ondemand::value& body_val,
                                 core::FastJsonWriter& writer) {
  auto type_res = body_val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return "";
  }
  auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::object) {
    auto b_obj_res = body_val.get_object();
    if (b_obj_res.error() != simdjson::SUCCESS) {
      return "";
    }
    auto b_obj = b_obj_res.value();
    writer.Reset();
    writer.StartObject();
    for (auto field_res : b_obj) {
      if (field_res.error() != simdjson::SUCCESS) continue;
      auto k_res = field_res.unescaped_key();
      if (k_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = k_res.value();
      auto fv_res = field_res.value();
      if (fv_res.error() != simdjson::SUCCESS) continue;
      auto fv = fv_res.value();

      if (k == "stringValue" || k == "string_value") {
        auto s_res = fv.get_string();
        if (s_res.error() == simdjson::SUCCESS) {
          return s_res.value();
        }
      }
      writer.Key(k);
      auto raw_res = fv.raw_json();
      if (raw_res.error() == simdjson::SUCCESS) {
        writer.RawJson(raw_res.value());
      } else {
        writer.Null();
      }
    }
    writer.EndObject();
    return writer.Str();
  } else if (type == simdjson::ondemand::json_type::string) {
    auto s_res = body_val.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      return s_res.value();
    }
    return "";
  } else if (type == simdjson::ondemand::json_type::null) {
    return "";
  } else {
    auto raw_res = body_val.raw_json();
    if (raw_res.error() == simdjson::SUCCESS) {
      return raw_res.value();
    }
    return "";
  }
}

}  // namespace

ExtractionResult ProcessLogs(lua_State* L, std::string_view raw_json, int emit_idx) {
  if (L != nullptr && !lua_checkstack(L, 32)) {
    return ExtractionResult{false, "Lua stack overflow in ProcessLogs", 0};
  }
  auto& parser = core::OtelParser::Instance();
  static thread_local core::FastJsonWriter writer;
  static thread_local core::FastJsonWriter body_writer;

  writer.Reset();
  body_writer.Reset();
  struct WriterTrimGuard {
    explicit WriterTrimGuard(core::FastJsonWriter& writer1, core::FastJsonWriter& writer2)
        : w1(writer1), w2(writer2) {}
    WriterTrimGuard(const WriterTrimGuard&) = delete;
    WriterTrimGuard& operator=(const WriterTrimGuard&) = delete;
    ~WriterTrimGuard() noexcept {
      w1.Trim();
      w2.Trim();
      core::ResetServiceNameScratch();
    }
    core::FastJsonWriter& w1;
    core::FastJsonWriter& w2;
  } trim_guard{writer, body_writer};

  core::LuaEmitter emitter(L, emit_idx);

  try {
    auto doc_res = parser.Iterate(raw_json);
    if (doc_res.error() != simdjson::SUCCESS) {
      return ExtractionResult{false, std::string(simdjson::error_message(doc_res.error())), 0};
    }

    auto doc = std::move(doc_res.value());
    auto doc_obj_res = doc.get_object();
    if (doc_obj_res.error() != simdjson::SUCCESS) {
      return ExtractionResult{false, std::string(simdjson::error_message(doc_obj_res.error())), 0};
    }
    auto doc_obj = doc_obj_res.value();

    std::array<char, 32> proc_buf;
    core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
    std::string_view processed_at(proc_buf.data(), 30);

    auto rl_res = doc_obj.find_field_unordered("resourceLogs");
    if (rl_res.error() == simdjson::NO_SUCH_FIELD) {
      rl_res = doc_obj.find_field_unordered("resource_logs");
    }
    if (rl_res.error() == simdjson::NO_SUCH_FIELD) {
      emitter.Flush();
      return ExtractionResult{true, "", emitter.EmittedCount()};
    }
    if (rl_res.error() != simdjson::SUCCESS) {
      return ExtractionResult{false, std::string(simdjson::error_message(rl_res.error())),
                              emitter.EmittedCount()};
    }

    auto rl_val = rl_res.value();
    auto rl_arr_res = rl_val.get_array();
    if (rl_arr_res.error() != simdjson::SUCCESS) {
      if (rl_arr_res.error() == simdjson::INCORRECT_TYPE) {
        emitter.Flush();
        return ExtractionResult{true, "", emitter.EmittedCount()};
      }
      return ExtractionResult{false, std::string(simdjson::error_message(rl_arr_res.error())),
                              emitter.EmittedCount()};
    }

    for (auto rl_elem_res : rl_arr_res.value()) {
      if (rl_elem_res.error() != simdjson::SUCCESS) {
        return ExtractionResult{false, std::string(simdjson::error_message(rl_elem_res.error())),
                                emitter.EmittedCount()};
      }
      auto rl_val_item = rl_elem_res.value();
      auto rl_obj_res = rl_val_item.get_object();
      if (rl_obj_res.error() != simdjson::SUCCESS) {
        if (rl_obj_res.error() == simdjson::INCORRECT_TYPE) {
          continue;
        }
        return ExtractionResult{false, std::string(simdjson::error_message(rl_obj_res.error())),
                                emitter.EmittedCount()};
      }
      auto rl_obj = rl_obj_res.value();

      // 1. Resource schema URL
      std::string_view resource_schema_url;
      auto res_schema_res = rl_obj.find_field_unordered("schemaUrl");
      if (res_schema_res.error() == simdjson::NO_SUCH_FIELD) {
        res_schema_res = rl_obj.find_field_unordered("schema_url");
      }
      if (res_schema_res.error() == simdjson::SUCCESS) {
        auto s_res = res_schema_res.value().get_string();
        if (s_res.error() == simdjson::SUCCESS) {
          resource_schema_url = s_res.value();
        }
      } else if (res_schema_res.error() != simdjson::NO_SUCH_FIELD) {
        return ExtractionResult{false, std::string(simdjson::error_message(res_schema_res.error())),
                                emitter.EmittedCount()};
      }

      // 2. Resource service name and attributes
      std::string_view service_name = "unknown_service";
      ScopedLuaRef res_attrs_scoped(L, LUA_NOREF);

      auto res_res = rl_obj.find_field_unordered("resource");
      if (res_res.error() == simdjson::SUCCESS) {
        auto r_val = res_res.value();
        auto r_obj_res = r_val.get_object();
        if (r_obj_res.error() == simdjson::SUCCESS) {
          auto r_obj = r_obj_res.value();
          if (L != nullptr) {
            auto attrs_res = r_obj.find_field_unordered("attributes");
            if (attrs_res.error() == simdjson::SUCCESS) {
              auto attrs_val = attrs_res.value();
              core::PushAttributesTable(L, attrs_val, writer, &service_name);
            } else if (attrs_res.error() == simdjson::NO_SUCH_FIELD) {
              lua_newtable(L);
            } else {
              return ExtractionResult{false,
                                      std::string(simdjson::error_message(attrs_res.error())),
                                      emitter.EmittedCount()};
            }
            res_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
          } else {
            service_name = core::ExtractServiceName(r_obj);
          }
        } else if (r_obj_res.error() != simdjson::INCORRECT_TYPE) {
          return ExtractionResult{false, std::string(simdjson::error_message(r_obj_res.error())),
                                  emitter.EmittedCount()};
        }
      } else if (res_res.error() != simdjson::NO_SUCH_FIELD) {
        return ExtractionResult{false, std::string(simdjson::error_message(res_res.error())),
                                emitter.EmittedCount()};
      }
      if (L != nullptr && res_attrs_scoped.get() == LUA_NOREF) {
        lua_newtable(L);
        res_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
      }

      // 3. Iterate scopeLogs
      auto sl_res = rl_obj.find_field_unordered("scopeLogs");
      if (sl_res.error() == simdjson::NO_SUCH_FIELD) {
        sl_res = rl_obj.find_field_unordered("scope_logs");
      }
      if (sl_res.error() == simdjson::NO_SUCH_FIELD) {
        continue;
      }
      if (sl_res.error() != simdjson::SUCCESS) {
        return ExtractionResult{false, std::string(simdjson::error_message(sl_res.error())),
                                emitter.EmittedCount()};
      }

      auto sl_val = sl_res.value();
      auto sl_arr_res = sl_val.get_array();
      if (sl_arr_res.error() != simdjson::SUCCESS) {
        if (sl_arr_res.error() == simdjson::INCORRECT_TYPE) {
          continue;
        }
        return ExtractionResult{false, std::string(simdjson::error_message(sl_arr_res.error())),
                                emitter.EmittedCount()};
      }

      for (auto sl_elem_res : sl_arr_res.value()) {
        if (sl_elem_res.error() != simdjson::SUCCESS) {
          return ExtractionResult{false, std::string(simdjson::error_message(sl_elem_res.error())),
                                  emitter.EmittedCount()};
        }
        auto sl_val_item = sl_elem_res.value();
        auto sl_obj_res = sl_val_item.get_object();
        if (sl_obj_res.error() != simdjson::SUCCESS) {
          if (sl_obj_res.error() == simdjson::INCORRECT_TYPE) {
            continue;
          }
          return ExtractionResult{false, std::string(simdjson::error_message(sl_obj_res.error())),
                                  emitter.EmittedCount()};
        }
        auto sl_obj = sl_obj_res.value();

        // Scope schema URL
        std::string_view scope_schema_url;
        auto sl_schema_res = sl_obj.find_field_unordered("schemaUrl");
        if (sl_schema_res.error() == simdjson::NO_SUCH_FIELD) {
          sl_schema_res = sl_obj.find_field_unordered("schema_url");
        }
        if (sl_schema_res.error() == simdjson::SUCCESS) {
          auto s_res = sl_schema_res.value().get_string();
          if (s_res.error() == simdjson::SUCCESS) {
            scope_schema_url = s_res.value();
          }
        } else if (sl_schema_res.error() != simdjson::NO_SUCH_FIELD) {
          return ExtractionResult{false,
                                  std::string(simdjson::error_message(sl_schema_res.error())),
                                  emitter.EmittedCount()};
        }

        // Scope metadata
        std::string_view scope_name;
        std::string_view scope_version;
        ScopedLuaRef scope_attrs_scoped(L, LUA_NOREF);

        auto scope_res = sl_obj.find_field_unordered("scope");
        if (scope_res.error() == simdjson::SUCCESS) {
          auto scope_val = scope_res.value();
          auto s_obj_res = scope_val.get_object();
          if (s_obj_res.error() == simdjson::SUCCESS) {
            auto s_obj = s_obj_res.value();
            scope_name = core::ExtractStringField(s_obj, "name", "");
            scope_version = core::ExtractStringField(s_obj, "version", "");
            if (L != nullptr) {
              auto s_attrs_res = s_obj.find_field_unordered("attributes");
              if (s_attrs_res.error() == simdjson::SUCCESS) {
                auto s_attrs_val = s_attrs_res.value();
                core::PushAttributesTable(L, s_attrs_val, writer);
              } else if (s_attrs_res.error() == simdjson::NO_SUCH_FIELD) {
                lua_newtable(L);
              } else {
                return ExtractionResult{false,
                                        std::string(simdjson::error_message(s_attrs_res.error())),
                                        emitter.EmittedCount()};
              }
              scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
            }
          } else if (s_obj_res.error() != simdjson::INCORRECT_TYPE) {
            return ExtractionResult{false, std::string(simdjson::error_message(s_obj_res.error())),
                                    emitter.EmittedCount()};
          }
        } else if (scope_res.error() != simdjson::NO_SUCH_FIELD) {
          return ExtractionResult{false, std::string(simdjson::error_message(scope_res.error())),
                                  emitter.EmittedCount()};
        }
        if (L != nullptr && scope_attrs_scoped.get() == LUA_NOREF) {
          lua_newtable(L);
          scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
        }

        // 4. Iterate logRecords
        auto recs_res = sl_obj.find_field_unordered("logRecords");
        if (recs_res.error() == simdjson::NO_SUCH_FIELD) {
          recs_res = sl_obj.find_field_unordered("log_records");
        }
        if (recs_res.error() == simdjson::NO_SUCH_FIELD) {
          continue;
        }
        if (recs_res.error() != simdjson::SUCCESS) {
          return ExtractionResult{false, std::string(simdjson::error_message(recs_res.error())),
                                  emitter.EmittedCount()};
        }

        auto recs_val = recs_res.value();
        auto recs_arr_res = recs_val.get_array();
        if (recs_arr_res.error() != simdjson::SUCCESS) {
          if (recs_arr_res.error() == simdjson::INCORRECT_TYPE) {
            continue;
          }
          return ExtractionResult{false, std::string(simdjson::error_message(recs_arr_res.error())),
                                  emitter.EmittedCount()};
        }

        for (auto rec_elem_res : recs_arr_res.value()) {
          if (rec_elem_res.error() != simdjson::SUCCESS) {
            return ExtractionResult{false,
                                    std::string(simdjson::error_message(rec_elem_res.error())),
                                    emitter.EmittedCount()};
          }
          auto rec_val_item = rec_elem_res.value();
          auto rec_obj_res = rec_val_item.get_object();
          if (rec_obj_res.error() != simdjson::SUCCESS) {
            if (rec_obj_res.error() == simdjson::INCORRECT_TYPE) {
              continue;
            }
            return ExtractionResult{false,
                                    std::string(simdjson::error_message(rec_obj_res.error())),
                                    emitter.EmittedCount()};
          }
          auto rec_obj = rec_obj_res.value();

          uint64_t t_ns = 0;
          uint64_t obs_t_ns = 0;
          std::string_view trace_id;
          std::string_view span_id;
          int64_t flags = 0;
          std::string_view sev_text;
          int64_t sev_num = 0;
          std::string_view body_sv;
          std::string_view event_name;
          bool has_log_attrs = false;
          ScopedLuaRef log_attrs_scoped(L, LUA_NOREF);

          core::IdHexBuffer trace_id_buf;
          core::IdHexBuffer span_id_buf;

          for (auto field_res : rec_obj) {
            if (field_res.error() != simdjson::SUCCESS) {
              return ExtractionResult{false,
                                      std::string(simdjson::error_message(field_res.error())),
                                      emitter.EmittedCount()};
            }
            auto k_res = field_res.unescaped_key();
            if (k_res.error() != simdjson::SUCCESS) {
              return ExtractionResult{false, std::string(simdjson::error_message(k_res.error())),
                                      emitter.EmittedCount()};
            }
            std::string_view k = k_res.value();
            auto fv_res = field_res.value();
            if (fv_res.error() != simdjson::SUCCESS) {
              return ExtractionResult{false, std::string(simdjson::error_message(fv_res.error())),
                                      emitter.EmittedCount()};
            }
            auto fv = fv_res.value();

            if (k == "timeUnixNano" || k == "time_unix_nano") {
              t_ns = core::ParseUint64(fv);
            } else if (k == "observedTimeUnixNano" || k == "observed_time_unix_nano") {
              obs_t_ns = core::ParseUint64(fv);
            } else if (k == "traceId" || k == "trace_id") {
              auto s_res = fv.get_string();
              if (s_res.error() == simdjson::SUCCESS) {
                trace_id = core::NormalizeTraceId(s_res.value(), trace_id_buf);
              }
            } else if (k == "spanId" || k == "span_id") {
              auto s_res = fv.get_string();
              if (s_res.error() == simdjson::SUCCESS) {
                span_id = core::NormalizeSpanId(s_res.value(), span_id_buf);
              }
            } else if (k == "flags" || k == "traceFlags" || k == "trace_flags") {
              flags = core::ParseInt64(fv);
            } else if (k == "severityText" || k == "severity_text") {
              auto s_res = fv.get_string();
              if (s_res.error() == simdjson::SUCCESS) {
                sev_text = s_res.value();
              }
            } else if (k == "severityNumber" || k == "severity_number") {
              sev_num = core::ParseSeverityNumber(fv);
            } else if (k == "body") {
              body_sv = ExtractBodyView(fv, body_writer);
            } else if (k == "eventName" || k == "event_name") {
              auto s_res = fv.get_string();
              if (s_res.error() == simdjson::SUCCESS) {
                event_name = s_res.value();
              }
            } else if (k == "attributes") {
              if (L != nullptr) {
                core::PushAttributesTable(L, fv, writer);
                log_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
                has_log_attrs = true;
              }
            }
          }

          if (t_ns == 0) {
            t_ns = obs_t_ns;
          }
          std::array<char, 32> ts_buf;
          core::FormatIso8601Nano(t_ns, ts_buf.data());

          emitter.StartRecord();
          emitter.SetString("Timestamp", std::string_view(ts_buf.data(), 30));
          emitter.SetString("TraceId", trace_id);
          emitter.SetString("SpanId", span_id);
          emitter.SetInt64("TraceFlags", flags);
          emitter.SetString("SeverityText", sev_text);
          emitter.SetInt64("SeverityNumber", sev_num);
          emitter.SetString("ServiceName", service_name);
          emitter.SetString("Body", body_sv);
          emitter.SetString("ResourceSchemaUrl", resource_schema_url);

          if (L != nullptr && res_attrs_scoped.get() != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, res_attrs_scoped.get());
            emitter.SetTableFromStack("ResourceAttributes");
          }

          emitter.SetString("ScopeSchemaUrl", scope_schema_url);
          emitter.SetString("ScopeName", scope_name);
          emitter.SetString("ScopeVersion", scope_version);

          if (L != nullptr && scope_attrs_scoped.get() != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, scope_attrs_scoped.get());
            emitter.SetTableFromStack("ScopeAttributes");
          }

          if (L != nullptr) {
            if (has_log_attrs && log_attrs_scoped.get() != LUA_NOREF) {
              lua_rawgeti(L, LUA_REGISTRYINDEX, log_attrs_scoped.get());
              log_attrs_scoped.reset();
            } else {
              lua_newtable(L);
            }
            emitter.SetTableFromStack("LogAttributes");
          }

          emitter.SetString("EventName", event_name);
          emitter.SetString("processed_at", processed_at);

          emitter.EndRecord();
          if (emitter.HasError()) {
            return ExtractionResult{false, std::string(emitter.ErrorMessage()),
                                    emitter.EmittedCount()};
          }
        }
      }
    }

    emitter.Flush();
    if (emitter.HasError()) {
      return ExtractionResult{false, std::string(emitter.ErrorMessage()), emitter.EmittedCount()};
    }
    return ExtractionResult{true, "", emitter.EmittedCount()};
  } catch (const std::exception& e) {
    return ExtractionResult{false, std::string("C++ exception: ") + e.what(),
                            emitter.EmittedCount()};
  } catch (...) {
    return ExtractionResult{false, "Unknown C++ exception", emitter.EmittedCount()};
  }
}

ExtractionResult ProcessLogsTable(lua_State* L, int table_idx, int emit_idx) {
  if (!lua_checkstack(L, 32)) {
    return ExtractionResult{false, "Lua stack overflow in ProcessLogsTable", 0};
  }
  const int entry_top = lua_gettop(L);
  int abs_idx = lua_absindex(L, table_idx);
  int abs_emit_idx = lua_absindex(L, emit_idx);
  if (!lua_istable(L, abs_idx)) {
    return ExtractionResult{false, "payload must be a table", 0};
  }

  static thread_local core::FastJsonWriter writer;
  static thread_local std::string tls_res_schema_scratch;
  static thread_local std::string tls_service_scratch;
  static thread_local std::string tls_scope_schema_scratch;
  static thread_local std::string tls_scope_name_scratch;
  static thread_local std::string tls_scope_ver_scratch;
  writer.Reset();
  core::ReserveScratchString(tls_res_schema_scratch, 128);
  core::ReserveScratchString(tls_service_scratch, 128);
  core::ReserveScratchString(tls_scope_schema_scratch, 128);
  core::ReserveScratchString(tls_scope_name_scratch, 128);
  core::ReserveScratchString(tls_scope_ver_scratch, 64);
  struct TableScopeGuard {
    TableScopeGuard(lua_State* l, int t, core::FastJsonWriter& w) : L(l), top(t), writer(w) {}
    TableScopeGuard(const TableScopeGuard&) = delete;
    TableScopeGuard& operator=(const TableScopeGuard&) = delete;
    ~TableScopeGuard() noexcept {
      if (L != nullptr) {
        lua_settop(L, top);
      }
      writer.Trim();
      core::ResetScratchString(tls_res_schema_scratch);
      core::ResetScratchString(tls_service_scratch);
      core::ResetScratchString(tls_scope_schema_scratch);
      core::ResetScratchString(tls_scope_name_scratch);
      core::ResetScratchString(tls_scope_ver_scratch);
    }
    lua_State* L;
    int top;
    core::FastJsonWriter& writer;
  } scope_guard{L, entry_top, writer};

  std::array<char, 32> proc_buf;
  core::FormatIso8601Nano(core::CurrentTimeUnixNano(), proc_buf.data());
  std::string_view processed_at(proc_buf.data(), 30);
  core::LuaEmitter emitter(L, abs_emit_idx);

  try {
    int rl_arr_idx = 0;

    if (core::GetTableField(L, abs_idx, "resourceLogs", "resource_logs")) {
      rl_arr_idx = lua_absindex(L, -1);
    } else if (core::GetTableField(L, abs_idx, "log")) {
      int log_idx = lua_absindex(L, -1);
      if (core::GetTableField(L, log_idx, "resourceLogs", "resource_logs")) {
        lua_remove(L, log_idx);
        rl_arr_idx = lua_absindex(L, -1);
      } else {
        lua_pop(L, 1);
      }
    }

    if (rl_arr_idx == 0) {
      rl_arr_idx = abs_idx;
    }

    if (!lua_istable(L, rl_arr_idx)) {
      return ExtractionResult{true, "", 0};
    }

    lua_Integer rl_len = static_cast<lua_Integer>(lua_rawlen(L, rl_arr_idx));
    for (lua_Integer i = 1; i <= rl_len; ++i) {
      lua_rawgeti(L, rl_arr_idx, i);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        continue;
      }
      int rl_obj_idx = lua_absindex(L, -1);

      // 1. Resource schema URL
      std::string_view resource_schema_url = core::ReadTableStringView(
          L, rl_obj_idx, "schemaUrl", "schema_url", tls_res_schema_scratch);

      // 2. Resource service name & attributes (single pass)
      std::string_view service_name = "unknown_service";
      ScopedLuaRef res_attrs_scoped(L, LUA_NOREF);

      if (core::GetTableField(L, rl_obj_idx, "resource")) {
        int res_idx = lua_absindex(L, -1);
        if (core::GetTableField(L, res_idx, "attributes")) {
          core::PushTableAttributes(L, -1, writer, &service_name);
          res_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
          lua_pop(L, 1);  // pop attributes
        }
        lua_pop(L, 1);  // pop resource
      }
      if (res_attrs_scoped.get() == LUA_NOREF) {
        lua_newtable(L);
        res_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
      }

      // 3. Iterate scopeLogs
      if (!core::GetTableField(L, rl_obj_idx, "scopeLogs", "scope_logs")) {
        lua_pop(L, 1);  // pop rl_obj
        continue;
      }
      int sl_arr_idx = lua_absindex(L, -1);
      if (!lua_istable(L, sl_arr_idx)) {
        lua_pop(L, 2);  // pop sl_arr and rl_obj
        continue;
      }

      lua_Integer sl_len = static_cast<lua_Integer>(lua_rawlen(L, sl_arr_idx));
      for (lua_Integer j = 1; j <= sl_len; ++j) {
        lua_rawgeti(L, sl_arr_idx, j);
        if (!lua_istable(L, -1)) {
          lua_pop(L, 1);
          continue;
        }
        int sl_obj_idx = lua_absindex(L, -1);

        std::string_view scope_schema_url = core::ReadTableStringView(
            L, sl_obj_idx, "schemaUrl", "schema_url", tls_scope_schema_scratch);
        std::string_view scope_name;
        std::string_view scope_version;
        ScopedLuaRef scope_attrs_scoped(L, LUA_NOREF);

        if (core::GetTableField(L, sl_obj_idx, "scope")) {
          int s_idx = lua_absindex(L, -1);
          scope_name =
              core::ReadTableStringView(L, s_idx, "name", nullptr, tls_scope_name_scratch);
          scope_version =
              core::ReadTableStringView(L, s_idx, "version", nullptr, tls_scope_ver_scratch);
          if (core::GetTableField(L, s_idx, "attributes")) {
            core::PushTableAttributes(L, -1, writer);
            scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
            lua_pop(L, 1);  // pop attributes
          }
          lua_pop(L, 1);  // pop scope
        }
        if (scope_attrs_scoped.get() == LUA_NOREF) {
          lua_newtable(L);
          scope_attrs_scoped = ScopedLuaRef(L, luaL_ref(L, LUA_REGISTRYINDEX));
        }

        // 4. Iterate logRecords
        if (core::GetTableField(L, sl_obj_idx, "logRecords", "log_records")) {
          int recs_arr_idx = lua_absindex(L, -1);
          if (lua_istable(L, recs_arr_idx)) {
            lua_Integer recs_len = static_cast<lua_Integer>(lua_rawlen(L, recs_arr_idx));
            for (lua_Integer k = 1; k <= recs_len; ++k) {
              lua_rawgeti(L, recs_arr_idx, k);
              if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
              }
              int rec_obj_idx = lua_absindex(L, -1);

              emitter.StartRecord();

              // 1. Timestamp
              uint64_t t_ns = core::ReadTableTimestampNano(L, rec_obj_idx);
              std::array<char, 32> ts_buf;
              core::FormatIso8601Nano(t_ns, ts_buf.data());
              emitter.SetString("Timestamp", std::string_view(ts_buf.data(), 30));

              // 2. TraceId
              core::CopyIdField(L, rec_obj_idx, "traceId", "trace_id", core::kTraceIdBinaryLen,
                                emitter, "TraceId", "TraceId");

              // 3. SpanId
              core::CopyIdField(L, rec_obj_idx, "spanId", "span_id", core::kSpanIdBinaryLen,
                                emitter, "SpanId", "SpanId");

              // 4. TraceFlags
              emitter.SetInt64(
                  "TraceFlags",
                  core::ReadTableInt64(L, rec_obj_idx, "flags", "traceFlags", 0, "trace_flags"));

              // 5. SeverityText
              core::CopyStringField(L, rec_obj_idx, "severityText", "severity_text", emitter,
                                    "SeverityText");

              // 6. SeverityNumber
              int64_t sev_num = 0;
              if (core::GetTableField(L, rec_obj_idx, "severityNumber", "severity_number")) {
                sev_num = core::ParseTableSeverityNumber(L, -1);
                lua_pop(L, 1);
              }
              emitter.SetInt64("SeverityNumber", sev_num);

              // 7. ServiceName
              emitter.SetString("ServiceName", service_name);

              // 8. Body
              core::CopyTableBody(L, rec_obj_idx, writer, emitter);

              // 9. ResourceSchemaUrl
              emitter.SetString("ResourceSchemaUrl", resource_schema_url);

              // 10. ResourceAttributes
              if (res_attrs_scoped.get() != LUA_NOREF) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, res_attrs_scoped.get());
                emitter.SetTableFromStack("ResourceAttributes");
              }

              // 11. ScopeSchemaUrl
              emitter.SetString("ScopeSchemaUrl", scope_schema_url);

              // 12. ScopeName
              emitter.SetString("ScopeName", scope_name);

              // 13. ScopeVersion
              emitter.SetString("ScopeVersion", scope_version);

              // 14. ScopeAttributes
              if (scope_attrs_scoped.get() != LUA_NOREF) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, scope_attrs_scoped.get());
                emitter.SetTableFromStack("ScopeAttributes");
              }

              // 15. LogAttributes
              if (core::GetTableField(L, rec_obj_idx, "attributes")) {
                core::PushTableAttributes(L, -1, writer);
                emitter.SetTableFromStack("LogAttributes");
                lua_pop(L, 1);  // pop attributes
              } else {
                lua_newtable(L);
                emitter.SetTableFromStack("LogAttributes");
              }

              // 16. EventName
              core::CopyStringField(L, rec_obj_idx, "eventName", "event_name", emitter,
                                    "EventName");

              // 17. processed_at
              emitter.SetString("processed_at", processed_at);

              emitter.EndRecord();
              lua_pop(L, 1);  // pop rec_obj
              if (emitter.HasError()) {
                return ExtractionResult{false, std::string(emitter.ErrorMessage()),
                                        emitter.EmittedCount()};
              }
            }
          }
          lua_pop(L, 1);  // pop recs_arr
        }
        lua_pop(L, 1);  // pop sl_obj
      }
      lua_pop(L, 1);  // pop sl_arr
      lua_pop(L, 1);  // pop rl_obj
    }

    emitter.Flush();
    if (emitter.HasError()) {
      return ExtractionResult{false, std::string(emitter.ErrorMessage()), emitter.EmittedCount()};
    }
    return ExtractionResult{true, "", emitter.EmittedCount()};
  } catch (const std::exception& e) {
    return ExtractionResult{false, std::string("C++ exception: ") + e.what(),
                            emitter.EmittedCount()};
  } catch (...) {
    return ExtractionResult{false, "Unknown C++ exception", emitter.EmittedCount()};
  }
}

}  // namespace vector_otel::extractors
