// SPDX-License-Identifier: GPL-2.0-only
#include "szstone_case/szstone_env.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/error/error_code.hpp"
#include "common/id/ids.hpp"
#include "runtime/instance/runtime_instance.hpp"
#include "runtime/jni/world/java_binder.hpp"
#include "runtime/jni/world/java_world.hpp"
#include "runtime/memory/address_space.hpp"
#include "szstone_case/szstone_cert.h"

namespace duckemu::user::szstone
{
namespace
{

using runtime::jni::ArrayElementKind;
using runtime::jni::InvokeContext;
using runtime::jni::JavaBinder;
using runtime::jni::JavaWorld;
using runtime::jni::ShimHandler;

// Java classes libszstone's lf() walks. Every name here is one the verbose
// JNI trace on the device actually asked for (SzStoneTest -Ddu.verbose).
constexpr const char *kActivityThread = "android/app/ActivityThread";
constexpr const char *kApplication = "android/app/Application";
constexpr const char *kPackageManager = "android/content/pm/PackageManager";
constexpr const char *kPackageInfo = "android/content/pm/PackageInfo";
constexpr const char *kSignature = "android/content/pm/Signature";
constexpr const char *kContentResolver = "android/content/ContentResolver";
constexpr const char *kSettingsSecure = "android/provider/Settings$Secure";
constexpr const char *kCertificateFactory = "java/security/cert/CertificateFactory";
constexpr const char *kX509Certificate = "java/security/cert/X509Certificate";
constexpr const char *kX500Principal = "javax/security/auth/x500/X500Principal";
constexpr const char *kBigInteger = "java/math/BigInteger";
constexpr const char *kByteArrayInputStream = "java/io/ByteArrayInputStream";

// Device values. They are project constants, not framework defaults; each can
// be overridden for a different profile.
constexpr const char *kPackageName = "com.shizhuang.duapp";
// The profile blob's dadir field on the capture.
constexpr const char *kCodePath =
	"/data/app/~~jKTCowhdea931IMmF_a-DA==/com.shizhuang.duapp-3R6fyRkBVCZKmrfOBdhqrA==/base.apk";
// Settings.Secure.getString("android_id") on the capture device.
constexpr const char *kAndroidId = "7a1c2569212074a4";
// Settings.Secure.getString("bluetooth_address") on the same device.
constexpr const char *kBluetoothAddress = "A4:50:46:9D:44:1E";
constexpr const char *kSubjectName =
	"CN=Jerry Ching,OU=R&D,O=Hupu,L=HongKou,ST=ShangHai,C=CN";
constexpr const char *kSerialHex = "4fb9d5a8";

JavaWorld *g_world = nullptr;
uint64_t g_activity_thread = 0;
uint64_t g_application = 0;
uint64_t g_package_manager = 0;
uint64_t g_package_info = 0;
uint64_t g_signature = 0;
uint64_t g_content_resolver = 0;
uint64_t g_certificate_factory = 0;
uint64_t g_x509_certificate = 0;
uint64_t g_x500_principal = 0;
uint64_t g_big_integer = 0;
uint64_t g_signature_array = 0;
uint64_t g_certificate_bytes = 0;

const char *EnvOr(const char *name, const char *fallback)
{
	const char *value = std::getenv(name);
	return value != nullptr ? value : fallback;
}

bool debugEnabled()
{
	return std::getenv("SZSTONE_DEBUG") != nullptr;
}

ShimHandler returnObject(const char *label, uint64_t object)
{
	return [label, object](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
		if (debugEnabled())
		{
			std::fprintf(stderr, "szstone shim %s -> object\n", label);
		}
		return object;
	};
}

ShimHandler returnString(std::string value)
{
	return [value = std::move(value)](JavaWorld &world, InvokeContext &) -> Result<uint64_t> {
		if (debugEnabled())
		{
			std::fprintf(stderr, "szstone shim returns string len=%zu '%s'\n",
				value.size(), value.substr(0, 48).c_str());
		}
		return world.newString(value);
	};
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
		auto written = world.memory().write(GuestVA{ view.value().storage }, data, size);
		if (!written.ok())
		{
			return written.error();
		}
	}
	return array;
}

// Android's Signature.toCharsString() is upper-case hex of the DER bytes
// (android.content.pm.Signature HEX_DIGITS). The exact case matters: the
// native feeds the returned string into a hash.
std::string certHexString()
{
	const char *digits = std::getenv("SZSTONE_CERT_LOWER") != nullptr
		? "0123456789abcdef"
		: "0123456789ABCDEF";
	std::string text;
	text.reserve(kSigningCertificateSize * 2);
	for (uint64_t index = 0; index < kSigningCertificateSize; ++index)
	{
		const uint8_t byte = kSigningCertificate[index];
		text.push_back(digits[byte >> 4]);
		text.push_back(digits[byte & 0x0F]);
	}
	return text;
}

} // namespace

