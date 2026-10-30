// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/instance/runtime_instance.hpp"

#include <utility>

#include "runtime/backend/unicorn/unicorn_backend.hpp"
#include "runtime/clock/runtime_clock.hpp"
#include "runtime/jni/env/jni_state.hpp"
#include "runtime/jni/envs/envs.hpp"
#include "runtime/jni/world/java_world.hpp"
#include "runtime/loader/bionic/shared_globals.hpp"
#include "runtime/loader/dl_service.hpp"
#include "runtime/loader/module_loader.hpp"
#include "runtime/memory/address_space.hpp"
#include "runtime/observer/trace/svc_trace.hpp"
#include "runtime/props/property_files.hpp"
#include "runtime/props/property_table.hpp"
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

struct RuntimeInstance::Impl : loader::DlService
{
	enum class State
	{
		Created,
		Ready,
		Faulted
	};

	RuntimeConfig config;
	// Instance-wide time source. Defaults to the host clock; a replay can pin
	// it with setWallTimeNs/setMonotonicTimeNs.
	RuntimeClock clock;
	RuntimeMemory memory;
	std::unique_ptr<observer::EventBus> events;
	std::unique_ptr<UnicornBackend> backend;
	std::unique_ptr<ThunkTable> thunks;
	// JNI is created right after the thunk table: it registers one thunk per
	// JNIEnv/JavaVM table slot and publishes the guest-visible tables.
	std::unique_ptr<jni::JniState> jni;
	std::unique_ptr<loader::BionicGlobals> bionic_globals;
	std::unique_ptr<loader::ModuleLoader> modules;
	// The /dev/__properties__ images. Declared before the VFS (which points at
	// it) so it is destroyed after: reverse declaration order.
	std::unique_ptr<props::PropertyFiles> properties;
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

	// loader::DlService. Reached from the __loader_dl* stubs while a guest
	// thread runs; the stub's call_guest is the only way back into guest code
	// from there, so it is threaded through open().
	Result<uint64_t> open(std::string_view name,
		uint64_t caller_address,
		const GuestCall &call_guest) override;
	Result<GuestVA> symbol(uint64_t handle, std::string_view name) override;
	bool address(uint64_t address,
		loader::DlModuleInfo &module,
		std::string &symbol_name,
		uint64_t &symbol_address) override;
	std::vector<loader::DlModuleInfo> moduleList() override;
	void close(uint64_t handle) override
	{
		(void)handle;
	}

	// Declared last: the scheduler references every member above and must be
	// destroyed first so its host threads are joined while the rest is alive.
	std::unique_ptr<threads::ThreadScheduler> scheduler;
};

Result<uint64_t> RuntimeInstance::Impl::open(std::string_view name,
	uint64_t caller_address,
	const GuestCall &call_guest)
{
	// dlopen(NULL) is the main-program handle; dlsym on it means the global
	// scope, which symbol() maps back to lookupGlobal.
	if (name.empty())
	{
		return loader::kMainProgramHandle;
	}
	if (state != State::Ready)
	{
		return Error{ ErrorCode::InvalidArgument, "instance is not ready to load modules" };
	}
	if (!call_guest)
	{
		return Error{ ErrorCode::Unsupported, "dlopen needs a guest call path" };
	}

	// bionic passes the caller's return address so the requester's directory
	// participates in the search, exactly like a DT_NEEDED entry.
	std::string directory;
	if (caller_address != 0)
	{
		if (const loader::Module *caller = modules->registry().findContaining(caller_address))
		{
			directory = caller->directory;
		}
	}

	auto loaded = modules->loadByName(name, directory);
	if (!loaded.ok())
	{
		// Same rule as loadLibrary: a half-loaded graph never runs
		// constructors.
		(void)modules->takeInitializers();
		(void)modules->takeIfuncFixups();
		return loaded.error();
	}

	// The rest mirrors loadLibrary, but through the nested engine frame: this
	// thread is already inside uc_emu_start, so scheduler->callFunction (which
	// posts to the main thread) would deadlock or be refused.
	const std::vector<loader::IfuncFixup> ifunc_fixups = modules->takeIfuncFixups();
	if (!ifunc_fixups.empty())
	{
		if (auto ensured = ensureIfuncArg(); !ensured.ok())
		{
			return ensured.error();
		}
	}
	for (const loader::IfuncFixup &fixup : ifunc_fixups)
	{
		auto called = call_guest(fixup.resolver, { kIfuncHwcap, ifunc_arg.value() });
		if (!called.ok())
		{
			return called.error();
		}
		uint64_t value = static_cast<uint64_t>(called.value());
		if (auto written = memory.write(fixup.place, &value, sizeof(value)); !written.ok())
		{
			return written.error();
		}
	}

	for (const loader::Initializer &initializer : modules->takeInitializers())
	{
		auto called = call_guest(initializer.function, {});
		if (!called.ok())
		{
			return called.error();
		}
	}

	// JNI_OnLoad is not called: only System.loadLibrary invokes it.
	return loaded.value();
}

