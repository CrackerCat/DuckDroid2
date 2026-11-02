// SPDX-License-Identifier: GPL-2.0-only
// Android device surface for SecurityGuard's OneMain collectors.
//
// 10103/10104 make the security-body and middletier plugins start their
// device-fingerprint threads. Those threads resolve a fixed list of Android
// classes and read a fixed set of values. Everything here is a *reading*
// (Build, telephony, network, settings, display, sensors, binder), not SDK
// logic, and the values follow the reference-device picture in
// taobao.properties (Mi 8 / Android 12 / API 32).
//
// Calls whose data the emulated process cannot have answer exactly what the
// real framework answers when the service is absent (null / 0 / empty), so
// the collectors take the same branch they take on a device with no SIM, no
// network and no sensors.

#include "taobao_case/taobao_device.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "runtime/jni/envs/envs.hpp"
#include "runtime/jni/world/java_world.hpp"

namespace duckemu::user::taobao
{
namespace
{

using runtime::jni::InvokeContext;
using runtime::jni::JavaWorld;
using runtime::jni::ShimHandler;

constexpr const char *kPackage = "com.taobao.taobao";
// Same picture as taobao.properties; Java-side Build must agree with the
// ro.* values the guest sees, or the SDK has two fingerprints.
constexpr const char *kOsRelease = "12";
constexpr int32_t kSdkInt = 32;
constexpr const char *kModel = "MI 8";
constexpr const char *kBrand = "Xiaomi";
constexpr const char *kDevice = "dipper";
constexpr const char *kBuildId = "OPM1.171019.011";
constexpr const char *kFingerprint =
	"Xiaomi/dipper/dipper:8.1.0/OPM1.171019.011/V9.5.5.0.OEAMIFA:user/release-keys";

// Mi 8: 1080x2248 at 2.75x / 440 dpi.
constexpr int32_t kWidthPixels = 1080;
constexpr int32_t kHeightPixels = 2248;
constexpr int32_t kDensityDpi = 440;
constexpr float kDensity = 2.75F;
constexpr float kXdpi = 403.411F;
constexpr float kYdpi = 403.411F;

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

Result<uint64_t> ensureClass(JavaWorld &world, const char *descriptor, const char *super)
{
	const uint64_t existing = world.findClass(descriptor);
	if (existing != 0)
	{
		return existing;
	}
	return world.declareClass(descriptor, super);
}

Result<void> setStaticString(JavaWorld &world,
	uint64_t klass,
	const char *name,
	const char *value)
{
	auto field = world.declareField(klass, name, "Ljava/lang/String;", true);
	if (!field.ok())
	{
		return field.error();
	}
	auto text = world.newString(value);
	if (!text.ok())
	{
		return text.error();
	}
	return world.writeStaticField(klass, field.value(), text.value());
}

Result<void> setStaticInt(JavaWorld &world, uint64_t klass, const char *name, int32_t value)
{
	auto field = world.declareField(klass, name, "I", true);
	if (!field.ok())
	{
		return field.error();
	}
	return world.writeStaticField(klass, field.value(),
		static_cast<uint64_t>(static_cast<uint32_t>(value)));
}

uint64_t floatBits(float value)
{
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

ShimHandler returnFloat(float value)
{
	return returnInt(floatBits(value));
}

// -- Context constants ------------------------------------------------------

// The SDK resolves these static String fields before it asks for the service;
// a miss makes the caller skip the whole service probe. Values are the AOSP
// constants.
Result<void> installContextConstants(JavaWorld &world)
{
	auto context = world.findClass("android/content/Context");
	if (context == 0)
	{
		return Error{ ErrorCode::NotFound, "android/content/Context is not installed" };
	}
	const struct
	{
		const char *name;
		const char *value;
	} constants[] = {
		{ "WIFI_SERVICE", "wifi" },
		{ "TELEPHONY_SERVICE", "phone" },
		{ "SENSOR_SERVICE", "sensor" },
		{ "ACTIVITY_SERVICE", "activity" },
		{ "WINDOW_SERVICE", "window" },
		{ "CONNECTIVITY_SERVICE", "connectivity" },
		{ "LOCATION_SERVICE", "location" },
		{ "AUDIO_SERVICE", "audio" },
		{ "DISPLAY_SERVICE", "display" },
		{ "POWER_SERVICE", "power" },
		{ "BATTERY_SERVICE", "batteryservice" },
	};
	for (const auto &constant : constants)
	{
		auto written = setStaticString(world, context, constant.name, constant.value);
		if (!written.ok())
		{
			return written;
		}
	}
	return {};
}

// -- Build ------------------------------------------------------------------

Result<void> installBuild(JavaWorld &world)
{
	auto klass = ensureClass(world, "android/os/Build", "java/lang/Object");
	if (!klass.ok())
	{
		return klass.error();
	}
	const struct
	{
		const char *name;
		const char *value;
	} fields[] = {
		{ "UNKNOWN", "unknown" },
		{ "BOARD", kDevice },
		{ "BOOTLOADER", "unknown" },
		{ "BRAND", kBrand },
		{ "CPU_ABI", "arm64-v8a" },
		{ "CPU_ABI2", "armeabi-v7a" },
		{ "DEVICE", kDevice },
		{ "DISPLAY", kBuildId },
		{ "FINGERPRINT", kFingerprint },
		{ "HARDWARE", "qcom" },
		{ "HOST", "unknown" },
		{ "ID", kBuildId },
		{ "MANUFACTURER", kBrand },
		{ "MODEL", kModel },
		{ "PRODUCT", kDevice },
		{ "RADIO", "unknown" },
		{ "SERIAL", "unknown" },
		{ "SOC_MODEL", "SDM845" },
		{ "TAGS", "release-keys" },
		{ "TIME", "unknown" },
		{ "TYPE", "user" },
		{ "USER", "unknown" },
	};
	for (const auto &field : fields)
	{
		auto written = setStaticString(world, klass.value(), field.name, field.value);
		if (!written.ok())
		{
			return written;
		}
	}

	auto version = ensureClass(world, "android/os/Build$VERSION", "java/lang/Object");
	if (!version.ok())
	{
		return version.error();
	}
	auto release = setStaticString(world, version.value(), "RELEASE", kOsRelease);
	if (!release.ok())
	{
		return release;
	}
	auto codename = setStaticString(world, version.value(), "CODENAME", "REL");
	if (!codename.ok())
	{
		return codename;
	}
	auto incremental = setStaticString(world, version.value(), "INCREMENTAL", "unknown");
	if (!incremental.ok())
	{
		return incremental;
	}
	return setStaticInt(world, version.value(), "SDK_INT", kSdkInt);
}

// -- Display ----------------------------------------------------------------

struct DisplayFields
{
	uint64_t klass = 0;
	uint64_t width = 0;
	uint64_t height = 0;
	uint64_t density = 0;
	uint64_t density_dpi = 0;
	uint64_t scaled_density = 0;
	uint64_t xdpi = 0;
	uint64_t ydpi = 0;
	uint64_t point_x = 0;
	uint64_t point_y = 0;
	uint64_t display_object = 0;
	uint64_t metrics_object = 0;
};

Result<void> fillDisplayMetrics(JavaWorld &world, const DisplayFields &fields, uint64_t object)
{
	if (object == 0)
	{
		return {};
	}
	const struct
	{
		uint64_t field;
		uint64_t value;
	} values[] = {
		{ fields.width, static_cast<uint64_t>(kWidthPixels) },
		{ fields.height, static_cast<uint64_t>(kHeightPixels) },
		{ fields.density, floatBits(kDensity) },
		{ fields.density_dpi, static_cast<uint64_t>(kDensityDpi) },
		{ fields.scaled_density, floatBits(kDensity) },
		{ fields.xdpi, floatBits(kXdpi) },
		{ fields.ydpi, floatBits(kYdpi) },
	};
	for (const auto &value : values)
	{
		auto written = world.writeInstanceField(object, value.field, value.value);
		if (!written.ok())
		{
			return written.error();
		}
	}
	return {};
}

Result<void> installDisplay(JavaWorld &world)
{
	DisplayFields fields;
	auto display = ensureClass(world, "android/view/Display", "java/lang/Object");
	if (!display.ok())
	{
		return display.error();
	}
	fields.klass = display.value();
	auto metrics = ensureClass(world, "android/util/DisplayMetrics", "java/lang/Object");
	if (!metrics.ok())
	{
		return metrics.error();
	}
	auto point = ensureClass(world, "android/graphics/Point", "java/lang/Object");
	if (!point.ok())
	{
		return point.error();
	}

	const struct
	{
		const char *name;
		const char *signature;
		uint64_t *slot;
	} metrics_fields[] = {
		{ "widthPixels", "I", &fields.width },
		{ "heightPixels", "I", &fields.height },
		{ "density", "F", &fields.density },
		{ "densityDpi", "I", &fields.density_dpi },
		{ "scaledDensity", "F", &fields.scaled_density },
		{ "xdpi", "F", &fields.xdpi },
		{ "ydpi", "F", &fields.ydpi },
	};
	for (const auto &field : metrics_fields)
	{
		auto declared = world.declareField(metrics.value(), field.name, field.signature, false);
		if (!declared.ok())
		{
			return declared.error();
		}
		*field.slot = declared.value();
	}
	auto metrics_ctor = declare(world, metrics.value(), "<init>", "()V", false, returnVoid());
	if (!metrics_ctor.ok())
	{
		return metrics_ctor;
	}
	auto metrics_object = world.allocateObject(metrics.value());
	if (!metrics_object.ok())
	{
		return metrics_object.error();
	}
	fields.metrics_object = metrics_object.value();
	auto filled = fillDisplayMetrics(world, fields, fields.metrics_object);
	if (!filled.ok())
	{
		return filled;
	}

	auto point_x = world.declareField(point.value(), "x", "I", false);
	auto point_y = world.declareField(point.value(), "y", "I", false);
	if (!point_x.ok() || !point_y.ok())
	{
		return !point_x.ok() ? point_x.error() : point_y.error();
	}
	fields.point_x = point_x.value();
	fields.point_y = point_y.value();

	auto display_object = world.allocateObject(fields.klass);
	if (!display_object.ok())
	{
		return display_object.error();
	}
	fields.display_object = display_object.value();

	const auto fill_metrics = [fields](JavaWorld &java,
					  InvokeContext &call) -> Result<uint64_t> {
		if (call.args.empty())
		{
			return uint64_t{ 0 };
		}
		auto filled = fillDisplayMetrics(java, fields, call.args[0]);
		if (!filled.ok())
		{
			return filled.error();
		}
		return uint64_t{ 0 };
	};
	if (auto result = declare(world, fields.klass, "getMetrics",
		    "(Landroid/util/DisplayMetrics;)V", false, fill_metrics);
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, fields.klass, "getRealMetrics",
		    "(Landroid/util/DisplayMetrics;)V", false, fill_metrics);
		!result.ok())
	{
		return result;
	}
	const auto fill_point = [fields](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
		if (call.args.empty() || call.args[0] == 0)
		{
			return uint64_t{ 0 };
		}
		auto x = java.writeInstanceField(call.args[0], fields.point_x,
			static_cast<uint64_t>(kWidthPixels));
		if (!x.ok())
		{
			return x.error();
		}
		auto y = java.writeInstanceField(call.args[0], fields.point_y,
			static_cast<uint64_t>(kHeightPixels));
		if (!y.ok())
		{
			return y.error();
		}
		return uint64_t{ 0 };
	};
	if (auto result = declare(world, fields.klass, "getSize", "(Landroid/graphics/Point;)V",
		    false, fill_point);
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, fields.klass, "getRealSize",
		    "(Landroid/graphics/Point;)V", false, fill_point);
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, fields.klass, "getWidth", "()I", false,
		    returnInt(kWidthPixels));
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, fields.klass, "getHeight", "()I", false,
		    returnInt(kHeightPixels));
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, fields.klass, "getRotation", "()I", false,
		    returnInt(0));
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, fields.klass, "getDisplayId", "()I", false,
		    returnInt(0));
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, fields.klass, "getState", "()I", false,
		    returnInt(2));
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, fields.klass, "getType", "()I", false,
		    returnInt(1));
		!result.ok())
	{
		return result;
	}
	// Display.Mode describes the physical mode; this display reports none.
	auto mode = ensureClass(world, "android/view/Display$Mode", "java/lang/Object");
	if (!mode.ok())
	{
		return mode.error();
	}
	if (auto result = declare(world, fields.klass, "getMode",
		    "()Landroid/view/Display$Mode;", false, returnNull());
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, mode.value(), "getRefreshRate", "()F", false,
		    returnFloat(60.0F));
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, mode.value(), "getModeId", "()I", false,
		    returnInt(0));
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, mode.value(), "getPhysicalWidth", "()I", false,
		    returnInt(kWidthPixels));
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, mode.value(), "getPhysicalHeight", "()I", false,
		    returnInt(kHeightPixels));
		!result.ok())
	{
		return result;
	}
	const uint64_t mode_class = mode.value();
	if (auto result = declare(world, fields.klass, "getSupportedModes",
		    "()[Landroid/view/Display$Mode;", false,
		    [mode_class](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			    return java.newArray(runtime::jni::ArrayElementKind::Object, 0,
				    mode_class);
		    });
		!result.ok())
	{
		return result;
	}

	// WindowManager hands out the one display this process has.
	uint64_t window_manager = world.findClass("android/view/WindowManager");
	if (window_manager == 0)
	{
		auto declared = world.declareClass("android/view/WindowManager", "java/lang/Object");
		if (!declared.ok())
		{
			return declared.error();
		}
		window_manager = declared.value();
	}
	const uint64_t display_object_value = fields.display_object;
	return declare(world, window_manager, "getDefaultDisplay",
		"()Landroid/view/Display;", false,
		[display_object_value](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
			return display_object_value;
		});
}

