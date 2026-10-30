// SPDX-License-Identifier: GPL-2.0-only
#include "worker/worker_process.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>

#include "common/log/logger.hpp"
#include "common/uds/socket.hpp"
#include "protocol/channel.hpp"
#include "protocol/frame.hpp"
#include "protocol/instance_messages.hpp"
#include "protocol/messages.hpp"

extern char **environ;

namespace duckemu::worker
{
namespace
{

// The instance's control channel always arrives at this number, so the spawn
// arguments never depend on whichever descriptor the supervisor happened to
// allocate.
constexpr int kInstanceChannelFd = 3;

// Backpressure per slot. Requests beyond this are refused with OVERLOADED
// rather than growing the queue without bound.
constexpr size_t kMaxQueuePerSlot = 8;

constexpr uint64_t kReapGraceMs = 1000;

enum class ReadStatus
{
	kOk,
	kTimeout,
	kClosed,
};

std::string SelfExecutable()
{
	char buffer[4096] = {};
	const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
	if (length <= 0)
	{
		return {};
	}
	return std::string(buffer, static_cast<size_t>(length));
}

// Waits for one whole instance message. Guest work has no host timeout, so a
// timeout here is enforced by killing the child (see Serve).
ReadStatus ReadInstanceMessage(const common::uds::Socket &channel, uint64_t timeout_us,
	protocol::instance::Message *message, std::string *error)
{
	uint64_t timeout_ms = timeout_us / 1000;
	if (timeout_us % 1000 != 0)
	{
		++timeout_ms;
	}
	if (timeout_ms > 0x7FFFFFFFULL)
	{
		timeout_ms = 0x7FFFFFFFULL;
	}
	pollfd descriptor = { channel.fd(), POLLIN, 0 };
	while (true)
	{
		const int ready = ::poll(&descriptor, 1, static_cast<int>(timeout_ms));
		if (ready == 0)
		{
			*error = "timed out waiting for the instance";
			return ReadStatus::kTimeout;
		}
		if (ready < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			*error = std::string("poll: ") + std::strerror(errno);
			return ReadStatus::kClosed;
		}
		break;
	}
	std::string frame;
	if (!protocol::read_frame(channel, &frame, error))
	{
		return ReadStatus::kClosed;
	}
	if (!protocol::instance::parse(frame, message, error))
	{
		return ReadStatus::kClosed;
	}
	return ReadStatus::kOk;
}

bool SendInstanceMessage(const common::uds::Socket &channel, const protocol::instance::Message &message)
{
	std::string frame;
	std::string error;
	if (!protocol::instance::serialize(message, &frame, &error))
	{
		DUCKEMU_ERROR("worker: serialize instance message failed: %s", error.c_str());
		return false;
	}
	if (!protocol::write_frame(channel, frame, &error))
	{
		DUCKEMU_ERROR("worker: write instance message failed: %s", error.c_str());
		return false;
	}
	return true;
}

} // namespace

struct WorkerProcess::Impl
{
	struct Job
	{
		uint32_t conn_id = 0;
		protocol::Request request;
		std::chrono::steady_clock::time_point queued_at;
	};

	// A client-visible session id is the instance id it was assigned to. The
	// binding dies with the instance (crash, recycle or shutdown).
	struct SessionBinding
	{
		size_t slot = 0;
		std::string instance_id;
	};

	struct Slot
	{
		Impl *owner = nullptr;
		size_t index = 0;
		std::thread thread;
		mutable std::mutex mutex;
		std::condition_variable cv;
		std::deque<Job> queue;
		bool stop = false;  // guarded by mutex
		bool ready = false; // guarded by mutex
		bool busy = false;  // guarded by mutex
		pid_t pid = -1;     // guarded by mutex
		std::string instance_id;
		uint64_t generation = 0;
		uint64_t init_us = 0;
		common::uds::Socket channel;
	};

