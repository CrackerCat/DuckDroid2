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

# M7: every project also implements the worker's AppPlugin seam. The registry
# is the only place app ids are named on this side of the boundary.
add_library(duckemu_user_registry STATIC
  registry.cpp
  libeleven_case/plugin.cpp
)
target_include_directories(duckemu_user_registry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(duckemu_user_registry PUBLIC
  duckemu_user_libeleven
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
