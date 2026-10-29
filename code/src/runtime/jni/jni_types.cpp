#include "runtime/jni/jni_types.hpp"

namespace duckemu::runtime::jni
{
namespace
{

bool skipObjectDescriptor(std::string_view signature, size_t &pos)
{
	// L<class>; -- arrays are represented as Object too, but their element
	// count is not known at parse time.
	if (signature[pos] == '[')
	{
		while (pos < signature.size() && signature[pos] == '[')
		{
			++pos;
		}
		if (pos >= signature.size())
		{
			return false;
		}
		if (signature[pos] == 'L')
		{
			const size_t end = signature.find(';', pos);
			if (end == std::string_view::npos)
			{
				return false;
			}
			pos = end + 1;
			return true;
		}
		bool ok = false;
		const ValueKind element = valueKindOf(signature[pos], &ok);
		if (!ok || element == ValueKind::Void || element == ValueKind::Object)
		{
			return false;
		}
		++pos;
		return true;
	}
	if (signature[pos] != 'L')
	{
		return false;
	}
	const size_t end = signature.find(';', pos);
	if (end == std::string_view::npos)
	{
		return false;
	}
	pos = end + 1;
	return true;
}

bool parseValueKind(std::string_view signature, size_t &pos, ValueKind *out)
{
	if (pos >= signature.size())
	{
		return false;
	}
	if (signature[pos] == 'L' || signature[pos] == '[')
	{
		if (!skipObjectDescriptor(signature, pos))
		{
			return false;
		}
		*out = ValueKind::Object;
		return true;
	}
	bool ok = false;
	const ValueKind kind = valueKindOf(signature[pos], &ok);
	if (!ok || kind == ValueKind::Void)
	{
		return false;
	}
	++pos;
	*out = kind;
	return true;
}

} // namespace

ValueKind valueKindOf(char descriptor, bool *ok)
{
	bool valid = true;
	ValueKind kind = ValueKind::Void;
	switch (descriptor)
	{
	case 'V':
		kind = ValueKind::Void;
		break;
	case 'Z':
		kind = ValueKind::Boolean;
		break;
	case 'B':
		kind = ValueKind::Byte;
		break;
	case 'C':
		kind = ValueKind::Char;
		break;
	case 'S':
		kind = ValueKind::Short;
		break;
	case 'I':
		kind = ValueKind::Int;
		break;
	case 'J':
		kind = ValueKind::Long;
		break;
	case 'F':
		kind = ValueKind::Float;
		break;
	case 'D':
		kind = ValueKind::Double;
		break;
	case 'L':
	case '[':
		kind = ValueKind::Object;
		break;
	default:
		valid = false;
		kind = ValueKind::Void;
		break;
	}
	if (ok != nullptr)
	{
		*ok = valid;
	}
	return kind;
}

bool parseMethodSignature(std::string_view signature, MethodSignature *out)
{
	if (out == nullptr || signature.size() < 3 || signature.front() != '(')
	{
		return false;
	}

	MethodSignature parsed;
	size_t pos = 1;
	while (pos < signature.size() && signature[pos] != ')')
	{
		ValueKind kind = ValueKind::Void;
		if (!parseValueKind(signature, pos, &kind))
		{
			return false;
		}
		parsed.args.push_back(kind);
	}
	if (pos >= signature.size() || signature[pos] != ')')
	{
		return false;
	}
	++pos;
	if (pos >= signature.size() || !parseValueKind(signature, pos, &parsed.result))
	{
		return false;
	}
	if (pos != signature.size())
	{
		return false;
	}
	*out = std::move(parsed);
	return true;
}

std::string descriptorToClassName(std::string_view descriptor)
{
	std::string name(descriptor);
	for (char &character : name)
	{
		if (character == '/')
		{
			character = '.';
		}
	}
	return name;
}

} // namespace duckemu::runtime::jni
