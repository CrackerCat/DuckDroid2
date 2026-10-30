// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>

#include "worker/app/app_plugin.hpp"

namespace duckemu::user::szstone
{

// libszstone behind the worker's target contract: install the Java world, load
// the ROM libc and libszstone.so, then run SzSdk.lf on the profile blob.
//
// invoke() op "lf":
//   params["blob"]  optional hex override; default = the captured profile
//   params["z"]     optional "0,1,0" (the three call-site booleans)
//   result["body_hex"] = lf output (base64url-decoded /call body), hex encoded
//   result["body_len"] = decimal length
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

} // namespace duckemu::user::szstone
