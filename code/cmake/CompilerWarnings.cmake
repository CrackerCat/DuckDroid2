# Curated warning set shared by every first-party target.
#
# third_party targets (unicorn) are never touched by this function.

option(DUCKEMU_WARNINGS_AS_ERRORS "Turn warnings into errors" OFF)

function(duckemu_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wnon-virtual-dtor
      -Wold-style-cast
      -Wcast-qual
      -Wcast-align
      -Wunused
      -Woverloaded-virtual
      -Wformat=2
      -Wnull-dereference
    )
  endif()

  if(DUCKEMU_WARNINGS_AS_ERRORS)
    if(MSVC)
      target_compile_options(${target} PRIVATE /WX)
    else()
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
