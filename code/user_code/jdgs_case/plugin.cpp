#include "jdgs_case/plugin.hpp"

#include <array>
#include <string>
#include <utility>

#include "jdgs_case/jdgs_env.hpp"
#include "runtime/instance/runtime_instance.hpp"

namespace duckemu::user::jdgs
{
namespace
{

constexpr uint64_t kSelectorInit = 103;
constexpr uint64_t kSelectorSign = 101;

bool callMain(runtime::RuntimeInstance &instance,
	uint64_t selector,
	uint64_t argument,
	uint64_t *result,
	std::string *error)
{
	const std::array<uint64_t, 8> arguments = { selector, argument };
	auto called = instance.callRegisteredNative(kBridgeClass, "main", kMainSignature, arguments);
	if (!called.ok())
	{
		*error = called.error().message;
		return false;
	}
	*result = called.value();
	return true;
}

bool runInit(runtime::RuntimeInstance &instance,
	std::string_view phase,
	bool with_elapsed,
	std::string *error)
{
	auto arguments = buildInitArguments(phase, with_elapsed);
	if (!arguments.ok())
	{
		*error = "cannot build init(" + std::string(phase) + "): " + arguments.error().message;
		return false;
	}
	uint64_t result = 0;
	if (!callMain(instance, kSelectorInit, arguments.value(), &result, error))
	{
		return false;
	}
	if (result == 0)
	{
		return true;
	}
	auto object = instance.jniResolveHandle(result);
	if (!object.ok())
	{
		*error = object.error().message;
		return false;
	}
	auto element = readResultElement(object.value());
	if (!element.ok() || element.value() == 0)
	{
		return true;
	}
	auto descriptor = classNameOf(element.value());
	if (!descriptor.ok() || descriptor.value() != "java/lang/Integer")
	{
		return true;
	}
	auto status = readIntegerField(element.value());
	if (!status.ok() || status.value() == 0)
	{
		return true;
	}
	*error = "Bridge.main(103, \"" + std::string(phase) + "\") returned " +
		std::to_string(status.value());
	return false;
}

bool sign(runtime::RuntimeInstance &instance,
	const SignParams &params,
	std::string *signature,
	std::string *error)
{
	auto arguments = buildSignArguments(params);
	if (!arguments.ok())
	{
		*error = "cannot build the argument array: " + arguments.error().message;
		return false;
	}
	uint64_t result = 0;
	if (!callMain(instance, kSelectorSign, arguments.value(), &result, error))
	{
		return false;
	}
	if (result == 0)
	{
		*error = "Bridge.main(101) returned null";
		return false;
	}
	auto object = instance.jniResolveHandle(result);
	if (!object.ok())
	{
		*error = "cannot resolve the result: " + object.error().message;
		return false;
	}
	auto element = readResultElement(object.value());
	if (!element.ok())
	{
		*error = "cannot read the result array: " + element.error().message;
		return false;
	}
	if (element.value() == 0)
	{
		*error = "Bridge.main(101) returned a null element";
		return false;
	}
	auto descriptor = classNameOf(element.value());
	if (descriptor.ok() && descriptor.value() == "java/lang/Integer")
	{
		auto status = readIntegerField(element.value());
		*error = status.ok() ? "Bridge.main(101) returned errno " +
					   std::to_string(status.value())
				     : "Bridge.main(101) returned an Integer result";
		return false;
	}
	auto text = readResultString(object.value());
	if (!text.ok())
	{
		*error = "cannot read the result string: " + text.error().message;
		return false;
	}
	*signature = std::move(text.value());
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
	if (!runInit(instance, "0", false, error) || !runInit(instance, "1", true, error))
	{
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
	SignParams params = signParamsFromEnvironment();
	const auto override_param = [&request](const char *name, std::string *value) {
		const auto found = request.params.find(name);
		if (found != request.params.end())
		{
			*value = found->second;
		}
	};
	override_param("request", &params.request);
	override_param("device_payload", &params.device_payload);
	override_param("dfp_eid", &params.dfp_eid);
	override_param("pay_version", &params.pay_version);
	override_param("openudid", &params.openudid);
	override_param("sdk_config", &params.sdk_config);

	std::string signature;
	std::string sign_error;
	if (!sign(instance, params, &signature, &sign_error))
	{
		error->code = "APP_ERROR";
		error->message = std::move(sign_error);
		return false;
	}
	result->values["value"] = std::move(signature);
	return true;
}

} // namespace duckemu::user::jdgs