Result<GuestVA> RuntimeInstance::Impl::symbol(uint64_t handle, std::string_view name)
{
	if (handle == 0 || handle == loader::kMainProgramHandle)
	{
		return modules->lookupGlobal(name);
	}
	return modules->lookup(handle, name);
}

bool RuntimeInstance::Impl::address(uint64_t address,
	loader::DlModuleInfo &module,
	std::string &symbol_name,
	uint64_t &symbol_address)
{
	const loader::Module *found = modules->registry().findContaining(address);
	if (found == nullptr)
	{
		return false;
	}
	module.path = found->path;
	module.base = found->base;
	module.phdr = found->phdr;
	module.phnum = found->phnum;

	// Nearest preceding definition that still encloses the address. STT_TLS
	// values are offsets into the TLS block, not module addresses.
	constexpr uint8_t kSttTls = 6;
	uint64_t best = 0;
	symbol_name.clear();
	for (const loader::ElfSymbol &symbol : found->symbols)
	{
		if (symbol.undefined() || symbol.name.empty() || symbol.type() == kSttTls)
		{
			continue;
		}
		const uint64_t value = found->base + symbol.value;
		if (value > address || value < best)
		{
			continue;
		}
		const bool encloses = symbol.size != 0 ? address < value + symbol.size : value == address;
		if (!encloses)
		{
			continue;
		}
		best = value;
		symbol_name = symbol.name;
	}
	symbol_address = best;
	return true;
}

std::vector<loader::DlModuleInfo> RuntimeInstance::Impl::moduleList()
{
	std::vector<loader::DlModuleInfo> modules_out;
	for (const auto &module : modules->registry().modules())
	{
		if (module->is_virtual)
		{
			continue;
		}
		loader::DlModuleInfo info;
		info.path = module->path;
		info.base = module->base;
		info.phdr = module->phdr;
		info.phnum = module->phnum;
		modules_out.push_back(std::move(info));
	}
	return modules_out;
}

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

	auto java_world = std::make_unique<jni::JavaWorld>(impl->memory, impl->clock);
	if (auto initialized = java_world->initialize(); !initialized.ok())
	{
		return initialized.error();
	}
	if (auto installed = jni::envs::installAll(*java_world); !installed.ok())
	{
		return installed.error();
	}
	auto jni_state = jni::JniState::create(impl->memory,
		*impl->thunks,
		impl->events.get(),
		std::move(java_world));
	if (!jni_state.ok())
	{
		return jni_state.error();
	}
	impl->jni = std::move(jni_state).value();

	loader::BionicGlobalsConfig globals_config;
	globals_config.pid = config.pid;
	globals_config.uid = config.device.uid;
	globals_config.gid = config.device.gid;
	globals_config.progname = config.device.process_name;
	globals_config.hwcap = config.device.hwcap;
	globals_config.hwcap2 = config.device.hwcap2;
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
	impl->modules->setDlService(impl.get());

	// Properties are data files under /dev/__properties__; bionic's own
	// preinit reads them when the module graph loads, so no guest entry point
	// has to be called here. The identity entries come from the same profile
	// that feeds uname(2), getuid(2) and /proc synthesis.
	auto property_table = props::PropertyTable::forDevice(config.device);
	if (!config.property_file.empty())
	{
		auto applied = property_table.applyFile(config.property_file);
		if (!applied.ok())
		{
			return applied.error();
		}
	}
	auto property_files = props::PropertyFiles::build(property_table);
	if (!property_files.ok())
	{
		return property_files.error();
	}
	impl->properties = std::move(property_files).value();

	vfs::VfsConfig vfs_config;
	vfs_config.pid = config.pid;
	vfs_config.tid = config.tid;
	vfs_config.progname = config.device.process_name;
	vfs_config.host_roots = config.filesystem_roots;
	vfs_config.write_root = config.filesystem_write_root;
	vfs_config.uid = config.device.uid;
	vfs_config.gid = config.device.gid;
	vfs_config.device = &config.device;
	vfs_config.synthetic = impl->properties.get();
	// Injected wall time also stamps file metadata, so a replay sees the same
	// stat(2) results.
	vfs_config.clock = &impl->clock;
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
	scheduler_config.clock = &impl->clock;
	scheduler_config.device = &config.device;

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

