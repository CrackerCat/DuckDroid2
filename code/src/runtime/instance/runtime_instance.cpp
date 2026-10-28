#include "runtime/instance/runtime_instance.hpp"

#include <utility>

#include "runtime/backend/unicorn/unicorn_backend.hpp"
#include "runtime/memory/address_space.hpp"
#include "runtime/observer/trace/svc_trace.hpp"
#include "runtime/threads/scheduler.hpp"
#include "runtime/threads/tls/tls_manager.hpp"

namespace duckemu::runtime
{
namespace
{

constexpr uint32_t kBranchToSelf = 0x14000000u; // b .

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
	std::unique_ptr<tls::TlsManager> tls;
	// Co-owned with the bus listener that writes to it: the listener captures
	// a shared_ptr, so the file outlives any Impl member regardless of
	// destruction order.
	std::shared_ptr<observer::SvcTraceFile> trace;

	GuestVA stack_base;
	uint64_t stack_top = 0;
	GuestVA return_trap;
	State state = State::Created;

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

	impl->tls = std::make_unique<tls::TlsManager>(impl->memory);
	auto tls_instance = impl->tls->create(config.tid, config.pid, impl->stack_top);
	if (!tls_instance.ok()) {
		return tls_instance.error();
	}

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

	impl->scheduler = std::make_unique<threads::ThreadScheduler>(
		impl->memory, *impl->backend, *impl->thunks, *impl->events, scheduler_config);
	if (auto started = impl->scheduler->start(); !started.ok()) {
		return started.error();
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

size_t RuntimeInstance::drainDiagnostics(std::vector<observer::DiagnosticEvent> &out)
{
	return impl_->scheduler->drainDiagnostics(out);
}

uint64_t RuntimeInstance::droppedDiagnostics() const
{
	return impl_->scheduler->droppedDiagnostics();
}

} // namespace duckemu::runtime