// -- SecurityGuard's resolved platform façades ------------------------------

Result<void> installWindowExtras(JavaWorld &world)
{
	auto global = ensureClass(world, "android/view/WindowManagerGlobal", "java/lang/Object");
	if (!global.ok())
	{
		return global.error();
	}
	auto instance = world.allocateObject(global.value());
	if (!instance.ok())
	{
		return instance.error();
	}
	const uint64_t global_object = instance.value();
	// mViews is the list of attached root views. A process with no activity
	// has an empty list, not a null one; collectors call size() on it.
	auto views = world.declareField(global.value(), "mViews", "Ljava/util/ArrayList;",
		false);
	if (!views.ok())
	{
		return views.error();
	}
	const uint64_t array_list = world.findClass("java/util/ArrayList");
	if (array_list != 0)
	{
		auto list_object = world.allocateObject(array_list);
		if (!list_object.ok())
		{
			return list_object.error();
		}
		auto written = world.writeInstanceField(global_object, views.value(),
			list_object.value());
		if (!written.ok())
		{
			return written.error();
		}
		// mParams is parallel to mViews; the SDK probes both.
		auto parameters = world.declareField(global.value(), "mParams",
			"Ljava/util/ArrayList;", false);
		if (!parameters.ok())
		{
			return parameters.error();
		}
		auto params_object = world.allocateObject(array_list);
		if (!params_object.ok())
		{
			return params_object.error();
		}
		auto params_written = world.writeInstanceField(global_object,
			parameters.value(), params_object.value());
		if (!params_written.ok())
		{
			return params_written;
		}
	}
	if (auto result = declare(world, global.value(), "getInstance",
		    "()Landroid/view/WindowManagerGlobal;", true,
		    [global_object](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
			    return global_object;
		    });
		!result.ok())
	{
		return result;
	}
	return declare(world, global.value(), "getWindowManagerService",
		"()Landroid/view/IWindowManager;", false, returnNull());
}

