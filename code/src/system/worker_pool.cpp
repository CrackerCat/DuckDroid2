#include "system/worker_pool.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "common/log/logger.hpp"
#include "common/uds/socket.hpp"
#include "protocol/channel.hpp"

extern char **environ;

namespace duckemu::system
{
namespace
{

constexpr int kWorkerChannelFd = 3;
constexpr uint64_t kReapGraceMs = 1000;

void Backoff(uint64_t milliseconds, const std::atomic<bool> &stopping)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	while (!stopping.load() && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}

} // namespace

struct WorkerPool::Impl
{
	struct Slot
	{
		Impl *owner = nullptr;
		size_t index = 0;
		std::thread thread;
		std::mutex mutex;      // guards pid / worker_id / generation
		std::mutex write_mutex; // guards channel replacement and writes
		common::uds::Socket channel;
		std::atomic<bool> healthy{ false };
		pid_t pid = -1;
		std::string worker_id;
		uint64_t generation = 0;
	};

	WorkerSpec spec;
	ResponseHandler on_response;
	LossHandler on_loss;
	std::vector<std::unique_ptr<Slot> > slots;
	std::atomic<bool> stopping{ false };
	std::atomic<size_t> next{ 0 };

	bool Start(std::string *error);
	void SlotLoop(Slot &slot);
	bool Spawn(Slot &slot, std::string *error);
	void Reap(Slot &slot);
	int Pick();
};

bool WorkerPool::Impl::Start(std::string *error)
{
	if (spec.workers == 0)
	{
		*error = "worker pool needs at least one worker";
		return false;
	}
	if (spec.worker_executable.empty() || spec.app.empty() || spec.library.empty() ||
		spec.rom_libc.empty())
	{
		*error = "worker pool is missing executable, app, library or rom libc";
		return false;
	}
	for (size_t index = 0; index < spec.workers; ++index)
	{
		auto slot = std::make_unique<Slot>();
		slot->owner = this;
		slot->index = index;
		Slot *raw = slot.get();
		slots.push_back(std::move(slot));
		raw->thread = std::thread([this, raw]() { SlotLoop(*raw); });
	}
	return true;
}

void WorkerPool::Impl::SlotLoop(Slot &slot)
{
	while (!stopping.load())
	{
		std::string error;
		if (!Spawn(slot, &error))
		{
			DUCKEMU_ERROR("system: worker slot %zu spawn failed: %s", slot.index, error.c_str());
			Backoff(spec.respawn_backoff_ms, stopping);
			continue;
		}
		DUCKEMU_INFO("system: worker %s pid=%d up", slot.worker_id.c_str(),
			static_cast<int>(slot.pid));

		while (!stopping.load())
		{
			pollfd descriptor = { slot.channel.fd(), POLLIN, 0 };
			const int ready = ::poll(&descriptor, 1, 200);
			if (ready == 0)
			{
				continue;
			}
			if (ready < 0)
			{
				if (errno == EINTR)
				{
					continue;
				}
				error = std::string("poll: ") + std::strerror(errno);
				break;
			}
			uint32_t conn_id = 0;
			std::string payload;
			if (!protocol::read_channel(slot.channel, &conn_id, &payload, &error))
			{
				break;
			}
			if (on_response)
			{
				on_response(conn_id, std::move(payload));
			}
		}

		slot.healthy.store(false);
		DUCKEMU_WARN("system: worker %s pid=%d gone", slot.worker_id.c_str(),
			static_cast<int>(slot.pid));
		if (on_loss)
		{
			on_loss(slot.index);
		}
		Reap(slot);
		Backoff(spec.respawn_backoff_ms, stopping);
	}
}

bool WorkerPool::Impl::Spawn(Slot &slot, std::string *error)
{
	common::uds::Socket parent_end;
	common::uds::Socket child_end;
	if (!common::uds::make_pair(&parent_end, &child_end, error))
	{
		return false;
	}

	uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(slot.mutex);
		generation = slot.generation++;
	}
	const std::string worker_id =
		spec.app + "-w" + std::to_string(slot.index) + "-g" + std::to_string(generation);

	std::vector<std::string> arguments;
	arguments.push_back(spec.worker_executable);
	arguments.push_back("--supervisor");
	arguments.push_back("--ipc-fd");
	arguments.push_back(std::to_string(kWorkerChannelFd));
	arguments.push_back("--worker-id");
	arguments.push_back(worker_id);
	arguments.push_back("--app");
	arguments.push_back(spec.app);
	arguments.push_back("--library");
	arguments.push_back(spec.library);
	arguments.push_back("--rom-libc");
	arguments.push_back(spec.rom_libc);
	for (const std::string &path : spec.library_search_paths)
	{
		arguments.push_back("--library-path");
		arguments.push_back(path);
	}
	arguments.push_back("--instances");
	arguments.push_back(std::to_string(spec.instances));
	arguments.push_back("--init-timeout-us");
	arguments.push_back(std::to_string(spec.init_timeout_us));
	arguments.push_back("--invoke-timeout-us");
	arguments.push_back(std::to_string(spec.invoke_timeout_us));
	arguments.push_back("--backoff-ms");
	arguments.push_back(std::to_string(spec.respawn_backoff_ms));
	arguments.push_back("--max-requests-per-instance");
	arguments.push_back(std::to_string(spec.max_requests_per_instance));
	if (!spec.trace_path.empty())
	{
		arguments.push_back("--trace");
		arguments.push_back(spec.trace_path);
	}
	if (spec.has_wall_time)
	{
		arguments.push_back("--wall-time-ns");
		arguments.push_back(std::to_string(spec.wall_time_ns));
	}
	if (spec.has_monotonic_time)
	{
		arguments.push_back("--monotonic-time-ns");
		arguments.push_back(std::to_string(spec.monotonic_time_ns));
	}

