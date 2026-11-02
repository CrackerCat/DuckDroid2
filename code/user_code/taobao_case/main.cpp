// SPDX-License-Identifier: GPL-2.0-only
// taobao_case: 淘宝 10.68.0 (com.taobao.taobao) SecurityGuard 6.8.260901.
//
// Stage 0 is the load probe: install the Java surface, load the ROM libc and
// libsgmainso, run its init_array + JNI_OnLoad, and print the strict-JNI
// failures the protected library would otherwise swallow. Everything the old
// DuckDroid case learned is in code/user_code/TaoBao/.
//
//   ./duckemu_taobao_case                      # init + 70102, prints the header map
//   ./duckemu_taobao_case --stage 70102        # same
//   ./duckemu_taobao_case --init --jni-events  # init diagnostics
//   ./duckemu_taobao_case --trace DIR          # same signing run, with an insn trace
//
// TAOBAO_VERBOSE=1 restores the progress lines on a signing run.
//
// The APK at assets/root/data/app/.../base.apk is not tracked by git; CMake
// tells the operator where to link/copy it from.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>


#include "runtime/instance/runtime_instance.hpp"
#include "runtime/observer/semantic/jni_event.hpp"
#include "runtime/observer/semantic/svc_event.hpp"
#include "taobao_case/taobao_case.hpp"
#include "taobao_case/taobao_env.hpp"

#ifndef DUCKEMU_ROM_BOOTSTRAP
#error "DUCKEMU_ROM_BOOTSTRAP must point at the ROM bootstrap lib directory"
#endif
#ifndef DUCKEMU_ROM_LIB64
#error "DUCKEMU_ROM_LIB64 must point at the ROM system/lib64 directory"
#endif
#ifndef DUCKEMU_TAOBAO_SO
#error "DUCKEMU_TAOBAO_SO must point at libsgmainso-6.8.260901.so"
#endif
#ifndef DUCKEMU_TAOBAO_ROOT
#error "DUCKEMU_TAOBAO_ROOT must point at the guest file-system root"
#endif
#ifndef DUCKEMU_TAOBAO_PROPERTIES
#error "DUCKEMU_TAOBAO_PROPERTIES must point at the device property picture"
#endif
#ifndef DUCKEMU_TAOBAO_SEED
#error "DUCKEMU_TAOBAO_SEED must point at the captured app data directory"
#endif
#ifndef DUCKEMU_TAOBAO_STATE
#error "DUCKEMU_TAOBAO_STATE must point at the persistent app-state directory"
#endif

using namespace duckemu;
using namespace duckemu::runtime;
using namespace duckemu::user;

namespace
{

// AVMP runs the whole doCommand path inside its VM; the old probe allowed
// 900 s per call and the DuckDroid reference even longer. Keep a guard that
// does not fire before the library's own internal budgets.
constexpr uint64_t kCallTimeoutUs = 900ULL * 1000 * 1000;

std::string romLibc()
{
	return std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
}

// SecurityGuard expects the app's data directory to exist and be writable:
// the dexopt directory, its lock files and the extracted AVMP modules. The
// state is persistent on purpose: the first run may extract lvm_* modules
// from base.apk, and later runs must reuse them instead of scanning 90 MB
// again. TAOBAO_STATE_DIR overrides the location.
class StateRoot
{
    public:
	StateRoot() = default;
	StateRoot(const StateRoot &) = delete;
	StateRoot &operator=(const StateRoot &) = delete;