// -- Sensors and motion -----------------------------------------------------

Result<void> installMotion(JavaWorld &world)
{
	auto sensor = ensureClass(world, "android/hardware/Sensor", "java/lang/Object");
	if (!sensor.ok())
	{
		return sensor.error();
	}
	auto sensor_listener = ensureClass(world, "android/hardware/SensorEventListener", "");
	if (!sensor_listener.ok())
	{
		return sensor_listener.error();
	}
	uint64_t sensor_manager = world.findClass("android/hardware/SensorManager");
	if (sensor_manager == 0)
	{
		auto declared = world.declareClass("android/hardware/SensorManager", "java/lang/Object");
		if (!declared.ok())
		{
			return declared.error();
		}
		sensor_manager = declared.value();
	}
	if (auto result = declare(world, sensor_manager, "registerListener",
		    "(Landroid/hardware/SensorEventListener;Landroid/hardware/Sensor;I"
		    "Landroid/os/Handler;)Z",
		    false, returnInt(0));
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, sensor_manager, "registerListener",
		    "(Landroid/hardware/SensorEventListener;Landroid/hardware/Sensor;I)Z", false,
		    returnInt(0));
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, sensor_manager, "unregisterListener",
		    "(Landroid/hardware/SensorEventListener;)V", false, returnVoid());
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, sensor_manager, "unregisterListener",
		    "(Landroid/hardware/SensorEventListener;Landroid/hardware/Sensor;)V", false,
		    returnVoid());
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, sensor_manager, "getDefaultSensor",
		    "(I)Landroid/hardware/Sensor;", false, returnNull());
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, sensor_manager, "getSensorList",
		    "(I)Ljava/util/List;", false, returnNull());
		!result.ok())
	{
		return result;
	}

	uint64_t motion = world.findClass("android/view/MotionEvent");
	if (motion == 0)
	{
		auto declared = world.declareClass("android/view/MotionEvent", "java/lang/Object");
		if (!declared.ok())
		{
			return declared.error();
		}
		motion = declared.value();
	}
	const struct
	{
		const char *name;
		const char *signature;
	} motion_methods[] = {
		{ "getAction", "()I" },
		{ "getToolType", "(I)I" },
		{ "getDeviceId", "()I" },
		{ "getSource", "()I" },
		{ "getHistorySize", "()I" },
		{ "getPointerCount", "()I" },
		{ "getActionMasked", "()I" },
		{ "getEventTime", "()J" },
		{ "getDownTime", "()J" },
		{ "getX", "()F" },
		{ "getY", "()F" },
		{ "getTouchMajor", "()F" },
		{ "getTouchMinor", "()F" },
		{ "getToolMajor", "()F" },
		{ "getToolMinor", "()F" },
		{ "getSize", "()F" },
		{ "getPressure", "()F" },
		{ "getRawX", "()F" },
		{ "getRawY", "()F" },
		{ "getOrientation", "()F" },
	};
	for (const auto &method : motion_methods)
	{
		if (auto result = declare(world, motion, method.name, method.signature, false,
			    returnInt(0));
			!result.ok())
		{
			return result;
		}
	}
	if (auto result = declare(world, motion, "getDevice",
		    "()Landroid/view/InputDevice;", false, returnNull());
		!result.ok())
	{
		return result;
	}

	auto input = ensureClass(world, "android/view/InputDevice", "java/lang/Object");
	if (!input.ok())
	{
		return input.error();
	}
	auto range = ensureClass(world, "android/view/InputDevice$MotionRange",
		"java/lang/Object");
	if (!range.ok())
	{
		return range.error();
	}
	for (const auto &method : { std::make_pair("getName", "()Ljava/lang/String;"),
		     std::make_pair("getDescriptor", "()Ljava/lang/String;"),
		     std::make_pair("getMotionRange",
			     "(I)Landroid/view/InputDevice$MotionRange;"),
		     std::make_pair("getMotionRanges", "()Ljava/util/List;") })
	{
		if (auto result = declare(world, input.value(), method.first, method.second,
			    false, returnNull());
			!result.ok())
		{
			return result;
		}
	}
	for (const auto &method : { std::make_pair("getVendorId", "()I"),
		     std::make_pair("getProductId", "()I"),
		     std::make_pair("getSources", "()I") })
	{
		if (auto result = declare(world, input.value(), method.first, method.second,
			    false, returnInt(0));
			!result.ok())
		{
			return result;
		}
	}
	// MotionRange: the collector resolves the range members before it has a
	// device, so they exist even though no device is ever returned.
	for (const auto &method : { std::make_pair("getAxis", "()I"),
		     std::make_pair("getSource", "()I"),
		     std::make_pair("getMin", "()F"),
		     std::make_pair("getMax", "()F"),
		     std::make_pair("getFlat", "()F"),
		     std::make_pair("getFuzz", "()F") })
	{
		if (auto result = declare(world, range.value(), method.first, method.second,
			    false, returnInt(0));
			!result.ok())
		{
			return result;
		}
	}
	return {};
}

// -- Telephony / activity / resources ---------------------------------------

Result<void> installTelephony(JavaWorld &world)
{
	auto klass = ensureClass(world, "android/telephony/TelephonyManager", "java/lang/Object");
	if (!klass.ok())
	{
		return klass.error();
	}
	const struct
	{
		const char *name;
		const char *signature;
	} strings[] = {
		{ "getDeviceId", "()Ljava/lang/String;" },
		{ "getImei", "()Ljava/lang/String;" },
		{ "getMeid", "()Ljava/lang/String;" },
		{ "getSubscriberId", "()Ljava/lang/String;" },
		{ "getSimSerialNumber", "()Ljava/lang/String;" },
		{ "getLine1Number", "()Ljava/lang/String;" },
		{ "getNetworkOperator", "()Ljava/lang/String;" },
		{ "getNetworkOperatorName", "()Ljava/lang/String;" },
		{ "getSimOperator", "()Ljava/lang/String;" },
		{ "getSimOperatorName", "()Ljava/lang/String;" },
		{ "getSimCountryIso", "()Ljava/lang/String;" },
		{ "getNetworkCountryIso", "()Ljava/lang/String;" },
		{ "getNetworkTypeName", "()Ljava/lang/String;" },
	};
	for (const auto &method : strings)
	{
		if (auto result = declare(world, klass.value(), method.name, method.signature,
			    false, returnNull());
			!result.ok())
		{
			return result;
		}
	}
	for (const auto &method : { std::make_pair("getPhoneType", "()I"),
		     std::make_pair("getSimState", "()I"),
		     std::make_pair("getNetworkType", "()I") })
	{
		if (auto result = declare(world, klass.value(), method.first, method.second,
			    false, returnInt(0));
			!result.ok())
		{
			return result;
		}
	}
	return {};
}

