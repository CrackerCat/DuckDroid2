#include "worker/instance_host.hpp"

#include <chrono>
#include <string>
#include <sys/prctl.h>

#include "common/log/logger.hpp"
#include "common/uds/socket.hpp"
#include "protocol/frame.hpp"
#include "protocol/instance_messages.hpp"
#include "runtime/instance/runtime_instance.hpp"

namespace duckemu::worker
{
namespace
{

uint64_t NowUs()
{
	using Clock = std::chrono::steady_clock;
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

void SetProcessName(std::string_view app)
{
	// TASK_COMM_LEN is 16; prctl truncates silently, which is what we want.
	const std::string name = "duckemu-inst:" + std::string(app);
	::prctl(PR_SET_NAME, name.c_str(), 0, 0, 0);
}

bool Send(const common::uds::Socket &channel, const protocol::instance::Message &message)
{
	std::string frame;
	std::string error;
	if (!protocol::instance::serialize(message, &frame, &error))
	{
		DUCKEMU_ERROR("instance: serialize failed: %s", error.c_str());
		return false;
	}
	if (!protocol::write_frame(channel, frame, &error))
	{
		DUCKEMU_ERROR("instance: write failed: %s", error.c_str());
		return false;
	}
	return true;
}

void SendFailed(const common::uds::Socket &channel, std::string_view code, std::string_view message)
{
	protocol::instance::Message failed;
	failed.type = protocol::instance::Type::kFailed;
	failed.error.code = std::string(code);
	failed.error.message = std::string(message);
	Send(channel, failed);
}

} // namespace

int run_instance_host(const InstanceHostConfig &config)
{
	if (config.ipc_fd < 0)
	{
		DUCKEMU_ERROR("instance: ipc fd is missing");
		return 2;
	}
	SetProcessName(config.app.app);

	common::uds::Socket channel(config.ipc_fd);

	// Everything below creates host threads (the scheduler starts one per guest
	// thread), which is exactly why the supervisor forks and execs first.
	const uint64_t start_us = NowUs();

	runtime::RuntimeConfig runtime_config;
	runtime_config.library_search_paths = config.app.library_search_paths;
	runtime_config.filesystem_roots = config.app.filesystem_roots;
	runtime_config.trace_path = config.trace_path;

	auto created = runtime::RuntimeInstance::create(runtime_config);
	if (!created.ok())
	{
		SendFailed(channel, protocol::error_code::kInitFailed, created.error().message);
		return 1;
	}
	runtime::RuntimeInstance &instance = *created.value();

	if (config.has_wall_time)
	{
		instance.setWallTimeNs(config.wall_time_ns);
	}
	if (config.has_monotonic_time)
	{
		instance.setMonotonicTimeNs(config.monotonic_time_ns);
	}

	auto plugin_result = make_app_plugin(config.app);
	if (!plugin_result.ok())
	{
		SendFailed(channel, protocol::error_code::kInitFailed, plugin_result.error().message);
		return 1;
	}
	std::unique_ptr<AppPlugin> plugin = std::move(plugin_result.value());

	std::string init_error;
	if (!plugin->init(instance, &init_error))
	{
		SendFailed(channel, protocol::error_code::kInitFailed, init_error);
		return 1;
	}

	protocol::instance::Message ready;
	ready.type = protocol::instance::Type::kReady;
	ready.instance = config.instance_id;
	ready.init_us = NowUs() - start_us;
	if (!Send(channel, ready))
	{
		return 1;
	}

	uint64_t requests = 0;
	while (true)
	{
		std::string frame;
		std::string error;
		if (!protocol::read_frame(channel, &frame, &error))
		{
			// The supervisor closed the channel; nothing left to do.
			DUCKEMU_DEBUG("instance: channel closed: %s", error.c_str());
			return 0;
		}
		protocol::instance::Message message;
		if (!protocol::instance::parse(frame, &message, &error))
		{
			DUCKEMU_ERROR("instance: malformed message: %s", error.c_str());
			return 1;
		}
		if (message.type == protocol::instance::Type::kShutdown)
		{
			return 0;
		}
		if (message.type != protocol::instance::Type::kInvoke)
		{
			DUCKEMU_ERROR("instance: unexpected message type");
			return 1;
		}

		AppRequest request;
		request.op = message.op;
		request.params = message.params;

		AppResult result;
		AppError app_error;
		const uint64_t invoke_start_us = NowUs();
		const bool ok = plugin->invoke(instance, request, &result, &app_error);
		const uint64_t invoke_us = NowUs() - invoke_start_us;
		++requests;

		protocol::instance::Message response;
		response.type = protocol::instance::Type::kResult;
		response.id = message.id;
		response.ok = ok;
		response.invoke_us = invoke_us;
		response.requests = requests;
		if (ok)
		{
			response.result = std::move(result.values);
		}
		else
		{
			response.error.code = app_error.code;
			response.error.message = app_error.message;
		}
		if (!Send(channel, response))
		{
			return 1;
		}
	}
}

} // namespace duckemu::worker
