-- Verifies the build-time record limit end to end through the REAL shared modules.
-- Usage: lua5.4 tests/lua/test_record_limit.lua [configured-limit-records]
-- (LUA_CPATH must point at the build directory holding vector_otel.so / vector_native.so.)
local limit = tonumber(arg[1]) or 2048
assert(limit and limit >= 1, "usage: test_record_limit.lua [limit-records]")

local expected = string.format("event exceeds %d-record limit", limit)
local failures = 0

local function noop() end

-- 1. Test vector_otel (OTLP table mode)
local otel = require("vector_otel")
for _, signal in ipairs({ "logs", "traces", "metrics" }) do
  local records = {}
  for i = 1, limit do
    if signal == "logs" then
      records[i] = { timeUnixNano = "1000000000", body = { stringValue = "msg" } }
    elseif signal == "traces" then
      records[i] = { traceId = "0123456789abcdef0123456789abcdef", spanId = "0123456789abcdef", name = "span" }
    else
      records[i] = { name = "m", gauge = { dataPoints = { { asInt = "1" } } } }
    end
  end

  local chunk_key = signal == "logs" and "resourceLogs" or (signal == "traces" and "resourceSpans" or "resourceMetrics")
  local scope_key = signal == "logs" and "scopeLogs" or (signal == "traces" and "scopeSpans" or "scopeMetrics")
  local item_key = signal == "logs" and "logRecords" or (signal == "traces" and "spans" or "metrics")

  local payload = {
    [chunk_key] = {
      {
        [scope_key] = {
          {
            [item_key] = records
          }
        }
      }
    }
  }

  local count, err = otel[signal].process(payload, noop)
  if count ~= limit or err ~= nil then
    failures = failures + 1
    io.stderr:write(string.format("FAIL vector_otel.%s at limit: got (%s, %s)\n", signal, tostring(count), tostring(err)))
  end

  -- Add one record to exceed the limit
  if signal == "logs" then
    records[limit + 1] = { timeUnixNano = "1000000000", body = { stringValue = "extra" } }
  elseif signal == "traces" then
    records[limit + 1] = { traceId = "0123456789abcdef0123456789abcdef", spanId = "0123456789abcdef", name = "extra" }
  else
    records[limit + 1] = { name = "m_extra", gauge = { dataPoints = { { asInt = "1" } } } }
  end

  local over_count, over_err, partial = otel[signal].process(payload, noop)
  if over_count ~= nil or over_err ~= expected or partial ~= 0 then
    failures = failures + 1
    io.stderr:write(string.format("FAIL vector_otel.%s over limit: got (%s, %s, %s)\n", signal,
      tostring(over_count), tostring(over_err), tostring(partial)))
  end
end

-- 2. Test vector_native (Native table mode)
local native = require("vector_native")
for _, signal in ipairs({ "logs", "traces", "metrics" }) do
  local records = {}
  for i = 1, limit do
    if signal == "logs" then
      records[i] = { message = "log " .. i }
    elseif signal == "traces" then
      records[i] = { trace_id = "0123456789abcdef0123456789abcdef", span_id = "0123456789abcdef", name = "span" }
    else
      records[i] = { name = "gauge_metric", gauge = { value = 1.0 } }
    end
  end

  local count, err = native[signal].process(records, noop)
  if count ~= limit or err ~= nil then
    failures = failures + 1
    io.stderr:write(string.format("FAIL vector_native.%s at limit: got (%s, %s)\n", signal, tostring(count), tostring(err)))
  end

  -- Add one record to exceed the limit
  if signal == "logs" then
    records[limit + 1] = { message = "extra" }
  elseif signal == "traces" then
    records[limit + 1] = { trace_id = "0123456789abcdef0123456789abcdef", span_id = "0123456789abcdef", name = "extra" }
  else
    records[limit + 1] = { name = "extra", gauge = { value = 2.0 } }
  end

  local over_count, over_err, partial = native[signal].process(records, noop)
  if over_count ~= nil or over_err ~= expected or partial ~= 0 then
    failures = failures + 1
    io.stderr:write(string.format("FAIL vector_native.%s over limit: got (%s, %s, %s)\n", signal,
      tostring(over_count), tostring(over_err), tostring(partial)))
  end
end

if failures > 0 then
  os.exit(1)
end
print(string.format("RECORD LIMIT OK (limit %d records, both modules, all signals)", limit))
