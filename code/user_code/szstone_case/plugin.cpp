// SPDX-License-Identifier: GPL-2.0-only
#include "szstone_case/plugin.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "runtime/instance/runtime_instance.hpp"
#include "szstone_case/szstone_codec.hpp"
#include "szstone_case/szstone_env.hpp"

namespace duckemu::user::szstone
{

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
		*error = "load libszstone failed: " + module.error().message;
		return false;
	}
	return true;
}

bool Plugin::invoke(runtime::RuntimeInstance &instance,
	const worker::AppRequest &request,
	worker::AppResult *result,
	worker::AppError *error)
{
	if (request.op != "lf")
	{
		error->code = "UNKNOWN_OP";
		error->message = "unsupported op '" + request.op + "'";
		return false;
	}

	const auto blob_param = request.params.find("blob");
	if (blob_param == request.params.end())
	{
		error->message = "params.blob (device profile, hex) is required";
		return false;
	}
	std::vector<uint8_t> blob;
	if (!decodeHex(blob_param->second, &blob))
	{
		error->message = "params.blob is not valid hex";
		return false;
	}

	// The real sk-registration call is lf(blob, true, true, true).
	std::array<uint64_t, 3> z = { 1, 1, 1 };
	const auto z_param = request.params.find("z");
	if (z_param != request.params.end() && !parseZ(z_param->second, &z))
	{
		error->message = "params.z must look like 0,1,0";
		return false;
	}

	const std::string_view blob_view = blob.empty()
		? std::string_view{}
		: std::string_view(reinterpret_cast<const char *>(blob.data()), blob.size());
	auto output = invokeLf(instance, blob_view, z);
	if (!output.ok())
	{
		error->message = "SzSdk.lf failed: " + output.error().message;
		return false;
	}
	result->values["body_hex"] = encodeHex(output.value());
	result->values["body_len"] = std::to_string(output.value().size());
	return true;
}

} // namespace duckemu::user::szstone