	WorkerConfig config;
	common::uds::Socket system;
	mutable std::mutex system_mutex;
	std::atomic<uint64_t> served{ 0 };
	std::atomic<uint64_t> sequence{ 0 };
	std::atomic<bool> stopping{ false };
	std::atomic<size_t> next_slot{ 0 };
	std::vector<std::unique_ptr<Slot> > slots;
	std::mutex session_mutex;
	std::unordered_map<std::string, SessionBinding> sessions;

	void ForgetSession(const std::string &instance_id);
	void ForgetSlotSessions(size_t slot_index);

	bool Start(std::string *error);
	void StopAndJoin();
	void SlotLoop(Slot &slot);
	bool Spawn(Slot &slot, std::string *error);
	void Serve(Slot &slot, uint32_t *failures);
	bool PickSlot(Slot **slot);
	void Dispatch(uint32_t conn_id, protocol::Request request);
	bool ChannelDead(Slot &slot);
	pid_t SlotPid(Slot &slot);
	void KillSlot(Slot &slot);
	void Reap(Slot &slot);
	bool WriteResponse(uint32_t conn_id, const protocol::Response &response);
	void RespondError(uint32_t conn_id, std::string_view id, std::string_view code,
		std::string_view message, std::string_view stage, bool retryable, bool new_session = false);
	size_t ReadyInstances() const;
};

bool WorkerProcess::Impl::Start(std::string *error)
{
	if (config.instances == 0)
	{
		*error = "worker needs at least one instance";
		return false;
	}
	if (config.instance_executable.empty())
	{
		config.instance_executable = SelfExecutable();
	}
	if (config.instance_executable.empty())
	{
		*error = "cannot resolve the worker executable";
		return false;
	}
	for (size_t index = 0; index < config.instances; ++index)
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

void WorkerProcess::Impl::StopAndJoin()
{
	stopping.store(true);
	for (auto &slot : slots)
	{
		{
			std::lock_guard<std::mutex> lock(slot->mutex);
			slot->stop = true;
		}
		slot->cv.notify_all();
	}
	for (auto &slot : slots)
	{
		if (slot->thread.joinable())
		{
			slot->thread.join();
		}
	}
}

void WorkerProcess::Impl::SlotLoop(Slot &slot)
{
	uint32_t failures = 0;
	while (true)
	{
		{
			std::lock_guard<std::mutex> lock(slot.mutex);
			if (slot.stop)
			{
				return;
			}
		}

		std::string error;
		if (!Spawn(slot, &error))
		{
			DUCKEMU_ERROR("worker: slot %zu spawn failed: %s", slot.index, error.c_str());
			Reap(slot);
			std::this_thread::sleep_for(std::chrono::milliseconds(config.respawn_backoff_ms));
			{
				std::lock_guard<std::mutex> lock(slot.mutex);
				if (slot.stop)
				{
					return;
				}
			}
			if (++failures > config.max_consecutive_failures)
			{
				DUCKEMU_ERROR("worker: slot %zu keeps failing; leaving it down", slot.index);
				return;
			}
			continue;
		}

		{
			std::lock_guard<std::mutex> lock(slot.mutex);
			slot.ready = true;
			slot.busy = false;
		}
		DUCKEMU_INFO("worker: slot %zu pid=%d instance=%s ready", slot.index,
			static_cast<int>(SlotPid(slot)), slot.instance_id.c_str());

		Serve(slot, &failures);

		{
			std::lock_guard<std::mutex> lock(slot.mutex);
			slot.ready = false;
			slot.busy = false;
		}
		ForgetSlotSessions(slot.index);
		// The channel may already be gone; the write is best effort.
		protocol::instance::Message shutdown;
		shutdown.type = protocol::instance::Type::kShutdown;
		SendInstanceMessage(slot.channel, shutdown);
		Reap(slot);

		{
			std::lock_guard<std::mutex> lock(slot.mutex);
			if (slot.stop)
			{
				return;
			}
		}
		if (++failures > config.max_consecutive_failures)
		{
			DUCKEMU_ERROR("worker: slot %zu keeps dying; leaving it down", slot.index);
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(config.respawn_backoff_ms));
	}
}

bool WorkerProcess::Impl::Spawn(Slot &slot, std::string *error)
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
	const std::string instance_id =
		config.worker_id + "-i" + std::to_string(slot.index) + "-g" + std::to_string(generation);

	std::vector<std::string> arguments;
	arguments.push_back(config.instance_executable);
	arguments.push_back("--instance");
	arguments.push_back("--ipc-fd");
	arguments.push_back(std::to_string(kInstanceChannelFd));
	arguments.push_back("--instance-id");
	arguments.push_back(instance_id);
	arguments.push_back("--app");
	arguments.push_back(config.app.app);
	arguments.push_back("--library");
	arguments.push_back(config.app.library);
	arguments.push_back("--rom-libc");
	arguments.push_back(config.app.rom_libc);
	for (const std::string &path : config.app.library_search_paths)
	{
		arguments.push_back("--library-path");
		arguments.push_back(path);
	}
	for (const std::string &root : config.app.filesystem_roots)
	{
		arguments.push_back("--filesystem-root");
		arguments.push_back(root);
	}
	if (!config.app.filesystem_write_root.empty())
	{
		arguments.push_back("--filesystem-write-root");
		arguments.push_back(config.app.filesystem_write_root);
	}
	if (!config.app.property_file.empty())
	{
		arguments.push_back("--property-file");
		arguments.push_back(config.app.property_file);
	}
	if (!config.trace_path.empty())
	{
		arguments.push_back("--trace");
		arguments.push_back(config.trace_path);
	}
	if (config.has_wall_time)
	{
		arguments.push_back("--wall-time-ns");
		arguments.push_back(std::to_string(config.wall_time_ns));
	}
	if (config.has_monotonic_time)
	{
		arguments.push_back("--monotonic-time-ns");
		arguments.push_back(std::to_string(config.monotonic_time_ns));
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
	::posix_spawn_file_actions_adddup2(&actions, child_end.fd(), kInstanceChannelFd);
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
		std::lock_guard<std::mutex> lock(slot.mutex);
		slot.pid = pid;
		slot.instance_id = instance_id;
	}
	slot.channel = std::move(parent_end);

	protocol::instance::Message ready;
	std::string read_error;
	const ReadStatus status = ReadInstanceMessage(slot.channel, config.init_timeout_us, &ready, &read_error);
	if (status == ReadStatus::kOk && ready.type == protocol::instance::Type::kFailed)
	{
		*error = ready.error.code + ": " + ready.error.message;
	}
	else if (status != ReadStatus::kOk || ready.type != protocol::instance::Type::kReady)
	{
		*error = "instance " + instance_id + " did not become ready: " + read_error;
	}
	else
	{
		slot.init_us = ready.init_us;
		return true;
	}
	::kill(pid, SIGKILL);
	return false;
}

void WorkerProcess::Impl::Serve(Slot &slot, uint32_t *failures)
{
	while (true)
	{
		Job job;
		{
			std::unique_lock<std::mutex> lock(slot.mutex);
			while (!slot.stop && slot.queue.empty())
			{
				if (slot.cv.wait_for(lock, std::chrono::milliseconds(200)) == std::cv_status::timeout)
				{
					lock.unlock();
					const bool dead = ChannelDead(slot);
					lock.lock();
					if (dead)
					{
						return;
					}
				}
			}
			if (slot.stop)
			{
				std::deque<Job> drained;
				drained.swap(slot.queue);
				lock.unlock();
				for (const Job &pending : drained)
				{
					RespondError(pending.conn_id, pending.request.id,
						protocol::error_code::kStopRequested, "worker is stopping",
						protocol::stage::kQueue, true);
				}
				return;
			}
			job = std::move(slot.queue.front());
			slot.queue.pop_front();
			slot.busy = true;
		}

		// A request that spent too long queued must not run at all: the client
		// has already given up on it.
		if (job.request.has_deadline && job.request.deadline_ms != 0)
		{
			const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - job.queued_at).count();
			if (waited_ms > static_cast<long long>(job.request.deadline_ms))
			{
				{
					std::lock_guard<std::mutex> lock(slot.mutex);
					slot.busy = false;
				}
				RespondError(job.conn_id, job.request.id, protocol::error_code::kDeadlineExceeded,
					"deadline expired in the queue", protocol::stage::kQueue, true);
				continue;
			}
		}

		const uint64_t timeout_us = job.request.has_deadline && job.request.deadline_ms != 0
			? job.request.deadline_ms * 1000
			: config.invoke_timeout_us;

		protocol::instance::Message invoke;
		invoke.type = protocol::instance::Type::kInvoke;
		invoke.id = "r" + std::to_string(sequence.fetch_add(1) + 1);
		invoke.op = job.request.op;
		invoke.params = job.request.params;
		invoke.has_deadline = job.request.has_deadline;
		invoke.deadline_ms = job.request.deadline_ms;

		if (!SendInstanceMessage(slot.channel, invoke))
		{
			RespondError(job.conn_id, job.request.id, protocol::error_code::kGuestFault,
				"instance channel write failed", protocol::stage::kInvoke, true);
			return;
		}

		protocol::instance::Message result;
		std::string read_error;
		const ReadStatus status = ReadInstanceMessage(slot.channel, timeout_us, &result, &read_error);
		if (status == ReadStatus::kTimeout)
		{
			KillSlot(slot);
			RespondError(job.conn_id, job.request.id, protocol::error_code::kCallTimeout,
				"invoke timed out", protocol::stage::kInvoke, true);
			return;
		}
		if (status != ReadStatus::kOk)
		{
			RespondError(job.conn_id, job.request.id, protocol::error_code::kGuestFault, read_error,
				protocol::stage::kInvoke, true);
			return;
		}

		protocol::Response response;
		response.id = job.request.id;
		response.app = config.app.app;
		response.worker = config.worker_id;
		response.instance = slot.instance_id;
		response.ok = result.ok;
		response.result = std::move(result.result);
		if (!result.ok)
		{
			response.error.code =
				result.error.code.empty() ? protocol::error_code::kAppError : result.error.code;
			response.error.message = result.error.message;
			response.error.stage = protocol::stage::kInvoke;
			response.error.retryable = true;
		}
		response.meta.init_us = slot.init_us;
		response.meta.invoke_us = result.invoke_us;
		response.meta.instance_requests = result.requests;
		const bool recycle = config.max_requests_per_instance != 0 &&
			result.requests >= config.max_requests_per_instance;
		response.meta.recycled = recycle;
		WriteResponse(job.conn_id, response);
		served.fetch_add(1);
		*failures = 0;
		{
			std::lock_guard<std::mutex> lock(slot.mutex);
			slot.busy = false;
		}
		if (recycle)
		{
			// The instance retires after this response; its session id becomes
			// invalid and the slot thread replaces the process.
			ForgetSession(slot.instance_id);
			return;
		}
	}
}

