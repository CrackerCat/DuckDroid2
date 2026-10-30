// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/loader/elf/elf_image.hpp"

#include <algorithm>
#include <cstring>
#include <elf.h>
#include <limits>
#include <utility>

namespace duckemu::runtime::loader
{
namespace
{

// Values newer than some <elf.h> copies carry.
constexpr int64_t kDtAndroidRel = 0x6000000F;
constexpr int64_t kDtAndroidRelsz = 0x60000010;
constexpr int64_t kDtAndroidRela = 0x60000011;
constexpr int64_t kDtAndroidRelasz = 0x60000012;
constexpr int64_t kDtRelrsz = 35;
constexpr int64_t kDtRelr = 36;

constexpr uint64_t kMaxSymbols = 1u << 20;
constexpr uint64_t kMaxRelocations = 1u << 21;

// APS2/SLEB128 decoder. Mirrors bionic's linker_sleb128.h: the value is
// accumulated unsigned and sign-extended from the final byte's bit 6.
class Sle128Decoder
{
    public:
	explicit Sle128Decoder(const uint8_t *begin, const uint8_t *end)
		: current_(begin)
		, end_(end)
	{
	}

	bool pop(int64_t &out)
	{
		uint64_t value = 0;
		unsigned shift = 0;
		uint8_t byte = 0;
		do
		{
			if (current_ >= end_ || shift >= 64)
			{
				return false;
			}
			byte = *current_++;
			value |= static_cast<uint64_t>(byte & 0x7F) << shift;
			shift += 7;
		} while ((byte & 0x80) != 0);

		if (shift < 64 && (byte & 0x40) != 0)
		{
			value |= ~0ULL << shift;
		}
		out = static_cast<int64_t>(value);
		return true;
	}

	[[nodiscard]] bool atEnd() const
	{
		return current_ == end_;
	}

    private:
	const uint8_t *current_;
	const uint8_t *end_;
};

Error badImage(std::string message)
{
	return Error{ ErrorCode::InvalidArgument, std::move(message) };
}

Error unsupported(std::string message)
{
	return Error{ ErrorCode::Unsupported, std::move(message) };
}

template <typename T> bool readAt(const std::vector<uint8_t> &bytes, uint64_t offset, T &out)
{
	if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
	{
		return false;
	}
	std::memcpy(&out, bytes.data() + offset, sizeof(T));
	return true;
}

bool readU32(const std::vector<uint8_t> &bytes, uint64_t offset, uint32_t &out)
{
	return readAt(bytes, offset, out);
}

} // namespace

std::optional<uint64_t> ElfImage::fileOffsetOf(uint64_t address) const
{
	for (const ElfSegment &segment : segments_)
	{
		if (address < segment.vaddr)
		{
			continue;
		}
		const uint64_t delta = address - segment.vaddr;
		if (delta < segment.filesz)
		{
			return segment.offset + delta;
		}
	}
	return std::nullopt;
}

Result<ElfImage> ElfImage::parse(std::vector<uint8_t> bytes, std::string_view source_name)
{
	ElfImage image;
	image.bytes_ = std::move(bytes);
	image.source_ = std::string(source_name);

	Elf64_Ehdr header{};
	if (!readAt(image.bytes_, 0, header))
	{
		return badImage(image.source_ + ": file is too small for an ELF header");
	}
	if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0)
	{
		return badImage(image.source_ + ": not an ELF file");
	}
	if (header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB)
	{
		return badImage(image.source_ + ": only ELF64 little-endian is supported");
	}
	if (header.e_machine != EM_AARCH64)
	{
		return badImage(image.source_ + ": not an AArch64 ELF");
	}
	if (header.e_type != ET_DYN)
	{
		return badImage(image.source_ + ": only shared objects (ET_DYN) are loadable");
	}
	if (header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum == 0)
	{
		return badImage(image.source_ + ": malformed program header table");
	}
	if (header.e_phoff > image.bytes_.size() ||
	    header.e_phnum > (image.bytes_.size() - header.e_phoff) / sizeof(Elf64_Phdr))
	{
		return badImage(image.source_ + ": program headers are out of bounds");
	}

