// SPDX-License-Identifier: GPL-2.0-only
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/id/ids.hpp"
#include "runtime/jni/envs/envs.hpp"
#include "runtime/jni/world/java_world.hpp"
#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime::jni::envs
{
namespace
{

constexpr const char *kByteArrayInputStream = "java/io/ByteArrayInputStream";
constexpr const char *kCertificateFactory = "java/security/cert/CertificateFactory";
constexpr const char *kCertificate = "java/security/cert/Certificate";
constexpr const char *kX509Certificate = "java/security/cert/X509Certificate";
constexpr const char *kX500Principal = "javax/security/auth/x500/X500Principal";
constexpr const char *kBigInteger = "java/math/BigInteger";

// One DER element: |begin|/|total| cover the tag and length too, which RFC
// 4514 needs for values it cannot render as text.
struct DerElement
{
	uint8_t tag = 0;
	const uint8_t *begin = nullptr;
	const uint8_t *content = nullptr;
	size_t length = 0;
	size_t total = 0;
};

// Reads consecutive definite-length DER elements out of a buffer. Construct a
// new reader over |content| to descend into a constructed element.
class DerReader
{
    public:
	DerReader(const uint8_t *data, size_t size) : cursor_(data), end_(data + size) {}

	bool read(DerElement *element)
	{
		if (cursor_ >= end_)
		{
			return false;
		}
		const uint8_t *const begin = cursor_;
		const uint8_t tag = *cursor_++;
		if (cursor_ >= end_)
		{
			return false;
		}
		const uint8_t first = *cursor_++;
		size_t length = 0;
		if (first < 0x80)
		{
			length = first;
		}
		else
		{
			const size_t count = first & 0x7F;
			// count == 0 is the indefinite form, which DER forbids.
			if (count == 0 || count > sizeof(size_t) ||
				static_cast<size_t>(end_ - cursor_) < count)
			{
				return false;
			}
			for (size_t index = 0; index < count; ++index)
			{
				length = (length << 8) | *cursor_++;
			}
		}
		if (static_cast<size_t>(end_ - cursor_) < length)
		{
			return false;
		}
		element->tag = tag;
		element->begin = begin;
		element->content = cursor_;
		element->length = length;
		element->total = static_cast<size_t>(cursor_ - begin) + length;
		cursor_ += length;
		return true;
	}

    private:
	const uint8_t *cursor_;
	const uint8_t *end_;
};

void appendCodePoint(std::string *out, uint32_t code_point)
{
	if (code_point <= 0x7F)
	{
		out->push_back(static_cast<char>(code_point));
	}
	else if (code_point <= 0x7FF)
	{
		out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
		out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
	}
	else if (code_point <= 0xFFFF)
	{
		out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
		out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
		out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
	}
	else
	{
		out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
		out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
		out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
		out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
	}
}

std::string latin1ToUtf8(const uint8_t *data, size_t size)
{
	std::string text;
	text.reserve(size);
	for (size_t index = 0; index < size; ++index)
	{
		appendCodePoint(&text, data[index]);
	}
	return text;
}

std::string utf16beToUtf8(const uint8_t *data, size_t size)
{
	std::string text;
	for (size_t index = 0; index + 1 < size; index += 2)
	{
		uint32_t code_point = (static_cast<uint32_t>(data[index]) << 8) | data[index + 1];
		// Surrogate pair: UTF-16BE highs are D800-DBFF, lows DC00-DFFF.
		if (code_point >= 0xD800 && code_point <= 0xDBFF && index + 3 < size)
		{
			const uint32_t low =
				(static_cast<uint32_t>(data[index + 2]) << 8) | data[index + 3];
			if (low >= 0xDC00 && low <= 0xDFFF)
			{
				code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
				index += 2;
			}
		}
		appendCodePoint(&text, code_point);
	}
	return text;
}

std::string utf32beToUtf8(const uint8_t *data, size_t size)
{
	std::string text;
	for (size_t index = 0; index + 3 < size; index += 4)
	{
		const uint32_t code_point = (static_cast<uint32_t>(data[index]) << 24) |
			(static_cast<uint32_t>(data[index + 1]) << 16) |
			(static_cast<uint32_t>(data[index + 2]) << 8) | data[index + 3];
		appendCodePoint(&text, code_point);
	}
	return text;
}

bool decodeOid(const uint8_t *data, size_t size, std::string *out)
{
	std::vector<uint64_t> arcs;
	uint64_t value = 0;
	bool pending = false;
	for (size_t index = 0; index < size; ++index)
	{
		const uint8_t byte = data[index];
		if (value > (std::numeric_limits<uint64_t>::max() >> 7))
		{
			return false;
		}
		value = (value << 7) | (byte & 0x7F);
		pending = true;
		if ((byte & 0x80) == 0)
		{
			arcs.push_back(value);
			value = 0;
			pending = false;
		}
	}
	if (pending || arcs.empty())
	{
		return false;
	}
	std::string text;
	if (arcs[0] < 40)
	{
		text = "0." + std::to_string(arcs[0]);
	}
	else if (arcs[0] < 80)
	{
		text = "1." + std::to_string(arcs[0] - 40);
	}
	else
	{
		text = "2." + std::to_string(arcs[0] - 80);
	}
	for (size_t index = 1; index < arcs.size(); ++index)
	{
		text += "." + std::to_string(arcs[index]);
	}
	*out = std::move(text);
	return true;
}

std::string_view attributeKeyword(std::string_view oid)
{
	struct Name
	{
		std::string_view oid;
		std::string_view keyword;
	};
	static constexpr Name kNames[] = {
		{ "2.5.4.3", "CN" },
		{ "2.5.4.4", "SN" },
		{ "2.5.4.5", "SERIALNUMBER" },
		{ "2.5.4.6", "C" },
		{ "2.5.4.7", "L" },
		{ "2.5.4.8", "ST" },
		{ "2.5.4.9", "STREET" },
		{ "2.5.4.10", "O" },
		{ "2.5.4.11", "OU" },
		{ "2.5.4.42", "GN" },
		{ "2.5.4.43", "INITIALS" },
		{ "1.2.840.113549.1.9.1", "emailAddress" },
		{ "0.9.2342.19200300.100.1.1", "UID" },
		{ "0.9.2342.19200300.100.1.25", "DC" },
	};
	for (const Name &name : kNames)
	{
		if (oid == name.oid)
		{
			return name.keyword;
		}
	}
	return oid;
}

// RFC 4514 string rendering of one attribute value.
std::string decodeAttributeValue(const DerElement &element)
{
	switch (element.tag)
	{
	case 0x0C: // UTF8String: already UTF-8.
		return std::string(reinterpret_cast<const char *>(element.content), element.length);
	case 0x12: // NumericString
	case 0x13: // PrintableString
	case 0x14: // TeletexString
	case 0x16: // IA5String
	case 0x1A: // VisibleString
	case 0x1B: // GeneralString
		return latin1ToUtf8(element.content, element.length);
	case 0x1E: // BMPString
		return utf16beToUtf8(element.content, element.length);
	case 0x1C: // UniversalString
		return utf32beToUtf8(element.content, element.length);
	default:
	{
		static constexpr char kHex[] = "0123456789ABCDEF";
		std::string text = "#";
		for (size_t index = 0; index < element.total; ++index)
		{
			const uint8_t byte = element.begin[index];
			text.push_back(kHex[byte >> 4]);
			text.push_back(kHex[byte & 0x0F]);
		}
		return text;
	}
	}
}

std::string escapeValue(std::string_view value)
{
	std::string text;
	text.reserve(value.size());
	for (size_t index = 0; index < value.size(); ++index)
	{
		const unsigned char byte = static_cast<unsigned char>(value[index]);
		if (byte == 0)
		{
			text += "\\00";
			continue;
		}
		const bool first = index == 0;
		const bool last = index + 1 == value.size();
		if (byte == '"' || byte == '+' || byte == ',' || byte == ';' || byte == '<' ||
			byte == '>' || byte == '\\' || byte == '=' ||
			(first && (byte == ' ' || byte == '#')) || (last && byte == ' '))
		{
			text.push_back('\\');
		}
		text.push_back(static_cast<char>(byte));
	}
	return text;
}

// Name ::= RDNSequence. RFC 4514 prints the RDNs in reverse DER order, so the
// CN that comes last in the encoding is printed first.
bool parseName(const DerElement &name, std::string *out)
{
	DerReader reader(name.content, name.length);
	std::vector<std::vector<std::pair<std::string, std::string> > > rdns;
	DerElement set;
	while (reader.read(&set))
	{
		if (set.tag != 0x31)
		{
			return false;
		}
		DerReader attributes(set.content, set.length);
		std::vector<std::pair<std::string, std::string> > rdn;
		DerElement attribute;
		while (attributes.read(&attribute))
		{
			if (attribute.tag != 0x30)
			{
				return false;
			}
			DerReader fields(attribute.content, attribute.length);
			DerElement oid;
			DerElement value;
			if (!fields.read(&oid) || oid.tag != 0x06 || !fields.read(&value))
			{
				return false;
			}
			std::string oid_text;
			if (!decodeOid(oid.content, oid.length, &oid_text))
			{
				return false;
			}
			rdn.emplace_back(std::move(oid_text), decodeAttributeValue(value));
		}
		if (rdn.empty())
		{
			return false;
		}
		rdns.push_back(std::move(rdn));
	}
	if (rdns.empty())
	{
		return false;
	}
	std::string text;
	for (size_t index = rdns.size(); index-- > 0;)
	{
		if (!text.empty())
		{
			text.push_back(',');
		}
		const auto &rdn = rdns[index];
		for (size_t attribute = 0; attribute < rdn.size(); ++attribute)
		{
			if (attribute != 0)
			{
				text.push_back('+');
			}
			text += attributeKeyword(rdn[attribute].first);
			text.push_back('=');
			text += escapeValue(rdn[attribute].second);
		}
	}
	*out = std::move(text);
	return true;
}

// CertificateSerialNumber is a signed INTEGER. X.509 serials are positive in
// practice, but a negative one is still valid DER, so keep its magnitude and
// sign instead of silently reinterpreting the two's complement bytes.
bool parseSerial(const DerElement &element,
	std::vector<uint8_t> *magnitude,
	bool *negative)
{
	if (element.tag != 0x02 || element.length == 0)
	{
		return false;
	}
	const uint8_t *const data = element.content;
	const size_t size = element.length;
	std::vector<uint8_t> bytes;
	if ((data[0] & 0x80) != 0)
	{
		*negative = true;
		bytes.resize(size);
		uint32_t carry = 1;
		for (size_t index = size; index-- > 0;)
		{
			const uint32_t value = static_cast<uint8_t>(~data[index]) + carry;
			bytes[index] = static_cast<uint8_t>(value & 0xFF);
			carry = value >> 8;
		}
	}
	else
	{
		*negative = false;
		bytes.assign(data, data + size);
	}
	size_t begin = 0;
	while (begin < bytes.size() && bytes[begin] == 0)
	{
		++begin;
	}
	bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(begin));
	*magnitude = std::move(bytes);
	return true;
}

struct CertificateInfo
{
	std::string subject;
	std::vector<uint8_t> serial;
	bool serial_negative = false;
};

// Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signature }
// TBSCertificate ::= SEQUENCE { [0] version, serialNumber, signature, issuer,
//                               validity, subject, subjectPublicKeyInfo, ... }
Result<CertificateInfo> parseCertificate(const std::vector<uint8_t> &der)
{
	if (der.empty())
	{
		return Error{ ErrorCode::Unsupported,
			"generateCertificate: the stream is empty" };
	}
	DerReader outer(der.data(), der.size());
	DerElement certificate;
	if (!outer.read(&certificate) || certificate.tag != 0x30)
	{
		return Error{ ErrorCode::Unsupported,
			"generateCertificate: the stream does not hold a DER certificate" };
	}
	DerReader body(certificate.content, certificate.length);
	DerElement tbs;
	if (!body.read(&tbs) || tbs.tag != 0x30)
	{
		return Error{ ErrorCode::Unsupported,
			"generateCertificate: the certificate has no tbsCertificate" };
	}
	DerReader fields(tbs.content, tbs.length);
	DerElement element;
	if (!fields.read(&element))
	{
		return Error{ ErrorCode::Unsupported,
			"generateCertificate: the tbsCertificate is empty" };
	}
	if (element.tag == 0xA0) // [0] EXPLICIT version
	{
		if (!fields.read(&element))
		{
			return Error{ ErrorCode::Unsupported,
				"generateCertificate: the certificate has no serial number" };
		}
	}
	CertificateInfo info;
	if (!parseSerial(element, &info.serial, &info.serial_negative))
	{
		return Error{ ErrorCode::Unsupported,
			"generateCertificate: the serial number is not a DER INTEGER" };
	}
	if (!fields.read(&element) || element.tag != 0x30) // signature algorithm
	{
		return Error{ ErrorCode::Unsupported,
			"generateCertificate: the certificate has no signature algorithm" };
	}
	if (!fields.read(&element) || element.tag != 0x30) // issuer name
	{
		return Error{ ErrorCode::Unsupported,
			"generateCertificate: the certificate has no issuer" };
	}
	if (!fields.read(&element) || element.tag != 0x30) // validity
	{
		return Error{ ErrorCode::Unsupported,
			"generateCertificate: the certificate has no validity" };
	}
	if (!fields.read(&element) || element.tag != 0x30) // subject name
	{
		return Error{ ErrorCode::Unsupported,
			"generateCertificate: the certificate has no subject" };
	}
	if (!parseName(element, &info.subject))
	{
		return Error{ ErrorCode::Unsupported,
			"generateCertificate: cannot parse the subject name" };
	}
	return info;
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

Result<uint64_t> makeByteArray(JavaWorld &world, const uint8_t *data, size_t size)
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

Result<std::vector<uint8_t> > readByteArray(JavaWorld &world, uint64_t object)
{
	auto view = world.arrayView(object);
	if (!view.ok())
	{
		return view.error();
	}
	if (view.value().kind != ArrayElementKind::Byte)
	{
		return Error{ ErrorCode::InvalidArgument, "the object is not a byte array" };
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

// Builds the BigInteger object the certificate pack hands to getSerialNumber.
Result<uint64_t> makeBigInteger(JavaWorld &world,
	uint64_t klass,
	uint64_t magnitude_field,
	uint64_t negative_field,
	const std::vector<uint8_t> &magnitude,
	bool negative)
{
	auto object = world.allocateObject(klass);
	if (!object.ok())
	{
		return object.error();
	}
	auto bytes = makeByteArray(world, magnitude.data(), magnitude.size());
	if (!bytes.ok())
	{
		return bytes.error();
	}
	auto written =
		world.writeInstanceField(object.value(), magnitude_field, bytes.value());
	if (!written.ok())
	{
		return written.error();
	}
	written = world.writeInstanceField(
		object.value(), negative_field, negative ? uint64_t{ 1 } : uint64_t{ 0 });
	if (!written.ok())
	{
		return written.error();
	}
	return object.value();
}

} // namespace

// The Java half of the signing-certificate chain:
//   CertificateFactory.getInstance("X.509") -> generateCertificate(InputStream)
//   -> X509Certificate.getSubjectX500Principal()/getSerialNumber()/getEncoded()
// Target packs still provide the Android objects (PackageInfo, Signature, ...)
// and the certificate bytes; parsing and rendering live here.
Result<void> installJavaSecurity(JavaWorld &world)
{
	const uint64_t stream_class = world.findClass(kByteArrayInputStream);
	const uint64_t stream_buffer =
		stream_class == 0 ? 0 : world.findField(stream_class, "buf", "[B", false);
	if (stream_buffer == 0)
	{
		return Error{ ErrorCode::Internal,
			"java.security needs the java.io pack to be installed first" };
	}

	const uint64_t big_integer_class = world.findClass(kBigInteger);
	const uint64_t big_integer_magnitude = big_integer_class == 0
		? 0
		: world.findField(big_integer_class, "mag", "[B", false);
	const uint64_t big_integer_negative = big_integer_class == 0
		? 0
		: world.findField(big_integer_class, "negative", "Z", false);
	if (big_integer_magnitude == 0 || big_integer_negative == 0)
	{
		return Error{ ErrorCode::Internal,
			"java.security needs the java.math pack to be installed first" };
	}

	auto factory =
		world.declareClass(kCertificateFactory, "java/lang/Object");
	if (!factory.ok())
	{
		return factory.error();
	}
	auto certificate = world.declareClass(kCertificate, "java/lang/Object");
	if (!certificate.ok())
	{
		return certificate.error();
	}
	auto x509 = world.declareClass(kX509Certificate, kCertificate);
	if (!x509.ok())
	{
		return x509.error();
	}
	auto principal = world.declareClass(kX500Principal, "java/lang/Object");
	if (!principal.ok())
	{
		return principal.error();
	}

	auto encoded = world.declareField(certificate.value(), "encoded", "[B", false);
	if (!encoded.ok())
	{
		return encoded.error();
	}
	auto subject =
		world.declareField(x509.value(), "subject", "Ljava/lang/String;", false);
	if (!subject.ok())
	{
		return subject.error();
	}
	auto serial =
		world.declareField(x509.value(), "serial", "Ljava/math/BigInteger;", false);
	if (!serial.ok())
	{
		return serial.error();
	}
	auto principal_name =
		world.declareField(principal.value(), "name", "Ljava/lang/String;", false);
	if (!principal_name.ok())
	{
		return principal_name.error();
	}

	auto factory_object = world.allocateObject(factory.value());
	if (!factory_object.ok())
	{
		return factory_object.error();
	}

	const uint64_t x509_class = x509.value();
	const uint64_t principal_class = principal.value();
	const uint64_t encoded_field = encoded.value();
	const uint64_t subject_field = subject.value();
	const uint64_t serial_field = serial.value();
	const uint64_t principal_name_field = principal_name.value();
	const uint64_t factory_address = factory_object.value();

	auto result = declare(world, factory.value(), "getInstance",
		"(Ljava/lang/String;)Ljava/security/cert/CertificateFactory;", true,
		[factory_address](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1 || call.args[0] == 0)
			{
				return Error{ ErrorCode::InvalidArgument,
					"CertificateFactory.getInstance expects an algorithm name" };
			}
			auto algorithm = java.stringUtf8(call.args[0]);
			if (!algorithm.ok())
			{
				return algorithm.error();
			}
			if (algorithm.value() != "X.509" && algorithm.value() != "X509")
			{
				return Error{ ErrorCode::Unsupported,
					"CertificateFactory.getInstance: only X.509 is installed" };
			}
			return factory_address;
		});
	if (!result.ok())
	{
		return result;
	}

	result = declare(world, factory.value(), "generateCertificate",
		"(Ljava/io/InputStream;)Ljava/security/cert/Certificate;", false,
		[stream_class,
			stream_buffer,
			x509_class,
			encoded_field,
			subject_field,
			serial_field,
			big_integer_class,
			big_integer_magnitude,
			big_integer_negative](JavaWorld &java,
			InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1 || call.args[0] == 0)
			{
				return Error{ ErrorCode::InvalidArgument,
					"CertificateFactory.generateCertificate expects a stream" };
			}
			if (!java.isInstanceOf(call.args[0], stream_class))
			{
				return Error{ ErrorCode::Unsupported,
					"generateCertificate: only ByteArrayInputStream is installed" };
			}
			auto buffer = java.readInstanceField(call.args[0], stream_buffer);
			if (!buffer.ok())
			{
				return buffer.error();
			}
			if (buffer.value() == 0)
			{
				return Error{ ErrorCode::InvalidArgument,
					"generateCertificate: the stream has no buffer" };
			}
			auto der = readByteArray(java, buffer.value());
			if (!der.ok())
			{
				return der.error();
			}
			auto info = parseCertificate(der.value());
			if (!info.ok())
			{
				return info.error();
			}

			auto object = java.allocateObject(x509_class);
			if (!object.ok())
			{
				return object.error();
			}
			auto encoded_bytes =
				makeByteArray(java, der.value().data(), der.value().size());
			if (!encoded_bytes.ok())
			{
				return encoded_bytes.error();
			}
			auto written = java.writeInstanceField(
				object.value(), encoded_field, encoded_bytes.value());
			if (!written.ok())
			{
				return written.error();
			}
			auto subject_text = java.newString(info.value().subject);
			if (!subject_text.ok())
			{
				return subject_text.error();
			}
			written = java.writeInstanceField(
				object.value(), subject_field, subject_text.value());
			if (!written.ok())
			{
				return written.error();
			}
			auto serial_object = makeBigInteger(java,
				big_integer_class,
				big_integer_magnitude,
				big_integer_negative,
				info.value().serial,
				info.value().serial_negative);
			if (!serial_object.ok())
			{
				return serial_object.error();
			}
			written = java.writeInstanceField(
				object.value(), serial_field, serial_object.value());
			if (!written.ok())
			{
				return written.error();
			}
			return object.value();
		});
	if (!result.ok())
	{
		return result;
	}

	result = declare(world, certificate.value(), "getEncoded", "()[B", false,
		[encoded_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			auto bytes = java.readInstanceField(call.receiver, encoded_field);
			if (!bytes.ok())
			{
				return bytes.error();
			}
			if (bytes.value() == 0)
			{
				return Error{ ErrorCode::Internal, "Certificate has no encoded bytes" };
			}
			return bytes.value();
		});
	if (!result.ok())
	{
		return result;
	}

	result = declare(world, x509.value(), "getSubjectX500Principal",
		"()Ljavax/security/auth/x500/X500Principal;", false,
		[principal_class, subject_field, principal_name_field](JavaWorld &java,
			InvokeContext &call) -> Result<uint64_t> {
			auto name = java.readInstanceField(call.receiver, subject_field);
			if (!name.ok())
			{
				return name.error();
			}
			if (name.value() == 0)
			{
				return Error{ ErrorCode::Internal, "X509Certificate has no subject" };
			}
			auto object = java.allocateObject(principal_class);
			if (!object.ok())
			{
				return object.error();
			}
			auto written =
				java.writeInstanceField(object.value(), principal_name_field, name.value());
			if (!written.ok())
			{
				return written.error();
			}
			return object.value();
		});
	if (!result.ok())
	{
		return result;
	}

	result = declare(world, x509.value(), "getSerialNumber", "()Ljava/math/BigInteger;",
		false,
		[serial_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			auto number = java.readInstanceField(call.receiver, serial_field);
			if (!number.ok())
			{
				return number.error();
			}
			if (number.value() == 0)
			{
				return Error{ ErrorCode::Internal,
					"X509Certificate has no serial number" };
			}
			return number.value();
		});
	if (!result.ok())
	{
		return result;
	}

	return declare(world, principal.value(), "getName", "()Ljava/lang/String;", false,
		[principal_name_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			auto name = java.readInstanceField(call.receiver, principal_name_field);
			if (!name.ok())
			{
				return name.error();
			}
			if (name.value() == 0)
			{
				return Error{ ErrorCode::Internal, "X500Principal has no name" };
			}
			return name.value();
		});
}

} // namespace duckemu::runtime::jni::envs
