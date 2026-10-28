#include "runtime/instance/runtime_instance.hpp"

#include <utility>

#include "runtime/backend/unicorn/unicorn_backend.hpp"
#include "runtime/loader/bionic/shared_globals.hpp"
#include "runtime/loader/module_loader.hpp"
#include "runtime/memory/address_space.hpp"
#include "runtime/observer/trace/svc_trace.hpp"
#include "runtime/threads/scheduler.hpp"
#include "runtime/threads/tls/tls_manager.hpp"
#include "runtime/vfs/virtual_file_system.hpp"

namespace duckemu::runtime
{
namespace
{

constexpr uint32_t kBranchToSelf = 0x14000000u; // b .
// bionic_call_ifunc_resolver.cpp passes (HWCAP | _IFUNC_ARG_HWCAP, &arg).
constexpr uint64_t kIfuncHwcap = 1ULL << 62;

} // namespace

struct RuntimeInstance::Impl
{
	enum class State
	{
		Created,
		Ready,
		Faulted
	};

	RuntimeConfig config;
	RuntimeMemory memory;
	std::unique_ptr<observer::EventBus> events;
	std::unique_ptr<UnicornBackend> backend;
	std::unique_ptr<ThunkTable> thunks;
	std::unique_ptr<loader::BionicGlobals> bionic_globals;
	std::unique_ptr<loader::ModuleLoader> modules;
	// Files outlive the modules whose names they resolve (/proc/self/maps).
	std::unique_ptr<vfs::VirtualFileSystem> files;
	std::unique_ptr<tls::TlsManager> tls;
	// Co-owned with the bus listener that writes to it: the listener captures
	// a shared_ptr, so the file outlives any Impl member regardless of
	// destruction order.
	std::shared_ptr<observer::SvcTraceFile> trace;

	GuestVA stack_base;
	uint64_t stack_top = 0;
	GuestVA return_trap;
	GuestVA ifunc_arg; // guest __ifunc_arg_t, allocated on first IFUNC resolution
	State state = State::Created;

	Result<void> ensureIfuncArg()
	{
		if (ifunc_arg.value() != 0) {
			return {};
		}
		auto page = memory.map(kGuestPageSize, PermReadWrite, "ifunc-arg");
		if (!page.ok()) {
			return page.error();
		}
		// __ifunc_arg_t { size, hwcap, hwcap2 }; the guest CPU advertises no
		// optional extensions, so both hwcaps stay zero.
		const uint64_t arg[3] = { 3 * sizeof(uint64_t), 0, 0 };
		auto written = memory.write(page.value(), arg, sizeof(arg));
		if (!written.ok()) {
			return written.error();
		}
		ifunc_arg = page.value();
		return {};
	}