	image.phdr_bytes_.assign(image.bytes_.begin() + header.e_phoff,
		image.bytes_.begin() + header.e_phoff + header.e_phnum * sizeof(Elf64_Phdr));
	image.phnum_ = static_cast<uint16_t>(header.e_phnum);

	// Copied, not pointed at: `program` is a loop-local.
	Elf64_Phdr dynamic_header{};
	bool has_dynamic = false;
	for (uint32_t i = 0; i < header.e_phnum; ++i)
	{
		Elf64_Phdr program{};
		if (!readAt(image.bytes_, header.e_phoff + i * sizeof(Elf64_Phdr), program))
		{
			return badImage(image.source_ + ": truncated program header table");
		}

		switch (program.p_type)
		{
		case PT_LOAD:
		{
			if (program.p_filesz > program.p_memsz)
			{
				return badImage(image.source_ + ": PT_LOAD filesz exceeds memsz");
			}
			if (program.p_offset > image.bytes_.size() ||
			    program.p_filesz > image.bytes_.size() - program.p_offset)
			{
				return badImage(image.source_ + ": PT_LOAD extends past end of file");
			}
			if (program.p_vaddr > std::numeric_limits<uint64_t>::max() - program.p_memsz)
			{
				return badImage(image.source_ + ": PT_LOAD address overflows");
			}

			ElfSegment segment;
			segment.vaddr = program.p_vaddr;
			segment.offset = program.p_offset;
			segment.filesz = program.p_filesz;
			segment.memsz = program.p_memsz;
			segment.align = program.p_align == 0 ? 1 : program.p_align;
			segment.flags = program.p_flags;
			image.segments_.push_back(segment);
			break;
		}
		case PT_TLS:
			if (program.p_filesz > program.p_memsz)
			{
				return badImage(image.source_ + ": PT_TLS filesz exceeds memsz");
			}
			if (program.p_offset > image.bytes_.size() ||
			    program.p_filesz > image.bytes_.size() - program.p_offset)
			{
				return badImage(image.source_ + ": PT_TLS extends past end of file");
			}
			image.tls_.present = true;
			image.tls_.vaddr = program.p_vaddr;
			image.tls_.offset = program.p_offset;
			image.tls_.filesz = program.p_filesz;
			image.tls_.memsz = program.p_memsz;
			image.tls_.align = program.p_align == 0 ? 1 : program.p_align;
			break;
		case PT_DYNAMIC:
			dynamic_header = program;
			has_dynamic = true;
			break;
		default:
			break;
		}
	}

	if (image.segments_.empty())
	{
		return badImage(image.source_ + ": no PT_LOAD segments");
	}

	// Dynamic tags. Needed strings are resolved after DT_STRTAB is known.
	std::vector<int64_t> needed_offsets;
	if (has_dynamic)
	{
		Elf64_Phdr dynamic = dynamic_header;
		if (dynamic.p_offset > image.bytes_.size() ||
		    dynamic.p_filesz > image.bytes_.size() - dynamic.p_offset)
		{
			return badImage(image.source_ + ": PT_DYNAMIC is out of bounds");
		}

		for (uint64_t i = 0; i + sizeof(Elf64_Dyn) <= dynamic.p_filesz; i += sizeof(Elf64_Dyn))
		{
			Elf64_Dyn entry{};
			if (!readAt(image.bytes_, dynamic.p_offset + i, entry))
			{
				return badImage(image.source_ + ": truncated dynamic section");
			}
			if (entry.d_tag == DT_NULL)
			{
				break;
			}

			switch (entry.d_tag)
			{
			case DT_NEEDED:
				needed_offsets.push_back(entry.d_un.d_val);
				break;
			default:
				break;
			}
		}
	}

