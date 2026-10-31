// SPDX-License-Identifier: GPL-2.0-only
// Unit tests for the platform packs that used to be hand-written in target
// projects: Integer, File, and the X.509 certificate chain. No guest code.

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "common/id/ids.hpp"
#include "runtime/clock/runtime_clock.hpp"
#include "runtime/jni/envs/envs.hpp"
#include "runtime/jni/world/java_world.hpp"
#include "runtime/memory/address_space.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;
using namespace duckemu::runtime::jni;

namespace
{

std::vector<uint8_t> concat(std::initializer_list<std::vector<uint8_t>> parts)
{
	std::vector<uint8_t> out;
	for (const auto &part : parts)
	{
		out.insert(out.end(), part.begin(), part.end());
	}
	return out;
}

std::vector<uint8_t> tlv(uint8_t tag, const std::vector<uint8_t> &content)
{
	std::vector<uint8_t> out{ tag };
	if (content.size() < 0x80)
	{
		out.push_back(static_cast<uint8_t>(content.size()));
	}
	else
	{
		size_t length = content.size();
		std::vector<uint8_t> bytes;
		while (length != 0)
		{
			bytes.insert(bytes.begin(), static_cast<uint8_t>(length & 0xFF));
			length >>= 8;
		}
		out.push_back(static_cast<uint8_t>(0x80 | bytes.size()));
		out.insert(out.end(), bytes.begin(), bytes.end());
	}
	out.insert(out.end(), content.begin(), content.end());
	return out;
}

// The smallest Certificate the pack parses: one RDN (CN=Test User) and serial
// 0x1234. The signature/issuer/validity elements are empty; the pack only
// checks their tags before reading the subject.
std::vector<uint8_t> makeCertificate()
{
	const auto cn_oid = tlv(0x06, { 0x55, 0x04, 0x03 });
	const auto cn_value = tlv(0x0C, { 'T', 'e', 's', 't', ' ', 'U', 's', 'e', 'r' });
	const auto attribute = tlv(0x30, concat({ cn_oid, cn_value }));
	const auto subject = tlv(0x30, tlv(0x31, attribute));
	const auto serial = tlv(0x02, { 0x12, 0x34 });
	const auto empty = tlv(0x30, {});
	const auto tbs = tlv(0x30, concat({ serial, empty, empty, empty, subject }));
	return tlv(0x30, tbs);
}

Result<uint64_t> invoke(JavaWorld &world,
	uint64_t klass,
	const char *name,
	const char *signature,
	bool is_static,
	InvokeContext call)
{
	const uint64_t id = world.findMethod(klass, name, signature, is_static);
	const MethodRecord *record = id == 0 ? nullptr : world.method(id);
	if (record == nullptr || !record->shim)
	{
		return Error{ ErrorCode::NotFound,
			std::string("no shim for ") + name + signature };
	}
	return record->shim(world, call);
}

Result<uint64_t> makeByteArray(JavaWorld &world, const std::vector<uint8_t> &bytes)
{
	auto array = world.newArray(ArrayElementKind::Byte, bytes.size());
	if (!array.ok())
	{
		return array.error();
	}
	auto view = world.arrayView(array.value());
	if (!view.ok())
	{
		return view.error();
	}
	if (!bytes.empty())
	{
		auto written =
			world.memory().write(GuestVA{ view.value().storage }, bytes.data(), bytes.size());
		if (!written.ok())
		{
			return written.error();
		}
	}
	return array.value();
}

Result<std::vector<uint8_t>> readByteArray(JavaWorld &world, uint64_t object)
{
	auto view = world.arrayView(object);
	if (!view.ok())
	{
		return view.error();
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(view.value().length));
	if (!bytes.empty())
	{
		auto read =
			world.memory().read(GuestVA{ view.value().storage }, bytes.data(), bytes.size());
		if (!read.ok())
		{
			return read.error();
		}
	}
	return bytes;
}

} // namespace

