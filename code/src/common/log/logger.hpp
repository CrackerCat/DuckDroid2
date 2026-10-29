// SPDX-License-Identifier: GPL-2.0-only
#pragma once

namespace duckemu::log
{

enum class Level : int
{
	Trace = 0,
	Debug,
	Info,
	Warn,
	Error,
	Off,
};

void set_level(Level level) noexcept;
[[nodiscard]] Level level() noexcept;

// printf-style. Thread-safe, writes one whole line to stderr.
void message(Level level, const char *file, int line, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 4, 5)))
#endif
	;

} // namespace duckemu::log

#define DUCKEMU_LOG(lvl, ...) ::duckemu::log::message((lvl), __FILE__, __LINE__, __VA_ARGS__)

#define DUCKEMU_TRACE(...) DUCKEMU_LOG(::duckemu::log::Level::Trace, __VA_ARGS__)
#define DUCKEMU_DEBUG(...) DUCKEMU_LOG(::duckemu::log::Level::Debug, __VA_ARGS__)
#define DUCKEMU_INFO(...) DUCKEMU_LOG(::duckemu::log::Level::Info, __VA_ARGS__)
#define DUCKEMU_WARN(...) DUCKEMU_LOG(::duckemu::log::Level::Warn, __VA_ARGS__)
#define DUCKEMU_ERROR(...) DUCKEMU_LOG(::duckemu::log::Level::Error, __VA_ARGS__)
