// SPDX-License-Identifier: GPL-2.0-only
#include "jdgs_case/jdgs_env.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#include "common/id/ids.hpp"
#include "jdgs_case/jdgs_cert.h"
#include "runtime/jni/envs/envs.hpp"
#include "runtime/jni/world/java_binder.hpp"
#include "runtime/jni/world/java_world.hpp"
#include "runtime/memory/address_space.hpp"

namespace duckemu::user::jdgs
{
namespace
{

using runtime::jni::ArrayElementKind;
using runtime::jni::InvokeContext;
using runtime::jni::JavaBinder;
using runtime::jni::JavaWorld;
using runtime::jni::ShimHandler;

constexpr const char *kBridge = "com/jd/security/jdguard/core/Bridge";
constexpr const char *kContext = "android/content/Context";
constexpr const char *kPackageManager = "android/content/pm/PackageManager";
constexpr const char *kPackageInfo = "android/content/pm/PackageInfo";
constexpr const char *kSigningInfo = "android/content/pm/SigningInfo";
constexpr const char *kSignature = "android/content/pm/Signature";

// 16.1.50 (versionCode 102918): assets/jdjdg-appkey.txt, getJDGVN() constant,
// and the two asset names Bridge reports to the native.
constexpr const char *kAppKey = "68dd5803-07c3-44d7-8881-0b213bf81b51";
constexpr const char *kJdgVersion = "3.6.5";
constexpr const char *kPicName = "68dd5803-07c3-44d7-8881-0b213bf81b51.jdg.jpg";
constexpr const char *kSecName = "68dd5803-07c3-44d7-8881-0b213bf81b51.jdg.xbt";
constexpr const char *kVersionName = "16.1.50";
constexpr uint64_t kVersionCode = 102918;
constexpr uint64_t kGetSigningCertificates = 0x08000000;
// There is no writable guest home yet; the directory the native asks for only
// needs to be an absolute path.
constexpr const char *kFilesDirPath = "/data/user/0/com.jingdong.app.mall/files";

// Captured request string, device payload and auxiliary arguments of the
// 13.1.0 coral sample; the native accepts the same shape for 16.1.50.
const char *kRequest =
	"POST /client.action avifSupport=1&b9aa898b3bc3febb04a7b0d1cc099577fded30aac89532ad35d0ceaab89be770=&bef=1&build=99208&client=android&clientVersion=13.1.0&ef=1&eid=eidA943781213fsbM1YH2pJ2Rfqo%2B%2B7swN3q3zNWCW%2B5amEMdjBONjMO5gqo6HorEr9UADNI1%2BJqE1zYmDNNluQ2UBN2%2FhIKcsN7ffqaBKB3jQrbejky&ep=%7B%22hdid%22%3A%22JM9F1ywUPwflvMIpYPok0tt5k9kW4ArJEU3lfLhxBqw%3D%22%2C%22ts%22%3A1739247938442%2C%22ridx%22%3A-1%2C%22cipher%22%3A%7B%22area%22%3A%22CV83Cv81DJY3DP8m%22%2C%22d_model%22%3A%22UQv4ZWm0WOm%3D%22%2C%22wifiBssid%22%3A%22dW5hbw93bq%3D%3D%22%2C%22osVersion%22%3A%22CJK%3D%22%2C%22d_brand%22%3A%22H29lZ2nv%22%2C%22screen%22%3A%22Ctu4DMenDNGm%22%2C%22uuid%22%3A%22YzG0CtOmDNczDzK5CWVtEG%3D%3D%22%2C%22aid%22%3A%22YzG0CtOmDNczDzK5CWVtEG%3D%3D%22%2C%22openudid%22%3A%22YzG0CtOmDNczDzK5CWVtEG%3D%3D%22%7D%2C%22ciphertype%22%3A5%2C%22version%22%3A%221.2.0%22%2C%22appname%22%3A%22com.jingdong.app.mall%22%7D&ext=%7B%22prstate%22%3A%220%22%2C%22pvcStu%22%3A%221%22%2C%22cfgExt%22%3A%22%7B%5C%22privacyOffline%5C%22%3A%5C%220%5C%22%7D%22%7D&functionId=search&harmonyOs=0&lang=zh_CN&networkType=wifi&partner=xiaomi001&sdkVersion=29&sign=3b5c882977fdb680c297d34adaf01931&st=1739248022333&sv=100&uemps=2-2-2&x-api-eid-token=jdd01IHFGXYKIKWYJTG3BV5U6TBHKBPYJKQ35JYSSNHEY434T633JS5HWSPQN5KE7PNU7YQXZ7VPOCSLZC2VQSK2SYTIBEYC4PO5OAKVE6GY01234567";
const char *kDevicePayload =
	"coral|-|coral|-|-|-|-|-|-|-|-|-|-|-|-|-|-§§0|1§§-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-|-§§-|53684973568§§-|-|1.0|-§§1|1|-|-|-|-|-|-|-|-|-§§google/coral/coral:10/QD1A.190821.007/5831595:user/release-keys/|-|-|1739164885415|-§§-|-|-|-|-|-|-";
constexpr const char *kArgDfpEid = "";
constexpr const char *kArgPayVersion = "1.0";
constexpr const char *kArgOpenUdid = "00000000-0000-0000-0000-000000000000";
constexpr const char *kArgSdkConfig = "83";

// Bridge.main(101) arguments come from yt.c.D(byte[]):
//   [0] byte[] request   [1] payload   [2] dfp eid   [3] pay version
//   [4] openudid         [5] sdk config   [6] null (ccc inactive)
JavaWorld *g_world = nullptr;

const char *EnvOr(const char *name, const char *fallback)
{
	const char *value = std::getenv(name);
	return value != nullptr ? value : fallback;
}

std::string EnvOrString(const char *name, const char *fallback)
{
	return std::string(EnvOr(name, fallback));
}

Result<uint64_t> makeString(JavaWorld &world, std::string_view text)
{
	return world.newString(text);
}

Result<uint64_t> makeByteArray(JavaWorld &world, const void *data, uint64_t size)
{
	auto array = world.newArray(ArrayElementKind::Byte, size);
	if (!array.ok())
	{
		return array.error();
	}
	auto view = world.arrayView(array.value());
	if (!view.ok())
	{
		return view.error();
	}
	if (size != 0)
	{
		auto written =
			world.memory().write(GuestVA{ view.value().storage }, data, size);
		if (!written.ok())
		{
			return written.error();
		}
	}
	return array.value();
}

ShimHandler returnObject(uint64_t object)
{
	return [object](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return object; };
}

ShimHandler returnString(std::string value)
{
	return [value = std::move(value)](JavaWorld &world, InvokeContext &) -> Result<uint64_t> {
		return makeString(world, value);
	};
}

ShimHandler returnNull()
{
	return [](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; };
}

// libjdg feeds the package certificate through the Java signature API. The
// bytes are the APK's META-INF/JINGDONG.RSA certificate, embedded in
// jdgs_cert.h; JDGS_BAD_CERT substitutes a dummy blob so a test can prove the
// native really inspects it.
ShimHandler returnCertificateBytes()
{
	return [](JavaWorld &world, InvokeContext &) -> Result<uint64_t> {
		static const uint8_t kDummy[32] = { 1 };
		if (std::getenv("JDGS_BAD_CERT") != nullptr)
		{
			return makeByteArray(world, kDummy, sizeof(kDummy));
		}
		return makeByteArray(world, ::jdgs::kSigningCertificate, ::jdgs::kSigningCertificateSize);
	};
}

} // namespace

const char *const kBridgeClass = kBridge;
const char *const kMainSignature = "(I[Ljava/lang/Object;)[Ljava/lang/Object;";
const char *const kPackageName = "com.jingdong.app.mall";
const char *const kApkPath =
	"/data/app/com.jingdong.app.mall-NMPcDqBzdYwQrAm-23cOyQ==/base.apk";

SignParams signParamsFromEnvironment()
{
	SignParams params;
	params.request = EnvOrString("JDGS_REQUEST", kRequest);
	params.device_payload = EnvOrString("JDGS_DEVICE_PAYLOAD", kDevicePayload);
	params.dfp_eid = EnvOrString("JDGS_DFP_EID", kArgDfpEid);
	params.pay_version = EnvOrString("JDGS_PAY_VERSION", kArgPayVersion);
	params.openudid = EnvOrString("JDGS_OPENUDID", kArgOpenUdid);
	params.sdk_config = EnvOrString("JDGS_SDK_CONFIG", kArgSdkConfig);
	return params;
}

Result<void> installEnvironment(JavaWorld &world)
{
	g_world = &world;

	uint64_t bridge = 0;
	uint64_t context = 0;
	uint64_t package_manager = 0;
	uint64_t package_info = 0;
	uint64_t signing_info = 0;
	uint64_t signature = 0;

	JavaBinder binder(world);

	struct ClassSlot
	{
		const char *descriptor;
		uint64_t *slot;
	};
	const ClassSlot classes[] = {
		{ kBridge, &bridge },
		{ kContext, &context },
		{ kPackageManager, &package_manager },
		{ kPackageInfo, &package_info },
		{ kSigningInfo, &signing_info },
		{ kSignature, &signature },
	};
	for (const ClassSlot &entry : classes)
	{
		binder.declareClass(entry.descriptor, entry.slot);
	}

	// The object graph the native walks: Context -> PackageManager ->
	// PackageInfo -> SigningInfo -> Signature[]. Values mirror the app process.
	uint64_t context_object = 0;
	uint64_t package_manager_object = 0;
	uint64_t package_info_object = 0;
	uint64_t signing_info_object = 0;
	uint64_t signature_object = 0;

	struct ObjectSlot
	{
		uint64_t klass;
		uint64_t *slot;
	};
	const ObjectSlot objects[] = {
		{ context, &context_object },
		{ package_manager, &package_manager_object },
		{ package_info, &package_info_object },
		{ signing_info, &signing_info_object },
		{ signature, &signature_object },
	};
	for (const ObjectSlot &entry : objects)
	{
		binder.allocateObject(entry.klass, entry.slot);
	}

	// java.io.File is platform-owned; the project only supplies the path the
	// app's Context.getFilesDir() reports.
	auto files_dir = runtime::jni::envs::newFile(world, kFilesDirPath);
	if (!files_dir.ok())
	{
		return files_dir.error();
	}
	const uint64_t files_dir_object = files_dir.value();

	uint64_t signature_array = 0;
	binder.newArray(
		ArrayElementKind::Object, 1, signature, signature_object, &signature_array);

	// PackageInfo: versionName, versionCode and the signingInfo the native
	// reads back. GET_SIGNING_CERTIFICATES is the flag value the app passes.
	uint64_t version_name_field = 0;
	uint64_t version_code_field = 0;
	uint64_t signing_info_field = 0;
	uint64_t signatures_field = 0;
	uint64_t signing_flags_field = 0;
	binder.declareField(package_info, "versionName", "Ljava/lang/String;", false,
			&version_name_field)
		.declareField(package_info, "versionCode", "I", false, &version_code_field)
		.declareField(package_info, "signingInfo", "Landroid/content/pm/SigningInfo;", false,
			&signing_info_field)
		// Legacy signature list: the native still reads it when it walks the
		// PackageInfo by hand.
		.declareField(package_info, "signatures", "[Landroid/content/pm/Signature;", false,
			&signatures_field)
		.declareField(package_manager, "GET_SIGNING_CERTIFICATES", "I", true,
			&signing_flags_field);

	uint64_t version_name = 0;
	binder.newString(kVersionName, &version_name)
		.writeInstanceField(package_info_object, version_name_field, version_name)
		.writeInstanceField(package_info_object, version_code_field, kVersionCode)
		.writeInstanceField(package_info_object, signing_info_field, signing_info_object)
		.writeInstanceField(package_info_object, signatures_field, signature_array)
		.writeStaticField(package_manager, signing_flags_field, kGetSigningCertificates);

	// Bridge: main is the registered native; the rest are the static host
	// methods the native calls back into.
	binder.declareMethod(bridge, "main", kMainSignature, true, {});
	const auto bridge_methods = {
		std::make_pair("getAppContext",
			std::make_pair("()Landroid/content/Context;", returnObject(context_object))),
		std::make_pair("getAppKey",
			std::make_pair("()Ljava/lang/String;", returnString(kAppKey))),
		std::make_pair("getJDGVN",
			std::make_pair("()Ljava/lang/String;", returnString(kJdgVersion))),
		std::make_pair("getPicName",
			std::make_pair("()Ljava/lang/String;", returnString(kPicName))),
		std::make_pair("getSecName",
			std::make_pair("()Ljava/lang/String;", returnString(kSecName))),
		std::make_pair("getBizNum",
			std::make_pair("(Ljava/lang/String;)Ljava/lang/String;", returnNull())),
	};
	for (const auto &method : bridge_methods)
	{
		binder.declareMethod(
			bridge, method.first, method.second.first, true, method.second.second);
	}

	const auto context_methods = {
		std::make_pair("getPackageName",
			std::make_pair("()Ljava/lang/String;", returnString(kPackageName))),
		std::make_pair("getPackageCodePath",
			std::make_pair("()Ljava/lang/String;", returnString(kApkPath))),
		std::make_pair("getPackageManager",
			std::make_pair("()Landroid/content/pm/PackageManager;",
				returnObject(package_manager_object))),
		std::make_pair("getFilesDir",
			std::make_pair("()Ljava/io/File;", returnObject(files_dir_object))),
	};
	for (const auto &method : context_methods)
	{
		binder.declareMethod(
			context, method.first, method.second.first, false, method.second.second);
	}

	binder.declareMethod(package_manager, "getPackageInfo",
			"(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", false,
			returnObject(package_info_object))
		.declareMethod(signing_info, "getApkContentsSigners",
			"()[Landroid/content/pm/Signature;", false, returnObject(signature_array))
		.declareMethod(signature, "toByteArray", "()[B", false, returnCertificateBytes());
	// The rest of the chain (ByteArrayInputStream, CertificateFactory,
	// Certificate/X509Certificate, File) is the platform pack: it parses the
	// DER these bytes carry.

	return std::move(binder).done();
}

Result<uint64_t> buildInitArguments(std::string_view phase, bool with_elapsed)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "jdgs: installEnvironment has not run" };
	}
	JavaWorld &world = *g_world;
	auto text = makeString(world, phase);
	if (!text.ok())
	{
		return text.error();
	}
	auto array = world.newArray(
		ArrayElementKind::Object, with_elapsed ? 2 : 1, world.objectClass(), 0);
	if (!array.ok())
	{
		return array.error();
	}
	auto written = world.writeArrayElement(array.value(), 0, text.value());
	if (!written.ok())
	{
		return written.error();
	}
	return array.value();
}

