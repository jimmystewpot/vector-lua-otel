#include "vector_otel/core/otlp_common.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "vector_otel/core/scratch.hpp"
#include "vector_otel/core/timestamp.hpp"

namespace vector_otel::core {

namespace {
// Thread-local scratch buffer for service.name to prevent use-after-free
// when Lua GC removes the attribute from ResourceAttributes.
thread_local std::string tls_service_name_scratch;

// Helper to capture and safely copy service.name from Lua stack into scratch.
// Returns string_view into tls_service_name_scratch if scalar string found,
// otherwise returns "unknown_service".
std::string_view CaptureServiceName(lua_State* L, bool is_scalar_str) noexcept {
  if (!is_scalar_str) {
    return "unknown_service";
  }
  size_t slen = 0;
  const char* s = lua_tolstring(L, -1, &slen);
  if (s == nullptr || slen == 0) {
    return "unknown_service";
  }
  // Copy into thread-local scratch (preserves embedded NULs)
  ReserveScratchString(tls_service_name_scratch, 128);
  tls_service_name_scratch.assign(s, slen);
  return std::string_view(tls_service_name_scratch);
}

constexpr char kHexDigits[] = "0123456789abcdef";

inline void HexEncode(std::string_view raw, char* out) noexcept {
  for (size_t i = 0; i < raw.size(); ++i) {
    const uint8_t b = static_cast<uint8_t>(raw[i]);
    out[i * 2] = kHexDigits[(b >> 4) & 0x0f];
    out[i * 2 + 1] = kHexDigits[b & 0x0f];
  }
}
}  // namespace

std::string_view NormalizeOtelId(std::string_view raw, size_t expected_binary_len,
                                 IdHexBuffer& stack_buf) noexcept {
  // 1. Empty Input
  if (raw.empty()) {
    return "";
  }

  // 2. Exact Binary Width (raw.size() == expected_binary_len)
  // Unconditionally encode all B bytes into 2B lowercase hex chars in stack_buf.
  if (raw.size() == expected_binary_len) {
    if (raw.size() * 2 <= kMaxIdHexBufSize) {
      HexEncode(raw, stack_buf.data());
      return std::string_view(stack_buf.data(), raw.size() * 2);
    }
    return "";
  }

  // 3. Single-Pass Character Classification (raw.size() != expected_binary_len)
  bool all_lower_hex = true;
  bool all_hex = true;
  bool all_printable_ascii = true;

  for (char c : raw) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || uc > 0x7e) {
      all_printable_ascii = false;
    }
    const bool is_digit = (c >= '0' && c <= '9');
    const bool is_lower = (c >= 'a' && c <= 'f');
    const bool is_upper = (c >= 'A' && c <= 'F');
    if (!is_digit && !is_lower) {
      all_lower_hex = false;
    }
    if (!is_digit && !is_lower && !is_upper) {
      all_hex = false;
    }
  }

  // Rule 3a — Already Lowercase Hex
  if (all_lower_hex) {
    return raw;
  }

  // Rule 3b — Valid Hex With Uppercase (all_hex == true and raw.size() <= 64)
  if (all_hex && raw.size() <= kMaxIdHexBufSize) {
    for (size_t i = 0; i < raw.size(); ++i) {
      char c = raw[i];
      if (c >= 'A' && c <= 'F') {
        stack_buf[i] = static_cast<char>(c + ('a' - 'A'));
      } else {
        stack_buf[i] = c;
      }
    }
    return std::string_view(stack_buf.data(), raw.size());
  }

  // Rule 3c — Non-Printable / Binary / Embedded NUL / High-Bit (!all_printable_ascii)
  if (!all_printable_ascii) {
    if (raw.size() <= 32) {
      HexEncode(raw, stack_buf.data());
      return std::string_view(stack_buf.data(), raw.size() * 2);
    }
    return "";
  }

  // Rule 3d — Printable Non-Hex ASCII (all_printable_ascii == true and !all_hex)
  return raw;
}

std::string_view ExtractStringField(simdjson::ondemand::object& obj, std::string_view key,
                                    std::string_view default_val) {
  auto val_res = obj.find_field_unordered(key);
  if (val_res.error() != simdjson::SUCCESS) {
    return default_val;
  }
  auto val = val_res.value();
  auto str_res = val.get_string();
  if (str_res.error() != simdjson::SUCCESS) {
    return default_val;
  }
  return str_res.value();
}

std::string_view ExtractStringField(simdjson::ondemand::object& obj, std::string_view key1,
                                    std::string_view key2, std::string_view default_val) {
  auto val_res = obj.find_field_unordered(key1);
  if (val_res.error() == simdjson::SUCCESS) {
    auto val = val_res.value();
    auto str_res = val.get_string();
    if (str_res.error() == simdjson::SUCCESS) {
      return str_res.value();
    }
  }
  val_res = obj.find_field_unordered(key2);
  if (val_res.error() == simdjson::SUCCESS) {
    auto val = val_res.value();
    auto str_res = val.get_string();
    if (str_res.error() == simdjson::SUCCESS) {
      return str_res.value();
    }
  }
  return default_val;
}

