# M0 gate: re-run all nine Unicorn probes.
add_test(NAME m0_probes COMMAND unicorn_spike all)

# ---------------------------------------------------------------------------
# Guest blobs: real aarch64 assembly, assembled to a flat .text binary.
# 01-03 use bare blobs (no ELF loader); the loader arrives in M4.
# ---------------------------------------------------------------------------
find_program(DUCKEMU_CLANG_EXECUTABLE clang REQUIRED)
find_program(DUCKEMU_LLVM_OBJCOPY llvm-objcopy REQUIRED)

set(DUCKEMU_BLOB_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/blobs")

function(duckemu_guest_blob out_var source)
  get_filename_component(name "${source}" NAME_WE)
  set(object "${DUCKEMU_BLOB_OUTPUT_DIR}/${name}.o")
  set(binary "${DUCKEMU_BLOB_OUTPUT_DIR}/${name}.bin")
  add_custom_command(
    OUTPUT "${binary}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_BLOB_OUTPUT_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} --target=aarch64-linux-android31
            -c "${source}" -o "${object}"
    COMMAND ${DUCKEMU_LLVM_OBJCOPY} -O binary --only-section=.text
            "${object}" "${binary}"
    DEPENDS "${source}"
    COMMENT "Assembling guest blob ${name}.bin")
  set(${out_var} "${binary}" PARENT_SCOPE)
endfunction()

duckemu_guest_blob(BLOB_TLS_READ
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/blobs/01_tls_read.S")
duckemu_guest_blob(BLOB_PURE_NATIVE
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/blobs/02_pure_native.S")
duckemu_guest_blob(BLOB_EXIT_GROUP
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/blobs/02_exit_group.S")
duckemu_guest_blob(BLOB_STACK_DUMP
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/blobs/02_stack_dump.S")
duckemu_guest_blob(BLOB_PTHREAD_JOIN
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/blobs/03_pthread_join.S")

add_custom_target(guest_blobs ALL
  DEPENDS ${BLOB_TLS_READ} ${BLOB_PURE_NATIVE} ${BLOB_EXIT_GROUP}
          ${BLOB_STACK_DUMP} ${BLOB_PTHREAD_JOIN})

# ---------------------------------------------------------------------------
# Guest shared objects for the M4 loader gate. Built for aarch64-linux-android
# with -nostdlib, so every libc import stays UND and the loader has to satisfy
# it from the virtual bionic table.
#
# --pack-dyn-relocs=none keeps the output in plain RELA form; aarch64-android
# defaults to DT_ANDROID_RELA (APS2), which the M4 loader refuses on purpose.
# ---------------------------------------------------------------------------
set(DUCKEMU_SO_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/so")
set(DUCKEMU_SO_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/so-build")
set(DUCKEMU_SO_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/so-runtime")

set(DUCKEMU_GUEST_SO_FLAGS
    --target=aarch64-linux-android31 -fPIC -shared -nostdlib -O1
    -Wl,--pack-dyn-relocs=none)

add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libb.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -Wl,-soname,libb.so "${DUCKEMU_SO_SOURCE_DIR}/libb.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libb.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libb.c"
    COMMENT "Building guest .so libb.so")

add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/liblog.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -Wl,-soname,liblog.so "${DUCKEMU_SO_SOURCE_DIR}/liblog_stub.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/liblog.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/liblog_stub.c"
    COMMENT "Building link-only stub liblog.so")

add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libc.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -Wl,-soname,libc.so "${DUCKEMU_SO_SOURCE_DIR}/libc_stub.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libc.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libc_stub.c"
    COMMENT "Building link-only stub libc.so")

add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/liba.so"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -Wl,-soname,liba.so "${DUCKEMU_SO_SOURCE_DIR}/liba.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/liba.so"
            -L"${DUCKEMU_SO_BUILD_DIR}" -Wl,--no-as-needed
            -l:libc.so -l:liblog.so -l:libb.so
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/liba.c"
            "${DUCKEMU_SO_BUILD_DIR}/libb.so"
            "${DUCKEMU_SO_BUILD_DIR}/liblog.so"
            "${DUCKEMU_SO_BUILD_DIR}/libc.so"
    COMMENT "Building guest .so liba.so")

add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libtls.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_BUILD_DIR}"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -Wl,-soname,libtls.so "${DUCKEMU_SO_SOURCE_DIR}/libtls.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libtls.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libtls.c"
    COMMENT "Building guest .so libtls.so (TLSDESC)")

add_custom_command(
    OUTPUT "${DUCKEMU_SO_BUILD_DIR}/libmissing.so"
    COMMAND ${DUCKEMU_CLANG_EXECUTABLE} ${DUCKEMU_GUEST_SO_FLAGS}
            -Wl,-soname,libmissing.so "${DUCKEMU_SO_SOURCE_DIR}/libmissing.c"
            -o "${DUCKEMU_SO_BUILD_DIR}/libmissing.so"
    DEPENDS "${DUCKEMU_SO_SOURCE_DIR}/libmissing.c"
    COMMENT "Building guest .so libmissing.so")

# The runtime view of the fixture: only what the loader may legitimately open.
# liblog.so and libc.so exist but are not ELF, so a loader that consults the
# filesystem before the virtual module table fails here instead of passing.
file(MAKE_DIRECTORY "${DUCKEMU_SO_RUNTIME_DIR}")
file(WRITE "${DUCKEMU_SO_RUNTIME_DIR}/liblog.so" "must not be read\n")
file(WRITE "${DUCKEMU_SO_RUNTIME_DIR}/libc.so" "must not be read\n")

add_custom_command(
    OUTPUT "${DUCKEMU_SO_RUNTIME_DIR}/liba.so"
           "${DUCKEMU_SO_RUNTIME_DIR}/libb.so"
           "${DUCKEMU_SO_RUNTIME_DIR}/libmissing.so"
           "${DUCKEMU_SO_RUNTIME_DIR}/libtls.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DUCKEMU_SO_RUNTIME_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/liba.so" "${DUCKEMU_SO_RUNTIME_DIR}/liba.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/libb.so" "${DUCKEMU_SO_RUNTIME_DIR}/libb.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/libmissing.so" "${DUCKEMU_SO_RUNTIME_DIR}/libmissing.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DUCKEMU_SO_BUILD_DIR}/libtls.so" "${DUCKEMU_SO_RUNTIME_DIR}/libtls.so"
    DEPENDS "${DUCKEMU_SO_BUILD_DIR}/liba.so"
            "${DUCKEMU_SO_BUILD_DIR}/libb.so"
            "${DUCKEMU_SO_BUILD_DIR}/libmissing.so"
            "${DUCKEMU_SO_BUILD_DIR}/libtls.so")

add_custom_target(guest_so ALL
    DEPENDS "${DUCKEMU_SO_RUNTIME_DIR}/liba.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/libb.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/libmissing.so"
            "${DUCKEMU_SO_RUNTIME_DIR}/libtls.so")

# ---------------------------------------------------------------------------
# Test support
# ---------------------------------------------------------------------------
add_library(duckemu_test_support STATIC support/blob_loader.cpp)
target_include_directories(duckemu_test_support PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(duckemu_test_support PUBLIC duckemu_common)
target_compile_definitions(duckemu_test_support PRIVATE
  DUCKEMU_BLOB_DIR="${DUCKEMU_BLOB_OUTPUT_DIR}")
duckemu_set_warnings(duckemu_test_support)

function(duckemu_add_integration_test name)
  add_executable(${name} "integration/${name}.cpp")
  target_link_libraries(${name} PRIVATE duckemu_runtime duckemu_test_support)
  target_compile_definitions(${name} PRIVATE
    DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces")
  add_dependencies(${name} guest_blobs)
  add_test(NAME ${name} COMMAND ${name})
  # Guest threads have no host timeout by design (M3 design §12): a spinning
  # guest hangs the caller, so the test runner is the backstop.
  set_tests_properties(${name} PROPERTIES TIMEOUT 60)
  duckemu_set_warnings(${name})
endfunction()

duckemu_add_integration_test(01_tls_read)
duckemu_add_integration_test(02_pure_native)
duckemu_add_integration_test(02_stack_dump)
duckemu_add_integration_test(03_pthread_join)

function(duckemu_add_loader_test name)
  add_executable(${name} "integration/${name}.cpp")
  target_link_libraries(${name} PRIVATE duckemu_runtime duckemu_test_support)
  target_compile_definitions(${name} PRIVATE
    DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
    DUCKEMU_SO_DIR="${DUCKEMU_SO_RUNTIME_DIR}")
  add_dependencies(${name} guest_so)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 60)
  duckemu_set_warnings(${name})
endfunction()

duckemu_add_loader_test(04_so_load)
duckemu_add_loader_test(04b_tlsdesc)

# ---------------------------------------------------------------------------
# M4.5 gate: real bionic from the local Android environment.
#
# Paths are cache variables so another machine can point them elsewhere; the
# defaults match this workspace's layout. The test fails loudly when an asset
# is missing rather than silently skipping.
# ---------------------------------------------------------------------------
set(DUCKEMU_ANDROID_ENV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    CACHE PATH "Root of the Android environment (contains DuckEmu/ and ROM/)")
set(DUCKEMU_BIONIC_DIR "${DUCKEMU_ANDROID_ENV_ROOT}/DuckEmu/bionic"
    CACHE PATH "Directory with the Android 13/14 libc/libm/libdl sample")
set(DUCKEMU_ROM_LIB64
    "${DUCKEMU_ANDROID_ENV_ROOT}/ROM/out/target/product/lemonade/symbols/system/lib64"
    CACHE PATH "ROM system/lib64 (libz/liblog/libc++)")
set(DUCKEMU_SGMAIN_SO
    "${DUCKEMU_ANDROID_ENV_ROOT}/DuckDroid/code/user_code/DaMai/ida/libsgmainso-6.8.260704.so"
    CACHE FILEPATH "libsgmainso sample with damaged section headers")
set(DUCKEMU_ROM_BOOTSTRAP "${DUCKEMU_ROM_LIB64}/bootstrap")

add_executable(04b_libc_map integration/04b_libc_map.cpp)
target_link_libraries(04b_libc_map PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(04b_libc_map PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_BIONIC_DIR="${DUCKEMU_BIONIC_DIR}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_LIB64}/bootstrap"
  DUCKEMU_SGMAIN_SO="${DUCKEMU_SGMAIN_SO}")
add_test(NAME 04b_libc_map COMMAND 04b_libc_map)
set_tests_properties(04b_libc_map PROPERTIES TIMEOUT 120)
duckemu_set_warnings(04b_libc_map)

# M4.6 S1: libc_shared_globals + auxv, exercised through the ROM libc's
# getauxval. Initializers stay off: preinit is a later step.
add_executable(04c_globals integration/04c_globals.cpp)
target_link_libraries(04c_globals PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(04c_globals PRIVATE
  DUCKEMU_TRACE_DIR="${CMAKE_CURRENT_BINARY_DIR}/traces"
  DUCKEMU_ROM_BOOTSTRAP="${DUCKEMU_ROM_BOOTSTRAP}"
  DUCKEMU_ROM_LIB64="${DUCKEMU_ROM_LIB64}")
add_test(NAME 04c_globals COMMAND 04c_globals)
set_tests_properties(04c_globals PROPERTIES TIMEOUT 60)
duckemu_set_warnings(04c_globals)

add_executable(elf_image_test unit/runtime/loader/elf_image_test.cpp)
target_link_libraries(elf_image_test PRIVATE duckemu_runtime duckemu_test_support)
target_compile_definitions(elf_image_test PRIVATE
  DUCKEMU_SO_DIR="${DUCKEMU_SO_RUNTIME_DIR}")
add_dependencies(elf_image_test guest_so)
add_test(NAME elf_image_test COMMAND elf_image_test)
duckemu_set_warnings(elf_image_test)

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
add_executable(runtime_memory_test unit/runtime/memory_test.cpp)
target_link_libraries(runtime_memory_test PRIVATE
  duckemu_runtime
  duckemu_test_support)
add_test(NAME runtime_memory_test COMMAND runtime_memory_test)
duckemu_set_warnings(runtime_memory_test)

add_executable(ring_buffer_test unit/runtime/ring_buffer_test.cpp)
target_link_libraries(ring_buffer_test PRIVATE duckemu_runtime duckemu_test_support)
add_test(NAME ring_buffer_test COMMAND ring_buffer_test)
duckemu_set_warnings(ring_buffer_test)

add_executable(guest_stack_test unit/runtime/guest_stack_test.cpp)
target_link_libraries(guest_stack_test PRIVATE duckemu_runtime duckemu_test_support)
add_test(NAME guest_stack_test COMMAND guest_stack_test)
duckemu_set_warnings(guest_stack_test)

add_executable(svc_trace_format_test unit/observer/svc_trace_format_test.cpp)
target_link_libraries(svc_trace_format_test PRIVATE duckemu_runtime duckemu_test_support)
add_test(NAME svc_trace_format_test COMMAND svc_trace_format_test)
duckemu_set_warnings(svc_trace_format_test)
