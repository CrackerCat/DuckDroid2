#pragma once

#include <cstdint>
#include <string_view>

namespace duckemu::runtime::svc
{

// Name of a Linux syscall number on arm64 (asm-generic table), or an empty
// view when the number is unknown.
[[nodiscard]] std::string_view syscallName(uint32_t number) noexcept;

} // namespace duckemu::runtime::svc