bool TryParseInt64(simdjson::ondemand::value& val, int64_t& out) {
  auto type_res = val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return false;
  }
  const auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::number) {
    auto num_res = val.get_number();
    if (num_res.error() != simdjson::SUCCESS) {
      return false;
    }
    const auto num = num_res.value();
    if (num.is_int64()) {
      out = num.get_int64();
      return true;
    }
    if (num.is_uint64()) {
      const uint64_t u = num.get_uint64();
      out = u > static_cast<uint64_t>(INT64_MAX) ? std::numeric_limits<int64_t>::max()
                                                 : static_cast<int64_t>(u);
      return true;
    }
    if (num.is_double()) {
      const double d = num.get_double();
      if (std::isfinite(d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0) {
        out = static_cast<int64_t>(d);
        return true;
      }
    }
    return false;
  }

  if (type == simdjson::ondemand::json_type::string) {
    auto str_res = val.get_string();
    if (str_res.error() != simdjson::SUCCESS) {
      return false;
    }
    std::string_view sv = TrimJsonWhitespace(str_res.value());
    if (!sv.empty() && sv.front() == '+') {
      sv.remove_prefix(1);
      if (!sv.empty() && sv.front() == '-') {
        return false;
      }
    }
    if (sv.empty()) {
      return false;
    }
    int64_t sval = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), sval);
    if (ec == std::errc() && ptr == sv.data() + sv.size()) {
      out = sval;
      return true;
    }
    uint64_t uval = 0;
    auto [uptr, uec] = std::from_chars(sv.data(), sv.data() + sv.size(), uval);
    if (uec == std::errc() && uptr == sv.data() + sv.size()) {
      out = uval > static_cast<uint64_t>(INT64_MAX) ? std::numeric_limits<int64_t>::max()
                                                    : static_cast<int64_t>(uval);
      return true;
    }
  }
  return false;
}

int64_t ParseInt64(simdjson::ondemand::value& val) {
  int64_t v = 0;
  TryParseInt64(val, v);
  return v;
}

int64_t ParseSeverityNumberString(std::string_view sv) noexcept {
  if (sv == "SEVERITY_NUMBER_TRACE") return 1;
  if (sv == "SEVERITY_NUMBER_TRACE2") return 2;
  if (sv == "SEVERITY_NUMBER_TRACE3") return 3;
  if (sv == "SEVERITY_NUMBER_TRACE4") return 4;
  if (sv == "SEVERITY_NUMBER_DEBUG") return 5;
  if (sv == "SEVERITY_NUMBER_DEBUG2") return 6;
  if (sv == "SEVERITY_NUMBER_DEBUG3") return 7;
  if (sv == "SEVERITY_NUMBER_DEBUG4") return 8;
  if (sv == "SEVERITY_NUMBER_INFO") return 9;
  if (sv == "SEVERITY_NUMBER_INFO2") return 10;
  if (sv == "SEVERITY_NUMBER_INFO3") return 11;
  if (sv == "SEVERITY_NUMBER_INFO4") return 12;
  if (sv == "SEVERITY_NUMBER_WARN") return 13;
  if (sv == "SEVERITY_NUMBER_WARN2") return 14;
  if (sv == "SEVERITY_NUMBER_WARN3") return 15;
  if (sv == "SEVERITY_NUMBER_WARN4") return 16;
  if (sv == "SEVERITY_NUMBER_ERROR") return 17;
  if (sv == "SEVERITY_NUMBER_ERROR2") return 18;
  if (sv == "SEVERITY_NUMBER_ERROR3") return 19;
  if (sv == "SEVERITY_NUMBER_ERROR4") return 20;
  if (sv == "SEVERITY_NUMBER_FATAL") return 21;
  if (sv == "SEVERITY_NUMBER_FATAL2") return 22;
  if (sv == "SEVERITY_NUMBER_FATAL3") return 23;
  if (sv == "SEVERITY_NUMBER_FATAL4") return 24;
  if (sv == "SEVERITY_NUMBER_UNSPECIFIED") return 0;

  sv = TrimJsonWhitespace(sv);
  if (!sv.empty() && sv.front() == '+') {
    sv.remove_prefix(1);
    if (!sv.empty() && sv.front() == '-') {
      return 0;
    }
  }
  if (sv.empty()) {
    return 0;
  }
  int64_t sval = 0;
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), sval);
  if (ec == std::errc() && ptr == sv.data() + sv.size()) {
    return sval;
  }
  return 0;
}

int64_t ParseSeverityNumber(simdjson::ondemand::value& val) {
  auto type_res = val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return 0;
  }
  if (type_res.value() == simdjson::ondemand::json_type::string) {
    auto str_res = val.get_string();
    if (str_res.error() != simdjson::SUCCESS) {
      return 0;
    }
    return ParseSeverityNumberString(str_res.value());
  }
  return ParseInt64(val);
}

