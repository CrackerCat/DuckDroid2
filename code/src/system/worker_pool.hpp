#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace duckemu::system
{

// One project's worker configuration. System fills everything the worker
// needs; the worker never reads global configuration.
struct WorkerSpec
{
	std::string app;
	std::string worker_executable;
	std::string library;
	std::string rom_libc;
	std::vector<std::string> library_search_paths;
	// Host directories the guest sees as "/", read-only. Targets that read
	// their own APK need one root holding that tree.
	std::vector<std::string> filesystem_roots;
	uint32_t workers = 1;
	uint32_t instances = 1;
	uint64_t init_timeout_us = 120ull * 1000 * 1000;
	uint64_t invoke_timeout_us = 5ull * 1000 * 1000;
	uint64_t respawn_backoff_ms = 500;
	uint32_t max_requests_per_instance = 0;
	std::string trace_path;
	bool has_wall_time = false;
	uint64_t wall_time_ns = 0;
	bool has_monotonic_time = false;
	uint64_t monotonic_time_ns = 0;
};

// Called on the slot's reader thread for every response frame.
using ResponseHandler = std::function<void(uint32_t conn_id, std::string payload)>;
// Called when a worker process is gone and its connections must be dropped.
using LossHandler = std::function<void(size_t worker_index)>;

// One WorkerPool manages `workers` slots for one app. A slot is a worker
// process plus a reader thread; the thread respawns the process on death.
class WorkerPool
{
    public:
	WorkerPool();
	~WorkerPool();

	WorkerPool(const WorkerPool &) = delete;
	WorkerPool &operator=(const WorkerPool &) = delete;

	bool start(const WorkerSpec &spec, ResponseHandler on_response, LossHandler on_loss,
		std::string *error);
	void stop();

	[[nodiscard]] size_t size() const;
	// Healthy slot index, round-robin, or -1 when no worker is up.
	[[nodiscard]] int pick();
	[[nodiscard]] size_t healthy_count() const;
	[[nodiscard]] std::vector<int> pids() const;

	// Writes one channel frame to a slot. Fails when the worker is gone.
	bool route(size_t worker_index, uint32_t conn_id, std::string_view payload, std::string *error);

    private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace duckemu::system
