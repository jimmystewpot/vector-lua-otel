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
// Thread-local scratch buffers for service.name / service_name to prevent use-after-free
// when Lua GC removes the attribute from ResourceAttributes or when fallback is needed.
thread_local std::string tls_service_name_scratch;
thread_local std::string tls_snake_service_name_scratch;

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
    throw std::runtime_error(simdjson::error_message(type_res.error()));
  }
  const auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::number) {
    auto num_res = val.get_number();
    if (num_res.error() != simdjson::SUCCESS) {
      if (num_res.error() != simdjson::NUMBER_OUT_OF_RANGE &&
          num_res.error() != simdjson::BIGINT_ERROR) {
        throw std::runtime_error(simdjson::error_message(num_res.error()));
      }
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
      throw std::runtime_error(simdjson::error_message(str_res.error()));
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
    return false;
  }

  if (type == simdjson::ondemand::json_type::boolean) {
    if (auto err = val.get_bool().error(); err != simdjson::SUCCESS) {
      throw std::runtime_error(simdjson::error_message(err));
    }
  } else if (type == simdjson::ondemand::json_type::null) {
    if (auto err = val.is_null().error(); err != simdjson::SUCCESS) {
      throw std::runtime_error(simdjson::error_message(err));
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
    throw std::runtime_error(simdjson::error_message(type_res.error()));
  }
  const auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::number) {
    auto num_res = val.get_number();
    if (num_res.error() != simdjson::SUCCESS) {
      if (num_res.error() != simdjson::NUMBER_OUT_OF_RANGE &&
          num_res.error() != simdjson::BIGINT_ERROR) {
        throw std::runtime_error(simdjson::error_message(num_res.error()));
      }
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
      throw std::runtime_error(simdjson::error_message(str_res.error()));
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
    return 0;
  }

  if (type == simdjson::ondemand::json_type::boolean) {
    if (auto err = val.get_bool().error(); err != simdjson::SUCCESS) {
      throw std::runtime_error(simdjson::error_message(err));
    }
  } else if (type == simdjson::ondemand::json_type::null) {
    if (auto err = val.is_null().error(); err != simdjson::SUCCESS) {
      throw std::runtime_error(simdjson::error_message(err));
    }
  }
  return 0;
}

namespace {

bool TryExtractJsonScalarString(simdjson::ondemand::value& val, std::string_view& out_str) {
  auto val_type_res = val.type();
  if (val_type_res.error() != simdjson::SUCCESS) {
    return false;
  }
  if (val_type_res.value() == simdjson::ondemand::json_type::string) {
    auto str_res = val.get_string();
    if (str_res.error() == simdjson::SUCCESS && !str_res.value().empty()) {
      out_str = str_res.value();
      return true;
    }
    return false;
  }
  if (val_type_res.value() == simdjson::ondemand::json_type::object) {
    auto val_obj_res = val.get_object();
    if (val_obj_res.error() != simdjson::SUCCESS) {
      return false;
    }
    auto val_obj = val_obj_res.value();
    auto count_res = val_obj.count_fields();
    if (count_res.error() != simdjson::SUCCESS || count_res.value() != 1) {
      return false;
    }
    auto sv_res = val_obj.find_field_unordered("stringValue");
    if (sv_res.error() == simdjson::SUCCESS) {
      auto str_res = sv_res.value().get_string();
      if (str_res.error() == simdjson::SUCCESS && !str_res.value().empty()) {
        out_str = str_res.value();
        return true;
      }
      return false;
    }
    auto sv_snake_res = val_obj.find_field_unordered("string_value");
    if (sv_snake_res.error() == simdjson::SUCCESS) {
      auto str_res = sv_snake_res.value().get_string();
      if (str_res.error() == simdjson::SUCCESS && !str_res.value().empty()) {
        out_str = str_res.value();
        return true;
      }
    }
  }
  return false;
}

}  // namespace

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

  std::string_view dot_svc;
  std::string_view snake_svc;
  bool has_dot_svc = false;
  bool has_snake_svc = false;

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
      std::string_view k = key_str_res.value();
      if (k != "service.name" && k != "service_name") continue;

      auto val_res = kv_obj.find_field_unordered("value");
      if (val_res.error() != simdjson::SUCCESS) continue;
      auto val = val_res.value();
      std::string_view candidate;
      const bool ok = TryExtractJsonScalarString(val, candidate);
      if (k == "service.name") {
        has_dot_svc = ok;
        if (ok) dot_svc = candidate;
      } else {
        has_snake_svc = ok;
        if (ok) snake_svc = candidate;
      }
    }
  } else if (type == simdjson::ondemand::json_type::object) {
    auto obj_res = attrs_val.get_object();
    if (obj_res.error() == simdjson::SUCCESS) {
      for (auto field_res : obj_res.value()) {
        if (field_res.error() != simdjson::SUCCESS) continue;
        auto k_res = field_res.unescaped_key();
        if (k_res.error() != simdjson::SUCCESS) continue;
        std::string_view k = k_res.value();
        if (k != "service.name" && k != "service_name") continue;
        auto v_res = field_res.value();
        if (v_res.error() != simdjson::SUCCESS) continue;
        auto v = v_res.value();
        std::string_view candidate;
        const bool ok = TryExtractJsonScalarString(v, candidate);
        if (k == "service.name") {
          has_dot_svc = ok;
          if (ok) dot_svc = candidate;
        } else {
          has_snake_svc = ok;
          if (ok) snake_svc = candidate;
        }
      }
    }
  }
  if (has_dot_svc) {
    return dot_svc;
  }
  if (has_snake_svc) {
    return snake_svc;
  }
  return "unknown_service";
}

