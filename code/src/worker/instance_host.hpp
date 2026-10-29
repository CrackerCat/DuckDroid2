// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string>

#include "worker/app/app_plugin.hpp"

namespace duckemu::worker
{

struct InstanceHostConfig
{
	int ipc_fd = -1;
	std::string instance_id;
	AppContext app;
	std::string trace_path;
	bool has_wall_time = false;
	uint64_t wall_time_ns = 0;
	bool has_monotonic_time = false;
	uint64_t monotonic_time_ns = 0;
};

// The `--instance` process. Creates its own RuntimeInstance (after exec, so no
// engine or host thread can survive from the parent), reports ready and then
// serves invoke/shutdown until the supervisor closes the channel.
//
// Returns the process exit code; 0 means the supervisor closed the channel.
int run_instance_host(const InstanceHostConfig &config);

} // namespace duckemu::worker