Result<void> installActivityManager(JavaWorld &world)
{
	auto manager = ensureClass(world, "android/app/ActivityManager", "java/lang/Object");
	if (!manager.ok())
	{
		return manager.error();
	}
	for (const auto &method : { std::make_pair("getRunningAppProcesses",
						   "()Ljava/util/List;"),
		     std::make_pair("getRunningServices", "(I)Ljava/util/List;"),
		     std::make_pair("getRunningTasks", "(I)Ljava/util/List;") })
	{
		if (auto result = declare(world, manager.value(), method.first, method.second,
			    false, returnNull());
			!result.ok())
		{
			return result;
		}
	}

	auto thread = ensureClass(world, "android/app/ActivityThread", "java/lang/Object");
	if (!thread.ok())
	{
		return thread.error();
	}
	// A live app process has this static set; collectors read it directly and
	// give up when it is null. Hand them the process's ActivityThread object.
	auto thread_object = world.allocateObject(thread.value());
	if (!thread_object.ok())
	{
		return thread_object.error();
	}
	auto thread_field = world.declareField(thread.value(), "sCurrentActivityThread",
		"Landroid/app/ActivityThread;", true);
	if (!thread_field.ok())
	{
		return thread_field.error();
	}
	auto thread_written = world.writeStaticField(thread.value(), thread_field.value(),
		thread_object.value());
	if (!thread_written.ok())
	{
		return thread_written;
	}
	// mActivities is the process's live activity map. No activity is running,
	// but the map itself exists; collectors read size() from it.
	auto array_map = ensureClass(world, "android/util/ArrayMap", "java/lang/Object");
	if (!array_map.ok())
	{
		return array_map.error();
	}
	if (auto result = declare(world, array_map.value(), "<init>", "()V", false, returnVoid());
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, array_map.value(), "size", "()I", false, returnInt(0));
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, array_map.value(), "isEmpty", "()Z", false,
		    returnInt(1));
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, array_map.value(), "get",
		    "(Ljava/lang/Object;)Ljava/lang/Object;", false, returnNull());
		!result.ok())
	{
		return result;
	}
	auto map_object = world.allocateObject(array_map.value());
	if (!map_object.ok())
	{
		return map_object.error();
	}
	auto activities = world.declareField(thread.value(), "mActivities",
		"Landroid/util/ArrayMap;", false);
	if (!activities.ok())
	{
		return activities.error();
	}
	auto activities_written = world.writeInstanceField(thread_object.value(),
		activities.value(), map_object.value());
	if (!activities_written.ok())
	{
		return activities_written;
	}
	for (const auto &method : { std::make_pair("getApplication",
						  "()Landroid/app/Application;"),
		     std::make_pair("getSystemContext",
			     "()Landroid/app/ContextImpl;") })
	{
		if (auto result = declare(world, thread.value(), method.first, method.second,
			    false, returnNull());
			!result.ok())
		{
			return result;
		}
	}
	for (const auto &method : { std::make_pair("currentActivityThread",
						  "()Landroid/app/ActivityThread;"),
		     std::make_pair("currentApplication",
			     "()Landroid/app/Application;") })
	{
		if (auto result = declare(world, thread.value(), method.first, method.second,
			    true, returnNull());
			!result.ok())
		{
			return result;
		}
	}
	if (auto result = declare(world, thread.value(), "getProcessName",
		    "()Ljava/lang/String;", true,
		    [](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			    return java.newString(kPackage);
		    });
		!result.ok())
	{
		return result;
	}

	auto ui = ensureClass(world, "android/app/UiModeManager", "java/lang/Object");
	if (!ui.ok())
	{
		return ui.error();
	}
	for (const auto &method : { std::make_pair("getCurrentModeType", "()I"),
		     std::make_pair("getNightMode", "()I") })
	{
		if (auto result = declare(world, ui.value(), method.first, method.second, false,
			    returnInt(0));
			!result.ok())
		{
			return result;
		}
	}

	auto resources = ensureClass(world, "android/content/res/Resources", "java/lang/Object");
	if (!resources.ok())
	{
		return resources.error();
	}
	auto configuration = ensureClass(world, "android/content/res/Configuration",
		"java/lang/Object");
	if (!configuration.ok())
	{
		return configuration.error();
	}
	uint64_t resources_metrics = 0;
	const uint64_t metrics_class = world.findClass("android/util/DisplayMetrics");
	if (metrics_class != 0)
	{
		auto metrics_object = world.allocateObject(metrics_class);
		if (!metrics_object.ok())
		{
			return metrics_object.error();
		}
		resources_metrics = metrics_object.value();
	}
	if (auto result = declare(world, resources.value(), "getDisplayMetrics",
		    "()Landroid/util/DisplayMetrics;", false,
		    [resources_metrics](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
			    return resources_metrics;
		    });
		!result.ok())
	{
		return result;
	}
	auto configuration_object = world.allocateObject(configuration.value());
	if (!configuration_object.ok())
	{
		return configuration_object.error();
	}
	const uint64_t resources_configuration = configuration_object.value();
	if (auto result = declare(world, resources.value(), "getConfiguration",
		    "()Landroid/content/res/Configuration;", false,
		    [resources_configuration](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
			    return resources_configuration;
		    });
		!result.ok())
	{
		return result;
	}
	return declare(world, resources.value(), "getIdentifier",
		"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I", false,
		returnInt(0));
}

// -- Settings / binder ------------------------------------------------------

Result<void> installSettings(JavaWorld &world)
{
	auto settings = ensureClass(world, "android/provider/Settings", "java/lang/Object");
	if (!settings.ok())
	{
		return settings.error();
	}
	auto secure = ensureClass(world, "android/provider/Settings$Secure", "java/lang/Object");
	if (!secure.ok())
	{
		return secure.error();
	}
	if (auto result = declare(world, secure.value(), "getString",
		    "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;", true,
		    returnNull());
		!result.ok())
	{
		return result;
	}
	// getInt/getLong answer their default argument, as on a device without the
	// setting; the default is the second value slot.
	const auto default_int = [](JavaWorld &, InvokeContext &call) -> Result<uint64_t> {
		return call.args.size() >= 3 ? call.args[2] : uint64_t{ 0 };
	};
	const auto default_long = [](JavaWorld &, InvokeContext &call) -> Result<uint64_t> {
		return call.args.size() >= 3 ? call.args[2] : uint64_t{ 0 };
	};
	if (auto result = declare(world, secure.value(), "getInt",
		    "(Landroid/content/ContentResolver;Ljava/lang/String;I)I", true,
		    default_int);
		!result.ok())
	{
		return result;
	}
	return declare(world, secure.value(), "getLong",
		"(Landroid/content/ContentResolver;Ljava/lang/String;J)J", true, default_long);
}

Result<void> installBinderProbes(JavaWorld &world)
{
	auto manager = ensureClass(world, "android/os/ServiceManager", "java/lang/Object");
	if (!manager.ok())
	{
		return manager.error();
	}
	auto binder = ensureClass(world, "android/os/IBinder", "");
	if (!binder.ok())
	{
		return binder.error();
	}
	// No service manager in the emulated process: every lookup is null, which
	// is what the framework answers for a service the process cannot reach.
	if (auto result = declare(world, manager.value(), "getService",
		    "(Ljava/lang/String;)Landroid/os/IBinder;", true, returnNull());
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, manager.value(), "checkService",
		    "(Ljava/lang/String;)Landroid/os/IBinder;", true, returnNull());
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, manager.value(), "addService",
		    "(Ljava/lang/String;Landroid/os/IBinder;)V", true, returnVoid());
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, manager.value(), "listServices",
		    "()[Ljava/lang/String;", true, returnNull());
		!result.ok())
	{
		return result;
	}
	auto service_manager = ensureClass(world, "android/os/IServiceManager", "");
	if (!service_manager.ok())
	{
		return service_manager.error();
	}
	if (auto result = declare(world, manager.value(), "getIServiceManager",
		    "()Landroid/os/IServiceManager;", true, returnNull());
		!result.ok())
	{
		return result;
	}

	auto parcel = ensureClass(world, "android/os/Parcel", "java/lang/Object");
	if (!parcel.ok())
	{
		return parcel.error();
	}
	auto parcel_object = world.allocateObject(parcel.value());
	if (!parcel_object.ok())
	{
		return parcel_object.error();
	}
	const uint64_t parcel_instance = parcel_object.value();
	if (auto result = declare(world, parcel.value(), "obtain",
		    "()Landroid/os/Parcel;", true,
		    [parcel_instance](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
			    return parcel_instance;
		    });
		!result.ok())
	{
		return result;
	}
	for (const auto &method : { std::make_pair("<init>", "()V"),
		     std::make_pair("recycle", "()V"),
		     std::make_pair("writeInterfaceToken", "(Ljava/lang/String;I)V"),
		     std::make_pair("writeInt", "(I)V"),
		     std::make_pair("writeLong", "(J)V"),
		     std::make_pair("writeString", "(Ljava/lang/String;)V"),
		     std::make_pair("enforceInterface", "(Ljava/lang/String;)V"),
		     std::make_pair("readException", "()V") })
	{
		if (auto result = declare(world, parcel.value(), method.first, method.second,
			    false, returnVoid());
			!result.ok())
		{
			return result;
		}
	}
	for (const auto &method : { std::make_pair("dataSize", "()I"),
		     std::make_pair("dataPosition", "()I"),
		     std::make_pair("readInt", "()I"),
		     std::make_pair("readLong", "()J") })
	{
		if (auto result = declare(world, parcel.value(), method.first, method.second,
			    false, returnInt(0));
			!result.ok())
		{
			return result;
		}
	}
	for (const auto &method : { std::make_pair("readString", "()Ljava/lang/String;"),
		     std::make_pair("marshall", "()[B"),
		     std::make_pair("createByteArray", "()[B") })
	{
		if (auto result = declare(world, parcel.value(), method.first, method.second,
			    false, returnNull());
			!result.ok())
		{
			return result;
		}
	}

	auto pfd = ensureClass(world, "android/os/ParcelFileDescriptor", "java/lang/Object");
	if (!pfd.ok())
	{
		return pfd.error();
	}
	if (auto result = declare(world, pfd.value(), "getFd", "()I", false, returnInt(0xFFFFFFFFu));
		!result.ok())
	{
		return result;
	}
	return declare(world, pfd.value(), "close", "()V", false, returnVoid());
}

