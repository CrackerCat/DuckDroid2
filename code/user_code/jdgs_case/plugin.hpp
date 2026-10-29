// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>

#include "worker/app/app_plugin.hpp"

namespace duckemu::user::jdgs
{

// libjdg behind the worker's target contract: install the Java world, load
// the ROM libc and libjdg.so, run the two Bridge.main(103) setup calls, then
// map request params onto Bridge.main(101).
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

} // namespace duckemu::user::jdgs