	// Declared last: the scheduler references every member above and must be
	// destroyed first so its host threads are joined while the rest is alive.
	std::unique_ptr<threads::ThreadScheduler> scheduler;
};

RuntimeInstance::RuntimeInstance(std::unique_ptr<Impl> impl)
	: impl_(std::move(impl))
{
}

RuntimeInstance::~RuntimeInstance() = default;

Result<std::unique_ptr<RuntimeInstance> > RuntimeInstance::create(const RuntimeConfig &config)
{
	auto impl = std::make_unique<Impl>();
	impl->config = config;

	impl->events = std::make_unique<observer::EventBus>();
	impl->memory.setEventBus(impl->events.get());

	auto backend = UnicornBackend::create(impl->memory);
	if (!backend.ok()) {
		return backend.error();
	}
	impl->backend = std::move(backend).value();
	impl->memory.setMapObserver(impl->backend.get());
	impl->memory.setCodeNotifier(impl->backend.get());

	const uint64_t stack_size = config.stack_size < kGuestPageSize ? kGuestPageSize :
										 alignUp(config.stack_size, kGuestPageSize);
	auto stack = impl->memory.map(stack_size, PermReadWrite, "stack");
	if (!stack.ok()) {
		return stack.error();
	}
	impl->stack_base = stack.value();
	impl->stack_top = stack.value().value() + stack_size - 16;

	auto thunks = ThunkTable::create(impl->memory);
	if (!thunks.ok()) {
		return thunks.error();
	}
	impl->thunks = std::move(thunks).value();

	loader::BionicGlobalsConfig globals_config;
	globals_config.pid = config.pid;
	globals_config.progname = "duckemu";
	globals_config.environment = { "ANDROID_ROOT=/system", "ANDROID_DATA=/data" };
	impl->bionic_globals =
		std::make_unique<loader::BionicGlobals>(impl->memory, std::move(globals_config));
	if (auto initialized = impl->bionic_globals->initialize(); !initialized.ok()) {
		return initialized.error();
	}

	impl->modules = std::make_unique<loader::ModuleLoader>(impl->memory,
		*impl->thunks,
		impl->events.get(),
		config.library_search_paths,
		config.enable_test_import_stubs);
	impl->modules->setBionicGlobals(impl->bionic_globals.get());

	vfs::VfsConfig vfs_config;
	vfs_config.pid = config.pid;
	vfs_config.tid = config.tid;
	vfs_config.progname = "duckemu";
	vfs_config.host_roots = config.filesystem_roots;
	impl->files =
		std::make_unique<vfs::VirtualFileSystem>(impl->memory, std::move(vfs_config));
	impl->files->setAuxvProvider([globals = impl->bionic_globals.get()] {
		return Result<std::pair<GuestVA, uint64_t> >(
			std::make_pair(globals->auxvAddress(), globals->auxvSize()));
	});
	impl->files->setRegionNameResolver([registry = &impl->modules->registry()](std::string_view name) {
		constexpr std::string_view prefix = "module:";
		if (name.rfind(prefix, 0) == 0)
		{
			const loader::Module *module = registry->findBySoname(name.substr(prefix.size()));
			if (module != nullptr)
			{
				return module->path;
			}
		}
		return std::string(name);
	});

	impl->tls = std::make_unique<tls::TlsManager>(impl->memory);
	auto tls_instance = impl->tls->create(config.tid, config.pid, impl->stack_top);
	if (!tls_instance.ok()) {
		return tls_instance.error();
	}

	tls::TlsManager *tls_manager = impl->tls.get();
	impl->modules->setTlsTemplateSink(
		[tls_manager](int64_t tls_offset, const void *data, uint64_t size, uint64_t memsz) {
			return tls_manager->materialize(tls_offset, data, size, memsz);
		});

	auto trap = impl->memory.map(kGuestPageSize, PermRead | PermExec, "return-trap");
	if (!trap.ok()) {
		return trap.error();
	}
	impl->return_trap = trap.value();
	if (auto written = impl->memory.write(impl->return_trap, &kBranchToSelf, sizeof(kBranchToSelf));
		!written.ok()) {
		return written.error();
	}

	threads::SchedulerConfig scheduler_config;
	scheduler_config.pid = config.pid;
	scheduler_config.tid = config.tid;
	scheduler_config.stack_top = impl->stack_top;
	scheduler_config.return_trap = impl->return_trap;
	scheduler_config.main_tls_tp = impl->tls->threadPointer();
	scheduler_config.default_timeout_us = config.default_timeout_us;
	scheduler_config.record_memory_access = config.record_memory_access;
	scheduler_config.diagnostic_capacity = config.diagnostic_capacity;
	scheduler_config.vfs = impl->files.get();

	impl->scheduler = std::make_unique<threads::ThreadScheduler>(
		impl->memory, *impl->backend, *impl->thunks, *impl->events, scheduler_config);
	if (auto started = impl->scheduler->start(); !started.ok()) {
		return started.error();
	}
	impl->files->setThreadCountProvider([scheduler = impl->scheduler.get()] {
		return static_cast<uint64_t>(scheduler->liveThreadCount());
	});

	if (!config.trace_path.empty()) {
		auto trace = observer::SvcTraceFile::open(config.trace_path);
		if (!trace.ok()) {
			return trace.error();
		}
		impl->trace = std::shared_ptr<observer::SvcTraceFile>(std::move(trace).value());
		impl->events->addSvcListener([writer = impl->trace](const observer::SvcEvent &event) {
			writer->write(event);
		});
	}

	impl->state = Impl::State::Ready;
	return std::unique_ptr<RuntimeInstance>(new RuntimeInstance(std::move(impl)));
}

Result<GuestVA> RuntimeInstance::mapMemory(uint64_t size, uint32_t perms, std::string_view name)
{
	// Host API vs. guest execution: the backend must not touch an engine while
	// it is running. Recursive, so a listener on the emulating thread re-enters.
	auto execution = impl_->scheduler->lockExecution();
	return impl_->memory.map(size, perms, name);
}

Result<void> RuntimeInstance::writeMemory(GuestVA address, const void *data, uint64_t size)
{
	auto execution = impl_->scheduler->lockExecution();
	return impl_->memory.write(address, data, size);
}

Result<void> RuntimeInstance::readMemory(GuestVA address, void *data, uint64_t size) const
{
	auto execution = impl_->scheduler->lockExecution();
	return impl_->memory.read(address, data, size);
}

Result<uint64_t> RuntimeInstance::loadLibrary(std::string_view path)
{
	if (impl_->state != Impl::State::Ready)
	{
		return Error{ ErrorCode::InvalidArgument, "RuntimeInstance is not ready to load modules" };
	}

	uint64_t handle = 0;
	std::vector<loader::IfuncFixup> ifunc_fixups;
	{
		// Mapping and relocation must not race a running engine; constructors
		// must not run while this thread holds the execution lock (the main
		// guest thread needs it to make progress), so the lock is released
		// before the initializer loop below.
		auto execution = impl_->scheduler->lockExecution();
		auto loaded = impl_->modules->load(path);
		if (!loaded.ok())
		{
			// Dependencies that did load keep their constructors pending; they
			// are discarded rather than run against a half-loaded graph.
			(void)impl_->modules->takeInitializers();
			(void)impl_->modules->takeIfuncFixups();
			return loaded.error();
		}
		handle = loaded.value();
		ifunc_fixups = impl_->modules->takeIfuncFixups();
		if (!ifunc_fixups.empty())
		{
			if (auto ensured = impl_->ensureIfuncArg(); !ensured.ok())
			{
				return ensured.error();
			}
		}
	}

	// IFUNC resolvers are guest code. They cannot run while this thread holds
	// the execution lock, so they run here, on the main guest thread, and the
	// resulting addresses are patched back under the lock.
	for (const loader::IfuncFixup &fixup : ifunc_fixups)
	{
		const std::array<uint64_t, 8> args = { kIfuncHwcap, impl_->ifunc_arg.value() };
		auto call = impl_->scheduler->callFunction(fixup.resolver, args);
		if (!call.ok())
		{
			if (call.error().code == ErrorCode::EmulationFault)
			{
				impl_->state = Impl::State::Faulted;
			}
			return call.error();
		}
		if (call.value().exit_code.has_value())
		{
			return Error{ ErrorCode::Unsupported, "an IFUNC resolver called exit_group" };
		}
		uint64_t value = call.value().return_value;
		auto execution = impl_->scheduler->lockExecution();
		auto written = impl_->memory.write(fixup.place, &value, sizeof(value));
		if (!written.ok())
		{
			return written.error();
		}
	}

	// preinit reads StaticTlsLayout through __libc_shared_globals; publish the
	// loader's final layout before any constructor runs.
	impl_->bionic_globals->setStaticTlsLayout(impl_->modules->tlsLayout().size(),
		16,
		0,
		impl_->tls->bionicTlsOffset());

	const std::vector<loader::Initializer> initializers = impl_->modules->takeInitializers();
	if (!impl_->config.run_module_initializers)
	{
		return handle;
	}
	for (const loader::Initializer &initializer : initializers)
	{
		auto call = impl_->scheduler->callFunction(initializer.function, {});
		if (!call.ok())
		{
			if (call.error().code == ErrorCode::EmulationFault)
			{
				impl_->state = Impl::State::Faulted;
			}
			return call.error();
		}
		if (call.value().exit_code.has_value())
		{
			return Error{ ErrorCode::Unsupported, "a module constructor called exit_group" };
		}
	}
	return handle;
}

Result<GuestVA> RuntimeInstance::lookupSymbol(uint64_t handle, std::string_view name) const
{
	// Resolving a virtual symbol materializes a thunk (writes guest memory), so
	// this needs the same guard as every other host API.
	auto execution = impl_->scheduler->lockExecution();
	return impl_->modules->lookup(handle, name);
}

Result<GuestVA> RuntimeInstance::lookupSymbol(std::string_view name) const
{
	auto execution = impl_->scheduler->lockExecution();
	return impl_->modules->lookupGlobal(name);
}

Result<GuestVA> RuntimeInstance::registerHostFunction(std::string_view name, HostStub stub)
{
	auto execution = impl_->scheduler->lockExecution();
	return impl_->thunks->add(name, std::move(stub));
}

Result<RuntimeInstance::CallResult> RuntimeInstance::callFunction(GuestVA entry, const std::array<uint64_t, 8> &args)
{
	if (impl_->state != Impl::State::Ready) {
		return Error{ ErrorCode::InvalidArgument, "RuntimeInstance is not ready to execute" };
	}

	auto call = impl_->scheduler->callFunction(entry, args);
	if (!call.ok()) {
		if (call.error().code == ErrorCode::EmulationFault) {
			impl_->state = Impl::State::Faulted;
		}
		return call.error();
	}

	CallResult result;
	result.return_value = call.value().return_value;
	result.exit_code = call.value().exit_code;
	return result;
}

uint32_t RuntimeInstance::addSvcListener(observer::EventBus::SvcListener listener)
{
	return impl_->events->addSvcListener(std::move(listener));
}

uint32_t RuntimeInstance::addModuleListener(observer::EventBus::ModuleListener listener)
{
	return impl_->events->addModuleListener(std::move(listener));
}

uint32_t RuntimeInstance::addMemoryListener(observer::EventBus::MemoryListener listener)
{
	return impl_->events->addMemoryListener(std::move(listener));
}

uint32_t RuntimeInstance::addThreadListener(observer::EventBus::ThreadListener listener)
{
	return impl_->events->addThreadListener(std::move(listener));
}

bool RuntimeInstance::removeSvcListener(uint32_t id)
{
	return impl_->events->removeSvcListener(id);
}

bool RuntimeInstance::removeMemoryListener(uint32_t id)
{
	return impl_->events->removeMemoryListener(id);
}

bool RuntimeInstance::removeThreadListener(uint32_t id)
{
	return impl_->events->removeThreadListener(id);
}

bool RuntimeInstance::removeModuleListener(uint32_t id)
{
	return impl_->events->removeModuleListener(id);
}

size_t RuntimeInstance::drainDiagnostics(std::vector<observer::DiagnosticEvent> &out)
{
	return impl_->scheduler->drainDiagnostics(out);
}

uint64_t RuntimeInstance::droppedDiagnostics() const
{
	return impl_->scheduler->droppedDiagnostics();
}

} // namespace duckemu::runtime