// -- Miscellaneous framework façades ----------------------------------------

Result<void> installIntents(JavaWorld &world)
{
	auto intent = ensureClass(world, "android/content/Intent", "java/lang/Object");
	if (!intent.ok())
	{
		return intent.error();
	}
	if (auto result = declare(world, intent.value(), "<init>", "()V", false, returnVoid());
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, intent.value(), "getAction",
		    "()Ljava/lang/String;", false, returnNull());
		!result.ok())
	{
		return result;
	}
	for (const char *name : { "setAction", "addCategory" })
	{
		const std::string signature =
			std::string("(Ljava/lang/String;)Landroid/content/Intent;");
		if (auto result = declare(world, intent.value(), name, signature.c_str(),
			    false,
			    [](JavaWorld &, InvokeContext &call) -> Result<uint64_t> {
				    return call.receiver;
			    });
			!result.ok())
		{
			return result;
		}
	}
	return {};
}

Result<void> installCookieManager(JavaWorld &world)
{
	auto klass = ensureClass(world, "android/webkit/CookieManager", "java/lang/Object");
	if (!klass.ok())
	{
		return klass.error();
	}
	auto object = world.allocateObject(klass.value());
	if (!object.ok())
	{
		return object.error();
	}
	const uint64_t instance = object.value();
	const auto return_instance = [instance](JavaWorld &,
						 InvokeContext &) -> Result<uint64_t> {
		return instance;
	};
	if (auto result = declare(world, klass.value(), "getInstance",
		    "()Landroid/webkit/CookieManager;", true, return_instance);
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, klass.value(), "getInstance",
		    "(Landroid/content/Context;)Landroid/webkit/CookieManager;", true,
		    return_instance);
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, klass.value(), "getCookie",
		    "(Ljava/lang/String;)Ljava/lang/String;", false, returnNull());
		!result.ok())
	{
		return result;
	}
	return declare(world, klass.value(), "setCookie", "(Ljava/lang/String;Ljava/lang/String;)V",
		false, returnVoid());
}