uint64_t ParseUint64(simdjson::ondemand::value& val) {
  auto type_res = val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return 0;
  }
  const auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::number) {
    auto num_res = val.get_number();
    if (num_res.error() != simdjson::SUCCESS) {
      return 0;
    }
    const auto num = num_res.value();
    if (num.is_uint64()) {
      return num.get_uint64();
    }
    if (num.is_int64()) {
      return num.get_int64() >= 0 ? static_cast<uint64_t>(num.get_int64()) : 0ULL;
    }
    if (num.is_double()) {
      const double d = num.get_double();
      if (std::isfinite(d) && d >= 0.0 && d < 18446744073709551616.0) {
        return static_cast<uint64_t>(d);
      }
    }
    return 0;
  }

  if (type == simdjson::ondemand::json_type::string) {
    auto str_res = val.get_string();
    if (str_res.error() != simdjson::SUCCESS) {
      return 0;
    }
    std::string_view sv = str_res.value();
    while (!sv.empty() &&
           (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
      sv.remove_prefix(1);
    }
    while (!sv.empty() &&
           (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
      sv.remove_suffix(1);
    }
    if (!sv.empty() && sv.front() == '+') {
      sv.remove_prefix(1);
      if (!sv.empty() && sv.front() == '-') {
        return 0;
      }
    }
    if (sv.empty()) {
      return 0;
    }
    uint64_t uval = 0;
    auto [uptr, uec] = std::from_chars(sv.data(), sv.data() + sv.size(), uval);
    if (uec == std::errc() && uptr == sv.data() + sv.size()) {
      return uval;
    }
  }
  return 0;
}

std::string_view ExtractServiceName(simdjson::ondemand::object& resource_obj) {
  auto attrs_res = resource_obj.find_field_unordered("attributes");
  if (attrs_res.error() != simdjson::SUCCESS) {
    return "unknown_service";
  }
  auto attrs_val = attrs_res.value();
  auto type_res = attrs_val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return "unknown_service";
  }
  auto type = type_res.value();

  if (type == simdjson::ondemand::json_type::array) {
    auto arr_res = attrs_val.get_array();
    if (arr_res.error() != simdjson::SUCCESS) {
      return "unknown_service";
    }
    for (auto elem_res : arr_res.value()) {
      if (elem_res.error() != simdjson::SUCCESS) continue;
      auto item_val = elem_res.value();
      auto obj_res = item_val.get_object();
      if (obj_res.error() != simdjson::SUCCESS) continue;
      auto kv_obj = obj_res.value();

      auto key_res = kv_obj.find_field_unordered("key");
      if (key_res.error() != simdjson::SUCCESS) continue;
      auto key_str_res = key_res.value().get_string();
      if (key_str_res.error() != simdjson::SUCCESS) continue;
      if (key_str_res.value() == "service.name") {
        auto val_res = kv_obj.find_field_unordered("value");
        if (val_res.error() != simdjson::SUCCESS) continue;
        auto val = val_res.value();
        auto val_type_res = val.type();
        if (val_type_res.error() != simdjson::SUCCESS) continue;
        if (val_type_res.value() == simdjson::ondemand::json_type::string) {
          auto str_res = val.get_string();
          if (str_res.error() == simdjson::SUCCESS) {
            return str_res.value();
          }
        } else if (val_type_res.value() == simdjson::ondemand::json_type::object) {
          auto val_obj_res = val.get_object();
          if (val_obj_res.error() == simdjson::SUCCESS) {
            auto val_obj = val_obj_res.value();
            auto sv_res = val_obj.find_field_unordered("stringValue");
            if (sv_res.error() == simdjson::SUCCESS) {
              auto str_res = sv_res.value().get_string();
              if (str_res.error() == simdjson::SUCCESS) {
                return str_res.value();
              }
            }
            auto sv_snake_res = val_obj.find_field_unordered("string_value");
            if (sv_snake_res.error() == simdjson::SUCCESS) {
              auto str_res = sv_snake_res.value().get_string();
              if (str_res.error() == simdjson::SUCCESS) {
                return str_res.value();
              }
            }
          }
        }
      }
    }
  } else if (type == simdjson::ondemand::json_type::object) {
    auto obj_res = attrs_val.get_object();
    if (obj_res.error() == simdjson::SUCCESS) {
      auto attrs_obj = obj_res.value();
      auto svc_res = attrs_obj.find_field_unordered("service.name");
      if (svc_res.error() == simdjson::SUCCESS) {
        auto svc_val = svc_res.value();
        auto val_type_res = svc_val.type();
        if (val_type_res.error() == simdjson::SUCCESS) {
          auto val_type = val_type_res.value();
          if (val_type == simdjson::ondemand::json_type::string) {
            auto s_res = svc_val.get_string();
            if (s_res.error() == simdjson::SUCCESS) {
              return s_res.value();
            }
          } else if (val_type == simdjson::ondemand::json_type::object) {
            auto val_obj_res = svc_val.get_object();
            if (val_obj_res.error() == simdjson::SUCCESS) {
              auto val_obj = val_obj_res.value();
              auto sv_res = val_obj.find_field_unordered("stringValue");
              if (sv_res.error() == simdjson::SUCCESS) {
                auto str_res = sv_res.value().get_string();
                if (str_res.error() == simdjson::SUCCESS) {
                  return str_res.value();
                }
              }
              auto sv_snake_res = val_obj.find_field_unordered("string_value");
              if (sv_snake_res.error() == simdjson::SUCCESS) {
                auto str_res = sv_snake_res.value().get_string();
                if (str_res.error() == simdjson::SUCCESS) {
                  return str_res.value();
                }
              }
            }
          }
        }
      }
    }
  }
  return "unknown_service";
}

bool TryParseDouble(simdjson::ondemand::value& val, double& out) {
  auto type_res = val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return false;
  }
  const auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::number) {
    auto num_res = val.get_double();
    if (num_res.error() != simdjson::SUCCESS) {
      return false;
    }
    const double d = num_res.value();
    if (!std::isfinite(d)) {
      return false;
    }
    out = d;
    return true;
  }

  if (type == simdjson::ondemand::json_type::string) {
    auto str_res = val.get_string();
    if (str_res.error() != simdjson::SUCCESS) {
      return false;
    }
    std::string_view sv = str_res.value();
    while (!sv.empty() &&
           (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
      sv.remove_prefix(1);
    }
    while (!sv.empty() &&
           (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
      sv.remove_suffix(1);
    }
    if (!sv.empty() && sv.front() == '+') {
      sv.remove_prefix(1);
      if (!sv.empty() && sv.front() == '-') {
        return false;
      }
    }
    if (sv.empty()) {
      return false;
    }
    double d = 0.0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), d);
    if (ec == std::errc() && ptr == sv.data() + sv.size() && std::isfinite(d)) {
      out = d;
      return true;
    }
  }
  return false;
}

