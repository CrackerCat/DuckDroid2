#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "worker/app/app_plugin.hpp"

namespace duckemu::worker
{

struct WorkerConfig
{
	std::string worker_id;
	AppContext app;
	// The binary spawned as `--instance`. Empty means /proc/self/exe, which is
	// what production uses; tests point it at the built duckemu_worker.
	std::string instance_executable;

	uint32_t instances = 1;
	uint64_t init_timeout_us = 120ull * 1000 * 1000;
	uint64_t invoke_timeout_us = 5ull * 1000 * 1000;
	uint32_t max_consecutive_failures = 5;
	uint64_t respawn_backoff_ms = 100;
	// 0 = never recycle. Otherwise the instance is replaced after this many
	// requests; its session id dies with it (M6 warm-pool reset semantics).
	uint32_t max_requests_per_instance = 0;

	std::string trace_path;
	bool has_wall_time = false;
	uint64_t wall_time_ns = 0;
	bool has_monotonic_time = false;
	uint64_t monotonic_time_ns = 0;
};

// One WorkerProcess serves one project. It owns no RuntimeInstance: each slot
// is a child process (exec'd after fork) plus one supervisor thread that only
// moves frames. Requests are dispatched to the least-queued ready slot.
class WorkerProcess
{
    public:
	static std::unique_ptr<WorkerProcess> create(const WorkerConfig &config, std::string *error);
	~WorkerProcess();

	WorkerProcess(const WorkerProcess &) = delete;
	WorkerProcess &operator=(const WorkerProcess &) = delete;

	// Serves the System channel until it closes. Takes ownership of
	// |system_fd|. Instance slots are already starting when create returns.
	bool run(int system_fd, std::string *error);

	// Thread-safe: stop accepting, drain slots, join their threads.
	void stop();

	// Observability for tests and the management plane.
	size_t ready_instances() const;
	std::vector<int> instance_pids() const;
	uint64_t requests_served() const;

    private:
	WorkerProcess();

	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace duckemu::worker
