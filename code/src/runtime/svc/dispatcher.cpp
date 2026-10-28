#include "runtime/svc/dispatcher.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/log/logger.hpp"
#include "runtime/observer/diagnostic/guest_stack.hpp"
#include "runtime/observer/event_bus.hpp"
#include "runtime/svc/handler.hpp"
#include "runtime/svc/syscall_names.hpp"
#include "runtime/svc/syscall_numbers.hpp"
#include "runtime/threads/runtime_thread.hpp"
#include "runtime/threads/thread_system.hpp"

namespace duckemu::runtime::svc
{
namespace
{

constexpr uint32_t kSvcInstructionMask = 0xFFE0001Fu;
constexpr uint32_t kSvcInstruction = 0xD4000001u;
constexpr uint32_t kHostStubBase = 0x8000u;
constexpr uint32_t kHostStubIndexMask = 0x7FFFu;
constexpr uint32_t kIntrSwi = 2;
constexpr size_t kSyscallArgCount = 6;
constexpr size_t kHostArgCount = 8;

constexpr int kArgRegs[kHostArgCount] = {
	UC_ARM64_REG_X0,
	UC_ARM64_REG_X1,
	UC_ARM64_REG_X2,
	UC_ARM64_REG_X3,
	UC_ARM64_REG_X4,
	UC_ARM64_REG_X5,
	UC_ARM64_REG_X6,
	UC_ARM64_REG_X7,
};

Result<uint64_t> readRegister(uc_engine *uc, int reg)
{
	uint64_t value = 0;
	const uc_err err = uc_reg_read(uc, reg, &value);
	if (err != UC_ERR_OK)
	{
		return Error{ ErrorCode::EmulationFault, uc_strerror(err) };
	}
	return value;
}

Result<void> writeRegister(uc_engine *uc, int reg, uint64_t value)
{
	const uc_err err = uc_reg_write(uc, reg, &value);
	if (err != UC_ERR_OK)
	{
		return Error{ ErrorCode::EmulationFault, uc_strerror(err) };
	}
	return {};
}

std::string hexString(uint64_t value)
{
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
	return buffer;
}

bool readArgRegisters(uc_engine *uc, size_t count, std::array<uint64_t, kHostArgCount> &values)
{
	for (size_t i = 0; i < count; ++i)
	{
		auto value = readRegister(uc, kArgRegs[i]);
		if (!value.ok())
		{
			return false;
		}
		values[i] = value.value();
	}
	return true;
}

} // namespace

SvcDispatcher::SvcDispatcher(RuntimeMemory &memory,
	ThunkTable &thunks,
	observer::EventBus &events,
	observer::TraceClock &clock,
	uint32_t pid,
	threads::RuntimeThread &thread,
	threads::ThreadSystem &threads)
	: memory_(memory)
	, thunks_(thunks)
	, events_(events)
	, clock_(clock)
	, pid_(pid)
	, thread_(thread)
	, threads_(threads)
{
}

void SvcDispatcher::resetRunState()
{
	stats_ = {};
}

void SvcDispatcher::onTrap(uc_engine *uc, uint32_t intno)
{
	if (intno != kIntrSwi)
	{
		DUCKEMU_ERROR("unexpected UC_HOOK_INTR intno=%u (only SVC is handled)", intno);
		uc_emu_stop(uc);
		return;
	}

	auto pc = readRegister(uc, UC_ARM64_REG_PC);
	if (!pc.ok() || pc.value() < 4)
	{
		DUCKEMU_ERROR("cannot read PC in the SVC hook");
		uc_emu_stop(uc);
		return;
	}

	const uint64_t svc_address = pc.value() - 4;
	auto instruction = memory_.readU32(GuestVA{ svc_address });
	if (!instruction.ok() || (instruction.value() & kSvcInstructionMask) != kSvcInstruction)
	{
		DUCKEMU_ERROR("UC_HOOK_INTR at 0x%llx but PC-4 is not an SVC",
			static_cast<unsigned long long>(pc.value()));
		uc_emu_stop(uc);
		return;
	}

	const uint32_t imm = (instruction.value() >> 5) & 0xFFFFu;

	observer::SvcEvent event;
	event.seq = clock_.next_seq++;
	event.tid = thread_.tid;
	event.svc_address = svc_address;

	int64_t result = 0;
	bool stop_after_publish = false;

	if (imm == 0)
	{
		event.kind = observer::SvcKind::Syscall;

		auto number = readRegister(uc, UC_ARM64_REG_X8);
		std::array<uint64_t, kHostArgCount> raw_args{};
		if (!number.ok() || !readArgRegisters(uc, kSyscallArgCount, raw_args))
		{
			uc_emu_stop(uc);
			return;
		}
		for (size_t i = 0; i < kSyscallArgCount; ++i)
		{
			event.args[i] = raw_args[i];
		}

		event.number = static_cast<uint32_t>(number.value());
		const std::string_view name = syscallName(event.number);
		event.name = name.empty() ? "sys#" + std::to_string(event.number) : std::string(name);
		result = dispatchSyscall(event.number, event.args, event.supported);
	}
	else if ((imm & kHostStubBase) != 0)
	{
		event.kind = observer::SvcKind::HostStub;
		event.number = imm & kHostStubIndexMask;

		const std::string_view name = thunks_.nameOf(event.number);
		event.name = name.empty() ? "host#" + std::to_string(event.number) : std::string(name);

		std::array<uint64_t, kHostArgCount> raw_args{};
		if (!readArgRegisters(uc, kHostArgCount, raw_args))
		{
			uc_emu_stop(uc);
			return;
		}
		for (size_t i = 0; i < kSyscallArgCount; ++i)
		{
			event.args[i] = raw_args[i];
		}
		result = invokeHostStub(event.number, raw_args, event.supported);
	}
	else
	{
		event.kind = observer::SvcKind::UnknownTrap;
		event.number = imm;
		event.name = "trap#" + hexString(imm);
		event.supported = false;
		result = kError(kEnosys);
		stop_after_publish = true;
	}

	// A blocking handler has parked the thread: the syscall is not done, so it
	// gets no x0 write and no event yet. The owner publishes on resume.
	if (thread_.state == threads::ThreadState::Blocked)
	{
		thread_.pending_event = std::move(event);
		uc_emu_stop(uc);
		return;
	}

	event.result = static_cast<uint64_t>(result);

	if (auto written = writeRegister(uc, UC_ARM64_REG_X0, event.result); !written.ok())
	{
		uc_emu_stop(uc);
		return;
	}

	if (!event.supported)
	{
		captureStack(uc, event);
		logUnsupported(event);
	}

	event.id = clock_.next_id++;
	events_.publishSvc(event);

	if (thread_.exit_requested || threads_.stopping() || stop_after_publish)
	{
		uc_emu_stop(uc);
	}
}

int64_t SvcDispatcher::dispatchSyscall(uint32_t number, const std::array<uint64_t, 6> &args, bool &supported)
{
	++stats_.svc_calls;
	stats_.last_syscall = number;

	SyscallContext ctx{ {}, memory_, pid_, thread_.tid, thread_, threads_ };
	for (size_t i = 0; i < kSyscallArgCount; ++i)
	{
		ctx.args[i] = args[i];
	}

	std::optional<int64_t> result = handleMemorySyscall(ctx, number);
	if (!result.has_value())
	{
		result = handleFsSyscall(ctx, number);
	}
	if (!result.has_value())
	{
		result = handleMiscSyscall(ctx, number);
	}
	if (!result.has_value())
	{
		result = handleThreadSyscall(ctx, number);
	}

	int64_t value = 0;
	if (result.has_value())
	{
		supported = true;
		value = result.value();
	}
	else
	{
		supported = false;
		++stats_.unknown;
		value = kError(kEnosys);
	}
	return value;
}

int64_t SvcDispatcher::invokeHostStub(uint32_t index, const std::array<uint64_t, 8> &args, bool &supported)
{
	HostCallArgs call;
	call.x = args;

	++stats_.host_calls;
	auto result = thunks_.invoke(index, call);
	if (!result.ok())
	{
		supported = false;
		return kError(kEnosys);
	}
	supported = true;
	return result.value();
}

void SvcDispatcher::captureStack(uc_engine *uc, observer::SvcEvent &event) const
{
	auto sp = readRegister(uc, UC_ARM64_REG_SP);
	auto fp = readRegister(uc, UC_ARM64_REG_X29);
	if (!sp.ok() || !fp.ok())
	{
		event.stack_stop = "cannot read sp/fp";
		return;
	}

	auto stack = observer::unwindGuestStack(memory_, event.svc_address, fp.value(), sp.value());
	event.stack = std::move(stack.frames);
	event.stack_stop = std::move(stack.stop);
}

void SvcDispatcher::logUnsupported(const observer::SvcEvent &event) const
{
	std::string message = "unsupported ";
	switch (event.kind)
	{
	case observer::SvcKind::Syscall:
		message += "syscall ";
		break;
	case observer::SvcKind::HostStub:
		message += "host stub ";
		break;
	case observer::SvcKind::UnknownTrap:
		message += "svc immediate ";
		break;
	}
	message += event.name;
	message += " at ";
	message += hexString(event.svc_address);
	if (!event.stack.empty())
	{
		message += "\n    guest stack:";
		for (const auto &frame : event.stack)
		{
			message += "\n    ";
			message += frame.name;
		}
		if (!event.stack_stop.empty())
		{
			message += "\n    (walk stopped: " + event.stack_stop + ")";
		}
	}
	DUCKEMU_WARN("%s", message.c_str());
}

} // namespace duckemu::runtime::svc
