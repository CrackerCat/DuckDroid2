#include "runtime/instance/runtime_instance.hpp"

#include <utility>

#include "runtime/backend/unicorn/unicorn_backend.hpp"
#include "runtime/memory/address_space.hpp"
#include "runtime/observer/trace/svc_trace.hpp"
#include "runtime/svc/dispatcher.hpp"
#include "runtime/threads/tls/tls_manager.hpp"

namespace duckemu::runtime
{
namespace
{

constexpr int kArgRegs[8] = {
	UC_ARM64_REG_X0,
	UC_ARM64_REG_X1,
	UC_ARM64_REG_X2,
	UC_ARM64_REG_X3,
	UC_ARM64_REG_X4,
	UC_ARM64_REG_X5,
	UC_ARM64_REG_X6,
	UC_ARM64_REG_X7,
};

constexpr uint32_t kBranchToSelf = 0x14000000u; // b .

	Error
	emulationFault(std::string message)
{
	return Error{ ErrorCode::EmulationFault, std::move(message) };
}

} // namespace

struct RuntimeInstance::Impl
{
	enum class State
	{
		Created,
		Ready,
		Executing,
		Faulted
	};

	RuntimeConfig config;
	RuntimeMemory memory;
	std::unique_ptr<observer::EventBus> events;
	std::unique_ptr<UnicornBackend> backend;
	std::unique_ptr<ThunkTable> thunks;
	std::unique_ptr<tls::TlsManager> tls;
	std::unique_ptr<svc::SvcDispatcher> dispatcher;
	// Co-owned with the bus listener that writes to it: the listener captures
	// a shared_ptr, so the file outlives any Impl member regardless of
	// destruction order. A raw pointer (one owner + one observer) would make
	// correctness depend on "trace is declared last", which no compiler checks
	// and which M3 would make much harder to reason about.
	std::shared_ptr<observer::SvcTraceFile> trace;

	GuestVA stack_base;
	uint64_t stack_top = 0;
	GuestVA return_trap;
	State state = State::Created;
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

	impl->events = std::make_unique<observer::EventBus>(config.diagnostic_capacity);
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

	impl->tls = std::make_unique<tls::TlsManager>(impl->memory);
	auto tls_instance = impl->tls->create(config.tid, config.pid, impl->stack_top);
	if (!tls_instance.ok()) {
		return tls_instance.error();
	}

