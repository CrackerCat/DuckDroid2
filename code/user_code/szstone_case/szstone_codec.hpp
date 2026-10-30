// SPDX-License-Identifier: GPL-2.0-only
#pragma once

// Wire helpers shared by the runner and the worker plugin: params travel as
// hex, and the three call-site booleans travel as "0,1,0".

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace duckemu::user::szstone
{

inline uint8_t hexDigit(char c)
{
	if (c >= '0' && c <= '9')
	{
		return static_cast<uint8_t>(c - '0');
	}
	if (c >= 'a' && c <= 'f')
	{
		return static_cast<uint8_t>(c - 'a' + 10);
	}
	if (c >= 'A' && c <= 'F')
	{
		return static_cast<uint8_t>(c - 'A' + 10);
	}
	return 0xFF;
}

inline bool decodeHex(std::string_view text, std::vector<uint8_t> *out)
{
	if (text.size() % 2 != 0)
	{
		return false;
	}
	out->clear();
	out->reserve(text.size() / 2);
	for (size_t i = 0; i < text.size(); i += 2)
	{
		const uint8_t high = hexDigit(text[i]);
		const uint8_t low = hexDigit(text[i + 1]);
		if (high == 0xFF || low == 0xFF)
		{
			return false;
		}
		out->push_back(static_cast<uint8_t>((high << 4) | low));
	}
	return true;
}

inline std::string encodeHex(const uint8_t *data, size_t size)
{
	static constexpr char kDigits[] = "0123456789abcdef";
	std::string text;
	text.reserve(size * 2);
	for (size_t i = 0; i < size; ++i)
	{
		text.push_back(kDigits[data[i] >> 4]);
		text.push_back(kDigits[data[i] & 0x0F]);
	}
	return text;
}

inline std::string encodeHex(const std::vector<uint8_t> &bytes)
{
	return encodeHex(bytes.data(), bytes.size());
}

// Exactly three comma-separated booleans; "1" is true, "0" is false.
inline bool parseZ(std::string_view text, std::array<uint64_t, 3> *z)
{
	size_t start = 0;
	for (size_t slot = 0; slot < z->size(); ++slot)
	{
		const size_t comma = text.find(',', start);
		const bool last = slot + 1 == z->size();
		if (last && comma != std::string_view::npos)
		{
			return false;
		}
		if (!last && comma == std::string_view::npos)
		{
			return false;
		}
		const std::string_view piece =
			text.substr(start, comma == std::string_view::npos ? std::string_view::npos
									   : comma - start);
		if (piece != "0" && piece != "1")
		{
			return false;
		}
		(*z)[slot] = piece == "1" ? 1 : 0;
		start = comma + 1;
	}
	return true;
}

} // namespace duckemu::user::szstone
