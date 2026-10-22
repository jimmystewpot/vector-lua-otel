#pragma once

// Compact, deterministic payload builders shared by the worker-isolation tests and the extraction
// latency benchmark. Every payload carries `tag` (service name / body / span name) so a consumer
// can tell which producer a row came from. `n` is the exact number of records.

#include <cstddef>
#include <string>

namespace vector_otel::core::fixtures {

inline std::string OtlpResource(const std::string& tag) {
  return "\"resource\":{\"attributes\":[{\"key\":\"service.name\",\"value\":{\"stringValue\":\"" +
         tag + "\"}}]}";
}

inline std::string OtlpLogs(const std::string& tag, int n) {
  std::string out =
      "{\"resourceLogs\":[{" + OtlpResource(tag) + ",\"scopeLogs\":[{\"logRecords\":[";
  for (int i = 0; i < n; ++i) {
    if (i > 0) out += ',';
    out += "{\"timeUnixNano\":\"170000000000000000" + std::to_string(i % 10) +
           "\",\"severityText\":\"INFO\",\"body\":{\"stringValue\":\"" + tag + "-msg-" +
           std::to_string(i) + "\"}}";
  }
  return out + "]}]}]}";
}

inline std::string OtlpTraces(const std::string& tag, int n) {
  std::string out = "{\"resourceSpans\":[{" + OtlpResource(tag) + ",\"scopeSpans\":[{\"spans\":[";
  for (int i = 0; i < n; ++i) {
    if (i > 0) out += ',';
    out +=
        "{\"traceId\":\"0102030405060708090a0b0c0d0e0f10\",\"spanId\":\"0102030405060708\","
        "\"name\":\"" +
        tag + "-span-" + std::to_string(i) +
        "\",\"kind\":2,\"startTimeUnixNano\":\"1700000000000000001\","
        "\"endTimeUnixNano\":\"1700000000000000999\",\"attributes\":[{\"key\":\"k\",\"value\":"
        "{\"stringValue\":\"v\"}}],\"events\":[{\"timeUnixNano\":\"1700000000000000002\","
        "\"name\":\"ev\"}]}";
  }
  return out + "]}]}]}";
}

inline std::string OtlpMetrics(const std::string& tag, int n) {
  std::string out = "{\"resourceMetrics\":[{" + OtlpResource(tag) +
                    ",\"scopeMetrics\":[{\"metrics\":[{\"name\":\"" + tag +
                    "-m\",\"gauge\":{\"dataPoints\":[";
  for (int i = 0; i < n; ++i) {
    if (i > 0) out += ',';
    out += "{\"timeUnixNano\":\"170000000000000000" + std::to_string(i % 10) + "\",\"asInt\":\"" +
           std::to_string(i) + "\"}";
  }
  return out + "]}}]}]}]}";
}

inline std::string NativeLogs(const std::string& tag, int n) {
  std::string out = "[";
  for (int i = 0; i < n; ++i) {
    if (i > 0) out += ',';
    out += "{\"timestamp\":\"2023-11-14T22:13:20.000000001Z\",\"message\":\"" + tag + "-msg-" +
           std::to_string(i) + "\",\"resources\":{\"service.name\":\"" + tag + "\"}}";
  }
  return out + "]";
}

inline std::string NativeTraces(const std::string& tag, int n) {
  std::string out = "[";
  for (int i = 0; i < n; ++i) {
    if (i > 0) out += ',';
    out +=
        "{\"start_time_unix_nano\":\"2023-11-14T22:13:20.000000001Z\","
        "\"end_time_unix_nano\":\"2023-11-14T22:13:20.000000999Z\","
        "\"trace_id\":\"0102030405060708090a0b0c0d0e0f10\",\"span_id\":\"0102030405060708\","
        "\"name\":\"" +
        tag + "-span-" + std::to_string(i) + "\",\"kind\":2,\"resources\":{\"service.name\":\"" +
        tag + "\"},\"attributes\":{\"k\":\"v\"}}";
  }
  return out + "]";
}

inline std::string NativeMetrics(const std::string& tag, int n) {
  std::string out = "[";
  for (int i = 0; i < n; ++i) {
    if (i > 0) out += ',';
    out += "{\"name\":\"" + tag + "-m-" + std::to_string(i) +
           "\",\"kind\":\"incremental\",\"timestamp\":\"2023-11-14T22:13:20.000000001Z\","
           "\"tags\":{\"resource.service.name\":\"" +
           tag + "\"},\"gauge\":{\"value\":1.5}}";
  }
  return out + "]";
}

// Converts the JSON used above (objects, arrays, strings without exotic escapes, numbers) into a
// Lua chunk that returns the equivalent table.
inline void ConvertValue(const std::string& j, std::size_t& p, std::string& out) {
  switch (j[p]) {
    case '{': {
      out += '{';
      ++p;
      while (j[p] != '}') {
        if (j[p] == ',') {
          out += ',';
          ++p;
          continue;
        }
        out += '[';
        ConvertValue(j, p, out);  // key (string)
        out += "]=";
        ++p;  // ':'
        ConvertValue(j, p, out);
      }
      ++p;
      out += '}';
      break;
    }
    case '[': {
      out += '{';
      ++p;
      while (j[p] != ']') {
        if (j[p] == ',') {
          out += ',';
          ++p;
          continue;
        }
        ConvertValue(j, p, out);
      }
      ++p;
      out += '}';
      break;
    }
    case '"': {
      const std::size_t start = p++;
      while (j[p] != '"') p += (j[p] == '\\') ? 2 : 1;
      ++p;
      out.append(j, start, p - start);
      break;
    }
    default: {
      const std::size_t start = p;
      while (j[p] != ',' && j[p] != '}' && j[p] != ']') ++p;
      out.append(j, start, p - start);
    }
  }
}

inline std::string JsonToLuaChunk(const std::string& json) {
  std::string out = "return ";
  std::size_t p = 0;
  ConvertValue(json, p, out);
  return out;
}

inline constexpr const char* kSignals[3] = {"logs", "traces", "metrics"};

inline std::string BuildJson(bool native, int signal, const std::string& tag, int n) {
  if (native) {
    return signal == 0 ? NativeLogs(tag, n)
                       : (signal == 1 ? NativeTraces(tag, n) : NativeMetrics(tag, n));
  }
  return signal == 0 ? OtlpLogs(tag, n) : (signal == 1 ? OtlpTraces(tag, n) : OtlpMetrics(tag, n));
}

}  // namespace vector_otel::core::fixtures