bool WorkerProcess::Impl::PickSlot(Slot **result)
{
	const size_t count = slots.size();
	if (count == 0)
	{
		*result = nullptr;
		return false;
	}
	// Scan from a rotating start so equally-loaded slots are chosen round-robin.
	// Without this, every new session would pin to slot 0 (it is idle between
	// requests), and one instance would serve the whole worker.
	const size_t start = next_slot.fetch_add(1) % count;
	Slot *best = nullptr;
	size_t best_load = 0;
	for (size_t offset = 0; offset < count; ++offset)
	{
		Slot *slot = slots[(start + offset) % count].get();
		std::lock_guard<std::mutex> lock(slot->mutex);
		if (!slot->ready || slot->stop)
		{
			continue;
		}
		if (slot->queue.size() >= kMaxQueuePerSlot)
		{
			continue;
		}
		// An idle slot is always preferable; a busy one only receives work
		// once every slot is busy.
		const size_t load = (slot->busy || !slot->queue.empty()) ? slot->queue.size() + 1 : 0;
		if (best == nullptr || load < best_load)
		{
			best = slot;
			best_load = load;
		}
	}
	*result = best;
	return best != nullptr;
}

void WorkerProcess::Impl::Dispatch(uint32_t conn_id, protocol::Request request)
{
	const auto now = std::chrono::steady_clock::now();

	if (request.session.present)
	{
		SessionBinding binding;
		bool found = false;
		{
			std::lock_guard<std::mutex> lock(session_mutex);
			const auto it = sessions.find(request.session.id);
			if (it != sessions.end())
			{
				binding = it->second;
				found = true;
			}
		}
		if (!found || binding.slot >= slots.size())
		{
			if (found)
			{
				ForgetSession(request.session.id);
			}
			RespondError(conn_id, request.id, protocol::error_code::kSessionNotFound,
				"unknown session", protocol::stage::kQueue, false, true);
			return;
		}

		Slot *slot = slots[binding.slot].get();
		bool alive = false;
		bool congested = false;
		{
			std::lock_guard<std::mutex> lock(slot->mutex);
			alive = slot->ready && !slot->stop && slot->instance_id == binding.instance_id;
			congested = slot->queue.size() >= kMaxQueuePerSlot;
			if (alive && !congested)
			{
				slot->queue.push_back(Job{ conn_id, std::move(request), now });
			}
		}
		if (!alive)
		{
			ForgetSession(binding.instance_id);
			RespondError(conn_id, request.id, protocol::error_code::kSessionGone,
				"the session's instance is gone", protocol::stage::kQueue, true, true);
			return;
		}
		if (congested)
		{
			RespondError(conn_id, request.id, protocol::error_code::kOverloaded,
				"the session's instance queue is full", protocol::stage::kQueue, true);
			return;
		}
		slot->cv.notify_one();
		return;
	}

	Slot *slot = nullptr;
	if (!PickSlot(&slot))
	{
		RespondError(conn_id, request.id, protocol::error_code::kOverloaded, "no ready instance",
			protocol::stage::kQueue, true);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(slot->mutex);
		// The instance id is the session id the client will echo back.
		sessions[slot->instance_id] = SessionBinding{ slot->index, slot->instance_id };
		slot->queue.push_back(Job{ conn_id, std::move(request), now });
	}
	slot->cv.notify_one();
}