double ParseDouble(simdjson::ondemand::value& val) {
  double d = 0.0;
  TryParseDouble(val, d);
  return d;
}

namespace {

bool PushAnyValueScalar(lua_State* L, simdjson::ondemand::value& val, FastJsonWriter& json_writer) {
  if (L == nullptr) {
    return false;
  }
  auto type_res = val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    lua_pushnil(L);
    return false;
  }
  auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::object) {
    auto obj_res = val.get_object();
    if (obj_res.error() != simdjson::SUCCESS) {
      lua_pushnil(L);
      return false;
    }
    auto obj = obj_res.value();
    bool matched = false;
    bool is_scalar_string = false;
    bool has_plain_fields = false;
    for (auto field_res : obj) {
      if (field_res.error() != simdjson::SUCCESS) continue;
      auto k_res = field_res.unescaped_key();
      if (k_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = k_res.value();

      auto val_res = field_res.value();
      if (val_res.error() != simdjson::SUCCESS) continue;
      auto fv = val_res.value();

      if (!has_plain_fields) {
        if (k == "stringValue" || k == "string_value") {
          auto s_res = fv.get_string();
          if (s_res.error() == simdjson::SUCCESS) {
            auto s = s_res.value();
            lua_pushlstring(L, s.data(), s.size());
            matched = true;
            is_scalar_string = true;
            break;
          }
        } else if (k == "intValue" || k == "int_value") {
          int64_t i = ParseInt64(fv);
          lua_pushinteger(L, static_cast<lua_Integer>(i));
          matched = true;
          break;
        } else if (k == "uintValue" || k == "uint_value") {
          uint64_t u = ParseUint64(fv);
          lua_Integer clamped = u > static_cast<uint64_t>(INT64_MAX)
                                    ? std::numeric_limits<lua_Integer>::max()
                                    : static_cast<lua_Integer>(u);
          lua_pushinteger(L, clamped);
          matched = true;
          break;
        } else if (k == "doubleValue" || k == "double_value") {
          lua_pushnumber(L, ParseDouble(fv));
          matched = true;
          break;
        } else if (k == "boolValue" || k == "bool_value") {
          auto b_res = fv.get_bool();
          if (b_res.error() == simdjson::SUCCESS) {
            lua_pushboolean(L, b_res.value() ? 1 : 0);
            matched = true;
            break;
          }
          lua_pushboolean(L, 0);
          matched = true;
          break;
        } else if (k == "arrayValue" || k == "array_value" || k == "kvlistValue" ||
                   k == "kvlist_value" || k == "bytesValue" || k == "bytes_value") {
          std::string_view raw_sv;
          auto raw_res = fv.raw_json();
          if (raw_res.error() == simdjson::SUCCESS) {
            raw_sv = raw_res.value();
          }
          json_writer.Reset();
          json_writer.StartObject();
          json_writer.Key(k);
          if (!raw_sv.empty()) {
            json_writer.RawJson(raw_sv);
          } else {
            json_writer.StartObject();
            json_writer.EndObject();
          }
          json_writer.EndObject();

          std::string_view sv = json_writer.Str();
          lua_pushlstring(L, sv.data(), sv.size());
          matched = true;
          break;
        }
      }

      if (!has_plain_fields) {
        json_writer.Reset();
        json_writer.StartObject();
        has_plain_fields = true;
      }
      json_writer.Key(k);
      auto raw_res = fv.raw_json();
      if (raw_res.error() == simdjson::SUCCESS) {
        std::string_view raw_fv = raw_res.value();
        while (!raw_fv.empty() && (raw_fv.back() == ' ' || raw_fv.back() == '\t' ||
                                   raw_fv.back() == '\r' || raw_fv.back() == '\n')) {
          raw_fv.remove_suffix(1);
        }
        json_writer.RawJson(raw_fv);
      } else {
        json_writer.Null();
      }
    }
    if (!matched) {
      if (has_plain_fields) {
        json_writer.EndObject();
        std::string_view sv = json_writer.Str();
        lua_pushlstring(L, sv.data(), sv.size());
      } else {
        lua_pushnil(L);
      }
    }
    return is_scalar_string;
  } else if (type == simdjson::ondemand::json_type::string) {
    auto s_res = val.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      auto s = s_res.value();
      lua_pushlstring(L, s.data(), s.size());
      return true;
    } else {
      lua_pushnil(L);
      return false;
    }
  } else if (type == simdjson::ondemand::json_type::number) {
    auto num_res = val.get_number();
    if (num_res.error() == simdjson::SUCCESS) {
      auto num = num_res.value();
      if (num.is_int64()) {
        lua_pushinteger(L, static_cast<lua_Integer>(num.get_int64()));
      } else if (num.is_uint64()) {
        uint64_t u = num.get_uint64();
        lua_Integer clamped = u > static_cast<uint64_t>(INT64_MAX)
                                  ? std::numeric_limits<lua_Integer>::max()
                                  : static_cast<lua_Integer>(u);
        lua_pushinteger(L, clamped);
      } else {
        lua_pushnumber(L, num.get_double());
      }
    } else {
      lua_pushnil(L);
    }
    return false;
  } else if (type == simdjson::ondemand::json_type::boolean) {
    auto b_res = val.get_bool();
    if (b_res.error() == simdjson::SUCCESS) {
      lua_pushboolean(L, b_res.value() ? 1 : 0);
    } else {
      lua_pushnil(L);
    }
    return false;
  } else if (type == simdjson::ondemand::json_type::array) {
    auto raw_res = val.raw_json();
    std::string_view raw_sv;
    if (raw_res.error() == simdjson::SUCCESS) {
      raw_sv = raw_res.value();
    }
    json_writer.Reset();
    if (!raw_sv.empty()) {
      json_writer.RawJson(raw_sv);
    } else {
      json_writer.StartArray();
      json_writer.EndArray();
    }
    std::string_view sv = json_writer.Str();
    lua_pushlstring(L, sv.data(), sv.size());
    return false;
  } else {
    lua_pushnil(L);
    return false;
  }
}

}  // namespace