bool TryParseDouble(simdjson::ondemand::value& val, double& out) {
  auto type_res = val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    throw std::runtime_error(simdjson::error_message(type_res.error()));
  }
  const auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::number) {
    auto num_res = val.get_double();
    if (num_res.error() != simdjson::SUCCESS) {
      if (num_res.error() != simdjson::NUMBER_OUT_OF_RANGE &&
          num_res.error() != simdjson::BIGINT_ERROR) {
        throw std::runtime_error(simdjson::error_message(num_res.error()));
      }
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
      throw std::runtime_error(simdjson::error_message(str_res.error()));
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
    return false;
  }

  if (type == simdjson::ondemand::json_type::boolean) {
    if (auto err = val.get_bool().error(); err != simdjson::SUCCESS) {
      throw std::runtime_error(simdjson::error_message(err));
    }
  } else if (type == simdjson::ondemand::json_type::null) {
    if (auto err = val.is_null().error(); err != simdjson::SUCCESS) {
      throw std::runtime_error(simdjson::error_message(err));
    }
  }
  return false;
}

double ParseDouble(simdjson::ondemand::value& val) {
  double d = 0.0;
  TryParseDouble(val, d);
  return d;
}

// ---- staged attributes (no Lua) ---------------------------------------------------------------

namespace {

// Stages one AnyValue under `key`. Mirrors PushAnyValueScalar(): unsupported values stage nothing.
// Returns true (and sets `*text`) when the value is a scalar string.
bool StageAnyValueImpl(StagedRow& row, Key key, simdjson::ondemand::value& val,
                       FastJsonWriter& json_writer, std::string_view* text) {
  auto type_res = val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return false;
  }
  auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::object) {
    auto obj_res = val.get_object();
    if (obj_res.error() != simdjson::SUCCESS) {
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
            const std::string_view s = s_res.value();
            row.Str(key, s);
            if (text != nullptr) *text = s;
            matched = true;
            is_scalar_string = true;
            break;
          }
        } else if (k == "intValue" || k == "int_value") {
          row.Int(key, ParseInt64(fv));
          matched = true;
          break;
        } else if (k == "uintValue" || k == "uint_value") {
          row.Uint(key, ParseUint64(fv));  // clamped to INT64_MAX by the row
          matched = true;
          break;
        } else if (k == "doubleValue" || k == "double_value") {
          row.Dbl(key, ParseDouble(fv));
          matched = true;
          break;
        } else if (k == "boolValue" || k == "bool_value") {
          auto b_res = fv.get_bool();
          row.Bool(key, b_res.error() == simdjson::SUCCESS ? b_res.value() : false);
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
          row.Str(key, json_writer.Str());
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
    if (!matched && has_plain_fields) {
      json_writer.EndObject();
      row.Str(key, json_writer.Str());
    }
    return is_scalar_string;
  } else if (type == simdjson::ondemand::json_type::string) {
    auto s_res = val.get_string();
    if (s_res.error() == simdjson::SUCCESS) {
      const std::string_view s = s_res.value();
      row.Str(key, s);
      if (text != nullptr) *text = s;
      return true;
    }
    return false;
  } else if (type == simdjson::ondemand::json_type::number) {
    auto num_res = val.get_number();
    if (num_res.error() == simdjson::SUCCESS) {
      auto num = num_res.value();
      if (num.is_int64()) {
        row.Int(key, num.get_int64());
      } else if (num.is_uint64()) {
        row.Uint(key, num.get_uint64());
      } else {
        row.Dbl(key, num.get_double());
      }
    }
    return false;
  } else if (type == simdjson::ondemand::json_type::boolean) {
    auto b_res = val.get_bool();
    if (b_res.error() == simdjson::SUCCESS) {
      row.Bool(key, b_res.value());
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
    row.Str(key, json_writer.Str());
    return false;
  }
  return false;
}

