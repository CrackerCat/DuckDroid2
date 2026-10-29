#pragma once

#include <string>

#include "worker/app/app_plugin.hpp"

namespace duckemu::user::libeleven
{

// libeleven behind the worker's target contract: install the Java world, load
// the ROM libc and the target .so, then map request params onto the registered
// native (data(I)Ljava/lang/String;).
class Plugin final : public worker::AppPlugin
{
    public:
	explicit Plugin(worker::AppContext context);

	bool init(runtime::RuntimeInstance &instance, std::string *error) override;
	bool invoke(runtime::RuntimeInstance &instance,
		const worker::AppRequest &request,
		worker::AppResult *result,
		worker::AppError *error) override;

    private:
	worker::AppContext context_;
};

} // namespace duckemu::user::libeleven
