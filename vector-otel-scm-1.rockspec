rockspec_format = "3.0"
package = "vector-otel"
version = "scm-1"

source = {
   url = "git+https://github.com/jimmystewpot/vector-lua-otel.git"
}

description = {
   summary = "High-performance C++ OpenTelemetry JSON batch flattener for Vector Lua 5.4 transforms",
   detailed = [[
      vector-otel is an ultra-fast Lua 5.4 C++ module using simdjson to stream-parse
      and flatten OTLP log, trace, and metric JSON batches into ClickHouse-aligned 
      columns with zero intermediate Lua table allocations.
   ]],
   homepage = "https://github.com/jimmystewpot/vector-lua-otel",
   license = "Apache-2.0"
}

dependencies = {
   "lua >= 5.4"
}

build = {
   type = "cmake",
   variables = {
      CMAKE_BUILD_TYPE = "Release",
      LUA_INCDIR = "$(LUA_INCDIR)",
      CMAKE_INSTALL_PREFIX = "$(PREFIX)",
      LIBDIR = "$(LIBDIR)"
   }
}