Result<void> installEmptyClasses(JavaWorld &world)
{
	for (const char *descriptor : {
		     "android/content/pm/ThemeInfo",
		     "android/content/pm/ResolveInfo",
		     "android/util/Singleton",
		     "android/net/wifi/WifiManager",
		     "mtopsdk/mtop/domain/MtopRequest",
		     "mtopsdk/mtop/common/MtopNetworkProp",
		     "mtopsdk/mtop/intf/MtopSetting",
		     "com/alibaba/one/android/inner/OneStubCenter",
		     "com/alibaba/wireless/security/middletierplugin/open/es/SGWindowManager",
		     "com/alibaba/wireless/security/middletierplugin/open/sensor/SensorEventWrapper",
	     })
	{
		auto declared = ensureClass(world, descriptor, "java/lang/Object");
		if (!declared.ok())
		{
			return declared.error();
		}
	}
	// PackageManager's hidden binder field: the value is null (no package
	// service), but reflection needs the field to exist.
	auto package_manager = world.findClass("android/content/pm/PackageManager");
	if (package_manager != 0)
	{
		auto ipm = ensureClass(world, "android/content/pm/IPackageManager", "");
		if (!ipm.ok())
		{
			return ipm.error();
		}
		auto field = world.declareField(package_manager, "mPM",
			"Landroid/content/pm/IPackageManager;", false);
		if (!field.ok())
		{
			return field.error();
		}
	}
	// The installed-package list is empty; java.util.List already models the
	// read-only members a caller would use on it.
	if (package_manager != 0)
	{
		if (auto result = declare(world, package_manager, "resolveActivity",
			    "(Landroid/content/Intent;I)Landroid/content/pm/ResolveInfo;", false,
			    returnNull());
			!result.ok())
		{
			return result;
		}
	}
	if (package_manager != 0)
	{
		if (auto result = declare(world, package_manager, "getInstalledPackages",
			    "(I)Ljava/util/List;", false,
			    [](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
				    const uint64_t list = java.findClass("java/util/List");
				    if (list == 0)
				    {
					    return uint64_t{ 0 };
				    }
				    return java.allocateObject(list);
			    });
			!result.ok())
		{
			return result;
		}
	}

	// wifi/DHCP probe: no connection, so no DHCP lease.
	auto wifi = world.findClass("android/net/wifi/WifiManager");
	if (wifi != 0)
	{
		auto dhcp = ensureClass(world, "android/net/DhcpInfo", "java/lang/Object");
		if (!dhcp.ok())
		{
			return dhcp.error();
		}
		if (auto result = declare(world, wifi, "getDhcpInfo",
			    "()Landroid/net/DhcpInfo;", false, returnNull());
			!result.ok())
		{
			return result;
		}
	}
	auto stub_center = ensureClass(world, "com/alibaba/one/android/inner/OneStubCenter",
		"java/lang/Object");
	if (!stub_center.ok())
	{
		return stub_center.error();
	}
	if (auto result = declare(world, stub_center.value(), "<init>", "()V", false,
		    returnVoid());
		!result.ok())
	{
		return result;
	}
	if (auto result = declare(world, stub_center.value(), "oneStubCenterCallFunc",
		    "(ILjava/lang/Object;Ljava/lang/Object;)V", true, {});
		!result.ok())
	{
		return result;
	}
	auto sensor_wrapper = world.findClass(
		"com/alibaba/wireless/security/middletierplugin/open/sensor/"
		"SensorEventWrapper");
	if (sensor_wrapper != 0)
	{
		if (auto result = declare(world, sensor_wrapper, "<init>", "()V", false,
			    returnVoid());
			!result.ok())
		{
			return result;
		}
	}
	// MtopBuilder.request: the SDK's own field, read reflectively by the
	// middletier plugin; a plain declaration keeps the GetFieldID exact.
	auto mtop_builder = world.findClass("mtopsdk/mtop/intf/MtopBuilder");
	if (mtop_builder != 0)
	{
		auto request = world.declareField(mtop_builder, "request",
			"Lmtopsdk/mtop/domain/MtopRequest;", false);
		if (!request.ok())
		{
			return request.error();
		}
	}
	if (mtop_builder != 0)
	{
		for (const auto &field : { std::make_pair("requestId", "Ljava/lang/String;"),
			     std::make_pair("mtopProp",
				     "Lmtopsdk/mtop/common/MtopNetworkProp;") })
		{
			auto declared = world.declareField(mtop_builder, field.first,
				field.second, false);
			if (!declared.ok())
			{
				return declared.error();
			}
		}
	}
	// MtopRequest/MtopNetworkProp are modelled in installMtop from the APK's
	// own decompiled sources (mtopsdk 2.x); they are not ad-hoc declarations.
	// SensorEventWrapper mirrors android.hardware.SensorEvent's fields; the
	// SDK sensor collector reads the timestamp directly.
	if (sensor_wrapper != 0)
	{
		for (const auto &field : { std::make_pair("timestamp", "J"),
			     std::make_pair("accuracy", "I"),
			     std::make_pair("values", "[F"),
			     std::make_pair("sensorType", "I"),
			     std::make_pair("sensor", "Landroid/hardware/Sensor;") })
		{
			auto declared = world.declareField(sensor_wrapper, field.first,
				field.second, false);
			if (!declared.ok())
			{
				return declared.error();
			}
		}
	}
	// DhcpInfo: no lease, fields keep the framework's unset zero.
	auto dhcp_info = world.findClass("android/net/DhcpInfo");
	if (dhcp_info != 0)
	{
		for (const char *name : { "ipAddress", "gateway", "netmask", "dns1", "dns2",
			     "serverAddress", "leaseDuration" })
		{
			auto declared = world.declareField(dhcp_info, name, "I", false);
			if (!declared.ok())
			{
				return declared.error();
			}
		}
	}
	for (const char *descriptor : { "android/view/View",
		     "android/view/WindowManager$LayoutParams",
		     "android/app/ActivityThread$ActivityClientRecord",
		     "com/alibaba/one/android/inner/OneStubCenter$OneNetworkCallback" })
	{
		auto declared = ensureClass(world, descriptor, "java/lang/Object");
		if (!declared.ok())
		{
			return declared.error();
		}
	}
	// LayoutParams members the window collector reads off the params list.
	auto layout_params = world.findClass("android/view/WindowManager$LayoutParams");
	if (layout_params != 0)
	{
		for (const auto &field : { std::make_pair("x", "I"),
			     std::make_pair("y", "I"),
			     std::make_pair("width", "I"),
			     std::make_pair("height", "I"),
			     std::make_pair("type", "I"),
			     std::make_pair("gravity", "I"),
			     std::make_pair("flags", "I"),
			     std::make_pair("format", "I"),
			     std::make_pair("windowAnimations", "I"),
			     std::make_pair("alpha", "F"),
			     std::make_pair("dimAmount", "F"),
			     std::make_pair("token", "Landroid/os/IBinder;") })
		{
			auto declared = world.declareField(layout_params, field.first,
				field.second, false);
			if (!declared.ok())
			{
				return declared.error();
			}
		}
	}
	auto activity_record = world.findClass("android/app/ActivityThread$ActivityClientRecord");
	if (activity_record != 0)
	{
		if (auto result = declare(world, activity_record, "<init>", "()V", false,
			    returnVoid());
			!result.ok())
		{
			return result;
		}
		// Fields the collector resolves on the record class before it walks
		// the (empty) activity map.
		for (const auto &field : { std::make_pair("stopped", "Z"),
			     std::make_pair("paused", "Z"),
			     std::make_pair("activity", "Landroid/app/Activity;"),
			     std::make_pair("token", "Landroid/os/IBinder;") })
		{
			auto declared = world.declareField(activity_record, field.first,
				field.second, false);
			if (!declared.ok())
			{
				return declared.error();
			}
		}
	}
	// Activity.mReferrer is read from the activity chain (no activity here).
	auto activity = world.findClass("android/app/Activity");
	if (activity != 0)
	{
		auto referrer = world.declareField(activity, "mReferrer",
			"Ljava/lang/String;", false);
		if (!referrer.ok())
		{
			return referrer.error();
		}
	}
	// Context.bindService: no service to bind, framework answers false.
	if (auto result = declare(world, world.findClass("android/content/Context"),
		    "bindService",
		    "(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z", false,
		    returnInt(0));
		!result.ok())
	{
		return result;
	}
	auto service_connection = ensureClass(world, "android/content/ServiceConnection", "");
	if (!service_connection.ok())
	{
		return service_connection.error();
	}
	// MtopSetting: the middletier plugin registers an ST listener; no mtop
	// stack exists here, so the registration is a no-op.
	auto mtop_setting = world.findClass("mtopsdk/mtop/intf/MtopSetting");
	if (mtop_setting != 0)
	{
		auto listener = ensureClass(world, "mtopsdk/mtop/common/MtopRequestStListener", "");
		if (!listener.ok())
		{
			return listener.error();
		}
		if (auto result = declare(world, mtop_setting, "setMtopRequestStListener",
			    "(Ljava/lang/String;Lmtopsdk/mtop/common/MtopRequestStListener;)V",
			    true, returnVoid());
			!result.ok())
		{
			return result;
		}
	}
	// OneStubCenter's TLS trust manager is built by the SDK; the class exists
	// and accepts the plain constructor.
	auto trust_manager = ensureClass(world,
		"com/alibaba/one/android/inner/OneStubCenter$OneX509TrustManager",
		"java/lang/Object");
	if (!trust_manager.ok())
	{
		return trust_manager.error();
	}
	if (auto result = declare(world, trust_manager.value(), "<init>", "()V", false,
		    returnVoid());
		!result.ok())
	{
		return result;
	}

	// The window façade the middletier plugin calls after its own load.
	auto sg_window = world.findClass(
		"com/alibaba/wireless/security/middletierplugin/open/es/SGWindowManager");
	if (sg_window != 0)
	{
		if (auto result = declare(world, sg_window, "windowMangerInit",
			    "(Landroid/content/Context;)Z", true, returnInt(0));
			!result.ok())
		{
			return result;
		}
	}

	// PackageInfo.signatures is filled by the package service on a device.
	// This install has no signer record, so the array stays null; the SDK's
	// own null check handles it and falls back to reading base.apk.
	auto package_info = world.findClass("android/content/pm/PackageInfo");
	if (package_info != 0)
	{
		auto signatures = world.declareField(package_info, "signatures",
			"[Landroid/content/pm/Signature;", false);
		if (!signatures.ok())
		{
			return signatures.error();
		}
		auto signature = ensureClass(world, "android/content/pm/Signature",
			"java/lang/Object");
		if (!signature.ok())
		{
			return signature.error();
		}
		if (auto result = declare(world, signature.value(), "toCharsString",
			    "()Ljava/lang/String;", false, returnNull());
			!result.ok())
		{
			return result;
		}
		if (auto result = declare(world, signature.value(), "toByteArray", "()[B", false,
			    returnNull());
			!result.ok())
		{
			return result;
		}
	}
	return {};
}

// -- OneMain façades --------------------------------------------------------

Result<void> installOneMain(JavaWorld &world)
{
	// Both spellings of the capturer are what the SDK probes through its class
	// loader; doCommandForString is a native method the so registers itself.
	for (const char *descriptor : { "com/alibaba/one/android/inner/DeviceInfoCapturer",
		     "com/alibaba/one/android/inner/DeviceInfoCapturerFull" })
	{
		auto klass = ensureClass(world, descriptor, "java/lang/Object");
		if (!klass.ok())
		{
			return klass.error();
		}
		if (auto result = declare(world, klass.value(), "doCommandForString",
			    "(I)Ljava/lang/String;", true, {});
			!result.ok())
		{
			return result;
		}
		// The Full variant also registers getUid(String).
		if (std::string_view(descriptor).find("DeviceInfoCapturerFull") !=
			std::string_view::npos)
		{
			if (auto result = declare(world, klass.value(), "getUid",
				    "(Ljava/lang/String;)Ljava/lang/String;", true, {});
				!result.ok())
			{
				return result;
			}
		}
	}

	auto main = world.findClass("com/alibaba/one/android/sdk/OneMain");
	if (main == 0)
	{
		return Error{ ErrorCode::NotFound, "OneMain is not installed" };
	}
	// RegisterNatives binds this entry; it must exist before the so loads.
	return declare(world, main, "playNative",
		"(IIILjava/lang/Object;)Ljava/lang/Object;", true, {});
}

