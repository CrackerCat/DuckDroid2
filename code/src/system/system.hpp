#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "system/worker_pool.hpp"

namespace duckemu::system
{

struct AppConfig
{
	WorkerSpec worker;
	// Empty means <socket_dir>/<app>.sock.
	std::string socket_path;
};

struct SystemConfig
{
	std::string socket_dir = "/tmp/duckemu";
	std::vector<AppConfig> apps;
};

// The local gateway. It owns one UDS listener per app and one WorkerPool per
// app, never parses request bodies, and never links the runtime.
class System
{
    public:
	static std::unique_ptr<System> create(const SystemConfig &config, std::string *error);
	~System();

	System(const System &) = delete;
	System &operator=(const System &) = delete;

	// Blocks until stop() is called.
	bool run(std::string *error);
	void stop();

	[[nodiscard]] std::string socket_path(std::string_view app) const;
	[[nodiscard]] std::vector<int> worker_pids(std::string_view app) const;

    private:
	System();

	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace duckemu::system
