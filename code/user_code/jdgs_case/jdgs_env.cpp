// SPDX-License-Identifier: GPL-2.0-only
#include "jdgs_case/jdgs_env.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#include "common/id/ids.hpp"
#include "jdgs_case/jdgs_cert.h"
#include "runtime/jni/world/java_world.hpp"
#include "runtime/memory/address_space.hpp"

namespace duckemu::user::jdgs
{
namespace
{

using runtime::jni::ArrayElementKind;
using runtime::jni::InvokeContext;
using runtime::jni::JavaWorld;
using runtime::jni::ShimHandler;

constexpr const char *kBridge = "com/jd/security/jdguard/core/Bridge";
constexpr const char *kContext = "android/content/Context";
constexpr const char *kPackageManager = "android/content/pm/PackageManager";
constexpr const char *kPackageInfo = "android/content/pm/PackageInfo";
constexpr const char *kSigningInfo = "android/content/pm/SigningInfo";
constexpr const char *kSignature = "android/content/pm/Signature";
constexpr const char *kCertificateFactory = "java/security/cert/CertificateFactory";
constexpr const char *kCertificate = "java/security/cert/Certificate";
constexpr const char *kByteArrayInputStream = "java/io/ByteArrayInputStream";
constexpr const char *kFile = "java/io/File";
constexpr const char *kInteger = "java/lang/Integer";

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
uint64_t g_integer_value_field = 0;

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

	auto bridge = world.declareClass(kBridge, "");
	if (!bridge.ok())
	{
		return bridge.error();
	}
	auto context = world.declareClass(kContext, "");
	if (!context.ok())
	{
		return context.error();
	}
	auto package_manager = world.declareClass(kPackageManager, "");
	if (!package_manager.ok())
	{
		return package_manager.error();
	}
	auto package_info = world.declareClass(kPackageInfo, "");
	if (!package_info.ok())
	{
		return package_info.error();
	}
	auto signing_info = world.declareClass(kSigningInfo, "");
	if (!signing_info.ok())
	{
		return signing_info.error();
	}
	auto signature = world.declareClass(kSignature, "");
	if (!signature.ok())
	{
		return signature.error();
	}
	auto certificate_factory = world.declareClass(kCertificateFactory, "");
	if (!certificate_factory.ok())
	{
		return certificate_factory.error();
	}
	auto certificate = world.declareClass(kCertificate, "");
	if (!certificate.ok())
	{
		return certificate.error();
	}
	auto byte_array_input_stream = world.declareClass(kByteArrayInputStream, "");
	if (!byte_array_input_stream.ok())
	{
		return byte_array_input_stream.error();
	}
	auto file = world.declareClass(kFile, "");
	if (!file.ok())
	{
		return file.error();
	}
	auto integer = world.declareClass(kInteger, "");
	if (!integer.ok())
	{
		return integer.error();
	}

	// The object graph the native walks: Context -> PackageManager ->
	// PackageInfo -> SigningInfo -> Signature[]. Values mirror the app process.
	auto context_object = world.allocateObject(context.value());
	if (!context_object.ok())
	{
		return context_object.error();
	}
	auto package_manager_object = world.allocateObject(package_manager.value());
	if (!package_manager_object.ok())
	{
		return package_manager_object.error();
	}
	auto package_info_object = world.allocateObject(package_info.value());
	if (!package_info_object.ok())
	{
		return package_info_object.error();
	}
	auto signing_info_object = world.allocateObject(signing_info.value());
	if (!signing_info_object.ok())
	{
		return signing_info_object.error();
	}
	auto signature_object = world.allocateObject(signature.value());
	if (!signature_object.ok())
	{
		return signature_object.error();
	}
	auto certificate_factory_object = world.allocateObject(certificate_factory.value());
	if (!certificate_factory_object.ok())
	{
		return certificate_factory_object.error();
	}
	auto certificate_object = world.allocateObject(certificate.value());
	if (!certificate_object.ok())
	{
		return certificate_object.error();
	}
	auto files_dir_object = world.allocateObject(file.value());
	if (!files_dir_object.ok())
	{
		return files_dir_object.error();
	}
	auto signature_array = world.newArray(
		ArrayElementKind::Object, 1, signature.value(), signature_object.value());
	if (!signature_array.ok())
	{
		return signature_array.error();
	}

	// java.lang.Integer carries the init/errno status in an instance field, so
	// main(103) can answer an Integer object instead of a string.
	auto integer_value_field = world.declareField(integer.value(), "value", "I", false);
	if (!integer_value_field.ok())
	{
		return integer_value_field.error();
	}
	g_integer_value_field = integer_value_field.value();
	auto integer_init = world.declareMethod(integer.value(), "<init>", "(I)V", false,
		[value_field = integer_value_field.value()](JavaWorld &java,
			InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument,
					"Integer.<init> expects one int" };
			}
			auto written =
				java.writeInstanceField(call.receiver, value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return uint64_t{ 0 };
		});
	if (!integer_init.ok())
	{
		return integer_init.error();
	}

	// PackageInfo: versionName, versionCode and the signingInfo the native
	// reads back. GET_SIGNING_CERTIFICATES is the flag value the app passes.
	auto version_name_field =
		world.declareField(package_info.value(), "versionName", "Ljava/lang/String;", false);
	if (!version_name_field.ok())
	{
		return version_name_field.error();
	}
	auto version_code_field = world.declareField(package_info.value(), "versionCode", "I", false);
	if (!version_code_field.ok())
	{
		return version_code_field.error();
	}
	auto signing_info_field = world.declareField(
		package_info.value(), "signingInfo", "Landroid/content/pm/SigningInfo;", false);
	if (!signing_info_field.ok())
	{
		return signing_info_field.error();
	}
	// Legacy signature list: the native still reads it when it walks the
	// PackageInfo by hand.
	auto signatures_field = world.declareField(
		package_info.value(), "signatures", "[Landroid/content/pm/Signature;", false);
	if (!signatures_field.ok())
	{
		return signatures_field.error();
	}
	auto signing_flags_field = world.declareField(
		package_manager.value(), "GET_SIGNING_CERTIFICATES", "I", true);
	if (!signing_flags_field.ok())
	{
		return signing_flags_field.error();
	}