// -- mtop object model ------------------------------------------------------
//
// When the AVMP code reports device telemetry it resolves mtopsdk's own
// classes reflectively. The shape below is taken verbatim from this APK's
// classes12.dex (jadx: mtopsdk.mtop.*), including MtopNetworkProp's field
// default values, so one run sees the whole class instead of discovering one
// missing field per run.

Result<void> installMtopEnums(JavaWorld &world)
{
	// MethodEnum { GET, POST, HEAD, PATCH } with a String per constant.
	auto method = ensureClass(world, "mtopsdk/mtop/domain/MethodEnum", "java/lang/Object");
	if (!method.ok())
	{
		return method.error();
	}
	auto method_value = world.declareField(method.value(), "method", "Ljava/lang/String;",
		false);
	if (!method_value.ok())
	{
		return method_value.error();
	}
	if (auto result = declare(world, method.value(), "getMethod", "()Ljava/lang/String;",
		    false,
		    [method_value](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			    return java.readInstanceField(call.receiver, method_value.value());
		    });
		!result.ok())
	{
		return result;
	}
	for (const char *name : { "GET", "POST", "HEAD", "PATCH" })
	{
		auto field = world.declareField(method.value(), name,
			"Lmtopsdk/mtop/domain/MethodEnum;", true);
		if (!field.ok())
		{
			return field.error();
		}
		auto object = world.allocateObject(method.value());
		if (!object.ok())
		{
			return object.error();
		}
		auto text = world.newString(name);
		if (!text.ok())
		{
			return text.error();
		}
		auto written = world.writeInstanceField(object.value(), method_value.value(),
			text.value());
		if (!written.ok())
		{
			return written;
		}
		auto stored = world.writeStaticField(method.value(), field.value(), object.value());
		if (!stored.ok())
		{
			return stored;
		}
	}

	// EnvModeEnum { ONLINE(0), PREPARE(1), TEST(2), TEST_SANDBOX(3) }.
	auto env = ensureClass(world, "mtopsdk/mtop/domain/EnvModeEnum", "java/lang/Object");
	if (!env.ok())
	{
		return env.error();
	}
	auto env_value = world.declareField(env.value(), "envMode", "I", false);
	if (!env_value.ok())
	{
		return env_value.error();
	}
	if (auto result = declare(world, env.value(), "getEnvMode", "()I", false,
		    [env_value](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			    return java.readInstanceField(call.receiver, env_value.value());
		    });
		!result.ok())
	{
		return result;
	}
	const struct
	{
		const char *name;
		uint32_t value;
	} env_modes[] = {
		{ "ONLINE", 0 },
		{ "PREPARE", 1 },
		{ "TEST", 2 },
		{ "TEST_SANDBOX", 3 },
	};
	for (const auto &mode : env_modes)
	{
		auto field = world.declareField(env.value(), mode.name,
			"Lmtopsdk/mtop/domain/EnvModeEnum;", true);
		if (!field.ok())
		{
			return field.error();
		}
		auto object = world.allocateObject(env.value());
		if (!object.ok())
		{
			return object.error();
		}
		auto written = world.writeInstanceField(object.value(), env_value.value(),
			static_cast<uint64_t>(mode.value));
		if (!written.ok())
		{
			return written;
		}
		auto stored = world.writeStaticField(env.value(), field.value(), object.value());
		if (!stored.ok())
		{
			return stored;
		}
	}

	// ProtocolEnum { HTTP("http://"), HTTPSECURE("https://") }.
	auto protocol = ensureClass(world, "mtopsdk/mtop/domain/ProtocolEnum",
		"java/lang/Object");
	if (!protocol.ok())
	{
		return protocol.error();
	}
	auto protocol_value = world.declareField(protocol.value(), "protocol",
		"Ljava/lang/String;", false);
	if (!protocol_value.ok())
	{
		return protocol_value.error();
	}
	if (auto result = declare(world, protocol.value(), "getProtocol",
		    "()Ljava/lang/String;", false,
		    [protocol_value](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			    return java.readInstanceField(call.receiver, protocol_value.value());
		    });
		!result.ok())
	{
		return result;
	}
	const struct
	{
		const char *name;
		const char *prefix;
	} protocols[] = {
		{ "HTTP", "http://" },
		{ "HTTPSECURE", "https://" },
	};
	for (const auto &entry : protocols)
	{
		auto field = world.declareField(protocol.value(), entry.name,
			"Lmtopsdk/mtop/domain/ProtocolEnum;", true);
		if (!field.ok())
		{
			return field.error();
		}
		auto object = world.allocateObject(protocol.value());
		if (!object.ok())
		{
			return object.error();
		}
		auto text = world.newString(entry.prefix);
		if (!text.ok())
		{
			return text.error();
		}
		auto written = world.writeInstanceField(object.value(), protocol_value.value(),
			text.value());
		if (!written.ok())
		{
			return written;
		}
		auto stored = world.writeStaticField(protocol.value(), field.value(),
			object.value());
		if (!stored.ok())
		{
			return stored;
		}
	}

	for (const char *descriptor : { "mtopsdk/mtop/domain/ApiTypeEnum",
		     "android/os/Handler" })
	{
		auto declared = ensureClass(world, descriptor, "java/lang/Object");
		if (!declared.ok())
		{
			return declared.error();
		}
	}
	return {};
}

