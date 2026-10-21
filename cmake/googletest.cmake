# Build vendored googletest instead of fetching from network
set(googletest_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest")
if(NOT EXISTS "${googletest_source_dir}/CMakeLists.txt")
  message(FATAL_ERROR
    "googletest CMakeLists.txt not found at: ${googletest_source_dir}\n"
    "The googletest dependency is vendored. Please ensure third_party/googletest is present.\n"
    "See: third_party/README.md"
  )
endif()

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${googletest_source_dir}" "${CMAKE_BINARY_DIR}/third_party/googletest")
