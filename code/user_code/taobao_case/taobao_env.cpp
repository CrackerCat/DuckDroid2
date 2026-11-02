// SPDX-License-Identifier: GPL-2.0-only
#include "taobao_case/taobao_env.hpp"

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <utility>

#include "runtime/jni/envs/envs.hpp"
#include "runtime/jni/world/java_binder.hpp"
#include "runtime/jni/world/java_world.hpp"
#include "taobao_case/taobao_device.hpp"

namespace duckemu::user::taobao
{

const char *const kNativeClass =
	"com/taobao/wireless/security/adapter/JNICLibrary";
const char *const kNativeMethod = "doCommandNative";
const char *const kNativeSignature = "(I[Ljava/lang/Object;)Ljava/lang/Object;";

const char *const kPackage = "com.taobao.taobao";
const char *const kSdkVersion = "6.8.260901";

namespace
{

using runtime::jni::InvokeContext;
using runtime::jni::JavaBinder;
using runtime::jni::JavaWorld;
using runtime::jni::ShimHandler;

// The old DuckDroid case kept this pointer for the same reason: target shims
// need to build strings/objects after installEnvironment has returned.
JavaWorld *g_world = nullptr;

// Class and member ids the host side needs after install. They are filled once
// per instance process and never change.
uint64_t g_context_class = 0;
uint64_t g_app_info_class = 0;
uint64_t g_app_info_package_name = 0;
uint64_t g_app_info_source_dir = 0;
uint64_t g_app_info_data_dir = 0;
uint64_t g_app_info_native_dir = 0;
uint64_t g_app_info_uid = 0;
uint64_t g_app_info_flags = 0;
uint64_t g_main_plugin_class = 0;
uint64_t g_package_manager_class = 0;
uint64_t g_package_info_class = 0;
uint64_t g_package_info_version = 0;
uint64_t g_package_info_package = 0;
uint64_t g_package_info_application_info = 0;
uint64_t g_integer_class = 0;
uint64_t g_integer_value_field = 0;
uint64_t g_boolean_class = 0;
uint64_t g_boolean_value_field = 0;
uint64_t g_loader_object = 0;

// Reference-device paths (logs/hook_a.log).
constexpr const char *kApkPath =
	"/data/app/~~ZkuUNV2frSgeDqVY6u13sw==/"
	"com.taobao.taobao-ooDG-cQxwXBd0pNhPDtRMA==/base.apk";
constexpr const char *kNativeLibraryDir =
	"/data/app/~~ZkuUNV2frSgeDqVY6u13sw==/"
	"com.taobao.taobao-ooDG-cQxwXBd0pNhPDtRMA==/lib/arm64";
constexpr const char *kFilesDir = "/data/user/0/com.taobao.taobao/files";
constexpr const char *kDataDir = "/data/user/0/com.taobao.taobao";
constexpr const char *kExternalFilesDir =
	"/storage/emulated/0/Android/data/com.taobao.taobao/files";

// Values captured from the reference device (logs/hook_a.log), 6.8.260901.
constexpr const char *kVersionName = "10.68.0";
constexpr const char *kUtdid = "arvEwsCpmYgDAPXgNx27U05R";

ShimHandler returnNull()
{
	return [](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; };
}

ShimHandler returnInt(uint64_t value)
{
	return [value](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return value; };
}

ShimHandler returnVoid()
{
	return [](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; };
}

ShimHandler returnEmptyString()
{
	return [](JavaWorld &world, InvokeContext &) -> Result<uint64_t> {
		return world.newString("");
	};
}

// Registers one method on an already-declared class. Keeps the declaration
// table in this file readable; the first failure is returned.
Result<void> declare(JavaWorld &world,
	uint64_t klass,
	const char *name,
	const char *signature,
	bool is_static,
	ShimHandler shim)
{
	auto declared = world.declareMethod(klass, name, signature, is_static, std::move(shim));
	if (!declared.ok())
	{
		return declared.error();
	}
	return {};
}

// DataReport / UserTrack bridges: the SDK asks whether the host has ACCS,
// Orange or windvane before it starts collecting. The reference device had
// none of them wired for the emulated process, so "not available" is the
// benign answer and keeps the SDK on its local path.
Result<void> installDataReportBridges(JavaWorld &world, uint64_t klass)
{
	const auto result = declare(world, klass, "accsAvaiableBridge", "()I", true,
		returnInt(0));
	if (!result.ok())
	{
		return result;
	}
	const auto orange = declare(world, klass, "orangeAvailableBridge", "()I", true,
		returnInt(0));
	if (!orange.ok())
	{
		return orange;
	}
	const auto accs_listener =
		declare(world, klass, "registerAccsListnerBridge", "()I", true, returnInt(0));
	if (!accs_listener.ok())
	{
		return accs_listener;
	}
	const auto orange_listener =
		declare(world, klass, "registerOrangeListenerBridge", "()I", true, returnInt(0));
	if (!orange_listener.ok())
	{
		return orange_listener;
	}
	const auto windvane =
		declare(world, klass, "registerWindVaneListenerBridge", "()I", true, returnInt(0));
	if (!windvane.ok())
	{
		return windvane;
	}
	const auto send =
		declare(world, klass, "sendReportBridge",
			"(Ljava/lang/String;Ljava/lang/String;[B)Ljava/lang/String;", true,
			returnNull());
	if (!send.ok())
	{
		return send;
	}
	if (auto https = declare(world, klass, "sendReportBridgeHttps",
		    "(Ljava/lang/String;Ljava/util/Map;[B)Ljava/lang/String;", true, returnNull());
		!https.ok())
	{
		return https;
	}
	if (auto https_bytes = declare(world, klass, "sendReportBridgeHttps",
		    "(Ljava/lang/String;[B)Ljava/lang/String;", true, returnNull());
		!https_bytes.ok())
	{
		return https_bytes;
	}
	if (auto mtop = declare(world, klass, "mtopAvaiableBridge", "()I", true, returnInt(0));
		!mtop.ok())
	{
		return mtop;
	}
	return declare(world, klass, "sendReportBridge",
		"(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;[B)Ljava/lang/String;", true,
		returnNull());
}

// SharedPreferences adapter: nothing has been persisted for this emulated
// install, and writes are accepted. The SDK treats an empty read as "no
// cached state" and proceeds.
Result<void> installSpUtility(JavaWorld &world, uint64_t klass)
{
	const auto read = declare(world, klass, "read",
		"(Ljava/lang/String;)Ljava/lang/String;", true, returnNull());
	if (!read.ok())
	{
		return read;
	}
	const auto write = declare(world, klass, "write",
		"(Ljava/lang/String;Ljava/lang/String;)V", true, returnVoid());
	if (!write.ok())
	{
		return write;
	}
	const auto read_ss = declare(world, klass, "readSS",
		"(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;", true,
		returnNull());
	if (!read_ss.ok())
	{
		return read_ss;
	}
	const auto write_ss = declare(world, klass, "writeSS",
		"(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Z", true,
		returnInt(1));
	if (!write_ss.ok())
	{
		return write_ss;
	}
	const auto unified = declare(world, klass, "readFromSPUnified",
		"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", true,
		returnNull());
	if (!unified.ok())
	{
		return unified;
	}
	const auto remove_unified = declare(world, klass, "removeFromSPUnified",
		"(Ljava/lang/String;Ljava/lang/String;Z)Z", true, returnInt(1));
	if (!remove_unified.ok())
	{
		return remove_unified;
	}
	return declare(world, klass, "saveToFileUnifiedForNative",
		"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)I", true, returnInt(0));
}

// The app-side object graph. Context.getApplicationInfo and
// PackageManager.getPackageInfo feed the SDK's package/signature checks; the
// values are the reference-device capture, not guesses from decompiled Java.
Result<void> writeStringField(JavaWorld &world,
	uint64_t object,
	uint64_t field,
	const char *value)
{
	auto text = world.newString(value);
	if (!text.ok())
	{
		return text.error();
	}
	return world.writeInstanceField(object, field, text.value());
}

Result<uint64_t> makeApplicationInfo(JavaWorld &world)
{
	auto object = world.allocateObject(g_app_info_class);
	if (!object.ok())
	{
		return object.error();
	}
	const struct
	{
		uint64_t field;
		const char *value;
	} strings[] = {
		{ g_app_info_package_name, kPackage },
		{ g_app_info_source_dir, kApkPath },
		{ g_app_info_data_dir, kDataDir },
		{ g_app_info_native_dir, kNativeLibraryDir },
	};
	for (const auto &entry : strings)
	{
		auto written = writeStringField(world, object.value(), entry.field, entry.value);
		if (!written.ok())
		{
			return written.error();
		}
	}
	if (auto uid = world.writeInstanceField(object.value(), g_app_info_uid, 10136); !uid.ok())
	{
		return uid.error();
	}
	if (auto flags = world.writeInstanceField(object.value(), g_app_info_flags, 0);
		!flags.ok())
	{
		return flags.error();
	}
	return object.value();
}

Result<uint64_t> makePackageInfo(JavaWorld &world)
{
	auto object = world.allocateObject(g_package_info_class);
	if (!object.ok())
	{
		return object.error();
	}
	auto version = writeStringField(world, object.value(), g_package_info_version, kVersionName);
	if (!version.ok())
	{
		return version.error();
	}
	auto package = writeStringField(world, object.value(), g_package_info_package, kPackage);
	if (!package.ok())
	{
		return package.error();
	}
	// Device collectors reach the package's ApplicationInfo through
	// PackageInfo.applicationInfo; fill the same object graph Context hands
	// out instead of leaving the field null.
	auto application = makeApplicationInfo(world);
	if (!application.ok())
	{
		return application.error();
	}
	auto written = world.writeInstanceField(object.value(),
		g_package_info_application_info, application.value());
	if (!written.ok())
	{
		return written.error();
	}
	return object.value();
}

} // namespace

Result<void> installEnvironment(JavaWorld &world)
{
	JavaBinder binder(world);

	// JNI_OnLoad resolves this class and RegisterNatives binds doCommandNative.
	uint64_t native_class = 0;
	binder.declareClass(kNativeClass, &native_class)
		.declareMethod(native_class, kNativeMethod, kNativeSignature, /*is_static=*/true, {});

	// Classes JNI_OnLoad resolves before it returns JNI_VERSION_1_6. All of
	// them were observed in the load probe; the bodies match the old case.
	uint64_t sec_exception = 0;
	uint64_t http_util = 0;
	uint64_t context = 0;
	uint64_t sp_utility = 0;
	uint64_t apm = 0;
	uint64_t device_info = 0;
	uint64_t data_report = 0;
	uint64_t mini_data_report = 0;
	uint64_t ec_misc = 0;
	uint64_t main_plugin = 0;
	uint64_t proxy_util = 0;
	uint64_t activity = 0;
	uint64_t window = 0;
	uint64_t system_clock = 0;
	uint64_t user_track = 0;
	uint64_t app_info = 0;
	uint64_t package_manager = 0;
	uint64_t package_info = 0;
	uint64_t callback_helper = 0;
	uint64_t one_main = 0;
	uint64_t umid_adapter = 0;
	uint64_t crash_bridge = 0;
	uint64_t breakpad = 0;
	uint64_t middle_tier = 0;
	uint64_t security_body = 0;
	uint64_t invocation_handler = 0;
	uint64_t sensor_manager = 0;
	uint64_t motion_event = 0;
	uint64_t window_manager = 0;
	uint64_t one_device_info = 0;
	uint64_t one_connection = 0;
	uint64_t security_body_adapter = 0;
	uint64_t mtop_builder = 0;
	uint64_t taobao_umid = 0;
	binder
		.declareClass("com/alibaba/wireless/security/open/SecException", &sec_exception)
		.declareClass("com/taobao/wireless/security/adapter/common/HttpUtil", &http_util)
		.declareClass("android/content/Context", &context)
		.declareClass("com/taobao/wireless/security/adapter/common/SPUtility2", &sp_utility)
		.declareClass("com/alibaba/wireless/security/framework/ApmMonitorAdapter", &apm)
		.declareClass("com/taobao/wireless/security/adapter/datacollection/DeviceInfoCapturer",
			&device_info)
		.declareClass("com/taobao/wireless/security/adapter/datareport/DataReportJniBridge",
			&data_report)
		.declareClass("com/alibaba/minilibc/android/DataReportJniBridge", &mini_data_report)
		.declareClass("com/alibaba/wireless/security/open/edgecomputing/ECMiscInfo", &ec_misc)
		.declareClass("com/alibaba/wireless/security/mainplugin/SecurityGuardMainPlugin",
			&main_plugin)
		.declareClass("com/alibaba/wireless/security/open/edgecomputing/ProxyUtil", &proxy_util)
		.declareClass("android/app/Activity", &activity)
		.declareClass("android/view/Window", &window)
		.declareClass("android/os/SystemClock", &system_clock)
		.declareClass("com/alibaba/wireless/security/framework/utils/UserTrackMethodJniBridge",
			&user_track)
		.declareClass("android/content/pm/ApplicationInfo", &app_info)
		.declareClass("android/content/pm/PackageManager", &package_manager)
		.declareClass("android/content/pm/PackageInfo", &package_info)
		.declareClass("com/taobao/dp/util/CallbackHelper", &callback_helper)
		.declareClass("com/alibaba/one/android/sdk/OneMain", &one_main)
		.declareClass("com/alibaba/wireless/security/adapter/umid/UmidAdapter", &umid_adapter)
		.declareClass("com/uc2/crashsdk/JNIBridge", &crash_bridge)
		.declareClass("com/taobao/android/tbreakpad/TBreakpadNativeBridge", &breakpad)
		.declareClass(
			"com/alibaba/wireless/security/middletierplugin/SecurityGuardMiddleTierPlugin",
			&middle_tier)
		.declareClass(
			"com/alibaba/wireless/security/securitybody/SecurityGuardSecurityBodyPlugin",
			&security_body)
		.declareClass(
			"com/alibaba/wireless/security/securitybody/InvocationHandlerAdapter",
			&invocation_handler)
		.declareClass("android/hardware/SensorManager", &sensor_manager)
		.declareClass("android/view/MotionEvent", &motion_event)
		.declareClass("android/view/WindowManager", &window_manager)
		.declareClass("com/alibaba/one/android/inner/DeviceInfoCapturer", &one_device_info)
		.declareClass("com/alibaba/one/android/inner/OneStubCenter$OneServiceConnection",
			&one_connection)
		.declareClass("com/alibaba/wireless/security/securitybody/SecurityBodyAdapter",
			&security_body_adapter)
		.declareClass("mtopsdk/mtop/intf/MtopBuilder", &mtop_builder)
		.declareClass("com/taobao/wireless/security/adapter/umid/UmidAdapter", &taobao_umid);

	// SecException: the SDK reports every init failure by throwing it. Store
	// the code/message so a host runner can print the cause, not a bare throw.
	uint64_t sec_message_field = 0;
	uint64_t sec_code_field = 0;
	binder.declareField(sec_exception, "message", "Ljava/lang/String;", false,
			&sec_message_field)
		.declareField(sec_exception, "code", "I", false, &sec_code_field);
	const auto sec_ctor = declare(world, sec_exception, "<init>",
		"(Ljava/lang/String;I)V", false,
		[sec_message_field, sec_code_field](JavaWorld &java, InvokeContext &call)
			-> Result<uint64_t> {
			if (call.args.size() != 2)
			{
				return Error{ ErrorCode::InvalidArgument,
					"SecException.<init> expects (String, int)" };
			}
			auto message = java.writeInstanceField(
				call.receiver, sec_message_field, call.args[0]);
			if (!message.ok())
			{
				return message.error();
			}
			auto code = java.writeInstanceField(call.receiver, sec_code_field, call.args[1]);
			if (!code.ok())
			{
				return code.error();
			}
			return uint64_t{ 0 };
		});
	if (!sec_ctor.ok())
	{
		return sec_ctor;
	}

	// HttpUtil: fail the bridge instead of pretending the server was reached.
	// The SDK falls back to its local/cached path, same as the old case.
	const struct
	{
		const char *name;
		const char *signature;
	} http_methods[] = {
		{ "sendSyncHttpGetRequestBridge", "(Ljava/lang/String;)Ljava/lang/String;" },
		{ "sendSyncHttpPostRequestBridge",
			"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;II)Ljava/lang/String;" },
		{ "downloadFileBridge",
			"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;" },
	};
	for (const auto &method : http_methods)
	{
		auto declared = declare(world, http_util, method.name, method.signature, true,
			returnNull());
		if (!declared.ok())
		{
			return declared;
		}
	}

	// Context permissions. The emulated process holds none, which is what a
	// real Taobao process without the runtime grants also reports.
	const auto self_permission = declare(world, context, "checkSelfPermission",
		"(Ljava/lang/String;)I", false, returnInt(0));
	if (!self_permission.ok())
	{
		return self_permission;
	}
	const auto calling_permission = declare(world, context,
		"checkCallingOrSelfPermission", "(Ljava/lang/String;)I", false, returnInt(0));
	if (!calling_permission.ok())
	{
		return calling_permission;
	}
	// The report thread asks for a ContentResolver to read the device's
	// advertising id. There is no system content provider here; null is the
	// documented "no resolver" answer and the SDK skips that factor.
	const auto resolver = declare(world, context, "getContentResolver",
		"()Landroid/content/ContentResolver;", false, returnNull());
	if (!resolver.ok())
	{
		return resolver;
	}

	const auto sp = installSpUtility(world, sp_utility);
	if (!sp.ok())
	{
		return sp;
	}

	// APM / UserTrack: no full-track recording; the app is foreground.
	const auto full_track = declare(world, apm, "isEnableFullTrackRecord", "()Z", true,
		returnInt(0));
	if (!full_track.ok())
	{
		return full_track;
	}
	const auto foreground = declare(world, apm, "isForeground", "()Z", true, returnInt(1));
	if (!foreground.ok())
	{
		return foreground;
	}
	const auto stage_start = declare(world, apm, "stageStart",
		"(Ljava/lang/String;Ljava/lang/String;)V", true, returnVoid());
	if (!stage_start.ok())
	{
		return stage_start;
	}
	// UserTrack is available (1) so the SDK keeps its track record; the host
	// has no stack to report, and addUtRecord is accepted.
	const auto ut_available = declare(world, user_track, "utAvaiable", "()I", true,
		returnInt(1));
	if (!ut_available.ok())
	{
		return ut_available;
	}
	const auto add_record = declare(world, user_track, "addUtRecord",
		"(Ljava/lang/String;IILjava/lang/String;JLjava/lang/String;Ljava/lang/String;"
		"Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",
		true, returnInt(0));
	if (!add_record.ok())
	{
		return add_record;
	}
	const auto ut_stack = declare(world, user_track, "getStackTrace",
		"(II)Ljava/lang/String;", true, returnEmptyString());
	if (!ut_stack.ok())
	{
		return ut_stack;
	}

	// DeviceInfoCapturer.doCommandForString: codes taken from the plugin
	// sources (0 wifi feature, 122 packageName, 123 versionName,
	// 135 UTDevice.getUtdid).
	const auto capture = declare(world, device_info, "doCommandForString",
		"(I)Ljava/lang/String;", true,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.empty())
			{
				return Error{ ErrorCode::InvalidArgument,
					"DeviceInfoCapturer expects one int" };
			}
			const int32_t code = static_cast<int32_t>(static_cast<uint32_t>(call.args[0]));
			const char *value = nullptr;
			switch (code)
			{
			case 0:
				value = "1";
				break;
			case 122:
				value = kPackage;
				break;
			case 123:
				value = kVersionName;
				break;
			case 135:
				value = kUtdid;
				break;
			default:
				break;
			}
			if (value == nullptr)
			{
				return uint64_t{ 0 };
			}
			return java.newString(value);
		});
	if (!capture.ok())
	{
		return capture;
	}

	const auto report = installDataReportBridges(world, data_report);
	if (!report.ok())
	{
		return report;
	}
	// The SDK's background report thread resolves the minilibc copy of the
	// same bridge. On a device both classes exist; answering both keeps the
	// thread from retrying a missing class forever.
	const auto mini_report = installDataReportBridges(world, mini_data_report);
	if (!mini_report.ok())
	{
		return mini_report;
	}

	// Edge-computing plugin callbacks: not wired in this process.
	const auto lifecycle = declare(world, ec_misc, "registerAppLifeCyCleCallBack", "()V",
		true, returnVoid());
	if (!lifecycle.ok())
	{
		return lifecycle;
	}
	const auto handler = declare(world, proxy_util, "getHandler",
		"()Landroid/os/Handler;", true, returnNull());
	if (!handler.ok())
	{
		return handler;
	}
	const auto proxy_instance = declare(world, proxy_util, "getProxyInstance",
		"(Ljava/lang/String;Ljava/lang/Object;I)Ljava/lang/Object;", true, returnNull());
	if (!proxy_instance.ok())
	{
		return proxy_instance;
	}

	// Activity / Window: JNI_OnLoad walks the activity chain to register a
	// lifecycle callback. This process has no Activity, so the chain ends at
	// the first getWindow().
	const auto get_window = declare(world, activity, "getWindow",
		"()Landroid/view/Window;", false, returnNull());
	if (!get_window.ok())
	{
		return get_window;
	}
	const auto get_callback = declare(world, window, "getCallback",
		"()Landroid/view/Window$Callback;", false, returnNull());
	if (!get_callback.ok())
	{
		return get_callback;
	}
	const auto set_callback = declare(world, window, "setCallback",
		"(Landroid/view/Window$Callback;)V", false, returnVoid());
	if (!set_callback.ok())
	{
		return set_callback;
	}
	// SystemClock.elapsedRealtime is a static platform method; the reference
	// case answered 0 (the SDK only uses it as a duration baseline).
	const auto elapsed = declare(world, system_clock, "elapsedRealtime", "()J", true,
		returnInt(0));
	if (!elapsed.ok())
	{
		return elapsed;
	}

	// The main plugin hands out the loader it was loaded with. The engine
	// models managed classes itself, so one stable ClassLoader object is
	// enough; a device returns the same PathClassLoader on every call.
	const uint64_t loader_class = world.findClass("java/lang/ClassLoader");
	if (loader_class == 0)
	{
		return Error{ ErrorCode::NotFound, "java/lang/ClassLoader is not installed" };
	}
	auto loader = world.allocateObject(loader_class);
	if (!loader.ok())
	{
		return loader.error();
	}
	const uint64_t loader_object = loader.value();
	const auto plugin_ctor = declare(world, main_plugin, "<init>", "()V", false, returnVoid());
	if (!plugin_ctor.ok())
	{
		return plugin_ctor;
	}
	const auto fast_path = declare(world, main_plugin, "mainPluginLoadFastPathNotify", "()V",
		false, returnVoid());
	if (!fast_path.ok())
	{
		return fast_path;
	}
	// The plugin's load-state flag. The reference device starts at 0 and the
	// native only writes it after a successful load path.
	auto load_version = world.declareField(main_plugin, "sLoadVersion", "I", true);
	if (!load_version.ok())
	{
		return load_version.error();
	}
	const auto plugin_loader = declare(world, main_plugin, "getMainPluginClassLoader",
		"()Ljava/lang/ClassLoader;", true,
		[loader_object](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
			return loader_object;
		});
	if (!plugin_loader.ok())
	{
		return plugin_loader;
	}
	// Device-info probes walk the app's class loader. Same stable object.
	const auto context_loader = declare(world, context, "getClassLoader",
		"()Ljava/lang/ClassLoader;", false,
		[loader_object](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
			return loader_object;
		});
	if (!context_loader.ok())
	{
		return context_loader;
	}
	// The other plugin façades hand out the same device-wide loader chain.
	for (const auto &entry : { std::make_pair(middle_tier, "getPluginClassLoader"),
		     std::make_pair(security_body, "getPluginClassLoader"),
		     std::make_pair(invocation_handler, "getClassLoader") })
	{
		const auto declared = declare(world, entry.first, entry.second,
			"()Ljava/lang/ClassLoader;", true,
			[loader_object](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
				return loader_object;
			});
		if (!declared.ok())
		{
			return declared;
		}
	}

	// Context: the object graph the SDK walks during 10101.
	g_context_class = context;
	auto context_ctor = declare(world, context, "<init>", "()V", false, returnVoid());
	if (!context_ctor.ok())
	{
		return context_ctor;
	}
	const auto package_name = declare(world, context, "getPackageName",
		"()Ljava/lang/String;", false,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return java.newString(kPackage);
		});
	if (!package_name.ok())
	{
		return package_name;
	}
	const auto application_context = declare(world, context, "getApplicationContext",
		"()Landroid/content/Context;", false,
		[](JavaWorld &, InvokeContext &call) -> Result<uint64_t> {
			return call.receiver;
		});
	if (!application_context.ok())
	{
		return application_context;
	}
	const auto code_path = declare(world, context, "getPackageCodePath",
		"()Ljava/lang/String;", false,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return java.newString(kApkPath);
		});
	if (!code_path.ok())
	{
		return code_path;
	}
	const auto files_dir = declare(world, context, "getFilesDir", "()Ljava/io/File;", false,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return runtime::jni::envs::newFile(java, kFilesDir);
		});
	if (!files_dir.ok())
	{
		return files_dir;
	}
	const auto data_dir = declare(world, context, "getDataDir", "()Ljava/io/File;", false,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return runtime::jni::envs::newFile(java, kDataDir);
		});
	if (!data_dir.ok())
	{
		return data_dir;
	}
	const auto external_files = declare(world, context, "getExternalFilesDir",
		"(Ljava/lang/String;)Ljava/io/File;", false,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return runtime::jni::envs::newFile(java, kExternalFilesDir);
		});
	if (!external_files.ok())
	{
		return external_files;
	}
	const auto op_package = declare(world, context, "getOpPackageName",
		"()Ljava/lang/String;", false,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return java.newString(kPackage);
		});
	if (!op_package.ok())
	{
		return op_package;
	}
	// The device-info collector asks for every system service by name. Print
	// each distinct name once so the pack can answer the ones that matter;
	// null is the "service unavailable" answer the SDK already handles.
	const auto system_service = declare(world, context, "getSystemService",
		"(Ljava/lang/String;)Ljava/lang/Object;", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.empty() || call.args[0] == 0)
			{
				return uint64_t{ 0 };
			}
			auto name = java.stringUtf8(call.args[0]);
			if (name.ok())
			{
				static std::set<std::string> seen;
				if (seen.insert(name.value()).second)
				{
					std::fprintf(stderr, "getSystemService(%s) -> null\n",
						name.value().c_str());
				}
			}
			return uint64_t{ 0 };
		});
	if (!system_service.ok())
	{
		return system_service;
	}

	// ApplicationInfo: fields the native reads directly.
	auto app_ctor = world.findMethod(app_info, "<init>", "()V", false);
	if (app_ctor == 0)
	{
		auto declared = declare(world, app_info, "<init>", "()V", false, returnVoid());
		if (!declared.ok())
		{
			return declared;
		}
	}
	struct FieldSpec
	{
		const char *name;
		const char *signature;
		uint64_t *slot;
	};
	const FieldSpec app_fields[] = {
		{ "packageName", "Ljava/lang/String;", &g_app_info_package_name },
		{ "sourceDir", "Ljava/lang/String;", &g_app_info_source_dir },
		{ "dataDir", "Ljava/lang/String;", &g_app_info_data_dir },
		{ "nativeLibraryDir", "Ljava/lang/String;", &g_app_info_native_dir },
		{ "uid", "I", &g_app_info_uid },
		{ "flags", "I", &g_app_info_flags },
	};
	for (const FieldSpec &spec : app_fields)
	{
		auto field = world.declareField(app_info, spec.name, spec.signature, false);
		if (!field.ok())
		{
			return field.error();
		}
		*spec.slot = field.value();
	}
	g_app_info_class = app_info;
	const auto get_app_info = declare(world, context, "getApplicationInfo",
		"()Landroid/content/pm/ApplicationInfo;", false,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return makeApplicationInfo(java);
		});
	if (!get_app_info.ok())
	{
		return get_app_info;
	}

	// PackageManager / PackageInfo: the SDK asks the Java side for installed
	// package metadata before it considers parsing base.apk itself.
	auto package_ctor = declare(world, package_manager, "<init>", "()V", false,
		returnVoid());
	if (!package_ctor.ok())
	{
		return package_ctor;
	}
	auto info_ctor = declare(world, package_info, "<init>", "()V", false, returnVoid());
	if (!info_ctor.ok())
	{
		return info_ctor;
	}
	auto version_field = world.declareField(package_info, "versionName",
		"Ljava/lang/String;", false);
	if (!version_field.ok())
	{
		return version_field.error();
	}
	auto package_field = world.declareField(package_info, "packageName",
		"Ljava/lang/String;", false);
	if (!package_field.ok())
	{
		return package_field.error();
	}
	g_package_info_version = version_field.value();
	g_package_info_package = package_field.value();
	auto application_field = world.declareField(package_info, "applicationInfo",
		"Landroid/content/pm/ApplicationInfo;", false);
	if (!application_field.ok())
	{
		return application_field.error();
	}
	g_package_info_application_info = application_field.value();
	for (const char *name : { "getPackageInfo", "getPackageArchiveInfo" })
	{
		const auto declared = declare(world, package_manager, name,
			"(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", false,
			[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
				return makePackageInfo(java);
			});
		if (!declared.ok())
		{
			return declared;
		}
	}
	const auto get_package_manager = declare(world, context, "getPackageManager",
		"()Landroid/content/pm/PackageManager;", false,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return java.allocateObject(g_package_manager_class);
		});
	if (!get_package_manager.ok())
	{
		return get_package_manager;
	}
	g_package_manager_class = package_manager;
	g_package_info_class = package_info;
	g_main_plugin_class = main_plugin;
	g_loader_object = loader_object;

	// CallbackHelper: a device-fingerprint singleton the native registers
	// onUpdated with. One stable object is enough.
	auto callback_ctor = declare(world, callback_helper, "<init>", "()V", false,
		returnVoid());
	if (!callback_ctor.ok())
	{
		return callback_ctor;
	}
	auto callback = world.allocateObject(callback_helper);
	if (!callback.ok())
	{
		return callback.error();
	}
	const uint64_t callback_object = callback.value();
	const auto callback_instance = declare(world, callback_helper, "getInstance",
		"()Lcom/taobao/dp/util/CallbackHelper;", true,
		[callback_object](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
			return callback_object;
		});
	if (!callback_instance.ok())
	{
		return callback_instance;
	}
	const auto on_updated = declare(world, callback_helper, "onUpdated",
		"(IILjava/lang/String;)V", false, returnVoid());
	if (!on_updated.ok())
	{
		return on_updated;
	}

	// OneMain / UmidAdapter: optional host integrations, absent here.
	const auto one_initialize = declare(world, one_main, "initialize",
		"(Landroid/content/Context;)V", true, returnVoid());
	if (!one_initialize.ok())
	{
		return one_initialize;
	}
	// RegisterNatives binds this entry; it must exist before the module loads.
	const auto one_init_native = declare(world, one_main, "initNative",
		"(Landroid/content/Context;)V", true, {});
	if (!one_init_native.ok())
	{
		return one_init_native;
	}
	const auto one_play = declare(world, one_main, "play",
		"(IIILjava/lang/Object;)Ljava/lang/Object;", true, returnNull());
	if (!one_play.ok())
	{
		return one_play;
	}
	const auto umid_init = declare(world, umid_adapter, "umidInitAdapter", "(I)I", true,
		returnInt(1));
	if (!umid_init.ok())
	{
		return umid_init;
	}
	// The SDK also asks the taobao-package spelling of the same adapter.
	const auto taobao_umid_init = declare(world, taobao_umid, "umidInitAdapter", "(I)I",
		true, returnInt(1));
	if (!taobao_umid_init.ok())
	{
		return taobao_umid_init;
	}
	// SecurityBodyAdapter.doAdapter: 6 battery level, 8 charging, 10 USB.
	// The reference case answered the same values unidbg uses.
	const auto body_adapter = declare(world, security_body_adapter, "doAdapter",
		"(I)Ljava/lang/String;", true,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.empty())
			{
				return Error{ ErrorCode::InvalidArgument,
					"SecurityBodyAdapter.doAdapter expects one int" };
			}
			const char *value = nullptr;
			switch (static_cast<int32_t>(static_cast<uint32_t>(call.args[0])))
			{
			case 6:
				value = "100";
				break;
			case 8:
				value = "1";
				break;
			case 10:
				value = "0";
				break;
			default:
				break;
			}
			if (value == nullptr)
			{
				return uint64_t{ 0 };
			}
			return java.newString(value);
		});
	if (!body_adapter.ok())
	{
		return body_adapter;
	}
	const auto crash_register = declare(world, crash_bridge, "registerInfoCallback",
		"(Ljava/lang/String;IJI)I", true, returnInt(0));
	if (!crash_register.ok())
	{
		return crash_register;
	}
	const auto add_crash = declare(world, breakpad, "addOnCrashListener", "(J)V", true,
		returnVoid());
	if (!add_crash.ok())
	{
		return add_crash;
	}
	const auto remove_crash = declare(world, breakpad, "removeOnCrashListener", "(J)V", true,
		returnVoid());
	if (!remove_crash.ok())
	{
		return remove_crash;
	}

	// Integer/Boolean boxes: the host side constructs the doCommand arguments.
	g_integer_class = world.findClass("java/lang/Integer");
	g_boolean_class = world.findClass("java/lang/Boolean");
	if (g_integer_class == 0 || g_boolean_class == 0)
	{
		return Error{ ErrorCode::NotFound, "box classes are not installed" };
	}
	g_integer_value_field = world.findField(g_integer_class, "value", "I", false);
	g_boolean_value_field = world.findField(g_boolean_class, "value", "Z", false);
	if (g_integer_value_field == 0 || g_boolean_value_field == 0)
	{
		return Error{ ErrorCode::NotFound, "box value fields are not installed" };
	}

	auto done = std::move(binder).done();
	if (!done.ok())
	{
		return done;
	}
	// 10103/10104 start SecurityGuard's device collectors; their surface lives
	// in its own file and only touches Android classes, never SDK logic.
	auto device = installDeviceEnvironment(world);
	if (!device.ok())
	{
		return device;
	}
	g_world = &world;
	return {};
}