int main()
{
	RuntimeMemory memory;
	RuntimeClock clock;
	JavaWorld world(memory, clock);
	CHECK_RESULT(world.initialize());
	CHECK_RESULT(envs::installAll(world));

	// java.lang.Integer: <init>(I) stores the value; integerValue reads it.
	const uint64_t integer_class = world.findClass("java/lang/Integer");
	CHECK(integer_class != 0);
	auto integer = world.allocateObject(integer_class);
	CHECK_RESULT(integer);
	if (integer.ok())
	{
		InvokeContext constructor;
		constructor.receiver = integer.value();
		constructor.args = { static_cast<uint64_t>(static_cast<int64_t>(-5)) };
		CHECK_RESULT(invoke(world, integer_class, "<init>", "(I)V", false, constructor));
		auto value = envs::integerValue(world, integer.value());
		CHECK_RESULT(value);
		if (value.ok())
		{
			CHECK(value.value() == -5);
		}
	}

	// java.io.File: newFile + getAbsolutePath answer the stored path.
	auto file = envs::newFile(world, "/data/user/0/com.example/files");
	CHECK_RESULT(file);
	if (file.ok())
	{
		InvokeContext call;
		call.receiver = file.value();
		auto path = invoke(world, world.classOf(file.value()), "getAbsolutePath",
			"()Ljava/lang/String;", false, call);
		CHECK_RESULT(path);
		if (path.ok())
		{
			auto text = world.stringUtf8(path.value());
			CHECK_RESULT(text);
			if (text.ok())
			{
				CHECK(text.value() == "/data/user/0/com.example/files");
			}
		}
	}

	// CertificateFactory -> generateCertificate -> X509Certificate.
	const std::vector<uint8_t> der = makeCertificate();
	auto bytes = makeByteArray(world, der);
	CHECK_RESULT(bytes);

	const uint64_t stream_class = world.findClass("java/io/ByteArrayInputStream");
	CHECK(stream_class != 0);
	auto stream = world.allocateObject(stream_class);
	CHECK_RESULT(stream);
	uint64_t certificate = 0;
	if (bytes.ok() && stream.ok())
	{
		InvokeContext stream_init;
		stream_init.receiver = stream.value();
		stream_init.args = { bytes.value() };
		CHECK_RESULT(invoke(world, stream_class, "<init>", "([B)V", false, stream_init));

		const uint64_t factory_class = world.findClass("java/security/cert/CertificateFactory");
		CHECK(factory_class != 0);
		auto algorithm = world.newString("X.509");
		CHECK_RESULT(algorithm);
		if (algorithm.ok() && factory_class != 0)
		{
			InvokeContext get_instance;
			get_instance.receiver = factory_class;
			get_instance.args = { algorithm.value() };
			auto factory = invoke(world, factory_class, "getInstance",
				"(Ljava/lang/String;)Ljava/security/cert/CertificateFactory;", true,
				get_instance);
			CHECK_RESULT(factory);
			if (factory.ok())
			{
				InvokeContext generate;
				generate.receiver = factory.value();
				generate.args = { stream.value() };
				auto generated = invoke(world, factory_class, "generateCertificate",
					"(Ljava/io/InputStream;)Ljava/security/cert/Certificate;", false,
					generate);
				CHECK_RESULT(generated);
				if (generated.ok())
				{
					certificate = generated.value();
				}
			}
		}
	}

	const uint64_t x509_class = world.findClass("java/security/cert/X509Certificate");
	const uint64_t certificate_class = world.findClass("java/security/cert/Certificate");
	const uint64_t principal_class =
		world.findClass("javax/security/auth/x500/X500Principal");
	const uint64_t big_integer_class = world.findClass("java/math/BigInteger");
	CHECK(x509_class != 0);
	CHECK(certificate_class != 0);
	CHECK(principal_class != 0);
	CHECK(big_integer_class != 0);

	if (certificate != 0)
	{
		// getSubjectX500Principal().getName() renders the RFC 4514 string.
		InvokeContext principal_call;
		principal_call.receiver = certificate;
		auto principal = invoke(world, x509_class, "getSubjectX500Principal",
			"()Ljavax/security/auth/x500/X500Principal;", false, principal_call);
		CHECK_RESULT(principal);
		if (principal.ok())
		{
			InvokeContext name_call;
			name_call.receiver = principal.value();
			auto name = invoke(world, principal_class, "getName",
				"()Ljava/lang/String;", false, name_call);
			CHECK_RESULT(name);
			if (name.ok())
			{
				auto text = world.stringUtf8(name.value());
				CHECK_RESULT(text);
				if (text.ok())
				{
					CHECK(text.value() == "CN=Test User");
				}
			}
		}

		// getSerialNumber().toString(16) is lower-case, as BigInteger prints it.
		InvokeContext serial_call;
		serial_call.receiver = certificate;
		auto serial = invoke(world, x509_class, "getSerialNumber",
			"()Ljava/math/BigInteger;", false, serial_call);
		CHECK_RESULT(serial);
		if (serial.ok())
		{
			InvokeContext text_call;
			text_call.receiver = serial.value();
			text_call.args = { 16 };
			auto serial_text = invoke(world, big_integer_class, "toString",
				"(I)Ljava/lang/String;", false, text_call);
			CHECK_RESULT(serial_text);
			if (serial_text.ok())
			{
				auto text = world.stringUtf8(serial_text.value());
				CHECK_RESULT(text);
				if (text.ok())
				{
					CHECK(text.value() == "1234");
				}
			}
		}

		// getEncoded returns exactly the DER the stream carried.
		InvokeContext encoded_call;
		encoded_call.receiver = certificate;
		auto encoded = invoke(world, certificate_class, "getEncoded", "()[B", false,
			encoded_call);
		CHECK_RESULT(encoded);
		if (encoded.ok())
		{
			auto roundtrip = readByteArray(world, encoded.value());
			CHECK_RESULT(roundtrip);
			if (roundtrip.ok())
			{
				CHECK(roundtrip.value() == der);
			}
		}
	}

	return test::finish("jni_platform_env_test");
}
