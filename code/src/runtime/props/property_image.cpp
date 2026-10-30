// SPDX-License-Identifier: GPL-2.0-only
//
// The images here are data files, not an API: bionic's own property reader
// parses them (system_properties/prop_area.cpp and
// system/core/property_service/libpropertyinfoparser). Keep the offsets in
// lockstep with those sources.
#include "runtime/props/property_image.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace duckemu::runtime::props
{
namespace
{

// bionic system_properties/prop_area.cpp.
constexpr uint32_t kPropAreaMagic = 0x504f5250;
constexpr uint32_t kPropAreaVersion = 0xfc6ed0ab;
constexpr uint32_t kAreaSize = 128 * 1024; // PA_SIZE without LARGE_SYSTEM_PROPERTY_NODE
constexpr uint32_t kHeaderSize = 128;

// prop_area header: bytes_used, serial, magic, version, reserved[28].
constexpr uint32_t kHeaderBytesUsed = 0;
constexpr uint32_t kHeaderMagic = 8;
constexpr uint32_t kHeaderVersion = 12;

// prop_trie_node: namelen, prop, left, right, children, name[0].
constexpr uint32_t kNodeNamelen = 0;
constexpr uint32_t kNodeProp = 4;
constexpr uint32_t kNodeLeft = 8;
constexpr uint32_t kNodeRight = 12;
constexpr uint32_t kNodeChildren = 16;
constexpr uint32_t kNodeName = 20;
constexpr uint32_t kRootNodeSize = 20;

// prop_info: serial, value[PROP_VALUE_MAX], name[0].
constexpr uint32_t kInfoSerial = 0;
constexpr uint32_t kInfoValue = 4;
constexpr uint32_t kInfoName = 96;
constexpr uint32_t kInfoSize = 96;
constexpr uint32_t kValueMax = 92;
constexpr uint32_t kLongFlag = 1u << 16;
constexpr uint32_t kLongErrorOffset = kInfoValue + 56; // long_property.offset

// The root node is followed by the dirty-backup area; allocations start after
// both (prop_area constructor: sizeof(prop_trie_node) + align(PROP_VALUE_MAX)).
constexpr uint32_t kFirstAllocation = kRootNodeSize + kValueMax;

constexpr uint32_t kNoOffset = 0xffffffffu;

// bionic prop_info.cpp: legacy readers get this message, not the value.
constexpr char kLongError[] = "Must use __system_property_read_callback() to read";
static_assert(sizeof(kLongError) - 1 < 56, "legacy long-property message must fit");

int compareNames(const char *one, uint32_t one_length, const char *two, uint32_t two_length)
{
	if (one_length != two_length)
	{
		return one_length < two_length ? -1 : 1;
	}
	const int order = std::memcmp(one, two, one_length);
	return order < 0 ? -1 : (order > 0 ? 1 : 0);
}

// prop_area image in the Android 13/14 trie layout. Insertion mirrors
// prop_area::find_property(alloc_if_needed=true): tokens split on '.', each
// level is a binary tree ordered by (length, bytes) exactly like
// cmp_prop_name.
class AreaBuilder
{
    public:
	AreaBuilder()
		: image_(kAreaSize, 0)
	{
		putHeader(kHeaderMagic, kPropAreaMagic);
		putHeader(kHeaderVersion, kPropAreaVersion);
	}

	Result<void> add(const std::string &name, const std::string &value)
	{
		uint32_t current = 0; // root trie node lives at data_+0
		size_t start = 0;
		while (true)
		{
			const size_t dot = name.find('.', start);
			const bool last = dot == std::string::npos;
			const size_t length = (last ? name.size() : dot) - start;
			if (length == 0)
			{
				return Error{ ErrorCode::InvalidArgument,
					"property name has an empty component: " + name };
			}
			const std::string_view token(name.data() + start, length);

			uint32_t child = getU32(current + kNodeChildren);
			if (child == 0)
			{
				child = newNode(token);
				if (child == kNoOffset)
				{
					return outOfSpace(name);
				}
				putU32(current + kNodeChildren, child);
			}

			uint32_t node = child;
			while (true)
			{
				const uint32_t node_length = getU32(node + kNodeNamelen);
				const int order = compareNames(token.data(),
					static_cast<uint32_t>(token.size()),
					reinterpret_cast<const char *>(at(node + kNodeName)),
					node_length);
				if (order == 0)
				{
					break;
				}
				const uint32_t slot = node + (order < 0 ? kNodeLeft : kNodeRight);
				const uint32_t next = getU32(slot);
				if (next == 0)
				{
					const uint32_t created = newNode(token);
					if (created == kNoOffset)
					{
						return outOfSpace(name);
					}
					putU32(slot, created);
					node = created;
					break;
				}
				node = next;
			}

			current = node;
			if (last)
			{
				break;
			}
			start = dot + 1;
		}

		const uint32_t info = newPropInfo(name, value);
		if (info == kNoOffset)
		{
			return outOfSpace(name);
		}
		putU32(current + kNodeProp, info);
		return {};
	}

	std::vector<uint8_t> finish()
	{
		putHeader(kHeaderBytesUsed, bytes_used_);
		return std::move(image_);
	}

    private:
	[[nodiscard]] static Error outOfSpace(const std::string &name)
	{
		return Error{ ErrorCode::OutOfMemory, "property area is full adding " + name };
	}

	uint8_t *at(uint32_t offset)
	{
		return image_.data() + kHeaderSize + offset;
	}

	uint32_t getU32(uint32_t offset) const
	{
		uint32_t value = 0;
		std::memcpy(&value, image_.data() + kHeaderSize + offset, sizeof(value));
		return value;
	}

	// Data-relative field (offsets are from data_, which starts after the
	// 128-byte header).
	void putU32(uint32_t offset, uint32_t value)
	{
		std::memcpy(image_.data() + kHeaderSize + offset, &value, sizeof(value));
	}

	// File-absolute field (the prop_area header itself).
	void putHeader(uint32_t offset, uint32_t value)
	{
		std::memcpy(image_.data() + offset, &value, sizeof(value));
	}

	uint32_t allocate(uint32_t size)
	{
		const uint32_t aligned = (size + 3u) & ~3u;
		if (bytes_used_ + aligned > kAreaSize - kHeaderSize)
		{
			return kNoOffset;
		}
		const uint32_t offset = bytes_used_;
		bytes_used_ += aligned;
		return offset;
	}

	uint32_t newNode(std::string_view token)
	{
		const uint32_t offset =
			allocate(kNodeName + static_cast<uint32_t>(token.size()) + 1);
		if (offset == kNoOffset)
		{
			return kNoOffset;
		}
		putU32(offset + kNodeNamelen, static_cast<uint32_t>(token.size()));
		std::memcpy(at(offset + kNodeName), token.data(), token.size());
		at(offset + kNodeName)[token.size()] = '\0';
		return offset;
	}

	uint32_t newPropInfo(const std::string &name, const std::string &value)
	{
		const uint32_t offset = allocate(kInfoSize + static_cast<uint32_t>(name.size()) + 1);
		if (offset == kNoOffset)
		{
			return kNoOffset;
		}
		if (value.size() < kValueMax)
		{
			putU32(offset + kInfoSerial, static_cast<uint32_t>(value.size()) << 24);
			std::memcpy(at(offset + kInfoValue), value.data(), value.size());
			at(offset + kInfoValue)[value.size()] = '\0';
		}
		else
		{
			const uint32_t long_offset = allocate(static_cast<uint32_t>(value.size()) + 1);
			if (long_offset == kNoOffset)
			{
				return kNoOffset;
			}
			std::memcpy(at(long_offset), value.data(), value.size());
			at(long_offset)[value.size()] = '\0';
			putU32(offset + kInfoSerial,
				(static_cast<uint32_t>(sizeof(kLongError) - 1) << 24) | kLongFlag);
			std::memcpy(at(offset + kInfoValue), kLongError, sizeof(kLongError));
			putU32(offset + kLongErrorOffset, long_offset - offset);
		}
		std::memcpy(at(offset + kInfoName), name.data(), name.size());
		at(offset + kInfoName)[name.size()] = '\0';
		return offset;
	}

	std::vector<uint8_t> image_;
	uint32_t bytes_used_ = kFirstAllocation;
};

// Serialized property_info trie (PropertyInfoArea). Every name resolves to
// context 0 through one zero-length prefix entry: CheckPrefixMatch accepts a
// prefix of length 0 for any remaining name. The header strings and arrays
// follow the parser's expectations exactly; offsets are from image start.
std::vector<uint8_t> buildPropertyInfo(const std::string &context)
{
	std::vector<uint8_t> image(24, 0); // header: 6 uint32 fields

	auto appendU32 = [&image](uint32_t value) -> uint32_t {
		while (image.size() % 4 != 0)
		{
			image.push_back(0);
		}
		const uint32_t offset = static_cast<uint32_t>(image.size());
		for (int shift = 0; shift < 32; shift += 8)
		{
			image.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
		}
		return offset;
	};
	auto appendString = [&image](std::string_view text) -> uint32_t {
		while (image.size() % 4 != 0)
		{
			image.push_back(0);
		}
		const uint32_t offset = static_cast<uint32_t>(image.size());
		image.insert(image.end(), text.begin(), text.end());
		image.push_back(0);
		return offset;
	};
	auto patchU32 = [&image](uint32_t offset, uint32_t value) {
		std::memcpy(image.data() + offset, &value, sizeof(value));
	};

	const uint32_t contexts_offset = appendU32(1); // num_contexts
	const uint32_t context_slot = appendU32(0);    // patched below
	const uint32_t types_offset = appendU32(1);    // num_types
	const uint32_t type_slot = appendU32(0);       // patched below

	const uint32_t context_string = appendString(context);
	const uint32_t type_string = appendString("default");
	patchU32(context_slot, context_string);
	patchU32(type_slot, type_string);

	// Root node's own entry: no context/type. Its name is unused (there are no
	// children to compare against) but must point at a valid string.
	const uint32_t root_entry = appendU32(0);
	appendU32(0);      // namelen
	appendU32(~0u);    // context_index
	appendU32(~0u);    // type_index
	const uint32_t empty_string = appendString("");
	patchU32(root_entry, empty_string);

	const uint32_t root_offset = appendU32(root_entry); // property_entry
	appendU32(0);                                       // num_child_nodes
	appendU32(0);                                       // child_nodes
	appendU32(1);                                       // num_prefixes
	const uint32_t prefix_array_slot = appendU32(0);    // patched below
	appendU32(0);                                       // num_exact_matches
	appendU32(0);                                       // exact_match_entries

	const uint32_t prefix_entry = appendU32(empty_string);
	appendU32(0);   // namelen 0 -> matches every remaining name
	appendU32(0);   // context_index
	appendU32(0);   // type_index

	const uint32_t prefix_array = appendU32(prefix_entry);
	patchU32(prefix_array_slot, prefix_array);

	const uint32_t size = static_cast<uint32_t>(image.size());
	patchU32(0, 1); // current_version
	patchU32(4, 1); // minimum_supported_version
	patchU32(8, size); // size, checked against the mmap length
	patchU32(12, contexts_offset);
	patchU32(16, types_offset);
	patchU32(20, root_offset);
	return image;
}

// The serial property area only needs a valid header; nothing reads its map.
std::vector<uint8_t> buildSerialArea()
{
	std::vector<uint8_t> image(kAreaSize, 0);
	const uint32_t bytes_used = kFirstAllocation;
	const uint32_t magic = kPropAreaMagic;
	const uint32_t version = kPropAreaVersion;
	std::memcpy(image.data() + kHeaderBytesUsed, &bytes_used, sizeof(bytes_used));
	std::memcpy(image.data() + kHeaderMagic, &magic, sizeof(magic));
	std::memcpy(image.data() + kHeaderVersion, &version, sizeof(version));
	return image;
}

} // namespace

Result<PropertyImageSet> buildImages(const PropertyTable &table, std::string_view context)
{
	PropertyImageSet images;
	images.context = std::string(context);

	AreaBuilder area;
	for (const auto &entry : table.entries())
	{
		auto added = area.add(entry.first, entry.second);
		if (!added.ok())
		{
			return added.error();
		}
	}
	images.context_area = area.finish();
	images.serial_area = buildSerialArea();
	images.property_info = buildPropertyInfo(images.context);
	return images;
}

} // namespace duckemu::runtime::props