	// Second dynamic pass: everything that needs DT_STRTAB.
	uint64_t strtab_address = 0;
	uint64_t strtab_size = 0;
	uint64_t symtab_address = 0;
	uint64_t rela_address = 0;
	uint64_t rela_size = 0;
	uint64_t rela_ent = sizeof(Elf64_Rela);
	uint64_t jmprel_address = 0;
	uint64_t jmprel_size = 0;
	bool jmprel_is_rela = true;
	uint64_t android_rel_address = 0;
	uint64_t android_rel_size = 0;
	uint64_t android_rela_address = 0;
	uint64_t android_rela_size = 0;
	uint64_t relr_address = 0;
	uint64_t relr_size = 0;
	uint64_t relr_ent = sizeof(uint64_t);
	uint64_t hash_address = 0;
	uint64_t gnu_hash_address = 0;
	uint64_t soname_offset = 0;
	bool have_soname = false;

	if (has_dynamic)
	{
		Elf64_Phdr dynamic = dynamic_header;
		for (uint64_t i = 0; i + sizeof(Elf64_Dyn) <= dynamic.p_filesz; i += sizeof(Elf64_Dyn))
		{
			Elf64_Dyn entry{};
			if (!readAt(image.bytes_, dynamic.p_offset + i, entry))
			{
				return badImage(image.source_ + ": truncated dynamic section");
			}
			if (entry.d_tag == DT_NULL)
			{
				break;
			}

			switch (entry.d_tag)
			{
			case DT_STRTAB:
				strtab_address = entry.d_un.d_ptr;
				break;
			case DT_STRSZ:
				strtab_size = entry.d_un.d_val;
				break;
			case DT_SYMTAB:
				symtab_address = entry.d_un.d_ptr;
				break;
			case DT_SONAME:
				soname_offset = entry.d_un.d_val;
				have_soname = true;
				break;
			case DT_RELA:
				rela_address = entry.d_un.d_ptr;
				break;
			case DT_RELASZ:
				rela_size = entry.d_un.d_val;
				break;
			case DT_RELAENT:
				rela_ent = entry.d_un.d_val;
				break;
			case DT_JMPREL:
				jmprel_address = entry.d_un.d_ptr;
				break;
			case DT_PLTRELSZ:
				jmprel_size = entry.d_un.d_val;
				break;
			case DT_PLTREL:
				jmprel_is_rela = entry.d_un.d_val == DT_RELA;
				break;
			case kDtAndroidRel:
				android_rel_address = entry.d_un.d_ptr;
				break;
			case kDtAndroidRelsz:
				android_rel_size = entry.d_un.d_val;
				break;
			case kDtAndroidRela:
				android_rela_address = entry.d_un.d_ptr;
				break;
			case kDtAndroidRelasz:
				android_rela_size = entry.d_un.d_val;
				break;
			case kDtRelr:
				relr_address = entry.d_un.d_ptr;
				break;
			case kDtRelrsz:
				relr_size = entry.d_un.d_val;
				break;
			case DT_RELRENT:
				relr_ent = entry.d_un.d_val;
				break;
			case DT_HASH:
				hash_address = entry.d_un.d_ptr;
				break;
			case DT_GNU_HASH:
				gnu_hash_address = entry.d_un.d_ptr;
				break;
			case DT_INIT:
				image.init_function_ = entry.d_un.d_ptr;
				break;
			case DT_INIT_ARRAY:
				image.init_array_ = entry.d_un.d_ptr;
				break;
			case DT_INIT_ARRAYSZ:
				image.init_array_size_ = entry.d_un.d_val;
				break;
			default:
				break;
			}
		}
	}

	if (!jmprel_is_rela)
	{
		return unsupported(image.source_ + ": DT_REL (not DT_RELA) PLT relocations are not supported");
	}
	if (rela_ent != sizeof(Elf64_Rela))
	{
		return badImage(image.source_ + ": unexpected DT_RELAENT");
	}