Result<void> RuntimeInstance::installJavaEnvironment(JavaEnvironmentInstaller installer)
{
	if (impl_->state != Impl::State::Ready)
	{
		return Error{ ErrorCode::InvalidArgument, "RuntimeInstance is not ready to install environments" };
	}
	if (installer == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "null Java environment installer" };
	}
	// Declarations allocate guest metadata; serialize with any running guest.
	auto execution = impl_->scheduler->lockExecution();
	// JNI_OnLoad runs inside loadLibrary, so every declaration must be in place
	// before the first module load. Refusing afterwards is deliberate: a late
	// install would look successful while the target already failed FindClass.
	if (impl_->modules->moduleCount() != 0)
	{
		return Error{ ErrorCode::InvalidArgument,
			"install Java environments before loading any module" };
	}
	return installer(impl_->jni->world());
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
	impl_->bionic_globals->setStaticTlsLayout(impl_->modules->tlsLayout().paddedSize(),
		impl_->modules->tlsLayout().alignment(),
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

	// JNI_OnLoad, when exported by the module being loaded, runs after its
	// constructors and before loadLibrary returns (the System.loadLibrary
	// analogue). Dependencies are not asked: dlopen only calls the entry point
	// of the library that was opened.
	std::optional<GuestVA> jni_on_load;
	{
		auto execution = impl_->scheduler->lockExecution();
		auto found = impl_->modules->lookup(handle, "JNI_OnLoad");
		if (found.ok())
		{
			jni_on_load = found.value();
		}
	}
	if (jni_on_load.has_value())
	{
		const std::array<uint64_t, 8> jni_args = { impl_->jni->javaVm(), 0 };
		auto call = impl_->scheduler->callFunction(jni_on_load.value(), jni_args);
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
			return Error{ ErrorCode::Unsupported, "JNI_OnLoad called exit_group" };
		}
		const int64_t version = static_cast<int32_t>(call.value().return_value);
		if (version < 0)
		{
			return Error{ ErrorCode::Unsupported,
				"JNI_OnLoad returned " + std::to_string(version) };
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

Result<uint64_t> RuntimeInstance::callRegisteredNative(std::string_view class_descriptor,
	std::string_view name,
	std::string_view signature,
	const std::array<uint64_t, 8> &args)
{
	if (impl_->state != Impl::State::Ready)
	{
		return Error{ ErrorCode::InvalidArgument, "RuntimeInstance is not ready to execute" };
	}
	return impl_->jni->callRegisteredNative(*impl_->scheduler,
		class_descriptor,
		name,
		signature,
		args);
}

Result<GuestVA> RuntimeInstance::lookupRegisteredNative(std::string_view class_descriptor,
	std::string_view name,
	std::string_view signature) const
{
	return impl_->jni->lookupRegisteredNative(class_descriptor, name, signature);
}

Result<std::string> RuntimeInstance::jniStringUtf8(uint64_t handle) const
{
	return impl_->jni->stringUtf8(handle);
}

Result<uint64_t> RuntimeInstance::jniResolveHandle(uint64_t handle) const
{
	return impl_->jni->resolveHandle(handle);
}

void RuntimeInstance::setWallTimeNs(uint64_t ns)
{
	auto execution = impl_->scheduler->lockExecution();
	impl_->clock.setWallTimeNs(ns);
}

void RuntimeInstance::setMonotonicTimeNs(uint64_t ns)
{
	auto execution = impl_->scheduler->lockExecution();
	impl_->clock.setMonotonicTimeNs(ns);
}

void RuntimeInstance::useHostWallClock()
{
	auto execution = impl_->scheduler->lockExecution();
	impl_->clock.useHostWallClock();
}

void RuntimeInstance::useHostMonotonicClock()
{
	auto execution = impl_->scheduler->lockExecution();
	impl_->clock.useHostMonotonicClock();
}

uint32_t RuntimeInstance::addJniListener(observer::EventBus::JniListener listener)
{
	return impl_->events->addJniListener(std::move(listener));
}

bool RuntimeInstance::removeJniListener(uint32_t id)
{
	return impl_->events->removeJniListener(id);
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