void PushAnyValue(lua_State* L, simdjson::ondemand::value& val, FastJsonWriter& json_writer) {
  (void)PushAnyValueScalar(L, val, json_writer);
}

void PushAttributesTable(lua_State* L, simdjson::ondemand::value& attrs_val,
                         FastJsonWriter& json_writer, std::string_view* out_service_name) {
  if (L == nullptr) {
    return;
  }
  if (!lua_checkstack(L, 8)) {
    throw std::runtime_error("Lua stack overflow in PushAttributesTable");
  }
  lua_newtable(L);

  auto type_res = attrs_val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return;
  }
  auto type = type_res.value();

  if (type == simdjson::ondemand::json_type::array) {
    auto arr_res = attrs_val.get_array();
    if (arr_res.error() != simdjson::SUCCESS) {
      return;
    }
    for (auto elem_res : arr_res.value()) {
      if (elem_res.error() != simdjson::SUCCESS) continue;
      auto item_val = elem_res.value();
      auto obj_res = item_val.get_object();
      if (obj_res.error() != simdjson::SUCCESS) continue;
      auto kv_obj = obj_res.value();

      auto key_res = kv_obj.find_field_unordered("key");
      if (key_res.error() != simdjson::SUCCESS) continue;
      auto key_str_res = key_res.value().get_string();
      if (key_str_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = key_str_res.value();

      auto val_res = kv_obj.find_field_unordered("value");
      if (val_res.error() != simdjson::SUCCESS) continue;
      auto v = val_res.value();

      lua_pushlstring(L, k.data(), k.size());
      const bool is_scalar_str = PushAnyValueScalar(L, v, json_writer);
      if (out_service_name != nullptr && (k == "service.name" || k == "service_name")) {
        *out_service_name = CaptureServiceName(L, is_scalar_str);
      }
      lua_rawset(L, -3);
    }
  } else if (type == simdjson::ondemand::json_type::object) {
    auto obj_res = attrs_val.get_object();
    if (obj_res.error() != simdjson::SUCCESS) {
      return;
    }
    for (auto field_res : obj_res.value()) {
      if (field_res.error() != simdjson::SUCCESS) continue;
      auto key_res = field_res.unescaped_key();
      if (key_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = key_res.value();

      auto val_res = field_res.value();
      if (val_res.error() != simdjson::SUCCESS) continue;
      auto v = val_res.value();

      lua_pushlstring(L, k.data(), k.size());
      const bool is_scalar_str = PushAnyValueScalar(L, v, json_writer);
      if (out_service_name != nullptr && (k == "service.name" || k == "service_name")) {
        *out_service_name = CaptureServiceName(L, is_scalar_str);
      }
      lua_rawset(L, -3);
    }
  }
}

namespace {

constexpr int kMaxJsonDepth = 32;

bool WriteAnyValueJsonImpl(FastJsonWriter& writer, simdjson::ondemand::value& val, int depth,
                           std::string_view* out_scalar_str = nullptr) {
  if (depth >= kMaxJsonDepth) {
    writer.String("[max_depth_exceeded]");
    return false;
  }
  auto type_res = val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    writer.Null();
    return false;
  }
  const auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::object) {
    auto obj_res = val.get_object();
    if (obj_res.error() != simdjson::SUCCESS) {
      writer.Null();
      return false;
    }
    auto obj = obj_res.value();
    bool matched = false;
    bool is_scalar_string = false;
    bool has_plain_fields = false;
    for (auto field_res : obj) {
      if (field_res.error() != simdjson::SUCCESS) continue;
      auto k_res = field_res.unescaped_key();
      if (k_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = k_res.value();

      auto val_res = field_res.value();
      if (val_res.error() != simdjson::SUCCESS) continue;
      auto fv = val_res.value();

      if (!has_plain_fields) {
        if (k == "stringValue" || k == "string_value") {
          auto s_res = fv.get_string();
          if (s_res.error() == simdjson::SUCCESS) {
            std::string_view s = s_res.value();
            writer.String(s);
            if (out_scalar_str != nullptr) {
              *out_scalar_str = s;
            }
            matched = true;
            is_scalar_string = true;
            break;
          }
          writer.String("");
          matched = true;
          break;
        } else if (k == "intValue" || k == "int_value") {
          writer.Int64(ParseInt64(fv));
          matched = true;
          break;
        } else if (k == "uintValue" || k == "uint_value") {
          writer.Uint64(ParseUint64(fv));
          matched = true;
          break;
        } else if (k == "doubleValue" || k == "double_value") {
          writer.Double(ParseDouble(fv));
          matched = true;
          break;
        } else if (k == "boolValue" || k == "bool_value") {
          auto b_res = fv.get_bool();
          writer.Bool(b_res.error() == simdjson::SUCCESS && b_res.value());
          matched = true;
          break;
        } else if (k == "bytesValue" || k == "bytes_value") {
          auto s_res = fv.get_string();
          if (s_res.error() == simdjson::SUCCESS) {
            writer.String(s_res.value());
          } else {
            writer.String("");
          }
          matched = true;
          break;
        } else if (k == "arrayValue" || k == "array_value") {
          writer.StartArray();
          auto fv_type_res = fv.type();
          if (fv_type_res.error() == simdjson::SUCCESS) {
            if (fv_type_res.value() == simdjson::ondemand::json_type::object) {
              auto av_obj_res = fv.get_object();
              if (av_obj_res.error() == simdjson::SUCCESS) {
                auto vals_res = av_obj_res.value().find_field_unordered("values");
                if (vals_res.error() == simdjson::SUCCESS) {
                  auto arr_res = vals_res.value().get_array();
                  if (arr_res.error() == simdjson::SUCCESS) {
                    for (auto elem_res : arr_res.value()) {
                      if (elem_res.error() != simdjson::SUCCESS) continue;
                      auto elem = elem_res.value();
                      WriteAnyValueJson(writer, elem, depth + 1);
                    }
                  }
                }
              }
            } else if (fv_type_res.value() == simdjson::ondemand::json_type::array) {
              auto arr_res = fv.get_array();
              if (arr_res.error() == simdjson::SUCCESS) {
                for (auto elem_res : arr_res.value()) {
                  if (elem_res.error() != simdjson::SUCCESS) continue;
                  auto elem = elem_res.value();
                  WriteAnyValueJson(writer, elem, depth + 1);
                }
              }
            }
          }
          writer.EndArray();
          matched = true;
          break;
        } else if (k == "kvlistValue" || k == "kvlist_value") {
          writer.StartObject();
          auto fv_type_res = fv.type();
          if (fv_type_res.error() == simdjson::SUCCESS &&
              fv_type_res.value() == simdjson::ondemand::json_type::object) {
            auto kv_obj_res = fv.get_object();
            if (kv_obj_res.error() == simdjson::SUCCESS) {
              bool has_direct_fields = false;
              for (auto kv_field_res : kv_obj_res.value()) {
                if (kv_field_res.error() != simdjson::SUCCESS) continue;
                auto sub_k_res = kv_field_res.unescaped_key();
                if (sub_k_res.error() != simdjson::SUCCESS) continue;
                std::string_view sub_k = sub_k_res.value();
                auto sub_v_res = kv_field_res.value();
                if (sub_v_res.error() != simdjson::SUCCESS) continue;
                auto sub_v = sub_v_res.value();

                if (!has_direct_fields && sub_k == "values") {
                  auto sub_t_res = sub_v.type();
                  if (sub_t_res.error() == simdjson::SUCCESS &&
                      sub_t_res.value() == simdjson::ondemand::json_type::array) {
                    auto arr_res = sub_v.get_array();
                    if (arr_res.error() == simdjson::SUCCESS) {
                      for (auto elem_res : arr_res.value()) {
                        if (elem_res.error() != simdjson::SUCCESS) continue;
                        auto item_val = elem_res.value();
                        auto item_obj_res = item_val.get_object();
                        if (item_obj_res.error() != simdjson::SUCCESS) continue;
                        auto item_obj = item_obj_res.value();
                        auto ik_res = item_obj.find_field_unordered("key");
                        if (ik_res.error() != simdjson::SUCCESS) continue;
                        auto ik_str_res = ik_res.value().get_string();
                        if (ik_str_res.error() != simdjson::SUCCESS) continue;
                        std::string_view ik = ik_str_res.value();
                        auto iv_res = item_obj.find_field_unordered("value");
                        if (iv_res.error() != simdjson::SUCCESS) continue;
                        auto iv = iv_res.value();
                        writer.Key(ik);
                        WriteAnyValueJson(writer, iv, depth + 1);
                      }
                    }
                    break;
                  }
                }
                has_direct_fields = true;
                writer.Key(sub_k);
                WriteAnyValueJson(writer, sub_v, depth + 1);
              }
            }
          }
          writer.EndObject();
          matched = true;
          break;
        }
      }

      if (!has_plain_fields) {
        writer.StartObject();
        has_plain_fields = true;
      }
      writer.Key(k);
      WriteAnyValueJson(writer, fv, depth + 1);
    }
    if (!matched) {
      if (!has_plain_fields) {
        writer.StartObject();
      }
      writer.EndObject();
    }
    return is_scalar_string;
  } else if (type == simdjson::ondemand::json_type::array) {
    writer.StartArray();
    auto arr_res = val.get_array();
    if (arr_res.error() == simdjson::SUCCESS) {
      for (auto elem_res : arr_res.value()) {
        if (elem_res.error() != simdjson::SUCCESS) continue;
        auto elem = elem_res.value();
        WriteAnyValueJson(writer, elem, depth + 1);
      }
    }
    writer.EndArray();
    return false;
  } else if (type == simdjson::ondemand::json_type::string) {
    auto s_res = val.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      std::string_view s = s_res.value();
      writer.String(s);
      if (out_scalar_str != nullptr) {
        *out_scalar_str = s;
      }
      return true;
    }
    writer.Null();
    return false;
  } else if (type == simdjson::ondemand::json_type::number) {
    auto num_res = val.get_number();
    if (num_res.error() == simdjson::SUCCESS) {
      auto num = num_res.value();
      if (num.is_int64()) {
        writer.Int64(num.get_int64());
      } else if (num.is_uint64()) {
        writer.Uint64(num.get_uint64());
      } else {
        writer.Double(num.get_double());
      }
    } else {
      writer.Null();
    }
    return false;
  } else if (type == simdjson::ondemand::json_type::boolean) {
    auto b_res = val.get_bool();
    writer.Bool(b_res.error() == simdjson::SUCCESS && b_res.value());
    return false;
  } else {
    writer.Null();
    return false;
  }
}