// service.name capture: last matching key wins; anything but a non-empty scalar string resets to
// "unknown_service" (exactly CaptureServiceName() above).
void CaptureServiceNameText(bool is_scalar_str, std::string_view text,
                            std::string_view* out_service_name) {
  if (!is_scalar_str || text.empty()) {
    *out_service_name = "unknown_service";
    return;
  }
  ReserveScratchString(tls_service_name_scratch, 128);
  tls_service_name_scratch.assign(text.data(), text.size());
  *out_service_name = std::string_view(tls_service_name_scratch);
}

}  // namespace

bool StageAnyValue(StagedRow& row, Key key, simdjson::ondemand::value& val,
                   FastJsonWriter& json_writer, std::string_view* scalar_text) {
  return StageAnyValueImpl(row, key, val, json_writer, scalar_text);
}

void StageAttributeFields(StagedRow& row, simdjson::ondemand::value& attrs_val,
                          FastJsonWriter& json_writer, std::string_view* out_service_name) {
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

      std::string_view text;
      const bool is_scalar_str = StageAnyValueImpl(row, Key::Dynamic(k), v, json_writer, &text);
      if (out_service_name != nullptr && (k == "service.name" || k == "service_name")) {
        CaptureServiceNameText(is_scalar_str, text, out_service_name);
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

      std::string_view text;
      const bool is_scalar_str = StageAnyValueImpl(row, Key::Dynamic(k), v, json_writer, &text);
      if (out_service_name != nullptr && (k == "service.name" || k == "service_name")) {
        CaptureServiceNameText(is_scalar_str, text, out_service_name);
      }
    }
  }
}

