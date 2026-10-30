// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/error/result.hpp"
#include "runtime/props/property_table.hpp"

namespace duckemu::runtime::props
{

// One SELinux property context. Android 14 keeps a property area per context;
// this backend puts every property in one, and property_info routes every
// name to it, so the layout is faithful while the split stays a future knob.
inline constexpr std::string_view kDefaultPropertyContext = "u:object_r:default_prop:s0";

// The three files libc's serialized-contexts reader opens under
// /dev/__properties__ (bionic system_properties/contexts_serialized.cpp).
struct PropertyImageSet
{
	std::string context;
	std::vector<uint8_t> property_info; // property_info
	std::vector<uint8_t> serial_area;   // properties_serial
	std::vector<uint8_t> context_area;  // u:object_r:<context>:s0
};

// Builds the exact in-memory image init would have produced: the prop_area
// trie (Android 13/14 prop_trie_node layout) and the serialized property_info
// trie. Values over PROP_VALUE_MAX use the long-property form.
[[nodiscard]] Result<PropertyImageSet> buildImages(const PropertyTable &table,
	std::string_view context = kDefaultPropertyContext);

} // namespace duckemu::runtime::props