void UpdateServiceNameFromAttr(std::string_view* out_service_name, bool is_scalar_str,
                               std::string_view scalar_str) noexcept {
  if (out_service_name == nullptr) {
    return;
  }
  if (is_scalar_str && !scalar_str.empty()) {
    ReserveScratchString(tls_service_name_scratch, 128);
    tls_service_name_scratch.assign(scalar_str.data(), scalar_str.size());
    *out_service_name = std::string_view(tls_service_name_scratch);
  } else {
    *out_service_name = "unknown_service";
  }
}

}  // namespace

namespace otlp_json_adl_barrier {

void WriteAnyValueJson(FastJsonWriter& writer, simdjson::ondemand::value& val, int depth) {
  (void)WriteAnyValueJsonImpl(writer, val, depth, nullptr);
}

void WriteAttributesEntriesJson(FastJsonWriter& writer, simdjson::ondemand::value& attrs_val,
                                std::string_view* out_service_name) {
  auto type_res = attrs_val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return;
  }
  const auto type = type_res.value();

  if (type == simdjson::ondemand::json_type::array) {
    auto arr_res = attrs_val.get_array();
    if (arr_res.error() != simdjson::SUCCESS) {
      return;
    }
    for (auto elem_res : arr_res.value()) {
      if (elem_res.error() != simdjson::SUCCESS) continue;
      auto item_val = elem_res.value();
      auto obj_res = item_val.get_object();
      if (obj_res.error() != simdjson::SUCCESS) continue;
      auto kv_obj = obj_res.value();

      auto key_res = kv_obj.find_field_unordered("key");
      if (key_res.error() != simdjson::SUCCESS) continue;
      auto key_str_res = key_res.value().get_string();
      if (key_str_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = key_str_res.value();

      auto val_res = kv_obj.find_field_unordered("value");
      if (val_res.error() != simdjson::SUCCESS) continue;
      auto v = val_res.value();

      writer.Key(k);
      std::string_view scalar_str;
      const bool is_scalar_str = WriteAnyValueJsonImpl(writer, v, 0, &scalar_str);
      if (out_service_name != nullptr && (k == "service.name" || k == "service_name")) {
        UpdateServiceNameFromAttr(out_service_name, is_scalar_str, scalar_str);
      }
    }
  } else if (type == simdjson::ondemand::json_type::object) {
    auto obj_res = attrs_val.get_object();
    if (obj_res.error() != simdjson::SUCCESS) {
      return;
    }
    for (auto field_res : obj_res.value()) {
      if (field_res.error() != simdjson::SUCCESS) continue;
      auto key_res = field_res.unescaped_key();
      if (key_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = key_res.value();

      auto val_res = field_res.value();
      if (val_res.error() != simdjson::SUCCESS) continue;
      auto v = val_res.value();

      writer.Key(k);
      std::string_view scalar_str;
      const bool is_scalar_str = WriteAnyValueJsonImpl(writer, v, 0, &scalar_str);
      if (out_service_name != nullptr && (k == "service.name" || k == "service_name")) {
        UpdateServiceNameFromAttr(out_service_name, is_scalar_str, scalar_str);
      }
    }
  }
}

void WriteAttributesJson(FastJsonWriter& writer, simdjson::ondemand::value& attrs_val,
                         std::string_view* out_service_name) {
  writer.StartObject();
  WriteAttributesEntriesJson(writer, attrs_val, out_service_name);
  writer.EndObject();
}

void SerializeExemplarsJson(simdjson::ondemand::value& val, FastJsonWriter& writer) {
  static thread_local FastJsonWriter tls_exemplar_attrs_writer;
  writer.StartArray();
  auto arr_res = val.get_array();
  if (arr_res.error() == simdjson::SUCCESS) {
    for (auto elem_res : arr_res.value()) {
      if (elem_res.error() != simdjson::SUCCESS) continue;
      auto elem_val = elem_res.value();
      auto ex_obj_res = elem_val.get_object();
      if (ex_obj_res.error() != simdjson::SUCCESS) continue;
      auto ex_obj = ex_obj_res.value();

      std::array<char, 32> time_iso_buf;
      size_t time_iso_len = 0;
      std::string_view span_id;
      std::string_view trace_id;
      IdHexBuffer span_id_buf{};
      IdHexBuffer trace_id_buf{};
      enum class ExValKind { kNone, kDouble, kInt64 } ex_val_kind = ExValKind::kNone;
      double ex_double_val = 0.0;
      int64_t ex_int_val = 0;
      bool has_filtered_attrs = false;
      tls_exemplar_attrs_writer.Reset();

      for (auto field_res : ex_obj) {
        if (field_res.error() != simdjson::SUCCESS) continue;
        auto k_res = field_res.unescaped_key();
        if (k_res.error() != simdjson::SUCCESS) continue;
        std::string_view k = k_res.value();
        auto v_res = field_res.value();
        if (v_res.error() != simdjson::SUCCESS) continue;
        auto fv = v_res.value();

        if (k == "timeUnixNano" || k == "time_unix_nano") {
          uint64_t nanos = ParseUint64(fv);
          if (nanos > 0) {
            FormatIso8601Nano(nanos, time_iso_buf.data());
            time_iso_len = 30;
          }
        } else if (k == "spanId" || k == "span_id") {
          auto s_res = fv.get_string();
          if (s_res.error() == simdjson::SUCCESS) {
            span_id = s_res.value();
          }
        } else if (k == "traceId" || k == "trace_id") {
          auto s_res = fv.get_string();
          if (s_res.error() == simdjson::SUCCESS) {
            trace_id = s_res.value();
          }
        } else if (k == "asDouble" || k == "as_double" || k == "value") {
          if (TryParseDouble(fv, ex_double_val)) {
            ex_val_kind = ExValKind::kDouble;
          }
        } else if (k == "asInt" || k == "as_int") {
          if (TryParseInt64(fv, ex_int_val)) {
            ex_val_kind = ExValKind::kInt64;
          }
        } else if (k == "filteredAttributes" || k == "filtered_attributes") {
          tls_exemplar_attrs_writer.Reset();
          WriteAttributesJson(tls_exemplar_attrs_writer, fv);
          has_filtered_attrs = true;
        }
      }

      writer.StartObject();
      writer.Key("FilteredAttributes");
      if (has_filtered_attrs) {
        writer.RawJson(tls_exemplar_attrs_writer.Str());
      } else {
        writer.StartObject();
        writer.EndObject();
      }
      writer.Key("TimeUnix");
      writer.String(std::string_view(time_iso_buf.data(), time_iso_len));
      if (ex_val_kind == ExValKind::kDouble) {
        writer.Key("Value");
        writer.Double(ex_double_val);
      } else if (ex_val_kind == ExValKind::kInt64) {
        writer.Key("Value");
        writer.Int64(ex_int_val);
      }
      writer.Key("SpanId");
      writer.String(NormalizeSpanId(span_id, span_id_buf));
      writer.Key("TraceId");
      writer.String(NormalizeTraceId(trace_id, trace_id_buf));
      writer.EndObject();
    }
  }
  writer.EndArray();
  tls_exemplar_attrs_writer.Trim();
}

}  // namespace otlp_json_adl_barrier

void ResetServiceNameScratch() noexcept { ResetScratchString(tls_service_name_scratch); }

std::string_view TrimJsonWhitespace(std::string_view sv) noexcept {
  while (!sv.empty() &&
         (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() &&
         (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
    sv.remove_suffix(1);
  }
  return sv;
}

bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'a' && ca <= 'z') {
      ca = static_cast<char>(ca - 'a' + 'A');
    }
    if (cb >= 'a' && cb <= 'z') {
      cb = static_cast<char>(cb - 'a' + 'A');
    }
    if (ca != cb) {
      return false;
    }
  }
  return true;
}