	bool create(std::string *error)
	{
		std::error_code code;
		const char *override_dir = std::getenv("TAOBAO_STATE_DIR");
		path_ = override_dir != nullptr ? std::filesystem::path(override_dir) :
						  std::filesystem::path(DUCKEMU_TAOBAO_STATE);
		const std::filesystem::path data_dir =
			path_ / "data/user/0/com.taobao.taobao";
		const std::filesystem::path marker = data_dir / "app_SGLib";
		const bool first_run = !std::filesystem::exists(marker);
		if (first_run)
		{
			std::filesystem::remove_all(path_, code);
			std::filesystem::create_directories(data_dir, code);
			if (code)
			{
				*error = "cannot create " + data_dir.string() + ": " +
					code.message();
				return false;
			}
			std::filesystem::copy(DUCKEMU_TAOBAO_SEED, data_dir,
				std::filesystem::copy_options::recursive |
					std::filesystem::copy_options::overwrite_existing,
				code);
			if (code)
			{
				*error = "cannot seed the app data directory: " + code.message();
				return false;
			}
			// Lock files are runtime artifacts. The SDK creates them with
			// O_CREAT|O_EXCL and reads EEXIST as "another process holds the
			// lock"; a seeded stale lock makes init wait forever.
			const std::filesystem::path files = data_dir / "files";
			for (const auto &entry : std::filesystem::directory_iterator(files, code))
			{
				if (entry.path().extension() == ".lock")
				{
					std::filesystem::remove(entry.path(), code);
					code.clear();
				}
			}
			// These three state files flip the SDK from a 14-read ZIP
			// walk to a 1 KiB-step backward scan of the 90 MB APK. The
			// reference device had them, but this emulated install did not
			// finish a real first launch, so serving them costs minutes.
			for (const char *relative : { "app_SGLib/SG_FAST_CONFIG",
				     "app_SGLib/.sf/.uf_wk", "app_SGLib/.sf/.uf_sw" })
			{
				std::filesystem::remove(data_dir / relative, code);
				code.clear();
			}
			// The captured directory is the install that produced the
			// snapshot; the SDK also probes the timestamp stored in its state
			// files. Seed the second name with the same content.
			const std::filesystem::path captured =
				data_dir / "app_SGLib/app_1793012793";
			const std::filesystem::path probed =
				data_dir / "app_SGLib/app_1793010560";
			if (std::filesystem::is_directory(captured) &&
				!std::filesystem::exists(probed))
			{
				std::filesystem::copy(captured, probed,
					std::filesystem::copy_options::recursive, code);
				if (code)
				{
					*error = "cannot seed the probed app directory: " +
						code.message();
					return false;
				}
			}
		}
		// Lock files are runtime artifacts and this runner never lets the SDK
		// release them (the process is torn down from the host). A stale lock
		// makes the next run's O_CREAT|O_EXCL see EEXIST, which the SDK reads
		// as "another process holds it" and retries for minutes. Clear them on
		// every start, not just the first seed.
		const std::filesystem::path files = data_dir / "files";
		for (const auto &entry : std::filesystem::directory_iterator(files, code))
		{
			if (entry.path().extension() == ".lock")
			{
				std::filesystem::remove(entry.path(), code);
				code.clear();
			}
		}
		for (const auto &entry :
			std::filesystem::directory_iterator(data_dir / "app_SGLib", code))
		{
			if (entry.is_directory() && entry.path().filename().string().rfind("app_", 0) == 0)
			{
				std::filesystem::remove(entry.path() / "lock.lock", code);
				code.clear();
			}
		}
		code.clear();
		const std::filesystem::path directories[] = {
			data_dir / "files/storage/com.taobao.maindex",
		};
		for (const auto &directory : directories)
		{
			std::filesystem::create_directories(directory, code);
			if (code)
			{
				*error = "cannot create " + directory.string() + ": " + code.message();
				return false;
			}
		}
		return true;
	}

	std::string path() const
	{
		return path_.string();
	}