	auto version_name = makeString(world, kVersionName);
	if (!version_name.ok())
	{
		return version_name.error();
	}
	auto filled = world.writeInstanceField(
		package_info_object.value(), version_name_field.value(), version_name.value());
	if (!filled.ok())
	{
		return filled.error();
	}
	filled = world.writeInstanceField(
		package_info_object.value(), version_code_field.value(), kVersionCode);
	if (!filled.ok())
	{
		return filled.error();
	}
	filled = world.writeInstanceField(package_info_object.value(),
		signing_info_field.value(),
		signing_info_object.value());
	if (!filled.ok())
	{
		return filled.error();
	}
	filled = world.writeInstanceField(
		package_info_object.value(), signatures_field.value(), signature_array.value());
	if (!filled.ok())
	{
		return filled.error();
	}
	filled = world.writeStaticField(
		package_manager.value(), signing_flags_field.value(), kGetSigningCertificates);
	if (!filled.ok())
	{
		return filled.error();
	}

	// Bridge: main is the registered native; the rest are the static host
	// methods the native calls back into.
	auto main = world.declareMethod(bridge.value(), "main", kMainSignature, true, {});
	if (!main.ok())
	{
		return main.error();
	}
	auto bridge_methods = {
		std::make_pair("getAppContext",
			std::make_pair("()Landroid/content/Context;", returnObject(context_object.value()))),
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
		auto declared = world.declareMethod(bridge.value(),
			method.first,
			method.second.first,
			true,
			method.second.second);
		if (!declared.ok())
		{
			return declared.error();
		}
	}

	auto context_methods = {
		std::make_pair("getPackageName",
			std::make_pair("()Ljava/lang/String;", returnString(kPackageName))),
		std::make_pair("getPackageCodePath",
			std::make_pair("()Ljava/lang/String;", returnString(kApkPath))),
		std::make_pair("getPackageManager",
			std::make_pair("()Landroid/content/pm/PackageManager;",
				returnObject(package_manager_object.value()))),
		std::make_pair("getFilesDir",
			std::make_pair("()Ljava/io/File;", returnObject(files_dir_object.value()))),
	};
	for (const auto &method : context_methods)
	{
		auto declared = world.declareMethod(
			context.value(), method.first, method.second.first, false, method.second.second);
		if (!declared.ok())
		{
			return declared.error();
		}
	}

	auto package_manager_get_package_info = world.declareMethod(package_manager.value(),
		"getPackageInfo",
		"(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",
		false,
		returnObject(package_info_object.value()));
	if (!package_manager_get_package_info.ok())
	{
		return package_manager_get_package_info.error();
	}
	auto get_signing_info = world.declareMethod(signing_info.value(),
		"getApkContentsSigners",
		"()[Landroid/content/pm/Signature;",
		false,
		returnObject(signature_array.value()));
	if (!get_signing_info.ok())
	{
		return get_signing_info.error();
	}
	auto signature_to_byte_array = world.declareMethod(signature.value(),
		"toByteArray",
		"()[B",
		false,
		returnCertificateBytes());
	if (!signature_to_byte_array.ok())
	{
		return signature_to_byte_array.error();
	}
	auto certificate_factory_get_instance = world.declareMethod(certificate_factory.value(),
		"getInstance",
		"(Ljava/lang/String;)Ljava/security/cert/CertificateFactory;",
		true,
		returnObject(certificate_factory_object.value()));
	if (!certificate_factory_get_instance.ok())
	{
		return certificate_factory_get_instance.error();
	}
	auto generate_certificate = world.declareMethod(certificate_factory.value(),
		"generateCertificate",
		"(Ljava/io/InputStream;)Ljava/security/cert/Certificate;",
		false,
		returnObject(certificate_object.value()));
	if (!generate_certificate.ok())
	{
		return generate_certificate.error();
	}
	auto certificate_get_encoded = world.declareMethod(certificate.value(),
		"getEncoded",
		"()[B",
		false,
		returnCertificateBytes());
	if (!certificate_get_encoded.ok())
	{
		return certificate_get_encoded.error();
	}
	// The native only checks that a stream exists; CertificateFactory answers
	// from the embedded certificate, so the stream contents are unused.
	auto stream_init = world.declareMethod(byte_array_input_stream.value(), "<init>", "([B)V",
		false,
		[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; });
	if (!stream_init.ok())
	{
		return stream_init.error();
	}
	auto file_get_absolute_path = world.declareMethod(file.value(),
		"getAbsolutePath",
		"()Ljava/lang/String;",
		false,
		returnString(kFilesDirPath));
	if (!file_get_absolute_path.ok())
	{
		return file_get_absolute_path.error();
	}

	return {};
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
	if (g_world == nullptr || g_integer_value_field == 0)
	{
		return Error{ ErrorCode::InvalidArgument, "jdgs: installEnvironment has not run" };
	}
	auto value = g_world->readInstanceField(object, g_integer_value_field);
	if (!value.ok())
	{
		return value.error();
	}
	return static_cast<int32_t>(value.value());
}

} // namespace duckemu::user::jdgs