	// String table. Needed/SONAME names live in it.
	if (strtab_address != 0)
	{
		const std::optional<uint64_t> strtab_offset = image.fileOffsetOf(strtab_address);
		if (!strtab_offset.has_value() || *strtab_offset > image.bytes_.size() ||
		    strtab_size > image.bytes_.size() - *strtab_offset)
		{
			return badImage(image.source_ + ": DT_STRTAB is out of bounds");
		}

		auto stringAt = [&](uint64_t offset) -> std::optional<std::string_view> {
			if (offset >= strtab_size)
			{
				return std::nullopt;
			}
			const char *start = reinterpret_cast<const char *>(image.bytes_.data() + *strtab_offset + offset);
			const uint64_t remaining = strtab_size - offset;
			const void *terminator = std::memchr(start, '\0', static_cast<size_t>(remaining));
			if (terminator == nullptr)
			{
				return std::nullopt;
			}
			return std::string_view(start,
				static_cast<size_t>(static_cast<const char *>(terminator) - start));
		};

		for (const int64_t offset : needed_offsets)
		{
			const std::optional<std::string_view> name = stringAt(static_cast<uint64_t>(offset));
			if (!name.has_value())
			{
				return badImage(image.source_ + ": DT_NEEDED name is out of bounds");
			}
			image.needed_.emplace_back(*name);
		}
		if (have_soname)
		{
			const std::optional<std::string_view> name = stringAt(soname_offset);
			if (!name.has_value())
			{
				return badImage(image.source_ + ": DT_SONAME is out of bounds");
			}
			image.soname_ = std::string(*name);
		}

		// Symbol count: DT_HASH gives it directly; GNU hash requires walking the
		// chains; stripped files without either fall back to section headers.
		uint64_t symbol_count = 0;
		if (hash_address != 0)
		{
			const std::optional<uint64_t> hash_offset = image.fileOffsetOf(hash_address);
			uint32_t hash_header[2] = {};
			if (!hash_offset.has_value() || !readAt(image.bytes_, *hash_offset, hash_header))
			{
				return badImage(image.source_ + ": DT_HASH is out of bounds");
			}
			symbol_count = hash_header[1]; // nchain
		}
		else if (gnu_hash_address != 0)
		{
			const std::optional<uint64_t> gnu_hash_offset = image.fileOffsetOf(gnu_hash_address);
			uint32_t hash_header[4] = {};
			if (!gnu_hash_offset.has_value() || !readAt(image.bytes_, *gnu_hash_offset, hash_header))
			{
				return badImage(image.source_ + ": DT_GNU_HASH is out of bounds");
			}
			const uint64_t nbuckets = hash_header[0];
			const uint64_t symoffset = hash_header[1];
			const uint64_t bloom_size = hash_header[2];
			if (nbuckets > kMaxSymbols || bloom_size > kMaxSymbols || symoffset > kMaxSymbols)
			{
				return badImage(image.source_ + ": implausible DT_GNU_HASH header");
			}
			const uint64_t buckets_offset = *gnu_hash_offset + 16 + bloom_size * 8;
			const uint64_t chain_offset = buckets_offset + nbuckets * 4;
			if (chain_offset > image.bytes_.size())
			{
				return badImage(image.source_ + ": DT_GNU_HASH is truncated");
			}
			for (uint64_t bucket = 0; bucket < nbuckets; ++bucket)
			{
				uint32_t first = 0;
				if (!readU32(image.bytes_, buckets_offset + bucket * 4, first))
				{
					return badImage(image.source_ + ": DT_GNU_HASH buckets are truncated");
				}
				if (first < symoffset)
				{
					continue;
				}
				uint64_t index = first;
				while (true)
				{
					if (index > kMaxSymbols)
					{
						return badImage(image.source_ + ": DT_GNU_HASH chain is implausible");
					}
					uint32_t value = 0;
					if (!readU32(image.bytes_, chain_offset + (index - symoffset) * 4, value))
					{
						return badImage(image.source_ + ": DT_GNU_HASH chain is truncated");
					}
					symbol_count = std::max(symbol_count, index + 1);
					if ((value & 1u) != 0)
					{
						break;
					}
					++index;
				}
			}
			symbol_count = std::max(symbol_count, symoffset);
		}
		else if (header.e_shoff != 0 && header.e_shentsize == sizeof(Elf64_Shdr) &&
			 header.e_shnum > 0 &&
			 header.e_shoff <= image.bytes_.size() &&
			 header.e_shnum <= (image.bytes_.size() - header.e_shoff) / sizeof(Elf64_Shdr))
		{
			for (uint32_t i = 0; i < header.e_shnum; ++i)
			{
				Elf64_Shdr section{};
				if (!readAt(image.bytes_, header.e_shoff + i * sizeof(Elf64_Shdr), section))
				{
					return badImage(image.source_ + ": truncated section header table");
				}
				if (section.sh_type == SHT_DYNSYM && section.sh_entsize == sizeof(Elf64_Sym))
				{
					symbol_count = section.sh_size / sizeof(Elf64_Sym);
					break;
				}
			}
		}

		if (symtab_address != 0 && symbol_count > 0)
		{
			if (symbol_count > kMaxSymbols)
			{
				return badImage(image.source_ + ": too many dynamic symbols");
			}
			const std::optional<uint64_t> symtab_offset = image.fileOffsetOf(symtab_address);
			if (!symtab_offset.has_value() || *symtab_offset > image.bytes_.size() ||
			    symbol_count > (image.bytes_.size() - *symtab_offset) / sizeof(Elf64_Sym))
			{
				return badImage(image.source_ + ": DT_SYMTAB is out of bounds");
			}

			image.symbols_.reserve(static_cast<size_t>(symbol_count));
			for (uint64_t i = 0; i < symbol_count; ++i)
			{
				Elf64_Sym symbol{};
				if (!readAt(image.bytes_, *symtab_offset + i * sizeof(Elf64_Sym), symbol))
				{
					return badImage(image.source_ + ": truncated dynamic symbol table");
				}
				ElfSymbol converted;
				if (symbol.st_name != 0)
				{
					const std::optional<std::string_view> name = stringAt(symbol.st_name);
					if (!name.has_value())
					{
						return badImage(image.source_ + ": symbol name is out of bounds");
					}
					converted.name = std::string(*name);
				}
				converted.info = symbol.st_info;
				converted.other = symbol.st_other;
				converted.shndx = symbol.st_shndx;
				converted.value = symbol.st_value;
				converted.size = symbol.st_size;
				image.symbols_.push_back(std::move(converted));
			}
		}
	}