    private:
	std::filesystem::path path_;
};

void printJniEvent(const observer::JniEvent &event)
{
	std::fprintf(stderr, "jni %s class=%s name=%s sig=%s slot=%u tid=%u\n",
		std::string(observer::jniEventKindName(event.kind)).c_str(),
		event.class_descriptor.c_str(),
		event.name.c_str(),
		event.signature.c_str(),
		event.slot,
		event.tid);
}

// Debug-only view of the syscalls the SDK uses to explain a failure: paths it
// opens, and what it writes to fd 2 before aborting. Off unless
// TAOBAO_SVC_DEBUG is set.
std::string readGuestString(RuntimeInstance &instance, uint64_t address, size_t limit)
{
	std::string text;
	for (size_t index = 0; index < limit; ++index)
	{
		char character = 0;
		if (!instance.readMemory(GuestVA{ address + index }, &character, 1).ok())
		{
			break;
		}
		if (character == '\0')
		{
			break;
		}
		text.push_back(character);
	}
	return text;
}

void printSvcEvent(RuntimeInstance &instance, const observer::SvcEvent &event)
{
	if (event.name == "openat" || event.name == "faccessat" ||
		event.name == "newfstatat" || event.name == "readlinkat")
	{
		const int64_t result = static_cast<int64_t>(event.result);
		// Only failures: the successful opens are noisy and already in svc.log.
		if (result >= 0)
		{
			return;
		}
		std::fprintf(stderr, "svc %s \"%s\" -> %lld\n", event.name.c_str(),
			readGuestString(instance, event.args[1], 512).c_str(),
			static_cast<long long>(result));
		return;
	}
	if (event.name == "write" || event.name == "writev")
	{
		const int64_t fd = static_cast<int64_t>(event.args[0]);
		if (fd != 2)
		{
			return;
		}
		std::string text;
		if (event.name == "write")
		{
			text = readGuestString(instance,
				event.args[1], static_cast<size_t>(event.args[2]));
		}
		else
		{
			const uint64_t count = event.args[2];
			for (uint64_t index = 0; index < count && index < 8; ++index)
			{
				uint64_t iovec[2] = {};
				if (!instance.readMemory(GuestVA{ event.args[1] + index * 16 }, iovec, sizeof(iovec)).ok())
				{
					break;
				}
				text += readGuestString(instance, iovec[0], static_cast<size_t>(iovec[1]));
			}
		}
		std::fprintf(stderr, "svc %s: %s\n", event.name.c_str(), text.c_str());
	}
}

} // namespace

