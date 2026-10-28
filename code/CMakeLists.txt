cmake_minimum_required(VERSION 3.20)
project(DuckEmu LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Global language settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

option(DUCKEMU_BUILD_TESTS "Build the test suite" ON)
option(DUCKEMU_USE_SYSTEM_UNICORN
       "Link the system libunicorn instead of code/third_party/unicorn" OFF)

duckemu_setup_sanitizers()

# ---------------------------------------------------------------------------
# Unicorn
#
# The pinned submodule is the default: the design docs cite source lines from
# it, and M0 measurements must come from the same tree.
# ---------------------------------------------------------------------------
if(DUCKEMU_USE_SYSTEM_UNICORN)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DUCKEMU_UNICORN REQUIRED IMPORTED_TARGET unicorn)
  add_library(duckemu_unicorn INTERFACE)
  target_link_libraries(duckemu_unicorn INTERFACE PkgConfig::DUCKEMU_UNICORN)
  message(STATUS "Unicorn: system library via pkg-config "
                 "(${DUCKEMU_UNICORN_VERSION})")
else()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/unicorn/uc.c")
    message(FATAL_ERROR
      "code/third_party/unicorn is missing. Run:\n"
      "  git submodule update --init code/third_party/unicorn")
  endif()

  # Keep the build minimal: the project only emulates AArch64.
  set(UNICORN_ARCH "aarch64" CACHE STRING "Unicorn architectures" FORCE)
  set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
  set(UNICORN_LEGACY_STATIC_ARCHIVE OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/unicorn EXCLUDE_FROM_ALL)

  add_library(duckemu_unicorn INTERFACE)
  target_link_libraries(duckemu_unicorn INTERFACE unicorn)
  target_include_directories(duckemu_unicorn INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/unicorn/include")
  message(STATUS "Unicorn: bundled submodule (aarch64 only)")
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(duckemu_common STATIC
  src/common/log/logger.cpp
)
target_include_directories(duckemu_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
duckemu_set_warnings(duckemu_common)

add_library(duckemu_runtime STATIC
  src/runtime/instance/runtime_instance.cpp
  src/runtime/memory/address_space.cpp
  src/runtime/memory/thunk/thunk_table.cpp
  src/runtime/backend/unicorn/unicorn_backend.cpp
  src/runtime/threads/tls/tls_manager.cpp
  src/runtime/svc/dispatcher.cpp
  src/runtime/svc/handlers/memory_handlers.cpp
  src/runtime/svc/handlers/fs_handlers.cpp
  src/runtime/svc/handlers/misc_handlers.cpp
)
target_include_directories(duckemu_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(duckemu_runtime PUBLIC
  duckemu_common
  duckemu_unicorn
  Threads::Threads
)
duckemu_set_warnings(duckemu_runtime)

# M0 spike: deliberately flat, no runtime abstractions.
add_executable(unicorn_spike tools/spike/unicorn_spike.cpp)
target_link_libraries(unicorn_spike PRIVATE
  duckemu_common
  duckemu_unicorn
  Threads::Threads
)
duckemu_set_warnings(unicorn_spike)

if(DUCKEMU_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