	auto readRelocations = [&](uint64_t address, uint64_t size, std::vector<ElfRelocation> &out) -> Result<void> {
		if (address == 0 || size == 0)
		{
			return {};
		}
		if (size % sizeof(Elf64_Rela) != 0)
		{
			return badImage(image.source_ + ": relocation table size is not a multiple of Rela");
		}
		if (size / sizeof(Elf64_Rela) > kMaxRelocations)
		{
			return badImage(image.source_ + ": too many relocations");
		}
		const std::optional<uint64_t> offset = image.fileOffsetOf(address);
		if (!offset.has_value() || *offset > image.bytes_.size() || size > image.bytes_.size() - *offset)
		{
			return badImage(image.source_ + ": relocation table is out of bounds");
		}

		const uint64_t count = size / sizeof(Elf64_Rela);
		out.reserve(static_cast<size_t>(count));
		for (uint64_t i = 0; i < count; ++i)
		{
			Elf64_Rela rela{};
			if (!readAt(image.bytes_, *offset + i * sizeof(Elf64_Rela), rela))
			{
				return badImage(image.source_ + ": truncated relocation table");
			}
			ElfRelocation converted;
			converted.offset = rela.r_offset;
			converted.type = static_cast<uint32_t>(ELF64_R_TYPE(rela.r_info));
			converted.symbol = static_cast<uint32_t>(ELF64_R_SYM(rela.r_info));
			converted.addend = rela.r_addend;
			if (converted.symbol >= image.symbols_.size())
			{
				return badImage(image.source_ + ": relocation references a symbol out of range");
			}
			out.push_back(converted);
		}
		return {};
	};

