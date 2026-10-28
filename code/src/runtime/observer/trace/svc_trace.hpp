#pragma once

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "common/error/result.hpp"
#include "runtime/observer/semantic/svc_event.hpp"

namespace duckemu::runtime::observer
{

// One line of svc.log. Format (tab separated, 9 columns):
//   id  seq  tid  svc  kind  name  args  ret  note
// `id` is the emission order (monotonic, gap-free), `seq` the call order.
// See docs/devlogs/2026-10-28-06-M2设计.md §4 for the column semantics.
[[nodiscard]] std::string formatSvcTraceLine(const SvcEvent &event);

// Returns nullopt when the line does not match the format. The writer and the
// trace viewer share this parser; there is no second definition of the format.
[[nodiscard]] std::optional<SvcEvent> parseSvcTraceLine(std::string_view line);

// Append-only svc.log writer. Flushes and closes on destruction.
class SvcTraceFile
{
    public:
	static Result<std::unique_ptr<SvcTraceFile> > open(std::string_view path);
	~SvcTraceFile();

	SvcTraceFile(const SvcTraceFile &) = delete;
	SvcTraceFile &operator=(const SvcTraceFile &) = delete;
	SvcTraceFile(SvcTraceFile &&) = delete;
	SvcTraceFile &operator=(SvcTraceFile &&) = delete;

	void write(const SvcEvent &event);

    private:
	explicit SvcTraceFile(std::FILE *file)
		: file_(file)
	{
	}

	std::FILE *file_ = nullptr;
	std::string buffer_;
};

} // namespace duckemu::runtime::observer