void StageAttributes(StagedRow& row, Key key, simdjson::ondemand::value& attrs_val,
                     FastJsonWriter& json_writer, std::string_view* out_service_name) {
  row.BeginMap(key);
  StageAttributeFields(row, attrs_val, json_writer, out_service_name);
  row.End();
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
    auto count_res = obj.count_fields();
    if (count_res.error() != simdjson::SUCCESS) {
      throw std::runtime_error(simdjson::error_message(count_res.error()));
    }
    const bool is_single_field = (count_res.value() == 1);
    bool matched = false;
    bool is_scalar_string = false;
    bool has_plain_fields = false;
    for (auto field_res : obj) {
      if (field_res.error() != simdjson::SUCCESS) {
        throw std::runtime_error(simdjson::error_message(field_res.error()));
      }
      auto k_res = field_res.unescaped_key();
      if (k_res.error() != simdjson::SUCCESS) {
        throw std::runtime_error(simdjson::error_message(k_res.error()));
      }
      std::string_view k = k_res.value();

      auto val_res = field_res.value();
      if (val_res.error() != simdjson::SUCCESS) {
        throw std::runtime_error(simdjson::error_message(val_res.error()));
      }
      auto fv = val_res.value();

      if (is_single_field && !has_plain_fields) {
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
                      if (elem_res.error() != simdjson::SUCCESS) {
                        throw std::runtime_error(simdjson::error_message(elem_res.error()));
                      }
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
                  if (elem_res.error() != simdjson::SUCCESS) {
                    throw std::runtime_error(simdjson::error_message(elem_res.error()));
                  }
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
                if (kv_field_res.error() != simdjson::SUCCESS) {
                  throw std::runtime_error(simdjson::error_message(kv_field_res.error()));
                }
                auto sub_k_res = kv_field_res.unescaped_key();
                if (sub_k_res.error() != simdjson::SUCCESS) {
                  throw std::runtime_error(simdjson::error_message(sub_k_res.error()));
                }
                std::string_view sub_k = sub_k_res.value();
                auto sub_v_res = kv_field_res.value();
                if (sub_v_res.error() != simdjson::SUCCESS) {
                  throw std::runtime_error(simdjson::error_message(sub_v_res.error()));
                }
                auto sub_v = sub_v_res.value();

                if (!has_direct_fields && sub_k == "values") {
                  auto sub_t_res = sub_v.type();
                  if (sub_t_res.error() == simdjson::SUCCESS &&
                      sub_t_res.value() == simdjson::ondemand::json_type::array) {
                    auto arr_res = sub_v.get_array();
                    if (arr_res.error() == simdjson::SUCCESS) {
                      for (auto elem_res : arr_res.value()) {
                        if (elem_res.error() != simdjson::SUCCESS) {
                          throw std::runtime_error(simdjson::error_message(elem_res.error()));
                        }
                        auto item_val = elem_res.value();
                        auto item_obj_res = item_val.get_object();
                        if (item_obj_res.error() != simdjson::SUCCESS) {
                          if (item_obj_res.error() != simdjson::INCORRECT_TYPE) {
                            throw std::runtime_error(simdjson::error_message(item_obj_res.error()));
                          }
                          continue;
                        }
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

void UpdateServiceNameCandidate(std::string_view* out_service_name, std::string_view k,
                                bool is_scalar_str, std::string_view scalar_str, bool& has_dot_svc,
                                bool& has_snake_svc) {
  if (out_service_name == nullptr) {
    return;
  }
  const bool valid = (is_scalar_str && !scalar_str.empty());
  if (k == "service.name") {
    has_dot_svc = valid;
    if (valid) {
      ReserveScratchString(tls_service_name_scratch, 128);
      tls_service_name_scratch.assign(scalar_str.data(), scalar_str.size());
    }
  } else if (k == "service_name") {
    has_snake_svc = valid;
    if (valid) {
      ReserveScratchString(tls_snake_service_name_scratch, 128);
      tls_snake_service_name_scratch.assign(scalar_str.data(), scalar_str.size());
    }
  } else {
    return;
  }
  if (has_dot_svc) {
    *out_service_name = std::string_view(tls_service_name_scratch);
  } else if (has_snake_svc) {
    *out_service_name = std::string_view(tls_snake_service_name_scratch);
  } else {
    *out_service_name = "unknown_service";
  }
}

bool TryParseExemplarGenericValue(simdjson::ondemand::value& val, int64_t& out_int,
                                  double& out_double, bool& out_is_int) {
  auto type_res = val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    throw std::runtime_error(simdjson::error_message(type_res.error()));
  }
  const auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::number) {
    auto num_res = val.get_number();
    if (num_res.error() != simdjson::SUCCESS) {
      if (num_res.error() != simdjson::NUMBER_OUT_OF_RANGE &&
          num_res.error() != simdjson::BIGINT_ERROR) {
        throw std::runtime_error(simdjson::error_message(num_res.error()));
      }
      return false;
    }
    const auto num = num_res.value();
    if (num.is_int64()) {
      out_int = num.get_int64();
      out_is_int = true;
      return true;
    }
    if (num.is_uint64()) {
      const uint64_t u = num.get_uint64();
      out_int = (u > static_cast<uint64_t>(INT64_MAX)) ? INT64_MAX : static_cast<int64_t>(u);
      out_is_int = true;
      return true;
    }
    if (num.is_double()) {
      const double d = num.get_double();
      if (std::isfinite(d)) {
        out_double = d;
        out_is_int = false;
        return true;
      }
    }
    return false;
  }
  if (type == simdjson::ondemand::json_type::string) {
    auto str_res = val.get_string();
    if (str_res.error() != simdjson::SUCCESS) {
      throw std::runtime_error(simdjson::error_message(str_res.error()));
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
    int64_t iv = 0;
    auto [iptr, iec] = std::from_chars(sv.data(), sv.data() + sv.size(), iv);
    if (iec == std::errc() && iptr == sv.data() + sv.size()) {
      out_int = iv;
      out_is_int = true;
      return true;
    }
    double d = 0.0;
    auto [dptr, dec] = std::from_chars(sv.data(), sv.data() + sv.size(), d);
    if (dec == std::errc() && dptr == sv.data() + sv.size() && std::isfinite(d)) {
      out_double = d;
      out_is_int = false;
      return true;
    }
    return false;
  }
  if (type == simdjson::ondemand::json_type::boolean) {
    if (auto err = val.get_bool().error(); err != simdjson::SUCCESS) {
      throw std::runtime_error(simdjson::error_message(err));
    }
  } else if (type == simdjson::ondemand::json_type::null) {
    if (auto err = val.is_null().error(); err != simdjson::SUCCESS) {
      throw std::runtime_error(simdjson::error_message(err));
    }
  }
  return false;
}

void WriteUint64Array(FastJsonWriter& w, const std::vector<uint64_t>& vec) {
  w.StartArray();
  for (size_t i = 0; i < vec.size(); ++i) {
    w.Uint64(vec[i]);
  }
  w.EndArray();
}

void WriteDoubleArray(FastJsonWriter& w, const std::vector<double>& vec) {
  w.StartArray();
  for (size_t i = 0; i < vec.size(); ++i) {
    w.Double(vec[i]);
  }
  w.EndArray();
}

void SerializeExpBucketsSubObject(FastJsonWriter& w, int64_t offset,
                                  const std::vector<uint64_t>& bucket_counts) {
  w.StartObject();
  w.Key("offset");
  w.Int64(offset);
  w.Key("bucketCounts");
  WriteUint64Array(w, bucket_counts);
  w.EndObject();
}

void WriteQuantilesArray(FastJsonWriter& w, const std::vector<SummaryQuantile>& quantiles) {
  w.StartArray();
  for (size_t i = 0; i < quantiles.size(); ++i) {
    w.StartObject();
    w.Key("quantile");
    w.Double(quantiles[i].quantile);
    w.Key("value");
    w.Double(quantiles[i].value);
    w.EndObject();
  }
  w.EndArray();
}

}  // namespace

void WriteAnyValueJson(FastJsonWriter& writer, simdjson::ondemand::value& val, int depth) {
  (void)WriteAnyValueJsonImpl(writer, val, depth, nullptr);
}

std::string_view ExtractOtelBodyView(simdjson::ondemand::value& body_val, FastJsonWriter& writer) {
  auto type_res = body_val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    return "";
  }
  const auto type = type_res.value();
  if (type == simdjson::ondemand::json_type::null) {
    return "";
  }
  if (type == simdjson::ondemand::json_type::string) {
    auto s_res = body_val.get_string();
    return (s_res.error() == simdjson::SUCCESS) ? s_res.value() : "";
  }
  if (type == simdjson::ondemand::json_type::object) {
    auto obj_res = body_val.get_object();
    if (obj_res.error() != simdjson::SUCCESS) {
      return "";
    }
    auto obj = obj_res.value();
    auto str_res = obj.find_field_unordered("stringValue");
    if (str_res.error() != simdjson::SUCCESS) {
      str_res = obj.find_field_unordered("string_value");
    }
    if (str_res.error() == simdjson::SUCCESS) {
      auto s_res = str_res.value().get_string();
      if (s_res.error() == simdjson::SUCCESS) {
        return s_res.value();
      }
    }
    obj.reset();
  }

  writer.Reset();
  WriteAnyValueJson(writer, body_val, 0);
  return writer.Str();
}

void WriteAttributesEntriesJson(FastJsonWriter& writer, simdjson::ondemand::value& attrs_val,
                                std::string_view* out_service_name) {
  auto type_res = attrs_val.type();
  if (type_res.error() != simdjson::SUCCESS) {
    throw std::runtime_error(simdjson::error_message(type_res.error()));
  }
  const auto type = type_res.value();
  bool has_dot_svc = false;
  bool has_snake_svc = false;

  if (type == simdjson::ondemand::json_type::array) {
    auto arr_res = attrs_val.get_array();
    if (arr_res.error() != simdjson::SUCCESS) {
      throw std::runtime_error(simdjson::error_message(arr_res.error()));
    }
    for (auto elem_res : arr_res.value()) {
      if (elem_res.error() != simdjson::SUCCESS) {
        throw std::runtime_error(simdjson::error_message(elem_res.error()));
      }
      auto item_val = elem_res.value();
      auto obj_res = item_val.get_object();
      if (obj_res.error() != simdjson::SUCCESS) {
        if (obj_res.error() != simdjson::INCORRECT_TYPE) {
          throw std::runtime_error(simdjson::error_message(obj_res.error()));
        }
        continue;
      }
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
        UpdateServiceNameCandidate(out_service_name, k, is_scalar_str, scalar_str, has_dot_svc,
                                   has_snake_svc);
      }
    }
  } else if (type == simdjson::ondemand::json_type::object) {
    auto obj_res = attrs_val.get_object();
    if (obj_res.error() != simdjson::SUCCESS) {
      throw std::runtime_error(simdjson::error_message(obj_res.error()));
    }
    for (auto field_res : obj_res.value()) {
      if (field_res.error() != simdjson::SUCCESS) {
        throw std::runtime_error(simdjson::error_message(field_res.error()));
      }
      auto key_res = field_res.unescaped_key();
      if (key_res.error() != simdjson::SUCCESS) {
        throw std::runtime_error(simdjson::error_message(key_res.error()));
      }
      std::string_view k = key_res.value();

      auto val_res = field_res.value();
      if (val_res.error() != simdjson::SUCCESS) {
        throw std::runtime_error(simdjson::error_message(val_res.error()));
      }
      auto v = val_res.value();

      writer.Key(k);
      std::string_view scalar_str;
      const bool is_scalar_str = WriteAnyValueJsonImpl(writer, v, 0, &scalar_str);
      if (out_service_name != nullptr && (k == "service.name" || k == "service_name")) {
        UpdateServiceNameCandidate(out_service_name, k, is_scalar_str, scalar_str, has_dot_svc,
                                   has_snake_svc);
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
      if (elem_res.error() != simdjson::SUCCESS) {
        throw std::runtime_error(simdjson::error_message(elem_res.error()));
      }
      auto elem_val = elem_res.value();
      auto ex_obj_res = elem_val.get_object();
      if (ex_obj_res.error() != simdjson::SUCCESS) {
        if (ex_obj_res.error() != simdjson::INCORRECT_TYPE) {
          throw std::runtime_error(simdjson::error_message(ex_obj_res.error()));
        }
        continue;
      }
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
        if (field_res.error() != simdjson::SUCCESS) {
          throw std::runtime_error(simdjson::error_message(field_res.error()));
        }
        auto k_res = field_res.unescaped_key();
        if (k_res.error() != simdjson::SUCCESS) {
          throw std::runtime_error(simdjson::error_message(k_res.error()));
        }
        std::string_view k = k_res.value();
        auto v_res = field_res.value();
        if (v_res.error() != simdjson::SUCCESS) {
          throw std::runtime_error(simdjson::error_message(v_res.error()));
        }
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
        } else if (k == "asDouble" || k == "as_double") {
          if (TryParseDouble(fv, ex_double_val)) {
            ex_val_kind = ExValKind::kDouble;
          }
        } else if (k == "asInt" || k == "as_int") {
          if (TryParseInt64(fv, ex_int_val)) {
            ex_val_kind = ExValKind::kInt64;
          }
        } else if (k == "value") {
          bool is_int = false;
          if (TryParseExemplarGenericValue(fv, ex_int_val, ex_double_val, is_int)) {
            ex_val_kind = is_int ? ExValKind::kInt64 : ExValKind::kDouble;
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
  } else if (arr_res.error() != simdjson::INCORRECT_TYPE) {
    throw std::runtime_error(simdjson::error_message(arr_res.error()));
  }
  writer.EndArray();
  tls_exemplar_attrs_writer.Trim();
}

void SerializeHistogramJson(FastJsonWriter& w, int64_t agg_temporality, uint64_t count,
                            bool has_sum, double sum, bool has_min, double min_val, bool has_max,
                            double max_val, const std::vector<uint64_t>& bucket_counts,
                            const std::vector<double>& explicit_bounds) {
  w.Reset();
  w.StartObject();
  w.Key("aggregationTemporality");
  w.Int64(agg_temporality);
  w.Key("count");
  w.Uint64(count);
  w.Key("sum");
  w.Double(has_sum ? sum : 0.0);
  if (has_min) {
    w.Key("min");
    w.Double(min_val);
  }
  if (has_max) {
    w.Key("max");
    w.Double(max_val);
  }
  w.Key("bucketCounts");
  WriteUint64Array(w, bucket_counts);
  w.Key("explicitBounds");
  WriteDoubleArray(w, explicit_bounds);
  w.EndObject();
}

void SerializeExponentialHistogramJson(FastJsonWriter& w, int64_t agg_temporality, uint64_t count,
                                       bool has_sum, double sum, bool has_min, double min_val,
                                       bool has_max, double max_val, int64_t scale,
                                       uint64_t zero_count, double zero_threshold,
                                       int64_t pos_offset, const std::vector<uint64_t>& pos_buckets,
                                       int64_t neg_offset,
                                       const std::vector<uint64_t>& neg_buckets) {
  w.Reset();
  w.StartObject();
  w.Key("aggregationTemporality");
  w.Int64(agg_temporality);
  w.Key("count");
  w.Uint64(count);
  if (has_sum) {
    w.Key("sum");
    w.Double(sum);
  }
  if (has_min) {
    w.Key("min");
    w.Double(min_val);
  }
  if (has_max) {
    w.Key("max");
    w.Double(max_val);
  }
  w.Key("scale");
  w.Int64(scale);
  w.Key("zeroCount");
  w.Uint64(zero_count);
  w.Key("zeroThreshold");
  w.Double(zero_threshold);
  w.Key("positive");
  SerializeExpBucketsSubObject(w, pos_offset, pos_buckets);
  w.Key("negative");
  SerializeExpBucketsSubObject(w, neg_offset, neg_buckets);
  w.EndObject();
}

void SerializeSummaryJson(FastJsonWriter& w, uint64_t count, bool has_sum, double sum,
                          const std::vector<SummaryQuantile>& quantiles) {
  w.Reset();
  w.StartObject();
  w.Key("count");
  w.Uint64(count);
  w.Key("sum");
  w.Double(has_sum ? sum : 0.0);
  w.Key("quantileValues");
  WriteQuantilesArray(w, quantiles);
  w.EndObject();
}

void ResetServiceNameScratch() noexcept {
  ResetScratchString(tls_service_name_scratch);
  ResetScratchString(tls_snake_service_name_scratch);
}

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

bool ParseJsonTimestamp(simdjson::ondemand::value& fv, uint64_t& out_ns) {
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