	std::vector<char *> argv;
	argv.reserve(arguments.size() + 1);
	for (std::string &argument : arguments)
	{
		argv.push_back(argument.data());
	}
	argv.push_back(nullptr);

	posix_spawn_file_actions_t actions;
	if (::posix_spawn_file_actions_init(&actions) != 0)
	{
		*error = "posix_spawn_file_actions_init failed";
		return false;
	}
	::posix_spawn_file_actions_adddup2(&actions, child_end.fd(), kWorkerChannelFd);
	pid_t pid = -1;
	const int spawned = ::posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
	::posix_spawn_file_actions_destroy(&actions);
	if (spawned != 0)
	{
		*error = std::string("posix_spawn: ") + std::strerror(spawned);
		return false;
	}
	child_end.close();
	{
		std::lock_guard<std::mutex> lock(slot.write_mutex);
		slot.channel = std::move(parent_end);
	}
	{
		std::lock_guard<std::mutex> lock(slot.mutex);
		slot.pid = pid;
		slot.worker_id = worker_id;
	}
	slot.healthy.store(true);
	return true;
}

void WorkerPool::Impl::Reap(Slot &slot)
{
	pid_t pid = -1;
	{
		std::lock_guard<std::mutex> lock(slot.mutex);
		pid = slot.pid;
		slot.pid = -1;
	}
	{
		std::lock_guard<std::mutex> lock(slot.write_mutex);
		slot.channel.close();
	}
	if (pid <= 0)
	{
		return;
	}
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::milliseconds(kReapGraceMs);
	while (std::chrono::steady_clock::now() < deadline)
	{
		int status = 0;
		const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
		if (reaped == pid)
		{
			if (WIFEXITED(status))
			{
				DUCKEMU_WARN("system: worker pid=%d exited with %d", static_cast<int>(pid),
					WEXITSTATUS(status));
			}
			else if (WIFSIGNALED(status))
			{
				DUCKEMU_WARN("system: worker pid=%d killed by signal %d", static_cast<int>(pid),
					WTERMSIG(status));
			}
			return;
		}
		if (reaped < 0 && errno != EINTR)
		{
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	::kill(pid, SIGKILL);
	int status = 0;
	while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
	{
	}
	DUCKEMU_WARN("system: worker pid=%d did not exit; killed", static_cast<int>(pid));
}

int WorkerPool::Impl::Pick()
{
	const size_t count = slots.size();
	if (count == 0)
	{
		return -1;
	}
	for (size_t attempt = 0; attempt < count; ++attempt)
	{
		const size_t index = next.fetch_add(1) % count;
		if (slots[index]->healthy.load())
		{
			return static_cast<int>(index);
		}
	}
	return -1;
}

WorkerPool::WorkerPool() : impl_(std::make_unique<Impl>()) {}

WorkerPool::~WorkerPool()
{
	stop();
}

bool WorkerPool::start(const WorkerSpec &spec, ResponseHandler on_response, LossHandler on_loss,
	std::string *error)
{
	impl_->spec = spec;
	impl_->on_response = std::move(on_response);
	impl_->on_loss = std::move(on_loss);
	return impl_->Start(error);
}

void WorkerPool::stop()
{
	if (impl_->stopping.exchange(true))
	{
		return;
	}
	for (auto &slot : impl_->slots)
	{
		if (slot->thread.joinable())
		{
			slot->thread.join();
		}
	}
}

size_t WorkerPool::size() const
{
	return impl_->slots.size();
}

int WorkerPool::pick()
{
	return impl_->Pick();
}

size_t WorkerPool::healthy_count() const
{
	size_t count = 0;
	for (const auto &slot : impl_->slots)
	{
		if (slot->healthy.load())
		{
			++count;
		}
	}
	return count;
}

std::vector<int> WorkerPool::pids() const
{
	std::vector<int> pids;
	for (const auto &slot : impl_->slots)
	{
		std::lock_guard<std::mutex> lock(slot->mutex);
		pids.push_back(slot->pid);
	}
	return pids;
}

bool WorkerPool::route(size_t worker_index, uint32_t conn_id, std::string_view payload,
	std::string *error)
{
	if (worker_index >= impl_->slots.size())
	{
		*error = "worker index is out of range";
		return false;
	}
	Impl::Slot &slot = *impl_->slots[worker_index];
	std::lock_guard<std::mutex> lock(slot.write_mutex);
	if (!slot.channel.valid())
	{
		*error = "worker channel is closed";
		return false;
	}
	return protocol::write_channel(slot.channel, conn_id, payload, error);
}

} // namespace duckemu::system