const char *const kSzSdkClass = "com/shizhuang/stone/main/SzSdk";
const char *const kLfSignature = "([BZZZ)[B";

namespace
{

Result<uint64_t> buildBlobArgument(std::string_view bytes)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "the Java environment is not installed" };
	}
	return makeByteArray(*g_world, bytes.data(), bytes.size());
}

Result<std::vector<uint8_t> > readByteArray(runtime::RuntimeInstance &instance, uint64_t handle)
{
	if (g_world == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "the Java environment is not installed" };
	}
	auto object = instance.jniResolveHandle(handle);
	if (!object.ok())
	{
		return object.error();
	}
	auto view = g_world->arrayView(object.value());
	if (!view.ok())
	{
		return view.error();
	}
	if (view.value().kind != ArrayElementKind::Byte)
	{
		return Error{ ErrorCode::InvalidArgument, "the result is not a byte array" };
	}
	std::vector<uint8_t> bytes(view.value().length);
	if (!bytes.empty())
	{
		auto read = g_world->memory().read(
			GuestVA{ view.value().storage }, bytes.data(), bytes.size());
		if (!read.ok())
		{
			return read.error();
		}
	}
	return bytes;
}

} // namespace

Result<std::vector<uint8_t> > invokeLf(runtime::RuntimeInstance &instance,
	std::string_view blob,
	const std::array<uint64_t, 3> &z)
{
	auto argument = buildBlobArgument(blob);
	if (!argument.ok())
	{
		return argument.error();
	}
	const std::array<uint64_t, 8> arguments = {
		argument.value(), z[0], z[1], z[2]
	};
	auto called = instance.callRegisteredNative(kSzSdkClass, "lf", kLfSignature, arguments);
	if (!called.ok())
	{
		return called.error();
	}
	if (called.value() == 0)
	{
		return Error{ ErrorCode::Unsupported, "SzSdk.lf returned null" };
	}
	return readByteArray(instance, called.value());
}

