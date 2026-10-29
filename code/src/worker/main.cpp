// duckemu_worker has two modes:
//
//   --instance      the RuntimeInstance host (S2). Forked and exec'd by the
//                   supervisor; it creates the only engine in its process.
//   (supervisor)    the pool/scheduler that owns instance processes (S3).
//
// The same binary serves both so a target plugin is linked exactly once and
// its world is identical no matter which process loads it.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/log/logger.hpp"
#include "worker/instance_host.hpp"

namespace
{

void Usage()
{
	std::fprintf(stderr,
		"usage: duckemu_worker --instance --ipc-fd FD --instance-id ID\n"
		"                      --app ID --library PATH --rom-libc PATH\n"
		"                      [--library-path DIR]... [--trace DIR]\n"
		"                      [--wall-time-ns N] [--monotonic-time-ns N]\n");
}

bool TakeValue(int argc, char **argv, int *index, const char *name, std::string *out)
{
	if (*index + 1 >= argc)
	{
		std::fprintf(stderr, "worker: %s needs a value\n", name);
		return false;
	}
	*out = argv[++(*index)];
	return true;
}

bool ParseU64(const std::string &text, uint64_t *out)
{
	errno = 0;
	char *end = nullptr;
	const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
	if (errno != 0 || end == text.c_str() || *end != '\0')
	{
		return false;
	}
	*out = static_cast<uint64_t>(value);
	return true;
}

} // namespace

int main(int argc, char **argv)
{
	duckemu::log::set_level(duckemu::log::Level::Info);

	bool instance_mode = false;
	bool has_ipc_fd = false;
	int ipc_fd = -1;
	duckemu::worker::InstanceHostConfig config;

	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		std::string value;
		uint64_t number = 0;

		if (argument == "--instance")
		{
			instance_mode = true;
		}
		else if (argument == "--ipc-fd")
		{
			if (!TakeValue(argc, argv, &index, "--ipc-fd", &value) || !ParseU64(value, &number) ||
				number > 1u << 20)
			{
				std::fprintf(stderr, "worker: --ipc-fd needs a file descriptor number\n");
				return 2;
			}
			ipc_fd = static_cast<int>(number);
			has_ipc_fd = true;
		}
		else if (argument == "--instance-id")
		{
			if (!TakeValue(argc, argv, &index, "--instance-id", &config.instance_id))
			{
				return 2;
			}
		}
		else if (argument == "--app")
		{
			if (!TakeValue(argc, argv, &index, "--app", &config.app.app))
			{
				return 2;
			}
		}
		else if (argument == "--library")
		{
			if (!TakeValue(argc, argv, &index, "--library", &config.app.library))
			{
				return 2;
			}
		}
		else if (argument == "--rom-libc")
		{
			if (!TakeValue(argc, argv, &index, "--rom-libc", &config.app.rom_libc))
			{
				return 2;
			}
		}
		else if (argument == "--library-path")
		{
			if (!TakeValue(argc, argv, &index, "--library-path", &value))
			{
				return 2;
			}
			config.app.library_search_paths.push_back(value);
		}
		else if (argument == "--trace")
		{
			if (!TakeValue(argc, argv, &index, "--trace", &config.trace_path))
			{
				return 2;
			}
		}
		else if (argument == "--wall-time-ns")
		{
			if (!TakeValue(argc, argv, &index, "--wall-time-ns", &value) || !ParseU64(value, &number))
			{
				std::fprintf(stderr, "worker: --wall-time-ns needs a number\n");
				return 2;
			}
			config.has_wall_time = true;
			config.wall_time_ns = number;
		}
		else if (argument == "--monotonic-time-ns")
		{
			if (!TakeValue(argc, argv, &index, "--monotonic-time-ns", &value) || !ParseU64(value, &number))
			{
				std::fprintf(stderr, "worker: --monotonic-time-ns needs a number\n");
				return 2;
			}
			config.has_monotonic_time = true;
			config.monotonic_time_ns = number;
		}
		else
		{
			std::fprintf(stderr, "worker: unknown argument %s\n", argument.c_str());
			return 2;
		}
	}

	if (!instance_mode)
	{
		std::fprintf(stderr, "worker: supervisor mode is not implemented yet (M7 S3)\n");
		Usage();
		return 2;
	}
	if (!has_ipc_fd || config.instance_id.empty() || config.app.app.empty() ||
		config.app.library.empty() || config.app.rom_libc.empty())
	{
		Usage();
		return 2;
	}
	config.ipc_fd = ipc_fd;
	return duckemu::worker::run_instance_host(config);
}