Result<uint64_t> makeString(std::string_view text)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "taobao: environment is not installed" };
	}
	return g_world->newString(text);
}

Result<uint64_t> makeIntBox(int32_t value)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "taobao: environment is not installed" };
	}
	auto object = g_world->allocateObject(g_integer_class);
	if (!object.ok())
	{
		return object.error();
	}
	auto written = g_world->writeInstanceField(object.value(), g_integer_value_field,
		static_cast<uint64_t>(static_cast<uint32_t>(value)));
	if (!written.ok())
	{
		return written.error();
	}
	return object.value();
}

Result<uint64_t> makeBooleanBox(bool value)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "taobao: environment is not installed" };
	}
	auto object = g_world->allocateObject(g_boolean_class);
	if (!object.ok())
	{
		return object.error();
	}
	auto written = g_world->writeInstanceField(object.value(), g_boolean_value_field,
		value ? 1 : 0);
	if (!written.ok())
	{
		return written.error();
	}
	return object.value();
}

Result<uint64_t> newContextObject()
{
	if (g_world == nullptr || g_context_class == 0)
	{
		return Error{ ErrorCode::InvalidArgument, "taobao: environment is not installed" };
	}
	return g_world->allocateObject(g_context_class);
}

Result<uint64_t> newMainPluginObject()
{
	if (g_world == nullptr || g_main_plugin_class == 0)
	{
		return Error{ ErrorCode::InvalidArgument, "taobao: environment is not installed" };
	}
	return g_world->allocateObject(g_main_plugin_class);
}

