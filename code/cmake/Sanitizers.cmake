# SPDX-License-Identifier: GPL-2.0-only
# Optional ASan/UBSan for first-party code.
#
# Unicorn's TCG generates code at runtime; ASan does not instrument it, which
# is fine for our purposes (we want to catch bugs in the host-side glue).
# Keep this OFF for probe measurements so timings are meaningful.

option(DUCKEMU_SANITIZE_ADDRESS "Build with AddressSanitizer" OFF)
option(DUCKEMU_SANITIZE_UNDEFINED "Build with UndefinedBehaviorSanitizer" OFF)

function(duckemu_setup_sanitizers)
  set(_flags "")

  if(DUCKEMU_SANITIZE_ADDRESS)
    list(APPEND _flags -fsanitize=address -fno-omit-frame-pointer)
  endif()

  if(DUCKEMU_SANITIZE_UNDEFINED)
    list(APPEND _flags -fsanitize=undefined -fno-sanitize-recover=undefined)
  endif()

  if(NOT _flags)
    return()
  endif()

  message(STATUS "Sanitizers: ${_flags}")
  add_compile_options(${_flags})
  add_link_options(${_flags})
endfunction()
