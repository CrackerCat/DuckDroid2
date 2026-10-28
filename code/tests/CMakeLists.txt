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

add_custom_target(guest_blobs ALL
  DEPENDS ${BLOB_TLS_READ} ${BLOB_PURE_NATIVE} ${BLOB_EXIT_GROUP})

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
  add_dependencies(${name} guest_blobs)
  add_test(NAME ${name} COMMAND ${name})
  duckemu_set_warnings(${name})
endfunction()

duckemu_add_integration_test(01_tls_read)
duckemu_add_integration_test(02_pure_native)

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
add_executable(runtime_memory_test unit/runtime/memory_test.cpp)
target_link_libraries(runtime_memory_test PRIVATE
  duckemu_runtime
  duckemu_test_support)
add_test(NAME runtime_memory_test COMMAND runtime_memory_test)
duckemu_set_warnings(runtime_memory_test)