int main(int argc, char **argv)
{
	std::string trace_path;
	bool print_jni = std::getenv("TAOBAO_JNI_EVENTS") != nullptr;
	bool run_init = false;
	std::string stage;

	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		if (argument == "--trace" && index + 1 < argc)
		{
			trace_path = argv[++index];
			continue;
		}
		if (argument == "--jni-events")
		{
			print_jni = true;
			continue;
		}
		if (argument == "--init")
		{
			run_init = true;
			continue;
		}
		if (argument == "--stage" && index + 1 < argc)
		{
			stage = argv[++index];
			run_init = true;
			continue;
		}
		std::fprintf(stderr,
			"usage: %s [--trace DIR] [--jni-events] [--init] "
			"[--stage 10102|10101|70102]\n"
			"       (no arguments: run init and print the default 70102 "
			"signature result)\n",
			argv[0]);
		return 2;
	}

	// With no request for a diagnostic stage, the default action is a plain
	// signing run. Progress lines are a debug aid: --init and the two init
	// stages keep them, 70102 prints only the final header map, and
	// TAOBAO_VERBOSE=1 restores them anywhere.
	const bool env_verbose = std::getenv("TAOBAO_VERBOSE") != nullptr;
	if (!run_init)
	{
		stage = "70102";
	}
	const bool verbose = env_verbose || (run_init && stage != "70102");
	taobao::setVerbose(verbose);

	if (!trace_path.empty())
	{
		std::error_code directory_error;
		std::filesystem::create_directories(trace_path, directory_error);
		if (directory_error)
		{
			std::fprintf(stderr, "cannot create trace directory %s: %s\n",
				trace_path.c_str(), directory_error.message().c_str());
			return 1;
		}
	}

	// The app data directory is writable by default: SecurityGuard creates
	// lock files, extracts lvm_* modules and rewrites its state. TAOBAO_READONLY_STATE=1
	// reproduces the old read-only baseline for comparison.
	StateRoot state;
	std::string state_error;
	const bool writable_state = std::getenv("TAOBAO_READONLY_STATE") == nullptr;
	if (writable_state && !state.create(&state_error))
	{
		std::fprintf(stderr, "%s\n", state_error.c_str());
		return 1;
	}

	RuntimeConfig config;
	config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	config.filesystem_roots = { DUCKEMU_TAOBAO_ROOT };
	config.filesystem_write_root = writable_state ? state.path() : std::string{};
	config.property_file = DUCKEMU_TAOBAO_PROPERTIES;
	config.trace_path = trace_path.empty() ? std::string{} : trace_path + "/svc.log";
	config.default_timeout_us = kCallTimeoutUs;
	// Pinning entropy is what makes a signing run reproducible: the SDK mixes
	// getrandom()/dev/urandom into the headers. TAOBAO_RANDOM_SEED=0 is a valid
	// seed; only an unset variable keeps host entropy.
	if (const char *seed = std::getenv("TAOBAO_RANDOM_SEED"))
	{
		config.random_seed = std::strtoull(seed, nullptr, 0);
	}

	auto created = RuntimeInstance::create(config);
	if (!created.ok())
	{
		std::fprintf(stderr, "RuntimeInstance::create failed: %s\n",
			created.error().message.c_str());
		return 1;
	}
	RuntimeInstance &instance = *created.value();

	if (const char *wall = std::getenv("TAOBAO_WALL_TIME_NS"))
	{
		instance.setWallTimeNs(std::strtoull(wall, nullptr, 10));
	}

	auto installed = instance.installJavaEnvironment(&taobao::installEnvironment);
	if (!installed.ok())
	{
		std::fprintf(stderr, "install Java environment failed: %s\n",
			installed.error().message.c_str());
		return 1;
	}

	if (print_jni)
	{
		instance.addJniListener(printJniEvent);
	}
	if (std::getenv("TAOBAO_SVC_DEBUG") != nullptr)
	{
		instance.addSvcListener([&instance](const observer::SvcEvent &event) {
			printSvcEvent(instance, event);
		});
	}

	auto libc = instance.loadLibrary(romLibc());
	if (!libc.ok())
	{
		std::fprintf(stderr, "load libc failed: %s\n", libc.error().message.c_str());
		return 1;
	}
	auto module = instance.loadLibrary(DUCKEMU_TAOBAO_SO);
	if (!module.ok())
	{
		std::fprintf(stderr, "load libsgmainso failed: %s\n", module.error().message.c_str());
		return 1;
	}

	auto range = instance.moduleRange(module.value());
	if (!range.ok())
	{
		std::fprintf(stderr, "module range failed: %s\n", range.error().message.c_str());
		return 1;
	}
	if (verbose)
	{
		std::printf("module base=0x%llx span=0x%llx\n",
			static_cast<unsigned long long>(range.value().base.value()),
			static_cast<unsigned long long>(range.value().span));
	}

	if (verbose)
	{
		std::fprintf(stderr, "--- init ---\n");
	}
	Result<void> result;
	if (stage == "70102")
	{
		// 70102 is the mtop signing entry, so the init chain runs first.
		result = taobao::init(instance);
		bool tracing = false;
		if (result.ok() && !trace_path.empty())
		{
			// Trace only this call: init() has already run and its
			// background guest threads are alive, so arm on the registered
			// native's entry instead of the first in-range instruction.
			auto entry = instance.lookupRegisteredNative(taobao::kNativeClass,
				taobao::kNativeMethod,
				taobao::kNativeSignature);
			if (!entry.ok())
			{
				result = entry.error();
			}
			else
			{
				std::fprintf(stderr, "70102 entry=0x%llx\n",
					static_cast<unsigned long long>(entry.value().value()));
				observer::InsnTraceOptions options;
				options.directory = trace_path + "/insn";
				options.begin = range.value().base;
				options.size = range.value().span;
				options.stop_at_return = true;
				options.arm_address = entry.value();
				auto started = instance.startInsnTrace(options);
				if (!started.ok())
				{
					result = started.error();
				}
				else
				{
					tracing = true;
				}
			}
		}
		if (result.ok())
		{
			// The one line a signing run prints: the header map the SDK
			// returned (x-sign / x-mini-wua / x-sgext / x-gst / x-umt).
			auto headers = taobao::sign70102(instance);
			if (headers.ok())
			{
				std::printf("%s\n", headers.value().c_str());
			}
			else
			{
				result = headers.error();
			}
		}
		if (tracing)
		{
			instance.stopInsnTrace();
		}
	}
	else
	{
		result = stage == "10102" ? taobao::init10102(instance) :
			 stage == "10101" ? taobao::init10101(instance) :
					   taobao::init(instance);
	}
	if (!result.ok())
	{
		std::fprintf(stderr, "%s failed: %s\n",
			stage.empty() ? "init" : stage.c_str(),
			result.error().message.c_str());
		return 1;
	}
	if (verbose)
	{
		std::printf("%s ok\n", stage.empty() ? "init" : stage.c_str());
	}

	return 0;
}
