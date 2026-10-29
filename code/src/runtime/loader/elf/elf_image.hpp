// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/error/result.hpp"

namespace duckemu::runtime::loader
{

// One PT_LOAD. `offset` is the file offset, `vaddr` the link-time address.
struct ElfSegment
{
	uint64_t vaddr = 0;
	uint64_t offset = 0;
	uint64_t filesz = 0;
	uint64_t memsz = 0;
	uint64_t align = 0;
	uint32_t flags = 0; // Linux PF_* bits
};

struct ElfTlsTemplate
{
	bool present = false;
	uint64_t vaddr = 0;
	uint64_t offset = 0; // file offset of the initialization image
	uint64_t filesz = 0;
	uint64_t memsz = 0;
	uint64_t align = 0;
};

struct ElfSymbol
{
	std::string name;
	uint8_t info = 0;
	uint8_t other = 0;
	uint16_t shndx = 0;
	uint64_t value = 0;
	uint64_t size = 0;

	[[nodiscard]] uint8_t bind() const
	{
		return static_cast<uint8_t>(info >> 4);
	}
	[[nodiscard]] uint8_t type() const
	{
		return static_cast<uint8_t>(info & 0xFu);
	}
	[[nodiscard]] bool undefined() const
	{
		return shndx == 0;
	}
};

struct ElfRelocation
{
	uint64_t offset = 0;
	uint32_t type = 0;
	uint32_t symbol = 0;
	int64_t addend = 0;
};

// A parsed ELF64/EM_AARCH64/ET_DYN file. Owns the file bytes; every accessor is
// bounds-checked during parse, so later reads of the vectors are safe.
class ElfImage
{
    public:
	static Result<ElfImage> parse(std::vector<uint8_t> bytes, std::string_view source_name);

	[[nodiscard]] const std::vector<uint8_t> &bytes() const
	{
		return bytes_;
	}
	[[nodiscard]] const std::vector<ElfSegment> &segments() const
	{
		return segments_;
	}
	[[nodiscard]] const ElfTlsTemplate &tls() const
	{
		return tls_;
	}
	[[nodiscard]] const std::vector<std::string> &needed() const
	{
		return needed_;
	}
	[[nodiscard]] const std::vector<ElfSymbol> &symbols() const
	{
		return symbols_;
	}
	[[nodiscard]] const std::vector<ElfRelocation> &relocations() const
	{
		return relocations_;
	}
	[[nodiscard]] const std::vector<ElfRelocation> &pltRelocations() const
	{
		return plt_relocations_;
	}
	[[nodiscard]] std::string_view soname() const
	{
		return soname_;
	}
	[[nodiscard]] std::optional<uint64_t> initFunction() const
	{
		return init_function_;
	}
	[[nodiscard]] std::optional<uint64_t> initArray() const
	{
		return init_array_;
	}
	[[nodiscard]] uint64_t initArraySize() const
	{
		return init_array_size_;
	}

    private:
	ElfImage() = default;

	// File offset for a link-time address inside a PT_LOAD's file image.
	[[nodiscard]] std::optional<uint64_t> fileOffsetOf(uint64_t address) const;

	std::vector<uint8_t> bytes_;
	std::string source_;
	std::vector<ElfSegment> segments_;
	ElfTlsTemplate tls_;
	std::vector<std::string> needed_;
	std::string soname_;
	std::vector<ElfSymbol> symbols_;
	std::vector<ElfRelocation> relocations_;
	std::vector<ElfRelocation> plt_relocations_;
	std::optional<uint64_t> init_function_;
	std::optional<uint64_t> init_array_;
	uint64_t init_array_size_ = 0;
};

} // namespace duckemu::runtime::loader