Result<void> installMtop(JavaWorld &world)
{
	auto enums = installMtopEnums(world);
	if (!enums.ok())
	{
		return enums;
	}

	// MtopNetworkProp: all fields from classes12.dex.
	auto prop = ensureClass(world, "mtopsdk/mtop/common/MtopNetworkProp",
		"java/lang/Object");
	if (!prop.ok())
	{
		return prop.error();
	}
	const struct
	{
		const char *name;
		const char *signature;
	} prop_fields[] = {
		{ "accessToken", "Ljava/lang/String;" },
		{ "apiType", "Lmtopsdk/mtop/domain/ApiTypeEnum;" },
		{ "authCode", "Ljava/lang/String;" },
		{ "backGround", "Z" },
		{ "bizId", "I" },
		{ "bizIdStr", "Ljava/lang/String;" },
		{ "bizTopic", "Ljava/lang/String;" },
		{ "clientTraceId", "Ljava/lang/String;" },
		{ "customDailyDomain", "Ljava/lang/String;" },
		{ "customDomain", "Ljava/lang/String;" },
		{ "customOnlineDomain", "Ljava/lang/String;" },
		{ "customPreDomain", "Ljava/lang/String;" },
		{ "enableProgressListener", "Z" },
		{ "falcoId", "Ljava/lang/String;" },
		{ "fullTraceId", "Ljava/lang/String;" },
		{ "handler", "Landroid/os/Handler;" },
		{ "isInnerOpen", "Z" },
		{ "isLoginTimeout", "Z" },
		{ "isTimeoutEnable", "Z" },
		{ "loginTime", "J" },
		{ "miniAppKey", "Ljava/lang/String;" },
		{ "netParam", "I" },
		{ "openBiz", "Ljava/lang/String;" },
		{ "openBizData", "Ljava/lang/String;" },
		{ "openTraceContext", "Ljava/util/Map;" },
		{ "pTraceId", "Ljava/lang/String;" },
		{ "pageIndex", "I" },
		{ "pageName", "Ljava/lang/String;" },
		{ "pageUrl", "Ljava/lang/String;" },
		{ "placeId", "Ljava/lang/String;" },
		{ "priorityFlag", "Z" },
		{ "queryParameterMap", "Ljava/util/Map;" },
		{ "reqAppKey", "Ljava/lang/String;" },
		{ "reqBizExt", "Ljava/lang/String;" },
		{ "reqSource", "I" },
		{ "reqUserId", "Ljava/lang/String;" },
		{ "requestHeaders", "Ljava/util/Map;" },
		{ "requestSourceAppKey", "Ljava/lang/String;" },
		{ "routerId", "Ljava/lang/String;" },
		{ "streamMode", "Z" },
		{ "ttid", "Ljava/lang/String;" },
		{ "protocol", "Lmtopsdk/mtop/domain/ProtocolEnum;" },
		{ "method", "Lmtopsdk/mtop/domain/MethodEnum;" },
		{ "allowSwitchToPOST", "Z" },
		{ "autoRedirect", "Z" },
		{ "retryTimes", "I" },
		{ "timeCalibrated", "Z" },
		{ "useCache", "Z" },
		{ "forceRefreshCache", "Z" },
		{ "skipCacheCallback", "Z" },
		{ "cacheKeyBlackList", "Ljava/util/List;" },
		{ "wuaFlag", "I" },
		{ "wuaRetry", "Z" },
		{ "openAppKey", "Ljava/lang/String;" },
		{ "connTimeout", "I" },
		{ "socketTimeout", "I" },
		{ "streamFirstStageReadTimeMills", "I" },
		{ "streamSubStageReadTimeMills", "I" },
		{ "envMode", "Lmtopsdk/mtop/domain/EnvModeEnum;" },
		{ "userInfo", "Ljava/lang/String;" },
		{ "allowNotDispatchMainThread", "Z" },
		{ "reqContext", "Ljava/lang/Object;" },
		{ "priorityData", "Ljava/util/Map;" },
	};
	for (const auto &field : prop_fields)
	{
		auto declared = world.declareField(prop.value(), field.name, field.signature,
			false);
		if (!declared.ok())
		{
			return declared.error();
		}
	}
	// Constructor defaults declared by the class itself.
	auto prop_ctor = declare(world, prop.value(), "<init>", "()V", false,
		[prop](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			const auto static_value = [&java](uint64_t klass, const char *name,
							  const char *signature) {
				const uint64_t field = java.findField(klass, name, signature, true);
				if (field == 0)
				{
					return uint64_t{ 0 };
				}
				auto value = java.readStaticField(klass, field);
				return value.ok() ? value.value() : uint64_t{ 0 };
			};
			const uint64_t method_get = static_value(
				java.findClass("mtopsdk/mtop/domain/MethodEnum"), "GET",
				"Lmtopsdk/mtop/domain/MethodEnum;");
			const uint64_t protocol_secure = static_value(
				java.findClass("mtopsdk/mtop/domain/ProtocolEnum"),
				"HTTPSECURE", "Lmtopsdk/mtop/domain/ProtocolEnum;");
			const uint64_t env_online = static_value(
				java.findClass("mtopsdk/mtop/domain/EnvModeEnum"), "ONLINE",
				"Lmtopsdk/mtop/domain/EnvModeEnum;");
			struct
			{
				const char *name;
				const char *signature;
				uint64_t value;
			} values[] = {
				{ "method", "Lmtopsdk/mtop/domain/MethodEnum;", method_get },
				{ "protocol", "Lmtopsdk/mtop/domain/ProtocolEnum;",
					protocol_secure },
				{ "envMode", "Lmtopsdk/mtop/domain/EnvModeEnum;", env_online },
				{ "autoRedirect", "Z", 1 },
				{ "retryTimes", "I", 1 },
				{ "wuaFlag", "I", 0xFFFFFFFFu },
			};
			for (const auto &entry : values)
			{
				const uint64_t field = java.findField(prop.value(), entry.name,
					entry.signature, false);
				if (field == 0 || entry.value == 0)
				{
					continue;
				}
				auto written = java.writeInstanceField(call.receiver, field,
					entry.value);
				if (!written.ok())
				{
					return written.error();
				}
			}
			for (const auto &entry : { std::make_pair("openAppKey", "DEFAULT_AUTH"),
				     std::make_pair("userInfo", "DEFAULT") })
			{
				const uint64_t field = java.findField(prop.value(), entry.first,
					"Ljava/lang/String;", false);
				if (field == 0)
				{
					continue;
				}
				auto text = java.newString(entry.second);
				if (!text.ok())
				{
					return text.error();
				}
				auto written = java.writeInstanceField(call.receiver, field,
					text.value());
				if (!written.ok())
				{
					return written.error();
				}
			}
			return uint64_t{ 0 };
		});
	if (!prop_ctor.ok())
	{
		return prop_ctor;
	}

	// MtopRequest: fields and accessors from classes.dex.
	auto request = ensureClass(world, "mtopsdk/mtop/domain/MtopRequest",
		"java/lang/Object");
	if (!request.ok())
	{
		return request.error();
	}
	const struct
	{
		const char *name;
		const char *signature;
	} request_fields[] = {
		{ "apiName", "Ljava/lang/String;" },
		{ "dataParams", "Ljava/util/Map;" },
		{ "needEcode", "Z" },
		{ "needSession", "Z" },
		{ "version", "Ljava/lang/String;" },
		{ "data", "Ljava/lang/String;" },
		{ "requestLog", "Ljava/lang/String;" },
	};
	for (const auto &field : request_fields)
	{
		auto declared = world.declareField(request.value(), field.name,
			field.signature, false);
		if (!declared.ok())
		{
			return declared.error();
		}
	}
	// The class initialises data to "{}" and requestLog to "".
	if (auto result = declare(world, request.value(), "<init>", "()V", false,
		    [request](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			    for (const auto &entry : { std::make_pair("data", "{}"),
				     std::make_pair("requestLog", "") })
			    {
				    const uint64_t field = java.findField(request.value(),
					    entry.first, "Ljava/lang/String;", false);
				    auto text = java.newString(entry.second);
				    if (!text.ok())
				    {
					    return text.error();
				    }
				    auto written = java.writeInstanceField(call.receiver, field,
					    text.value());
				    if (!written.ok())
				    {
					    return written.error();
				    }
			    }
			    return uint64_t{ 0 };
		    });
		!result.ok())
	{
		return result;
	}
	for (const auto &setter : { std::make_pair("setApiName", "apiName"),
		     std::make_pair("setVersion", "version"),
		     std::make_pair("setData", "data") })
	{
		const uint64_t field = world.findField(request.value(), setter.second,
			"Ljava/lang/String;", false);
		if (field == 0)
		{
			return Error{ ErrorCode::NotFound,
				std::string("MtopRequest.") + setter.second + " is missing" };
		}
		if (auto result = declare(world, request.value(), setter.first,
			    "(Ljava/lang/String;)V", false,
			    [field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
				    if (call.args.empty())
				    {
					    return uint64_t{ 0 };
				    }
				    auto written = java.writeInstanceField(call.receiver, field,
					    call.args[0]);
				    if (!written.ok())
				    {
					    return written.error();
				    }
				    return uint64_t{ 0 };
			    });
			!result.ok())
		{
			return result;
		}
	}
	return {};
}

Result<void> installRegistryBridges(JavaWorld &world)
{
	auto apm = world.findClass("com/alibaba/wireless/security/framework/ApmMonitorAdapter");
	if (apm == 0)
	{
		return Error{ ErrorCode::NotFound, "ApmMonitorAdapter is not installed" };
	}
	return declare(world, apm, "stageEnd",
		"(Ljava/lang/String;Ljava/lang/String;)V", true, returnVoid());
}

} // namespace

Result<void> installDeviceEnvironment(JavaWorld &world)
{
	const auto steps = {
		installContextConstants,
		installBuild,
		installDisplay,
		installWindowExtras,
		installMotion,
		installTelephony,
		installActivityManager,
		installSettings,
		installBinderProbes,
		installIntents,
		installCookieManager,
		installEmptyClasses,
		installMtop,
		installOneMain,
		installRegistryBridges,
	};
	for (const auto step : steps)
	{
		auto installed = step(world);
		if (!installed.ok())
		{
			return installed;
		}
	}
	return {};
}

} // namespace duckemu::user::taobao
