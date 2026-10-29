#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/error/result.hpp"

namespace duckemu::runtime
{
class RuntimeInstance;
}

namespace duckemu::worker
{

// Everything a target needs to build its guest world. Assembled by the worker
// from configuration; the plugin never reads process-global paths.
struct AppContext
{
	std::string app;    // signer id, e.g. "libeleven"
	std::string library; // the target .so
	std::string rom_libc; // the ROM's libc.so
	std::vector<std::string> library_search_paths;
};

// One request as the plugin sees it. Deliberately not protocol::Request: the
// wire format may change without touching target code.
struct AppRequest
{
	std::string op;
	std::map<std::string, std::string> params;
};

struct AppResult
{
	std::map<std::string, std::string> values;
};

struct AppError
{
	std::string code = "APP_ERROR";
	std::string message;
};

// The only seam between the worker and a target project. Implementations live
// in code/user_code/<case>/; the worker never names a target.
class AppPlugin
{
    public:
	virtual ~AppPlugin() = default;

	// Runs once per instance process, after RuntimeInstance::create and before
	// the instance reports ready. Loads modules and installs the Java world.
	virtual bool init(runtime::RuntimeInstance &instance, std::string *error) = 0;

	// Runs once per request. Must leave the instance usable for the next call,
	// even when it returns false.
	virtual bool invoke(runtime::RuntimeInstance &instance,
		const AppRequest &request,
		AppResult *result,
		AppError *error) = 0;
};

} // namespace duckemu::worker

// Implemented by the target side (code/user_code/registry.cpp): app id ->
// plugin. NotFound when the running binary was not built with that project.
namespace duckemu::worker
{
Result<std::unique_ptr<AppPlugin> > make_app_plugin(const AppContext &context);
}