Result<uint64_t> newObjectArray(const std::vector<uint64_t> &elements)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "taobao: environment is not installed" };
	}
	auto array = g_world->newArray(runtime::jni::ArrayElementKind::Object,
		elements.size(), g_world->objectClass());
	if (!array.ok())
	{
		return array.error();
	}
	for (uint64_t index = 0; index < elements.size(); ++index)
	{
		if (elements[index] == 0)
		{
			continue;
		}
		auto written = g_world->writeArrayElement(array.value(), index, elements[index]);
		if (!written.ok())
		{
			return written.error();
		}
	}
	return array.value();
}

Result<std::string> readJavaString(uint64_t object)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "taobao: environment is not installed" };
	}
	if (object == 0)
	{
		return std::string{};
	}
	return g_world->stringUtf8(object);
}

Result<int32_t> readIntegerValue(uint64_t object)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "taobao: environment is not installed" };
	}
	return runtime::jni::envs::integerValue(*g_world, object);
}

Result<bool> readBooleanValue(uint64_t object)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "taobao: environment is not installed" };
	}
	return runtime::jni::envs::booleanValue(*g_world, object);
}

Result<std::string> classDescriptorOf(uint64_t object)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "taobao: environment is not installed" };
	}
	if (object == 0)
	{
		return std::string{ "<null>" };
	}
	const uint64_t klass = g_world->classOf(object);
	if (klass == 0)
	{
		return std::string{ "<unknown>" };
	}
	return std::string(g_world->descriptorOfClass(klass));
}

Result<std::string> mapToString(uint64_t object)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "taobao: environment is not installed" };
	}
	if (object == 0)
	{
		return std::string{ "<null>" };
	}
	const uint64_t map_class = g_world->findClass("java/util/Map");
	if (map_class == 0)
	{
		return Error{ ErrorCode::NotFound, "java/util/Map is not installed" };
	}
	// The SDK builds a HashMap and the map shims are declared on both Map and
	// HashMap; resolving on Map is enough because findMethod walks the class.
	const uint64_t method_id =
		g_world->findMethod(map_class, "toString", "()Ljava/lang/String;", false);
	const runtime::jni::MethodRecord *record = g_world->method(method_id);
	if (record == nullptr || !record->shim)
	{
		return Error{ ErrorCode::NotFound, "java/util/Map.toString has no implementation" };
	}
	runtime::jni::InvokeContext call;
	call.receiver = object;
	auto text = record->shim(*g_world, call);
	if (!text.ok())
	{
		return text.error();
	}
	if (text.value() == 0)
	{
		return std::string{};
	}
	return g_world->stringUtf8(text.value());
}

} // namespace duckemu::user::taobao
