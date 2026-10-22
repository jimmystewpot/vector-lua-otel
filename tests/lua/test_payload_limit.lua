-- Verifies the build-time payload limit end to end through the REAL shared modules.
-- Usage: lua5.4 tests/lua/test_payload_limit.lua <configured-limit-bytes>
-- (LUA_CPATH must point at the build directory holding vector_otel.so / vector_native.so.)
local limit = tonumber(arg[1])
assert(limit and limit >= 1, "usage: test_payload_limit.lua <limit-bytes>")

local expected = string.format("payload exceeds %d-byte uncompressed JSON limit", limit)
local failures = 0

local function noop() end

for _, module_name in ipairs({ "vector_otel", "vector_native" }) do
  local mod = require(module_name)
  for _, signal in ipairs({ "logs", "traces", "metrics" }) do
    -- One byte over the limit is rejected with the exact text, nothing is delivered.
    local count, err, partial = mod[signal].process(string.rep(" ", limit + 1), noop)
    if count ~= nil or err ~= expected or partial ~= 0 then
      failures = failures + 1
      io.stderr:write(string.format("FAIL %s.%s over-limit: got (%s, %s, %s)\n", module_name,
        signal, tostring(count), tostring(err), tostring(partial)))
    end
    -- Within the limit the call is not rejected for size (an empty object is a valid no-op).
    local ok_count, ok_err = mod[signal].process("{}", noop)
    if ok_count ~= 0 or ok_err ~= nil then
      failures = failures + 1
      io.stderr:write(string.format("FAIL %s.%s small payload: got (%s, %s)\n", module_name, signal,
        tostring(ok_count), tostring(ok_err)))
    end
  end
end

if failures > 0 then
  os.exit(1)
end
print(string.format("PAYLOAD LIMIT OK (limit %d bytes, both modules, all signals)", limit))