void WorkerProcess::Impl::ForgetSession(const std::string &instance_id)
{
	std::lock_guard<std::mutex> lock(session_mutex);
	sessions.erase(instance_id);
}

void WorkerProcess::Impl::ForgetSlotSessions(size_t slot_index)
{
	std::lock_guard<std::mutex> lock(session_mutex);
	for (auto it = sessions.begin(); it != sessions.end();)
	{
		if (it->second.slot == slot_index)
		{
			it = sessions.erase(it);
		}
		else
		{
			++it;
		}
	}
}

bool WorkerProcess::Impl::ChannelDead(Slot &slot)
{
	pollfd descriptor = { slot.channel.fd(), POLLIN, 0 };
	const int ready = ::poll(&descriptor, 1, 0);
	if (ready <= 0)
	{
		return false;
	}
	// An idle instance never sends unsolicited frames: either this is EOF from
	// a dead child or an unexpected frame. Both mean this slot must restart.
	std::string frame;
	std::string error;
	if (!protocol::read_frame(slot.channel, &frame, &error))
	{
		return true;
	}
	DUCKEMU_WARN("worker: slot %zu sent an unsolicited frame; restarting it", slot.index);
	return true;
}

pid_t WorkerProcess::Impl::SlotPid(Slot &slot)
{
	std::lock_guard<std::mutex> lock(slot.mutex);
	return slot.pid;
}