	impl->dispatcher = std::make_unique<svc::SvcDispatcher>(
		impl->memory, *impl->thunks, *impl->events, config.pid, config.tid);
	impl->backend->setSvcTrapHandler([dispatcher = impl->dispatcher.get()](uc_engine *uc, uint32_t intno) {
		dispatcher->onTrap(uc, intno);
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

	if (config.record_memory_access) {
		impl->backend->setDiagnosticSink(
			[bus = impl->events.get(), tid = config.tid](const observer::DiagnosticEvent &source) {
				observer::DiagnosticEvent event = source;
				event.tid = tid;
				(void)bus->diagnostics().tryPush(event);
			});
	}

	impl->state = Impl::State::Ready;
	return std::unique_ptr<RuntimeInstance>(new RuntimeInstance(std::move(impl)));
}

Result<GuestVA> RuntimeInstance::mapMemory(uint64_t size, uint32_t perms, std::string_view name)
{
	return impl_->memory.map(size, perms, name);
}

Result<void> RuntimeInstance::writeMemory(GuestVA address, const void *data, uint64_t size)
{
	return impl_->memory.write(address, data, size);
}

Result<void> RuntimeInstance::readMemory(GuestVA address, void *data, uint64_t size) const
{
	return impl_->memory.read(address, data, size);
}

Result<GuestVA> RuntimeInstance::registerHostFunction(std::string_view name, HostStub stub)
{
	return impl_->thunks->add(name, std::move(stub));
}

Result<RuntimeInstance::CallResult> RuntimeInstance::callFunction(GuestVA entry, const std::array<uint64_t, 8> &args)
{
	// Listeners run inside the Unicorn hook. Starting the engine from one
	// nests uc_emu_start on the same engine, which overwrites the outer PC/GPR
	// (M0 probe 6). Refuse instead of corrupting state silently.
	if (impl_->events->delivering()) {
		return Error{ ErrorCode::InvalidArgument,
			"callFunction from an event listener would re-enter the emulator" };
	}

	if (impl_->state != Impl::State::Ready) {
		return Error{ ErrorCode::InvalidArgument, "RuntimeInstance is not ready to execute" };
	}

	impl_->dispatcher->resetRunState();

	auto sp = impl_->backend->writeRegister(UC_ARM64_REG_SP, impl_->stack_top);
	if (!sp.ok()) {
		return sp.error();
	}
	auto lr = impl_->backend->writeRegister(UC_ARM64_REG_X30, impl_->return_trap.value());
	if (!lr.ok()) {
		return lr.error();
	}
	// Terminates the guest frame chain: the unwinder stops at fp == 0 instead
	// of reading whatever the previous run left in x29.
	auto fp = impl_->backend->writeRegister(UC_ARM64_REG_X29, 0);
	if (!fp.ok()) {
		return fp.error();
	}
	auto tp = impl_->backend->writeRegister(UC_ARM64_REG_TPIDR_EL0, impl_->tls->threadPointer().value());
	if (!tp.ok()) {
		return tp.error();
	}
	for (size_t i = 0; i < args.size(); ++i) {
		auto written = impl_->backend->writeRegister(kArgRegs[i], args[i]);
		if (!written.ok()) {
			return written.error();
		}
	}

	impl_->state = Impl::State::Executing;
	auto outcome = impl_->backend->run(entry, impl_->return_trap, impl_->config.default_timeout_us);
	impl_->state = Impl::State::Ready;

	if (!outcome.ok()) {
		impl_->state = Impl::State::Faulted;
		return outcome.error();
	}

	if (impl_->dispatcher->exitRequested()) {
		CallResult result;
		result.exit_code = impl_->dispatcher->exitCode();
		return result;
	}

	if (outcome.value().err != UC_ERR_OK) {
		impl_->state = Impl::State::Faulted;
		return emulationFault(std::string("uc_emu_start: ") + uc_strerror(outcome.value().err));
	}
	if (outcome.value().pc != impl_->return_trap.value()) {
		impl_->state = Impl::State::Faulted;
		return emulationFault("guest stopped outside the return trap");
	}

	auto return_value = impl_->backend->readRegister(UC_ARM64_REG_X0);
	if (!return_value.ok()) {
		impl_->state = Impl::State::Faulted;
		return return_value.error();
	}

	CallResult result;
	result.return_value = return_value.value();
	return result;
}

uint32_t RuntimeInstance::addSvcListener(observer::EventBus::SvcListener listener)
{
	return impl_->events->addSvcListener(std::move(listener));
}

uint32_t RuntimeInstance::addMemoryListener(observer::EventBus::MemoryListener listener)
{
	return impl_->events->addMemoryListener(std::move(listener));
}

bool RuntimeInstance::removeSvcListener(uint32_t id)
{
	return impl_->events->removeSvcListener(id);
}

bool RuntimeInstance::removeMemoryListener(uint32_t id)
{
	return impl_->events->removeMemoryListener(id);
}

size_t RuntimeInstance::drainDiagnostics(std::vector<observer::DiagnosticEvent> &out)
{
	size_t drained = 0;
	observer::DiagnosticEvent event;
	while (impl_->events->diagnostics().tryPop(event)) {
		out.push_back(event);
		++drained;
	}
	return drained;
}

uint64_t RuntimeInstance::droppedDiagnostics() const
{
	return impl_->events->diagnostics().dropped();
}

} // namespace duckemu::runtime