	// Android packed RELA (APS2). Format per bionic's
	// linker_reloc_iterators.h::for_all_packed_relocs: a header pair (count,
	// initial offset) followed by flag-described groups of SLEB128 deltas.
	auto readAndroidRela = [&](uint64_t address, uint64_t size) -> Result<void> {
		if (address == 0 || size == 0)
		{
			return {};
		}
		if (size < 8)
		{
			return badImage(image.source_ + ": Android packed relocations are truncated");
		}
		const std::optional<uint64_t> offset = image.fileOffsetOf(address);
		if (!offset.has_value() || *offset > image.bytes_.size() ||
		    size > image.bytes_.size() - *offset)
		{
			return badImage(image.source_ + ": Android packed relocations are out of bounds");
		}
		const uint8_t *begin = image.bytes_.data() + *offset;
		if (std::memcmp(begin, "APS2", 4) != 0)
		{
			return badImage(image.source_ + ": bad Android packed relocation magic");
		}

		Sle128Decoder decoder(begin + 4, begin + size);
		int64_t count = 0;
		int64_t relocation_offset = 0;
		int64_t info = 0;
		int64_t addend = 0;
		if (!decoder.pop(count) || count < 0 || static_cast<uint64_t>(count) > kMaxRelocations)
		{
			return badImage(image.source_ + ": Android packed relocation count is invalid");
		}
		if (!decoder.pop(relocation_offset))
		{
			return badImage(image.source_ + ": Android packed relocations are truncated");
		}

		image.relocations_.reserve(image.relocations_.size() + static_cast<size_t>(count));
		int64_t emitted = 0;
		while (emitted < count)
		{
			int64_t group_size = 0;
			int64_t flags = 0;
			if (!decoder.pop(group_size) || group_size < 0 ||
			    group_size > count - emitted)
			{
				return badImage(image.source_ + ": Android packed relocation group is invalid");
			}
			if (!decoder.pop(flags) || flags < 0)
			{
				return badImage(image.source_ + ": Android packed relocations are truncated");
			}

			constexpr int64_t kGroupedByInfo = 1;
			constexpr int64_t kGroupedByOffsetDelta = 2;
			constexpr int64_t kGroupedByAddend = 4;
			constexpr int64_t kHasAddend = 8;

			int64_t group_offset_delta = 0;
			if ((flags & kGroupedByOffsetDelta) != 0 && !decoder.pop(group_offset_delta))
			{
				return badImage(image.source_ + ": Android packed relocations are truncated");
			}
			if ((flags & kGroupedByInfo) != 0 && !decoder.pop(info))
			{
				return badImage(image.source_ + ": Android packed relocations are truncated");
			}

			const int64_t addend_mode = flags & (kGroupedByAddend | kHasAddend);
			if (addend_mode == (kGroupedByAddend | kHasAddend))
			{
				int64_t delta = 0;
				if (!decoder.pop(delta))
				{
					return badImage(image.source_ + ": Android packed relocations are truncated");
				}
				addend += delta;
			}
			else if (addend_mode == 0)
			{
				addend = 0;
			}

			for (int64_t i = 0; i < group_size; ++i)
			{
				int64_t delta = 0;
				if ((flags & kGroupedByOffsetDelta) != 0)
				{
					delta = group_offset_delta;
				}
				else if (!decoder.pop(delta))
				{
					return badImage(image.source_ + ": Android packed relocations are truncated");
				}
				relocation_offset += delta;

				if ((flags & kGroupedByInfo) == 0 && !decoder.pop(info))
				{
					return badImage(image.source_ + ": Android packed relocations are truncated");
				}
				if (addend_mode == kHasAddend)
				{
					int64_t addend_delta = 0;
					if (!decoder.pop(addend_delta))
					{
						return badImage(image.source_ + ": Android packed relocations are truncated");
					}
					addend += addend_delta;
				}

				ElfRelocation converted;
				converted.offset = static_cast<uint64_t>(relocation_offset);
				converted.type = static_cast<uint32_t>(static_cast<uint64_t>(info) & 0xffffffffu);
				converted.symbol = static_cast<uint32_t>(static_cast<uint64_t>(info) >> 32);
				converted.addend = addend;
				if (converted.symbol >= image.symbols_.size())
				{
					return badImage(image.source_ +
						": Android packed relocation references a symbol out of range");
				}
				image.relocations_.push_back(converted);
				++emitted;
			}
		}
		return {};
	};

