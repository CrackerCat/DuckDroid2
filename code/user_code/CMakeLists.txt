# SPDX-License-Identifier: GPL-2.0-only
# Target projects.
#
# The installer is referenced explicitly by the project's main.cpp and by the
# target-specific tests, so a plain STATIC library is enough; no static-init
# registration and no whole-archive tricks.

set(DUCKEMU_ANDROID_ENV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    CACHE PATH "Root of the Android environment (contains DuckEmu/ and ROM/)")
set(DUCKEMU_ROM_LIB64
    "${DUCKEMU_ANDROID_ENV_ROOT}/ROM/out/target/product/lemonade/symbols/system/lib64"
    CACHE PATH "ROM system/lib64")
set(DUCKEMU_ROM_BOOTSTRAP "${DUCKEMU_ROM_LIB64}/bootstrap")

add_library(duckemu_user_libeleven STATIC
  libeleven_case/libeleven_env.cpp
)
target_include_directories(duckemu_user_libeleven PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(duckemu_user_libeleven PRIVATE duckemu_runtime)
duckemu_set_warnings(duckemu_user_libeleven)

# JD 16.1.50: libjdg.so Bridge.main. The trimmed guest APK and the .so live in
# this project's assets/; nothing here is shared with the runtime.
set(DUCKEMU_JDGS_CASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/jdgs_case")
add_library(duckemu_user_jdgs STATIC
  jdgs_case/jdgs_env.cpp
)
target_include_directories(duckemu_user_jdgs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(duckemu_user_jdgs PRIVATE duckemu_runtime)
duckemu_set_warnings(duckemu_user_jdgs)

# M7: every project also implements the worker's AppPlugin seam. The registry
# is the only place app ids are named on this side of the boundary.
add_library(duckemu_user_registry STATIC
  registry.cpp
  libeleven_case/plugin.cpp
  jdgs_case/plugin.cpp
)
target_include_directories(duckemu_user_registry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(duckemu_user_registry PUBLIC
  duckemu_user_libeleven
  duckemu_user_jdgs
  duckemu_common
)
duckemu_set_warnings(duckemu_user_registry)

# The project runner: loads ROM libc + assets/libeleven.so and calls the
# dynamically registered data(int). The .so is part of the project, not a
# test fixture.
add_executable(duckemu_libeleven_case
  libeleven_case/main.cpp
)
target_link_libraries(duckemu_libeleven_case PRIVATE
  duckemu_runtime
  duckemu_user_libeleven)
target_compile_definitions(duckemu_libeleven_case PRIVATE
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_LIBELEVEN_SO="${CMAKE_CURRENT_SOURCE_DIR}/libeleven_case/assets/libeleven.so")
duckemu_set_warnings(duckemu_libeleven_case)

# JD 16.1.50 runner: ROM libc + libz and the project's libjdg.so, with the
# trimmed APK mounted at its device path through a guest file system root.
add_executable(duckemu_jdgs_case
  jdgs_case/main.cpp
)
target_link_libraries(duckemu_jdgs_case PRIVATE
  duckemu_runtime
  duckemu_user_jdgs)
target_compile_definitions(duckemu_jdgs_case PRIVATE
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_JDGS_SO="${DUCKEMU_JDGS_CASE_DIR}/assets/libjdg.so"
  DUCKEMU_JDGS_ROOT="${DUCKEMU_JDGS_CASE_DIR}/assets/root")
duckemu_set_warnings(duckemu_jdgs_case)