void WorkerProcess::Impl::KillSlot(Slot &slot)
{
	const pid_t pid = SlotPid(slot);
	if (pid > 0)
	{
		::kill(pid, SIGKILL);
	}
}

void WorkerProcess::Impl::Reap(Slot &slot)
{
	pid_t pid = -1;
	{
		std::lock_guard<std::mutex> lock(slot.mutex);
		pid = slot.pid;
		slot.pid = -1;
	}
	slot.channel.close();
	if (pid <= 0)
	{
		return;
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kReapGraceMs);
	while (std::chrono::steady_clock::now() < deadline)
	{
		const pid_t reaped = ::waitpid(pid, nullptr, WNOHANG);
		if (reaped == pid || (reaped < 0 && errno != EINTR))
		{
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	::kill(pid, SIGKILL);
	while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR)
	{
	}
}

bool WorkerProcess::Impl::WriteResponse(uint32_t conn_id, const protocol::Response &response)
{
	std::string payload;
	std::string error;
	if (!protocol::serialize_response(response, &payload, &error))
	{
		DUCKEMU_ERROR("worker: serialize response failed: %s", error.c_str());
		return false;
	}
	std::lock_guard<std::mutex> lock(system_mutex);
	if (!system.valid())
	{
		return false;
	}
	if (!protocol::write_channel(system, conn_id, payload, &error))
	{
		DUCKEMU_ERROR("worker: response write failed: %s", error.c_str());
		return false;
	}
	return true;
}

void WorkerProcess::Impl::RespondError(uint32_t conn_id, std::string_view id, std::string_view code,
	std::string_view message, std::string_view stage, bool retryable, bool new_session)
{
	WriteResponse(conn_id,
		protocol::make_error_response(id, config.app.app, config.worker_id, "", code, message, stage,
			retryable, new_session));
}

size_t WorkerProcess::Impl::ReadyInstances() const
{
	size_t count = 0;
	for (const auto &slot : slots)
	{
		std::lock_guard<std::mutex> lock(slot->mutex);
		if (slot->ready)
		{
			++count;
		}
	}
	return count;
}

WorkerProcess::WorkerProcess() : impl_(std::make_unique<Impl>()) {}

WorkerProcess::~WorkerProcess()
{
	impl_->StopAndJoin();
}

std::unique_ptr<WorkerProcess> WorkerProcess::create(const WorkerConfig &config, std::string *error)
{
	std::unique_ptr<WorkerProcess> worker(new WorkerProcess());
	worker->impl_->config = config;
	if (!worker->impl_->Start(error))
	{
		return nullptr;
	}
	return worker;
}

bool WorkerProcess::run(int system_fd, std::string *error)
{
	impl_->system = common::uds::Socket(system_fd);
	while (true)
	{
		uint32_t conn_id = 0;
		std::string payload;
		if (!protocol::read_channel(impl_->system, &conn_id, &payload, error))
		{
			break;
		}
		protocol::Request request;
		std::string message;
		std::string request_id;
		const protocol::ParseStatus status =
			protocol::parse_request(payload, &request, &message, &request_id);
		if (status != protocol::ParseStatus::kOk)
		{
			std::string_view code = protocol::error_code::kBadRequest;
			if (status == protocol::ParseStatus::kBadJson)
			{
				code = protocol::error_code::kBadJson;
			}
			else if (status == protocol::ParseStatus::kBadVersion)
			{
				code = protocol::error_code::kBadVersion;
			}
			impl_->RespondError(conn_id, request_id, code, message, protocol::stage::kParse, false);
			continue;
		}
		if (!request.app.empty() && request.app != impl_->config.app.app)
		{
			impl_->RespondError(conn_id, request.id, protocol::error_code::kAppMismatch,
				"request app does not match this worker", protocol::stage::kParse, false);
			continue;
		}
		impl_->Dispatch(conn_id, std::move(request));
	}
	impl_->StopAndJoin();
	return true;
}

void WorkerProcess::stop()
{
	impl_->StopAndJoin();
}

size_t WorkerProcess::ready_instances() const
{
	return impl_->ReadyInstances();
}

std::vector<int> WorkerProcess::instance_pids() const
{
	std::vector<int> pids;
	for (const auto &slot : impl_->slots)
	{
		std::lock_guard<std::mutex> lock(slot->mutex);
		pids.push_back(slot->pid);
	}
	return pids;
}

uint64_t WorkerProcess::requests_served() const
{
	return impl_->served.load();
}

} // namespace duckemu::worker