	// DT_RELR bitmap chains, per bionic's soinfo::relocate_relr(). Expanded to
	// plain RELATIVE entries whose addend is the link-time value already stored
	// in the file image (0 for .bss slots).
	auto readRelr = [&](uint64_t address, uint64_t size, uint64_t entry_size) -> Result<void> {
		if (address == 0 || size == 0)
		{
			return {};
		}
		if (entry_size != sizeof(uint64_t))
		{
			return badImage(image.source_ + ": unexpected DT_RELRENT");
		}
		if (size % entry_size != 0)
		{
			return badImage(image.source_ + ": DT_RELRSZ is not a multiple of the entry size");
		}
		const uint64_t count = size / entry_size;
		if (count > kMaxRelocations)
		{
			return badImage(image.source_ + ": too many RELR relocations");
		}
		const std::optional<uint64_t> table_offset = image.fileOffsetOf(address);
		if (!table_offset.has_value() || *table_offset > image.bytes_.size() ||
		    size > image.bytes_.size() - *table_offset)
		{
			return badImage(image.source_ + ": DT_RELR table is out of bounds");
		}

		constexpr uint32_t kRelative = 1027;
		auto addRelative = [&](uint64_t link_address) -> Result<void> {
			uint64_t addend = 0;
			if (const std::optional<uint64_t> slot = image.fileOffsetOf(link_address);
				slot.has_value() && *slot <= image.bytes_.size() &&
				image.bytes_.size() - *slot >= sizeof(addend))
			{
				std::memcpy(&addend, image.bytes_.data() + *slot, sizeof(addend));
			}
			ElfRelocation converted;
			converted.offset = link_address;
			converted.type = kRelative;
			converted.symbol = 0;
			converted.addend = static_cast<int64_t>(addend);
			image.relocations_.push_back(converted);
			return {};
		};

		uint64_t base = 0;
		for (uint64_t i = 0; i < count; ++i)
		{
			uint64_t entry = 0;
			if (!readAt(image.bytes_, *table_offset + i * entry_size, entry))
			{
				return badImage(image.source_ + ": DT_RELR table is truncated");
			}
			if ((entry & 1) == 0)
			{
				if (auto applied = addRelative(entry); !applied.ok())
				{
					return applied.error();
				}
				base = entry + sizeof(uint64_t);
				continue;
			}
			uint64_t offset = base;
			for (uint64_t bits = entry >> 1; bits != 0; bits >>= 1, offset += sizeof(uint64_t))
			{
				if ((bits & 1) != 0)
				{
					if (auto applied = addRelative(offset); !applied.ok())
					{
						return applied.error();
					}
				}
			}
			base += 63 * sizeof(uint64_t);
		}
		return {};
	};

	if (android_rel_address != 0 || android_rel_size != 0)
	{
		return unsupported(image.source_ +
			": DT_ANDROID_REL (32-bit packed relocations) is not supported");
	}
	if (auto result = readAndroidRela(android_rela_address, android_rela_size); !result.ok())
	{
		return result.error();
	}
	if (auto result = readRelr(relr_address, relr_size, relr_ent); !result.ok())
	{
		return result.error();
	}
	if (auto result = readRelocations(rela_address, rela_size, image.relocations_); !result.ok())
	{
		return result.error();
	}
	if (auto result = readRelocations(jmprel_address, jmprel_size, image.plt_relocations_); !result.ok())
	{
		return result.error();
	}

	return image;
}

} // namespace duckemu::runtime::loader