Result<void> installEnvironment(JavaWorld &world)
{
	g_world = &world;
	JavaBinder binder(world);

	// -- target .so -------------------------------------------------------
	uint64_t szsdk = 0;
	binder.declareClass(kSzSdkClass, &szsdk);
	// RegisterNatives carries no staticness, so every entry libszstone
	// registers must already be declared here and match its Java signature.
	struct NativeMethod
	{
		const char *name;
		const char *signature;
	};
	constexpr NativeMethod kNativeMethods[] = {
		{ "lf", "([BZZZ)[B" },
		{ "ltn", "(Ljava/lang/String;)Ljava/lang/String;" },
		{ "ltk", "(Ljava/lang/String;Ljava/lang/String;J)Ljava/lang/String;" },
		{ "adi", "(Ljava/lang/String;ILjava/lang/String;I)Ljava/lang/String;" },
		{ "gdk", "([B)[B" },
		{ "vt", "([B)Ljava/lang/String;" },
		{ "vdi", "([BLjava/lang/String;ILjava/lang/String;I)I" },
		{ "gadi", "()Ljava/lang/String;" },
		{ "upt", "(Ljava/lang/String;ILjava/lang/String;I)I" },
		{ "gst", "(Ljava/lang/String;J)Ljava/lang/String;" },
	};
	for (const NativeMethod &method : kNativeMethods)
	{
		binder.declareMethod(
			szsdk, method.name, method.signature, /*is_static=*/true, {});
	}

	// -- Android object graph ---------------------------------------------
	uint64_t activity_thread_class = 0;
	uint64_t application_class = 0;
	uint64_t package_manager_class = 0;
	uint64_t package_info_class = 0;
	uint64_t signature_class = 0;
	uint64_t content_resolver_class = 0;
	uint64_t settings_secure_class = 0;
	uint64_t certificate_factory_class = 0;
	uint64_t x509_class = 0;
	uint64_t x500_class = 0;
	uint64_t big_integer_class = 0;
	uint64_t byte_stream_class = 0;
	struct ClassSlot
	{
		const char *descriptor;
		uint64_t *slot;
	};
	const ClassSlot classes[] = {
		{ kActivityThread, &activity_thread_class },
		{ kApplication, &application_class },
		{ kPackageManager, &package_manager_class },
		{ kPackageInfo, &package_info_class },
		{ kSignature, &signature_class },
		{ kContentResolver, &content_resolver_class },
		{ kSettingsSecure, &settings_secure_class },
		{ kCertificateFactory, &certificate_factory_class },
		{ kX509Certificate, &x509_class },
		{ kX500Principal, &x500_class },
		{ kBigInteger, &big_integer_class },
		{ kByteArrayInputStream, &byte_stream_class },
	};
	for (const ClassSlot &entry : classes)
	{
		binder.declareClass(entry.descriptor, entry.slot);
	}

	// The object instances the native walks. They carry no state the shims
	// need, so one instance each is enough.
	struct Instance
	{
		uint64_t klass;
		uint64_t *slot;
	};
	const Instance instances[] = {
		{ activity_thread_class, &g_activity_thread },
		{ application_class, &g_application },
		{ package_manager_class, &g_package_manager },
		{ package_info_class, &g_package_info },
		{ signature_class, &g_signature },
		{ content_resolver_class, &g_content_resolver },
		{ certificate_factory_class, &g_certificate_factory },
		{ x509_class, &g_x509_certificate },
		{ x500_class, &g_x500_principal },
		{ big_integer_class, &g_big_integer },
	};
	for (const Instance &instance : instances)
	{
		binder.allocateObject(instance.klass, instance.slot);
	}

	uint64_t signatures_field = 0;
	binder.newArray(ArrayElementKind::Object, 1, signature_class, g_signature, &g_signature_array)
		.assign(makeByteArray(world, kSigningCertificate, kSigningCertificateSize),
			&g_certificate_bytes)
		// PackageInfo.signatures is the only field lf() reads.
		.declareField(package_info_class, "signatures",
			"[Landroid/content/pm/Signature;", false, &signatures_field)
		.writeInstanceField(g_package_info, signatures_field, g_signature_array);

	// -- ActivityThread / Application / Context ---------------------------
	binder.declareMethod(activity_thread_class, "currentActivityThread",
			"()Landroid/app/ActivityThread;", true,
			returnObject("currentActivityThread", g_activity_thread))
		.declareMethod(activity_thread_class, "getApplication",
			"()Landroid/app/Application;", false,
			returnObject("getApplication", g_application))
		.declareMethod(application_class, "getPackageName", "()Ljava/lang/String;", false,
			returnString(kPackageName))
		.declareMethod(application_class, "getPackageCodePath", "()Ljava/lang/String;", false,
			returnString(EnvOr("SZSTONE_APK_PATH", kCodePath)))
		.declareMethod(application_class, "getPackageManager",
			"()Landroid/content/pm/PackageManager;", false,
			returnObject("getPackageManager", g_package_manager))
		.declareMethod(application_class, "getContentResolver",
			"()Landroid/content/ContentResolver;", false,
			returnObject("getContentResolver", g_content_resolver))
		// -- PackageManager --------------------------------------------
		.declareMethod(package_manager_class, "getPackageInfo",
			"(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", false,
			returnObject("getPackageInfo", g_package_info))
		// -- Signature -------------------------------------------------
		.declareMethod(signature_class, "toCharsString", "()Ljava/lang/String;", false,
			returnString(certHexString()))
		.declareMethod(signature_class, "toByteArray", "()[B", false,
			[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
				if (debugEnabled())
				{
					std::fprintf(stderr, "szstone shim toByteArray -> %llu bytes\n",
						static_cast<unsigned long long>(kSigningCertificateSize));
				}
				return makeByteArray(
					java, kSigningCertificate, kSigningCertificateSize);
			})
		// -- Settings.Secure -------------------------------------------
		.declareMethod(settings_secure_class, "getString",
			"(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;", true,
			[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
				if (call.args.size() != 2)
				{
					return Error{ ErrorCode::InvalidArgument,
						"Settings.Secure.getString expects (resolver, key)" };
				}
				auto key = java.stringUtf8(call.args[1]);
				if (!key.ok())
				{
					return key.error();
				}
				if (debugEnabled())
				{
					std::fprintf(stderr, "szstone shim Settings.Secure.getString('%s')\n",
						key.value().c_str());
				}
				if (key.value() == "android_id")
				{
					return java.newString(
						EnvOr("SZSTONE_ANDROID_ID", kAndroidId));
				}
				if (key.value() == "bluetooth_address")
				{
					return java.newString(EnvOr(
						"SZSTONE_BT_ADDRESS", kBluetoothAddress));
				}
				return uint64_t{ 0 };
			})
		// -- Certificate chain -----------------------------------------
		.declareMethod(certificate_factory_class, "getInstance",
			"(Ljava/lang/String;)Ljava/security/cert/CertificateFactory;", true,
			returnObject("CertificateFactory.getInstance", g_certificate_factory))
		.declareMethod(certificate_factory_class, "generateCertificate",
			"(Ljava/io/InputStream;)Ljava/security/cert/Certificate;", false,
			returnObject("generateCertificate", g_x509_certificate))
		.declareMethod(byte_stream_class, "<init>", "([B)V", false,
			[](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
				if (debugEnabled())
				{
					std::fprintf(stderr, "szstone shim ByteArrayInputStream.<init>\n");
				}
				return uint64_t{ 0 };
			})
		.declareMethod(x509_class, "getSubjectX500Principal",
			"()Ljavax/security/auth/x500/X500Principal;", false,
			returnObject("getSubjectX500Principal", g_x500_principal))
		.declareMethod(x509_class, "getSerialNumber", "()Ljava/math/BigInteger;", false,
			returnObject("getSerialNumber", g_big_integer))
		.declareMethod(x500_class, "getName", "()Ljava/lang/String;", false,
			returnString(kSubjectName))
		.declareMethod(big_integer_class, "toString", "(I)Ljava/lang/String;", false,
			[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
				// The native asks for radix 16; the captured certificate serial is
				// all this object can ever answer.
				if (debugEnabled())
				{
					std::fprintf(stderr, "szstone shim BigInteger.toString(%llu)\n",
						static_cast<unsigned long long>(call.args.empty() ? 0 : call.args[0]));
				}
				if (call.args.size() != 1 || call.args[0] != 16)
				{
					return Error{ ErrorCode::Unsupported,
						"BigInteger.toString: only radix 16 is declared" };
				}
				return java.newString(kSerialHex);
			});

	return std::move(binder).done();
}

} // namespace duckemu::user::szstone