bool ParseJsonTimestamp(simdjson::ondemand::value& fv, uint64_t& out_ns) noexcept {
  auto type_res = fv.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return false;
  }
  const auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::string) {
    auto s_res = fv.get_string();
    if (s_res.error() != simdjson::SUCCESS) {
      return false;
    }
    return ParseIso8601ToUnixNano(s_res.value(), out_ns);
  }
  if (type == simdjson::ondemand::json_type::number) {
    auto num_res = fv.get_number();
    if (num_res.error() != simdjson::SUCCESS) {
      return false;
    }
    const auto num = num_res.value();
    if (num.is_uint64()) {
      out_ns = num.get_uint64();
      return true;
    }
    if (num.is_int64()) {
      const int64_t iv = num.get_int64();
      if (iv < 0) {
        return false;
      }
      out_ns = static_cast<uint64_t>(iv);
      return true;
    }
    if (num.is_double()) {
      const double d = num.get_double();
      if (std::isfinite(d) && d >= 0.0 && d < 18446744073709551616.0) {
        out_ns = static_cast<uint64_t>(d);
        return true;
      }
    }
    return false;
  }
  if (type == simdjson::ondemand::json_type::object) {
    auto obj_res = fv.get_object();
    if (obj_res.error() != simdjson::SUCCESS) {
      return false;
    }
    int64_t year = 0;
    int64_t month = 0;
    int64_t day = 0;
    int64_t hour = 0;
    int64_t min = 0;
    int64_t sec = 0;
    int64_t nanosec = 0;
    bool has_year = false;
    bool has_month = false;
    bool has_day = false;
    for (auto field_res : obj_res.value()) {
      if (field_res.error() != simdjson::SUCCESS) continue;
      auto k_res = field_res.unescaped_key();
      if (k_res.error() != simdjson::SUCCESS) continue;
      std::string_view k = k_res.value();
      auto v_res = field_res.value();
      if (v_res.error() != simdjson::SUCCESS) continue;
      auto v = v_res.value();
      if (k == "year") {
        year = ParseInt64(v);
        has_year = true;
      } else if (k == "month") {
        month = ParseInt64(v);
        has_month = true;
      } else if (k == "day") {
        day = ParseInt64(v);
        has_day = true;
      } else if (k == "hour") {
        hour = ParseInt64(v);
      } else if (k == "min") {
        min = ParseInt64(v);
      } else if (k == "sec") {
        sec = ParseInt64(v);
      } else if (k == "nanosec" || k == "nanosecond" || k == "nsec") {
        nanosec = ParseInt64(v);
      }
    }
    if (!has_year || !has_month || !has_day) {
      return false;
    }
    return CivilToUnixNano(year, month, day, hour, min, sec, nanosec, out_ns);
  }
  return false;
}

}  // namespace vector_otel::core