Result<uint64_t> buildSignArguments(const SignParams &params)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "jdgs: installEnvironment has not run" };
	}
	JavaWorld &world = *g_world;
	auto request = makeByteArray(world, params.request.data(), params.request.size());
	if (!request.ok())
	{
		return request.error();
	}
	auto payload = makeString(world, params.device_payload);
	if (!payload.ok())
	{
		return payload.error();
	}
	auto eid = makeString(world, params.dfp_eid);
	if (!eid.ok())
	{
		return eid.error();
	}
	auto pay = makeString(world, params.pay_version);
	if (!pay.ok())
	{
		return pay.error();
	}
	auto udid = makeString(world, params.openudid);
	if (!udid.ok())
	{
		return udid.error();
	}
	auto sdk = makeString(world, params.sdk_config);
	if (!sdk.ok())
	{
		return sdk.error();
	}
	auto array = world.newArray(ArrayElementKind::Object, 7, world.objectClass(), 0);
	if (!array.ok())
	{
		return array.error();
	}
	const uint64_t elements[] = { request.value(),
		payload.value(),
		eid.value(),
		pay.value(),
		udid.value(),
		sdk.value() };
	for (uint64_t index = 0; index < 6; ++index)
	{
		auto written = world.writeArrayElement(array.value(), index, elements[index]);
		if (!written.ok())
		{
			return written.error();
		}
	}
	return array.value();
}

Result<std::string> classNameOf(uint64_t object)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "jdgs: installEnvironment has not run" };
	}
	const uint64_t klass = g_world->classOf(object);
	if (klass == 0)
	{
		return Error{ ErrorCode::InvalidArgument, "jdgs: object is not a Java object" };
	}
	return std::string(g_world->descriptorOfClass(klass));
}

Result<uint64_t> readResultElement(uint64_t object)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "jdgs: installEnvironment has not run" };
	}
	return g_world->readArrayElement(object, 0);
}

Result<std::string> readResultString(uint64_t object)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "jdgs: installEnvironment has not run" };
	}
	auto element = g_world->readArrayElement(object, 0);
	if (!element.ok())
	{
		return element.error();
	}
	return g_world->stringUtf8(element.value());
}

Result<int32_t> readIntegerField(uint64_t object)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "jdgs: installEnvironment has not run" };
	}
	return runtime::jni::envs::integerValue(*g_world, object);
}

} // namespace duckemu::user::jdgs
