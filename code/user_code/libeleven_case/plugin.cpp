#include "libeleven_case/plugin.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>

#include "libeleven_case/libeleven_env.hpp"
#include "runtime/instance/runtime_instance.hpp"

namespace duckemu::user::libeleven
{
namespace
{

constexpr const char *kClassName =
	"com/yuanrenxue/challenge/fragment/challenge/ChallengeElevenFragment";
constexpr const char *kMethodName = "data";
constexpr const char *kSignature = "(I)Ljava/lang/String;";

bool ParseInt(const std::string &text, int *out)
{
	errno = 0;
	char *end = nullptr;
	const long value = std::strtol(text.c_str(), &end, 10);
	if (errno != 0 || end == text.c_str() || *end != '\0')
	{
		return false;
	}
	*out = static_cast<int>(value);
	return true;
}

} // namespace

Plugin::Plugin(worker::AppContext context) : context_(std::move(context)) {}

bool Plugin::init(runtime::RuntimeInstance &instance, std::string *error)
{
	auto installed = instance.installJavaEnvironment(&installEnvironment);
	if (!installed.ok())
	{
		*error = "install Java environment failed: " + installed.error().message;
		return false;
	}
	auto libc = instance.loadLibrary(context_.rom_libc);
	if (!libc.ok())
	{
		*error = "load libc failed: " + libc.error().message;
		return false;
	}
	auto module = instance.loadLibrary(context_.library);
	if (!module.ok())
	{
		*error = "load library failed: " + module.error().message;
		return false;
	}
	return true;
}

bool Plugin::invoke(runtime::RuntimeInstance &instance,
	const worker::AppRequest &request,
	worker::AppResult *result,
	worker::AppError *error)
{
	if (request.op != "sign")
	{
		error->code = "UNKNOWN_OP";
		error->message = "unsupported op '" + request.op + "'";
		return false;
	}
	const auto found = request.params.find("value");
	if (found == request.params.end())
	{
		error->code = "BAD_REQUEST";
		error->message = "missing 'value' param";
		return false;
	}
	int value = 0;
	if (!ParseInt(found->second, &value))
	{
		error->code = "BAD_REQUEST";
		error->message = "'value' is not an integer";
		return false;
	}
	const std::array<uint64_t, 8> arguments = {
		static_cast<uint64_t>(static_cast<uint32_t>(value))
	};
	auto called = instance.callRegisteredNative(kClassName, kMethodName, kSignature, arguments);
	if (!called.ok())
	{
		error->code = "APP_ERROR";
		error->message = "data(" + found->second + ") failed: " + called.error().message;
		return false;
	}
	if (called.value() == 0)
	{
		error->code = "APP_ERROR";
		error->message = "data(" + found->second + ") returned null";
		return false;
	}
	auto text = instance.jniStringUtf8(called.value());
	if (!text.ok())
	{
		error->code = "APP_ERROR";
		error->message = "string decode failed: " + text.error().message;
		return false;
	}
	result->values["value"] = text.value();
	return true;
}

} // namespace duckemu::user::libeleven
