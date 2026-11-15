//
// CPU name discovery and private-page writes. No file-backed page is writable
// through this interface. The kernel adapter supplies current-task VM operations.
//
#ifndef CPU_NAME_PATCH_HPP
#define CPU_NAME_PATCH_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace CpuNamePatch {
constexpr uint64_t UserLimit = 0x0000800000000000ULL;
constexpr size_t PageSize = 4096;
constexpr size_t MaxPatchSize = 64;
constexpr int Read = 1, Write = 2, Execute = 4, Copy = 0x10;

struct Memory {
	void *context;
	int (*read)(void *, uint64_t, void *, size_t);
	bool (*query)(void *, uint64_t, int &, int &);
	int (*protect)(void *, uint64_t, bool, int);
	int (*write)(void *, uint64_t, const void *, size_t);
};

struct Request {
	const void *find;
	size_t findSize;
	const void *replace;
	size_t replaceSize;
	const void *coreFind;
	size_t coreFindSize;
	const void *coreReplace;
	size_t coreReplaceSize;
};

enum class Result { Unmatched, AlreadyApplied, Applied, Failed };

struct Range {
	uint64_t address {0}, size {0};
	bool contains(uint64_t value, uint64_t length) const {
		return value >= address && value - address <= size &&
			length <= size - (value - address);
	}
};

struct Site {
	uint64_t address {0};
	size_t size {0};
	const void *replacement {nullptr};
	uint8_t saved[MaxPatchSize] {};
};

struct Page {
	uint64_t address {0};
	int protection {0}, maximum {0};
	bool changed {false};
};

struct Workspace {
	uint8_t scan[PageSize + MaxPatchSize] {};
	uint8_t entries[512] {};
	Range cache, strings, constants[8];
	size_t constantCount {0}, siteCount {0}, pageCount {0}, reads {0}, bytes {0};
	Site sites[2];
	Page pages[4];
	const char *error {nullptr};
};

inline uint32_t word(const uint8_t *data) {
	uint32_t value;
	memcpy(&value, data, sizeof(value));
	return value;
}

inline uint64_t wide(const uint8_t *data) {
	uint64_t value;
	memcpy(&value, data, sizeof(value));
	return value;
}

inline bool read(Memory &memory, Workspace &work, uint64_t address, void *data, size_t size) {
	if (address >= UserLimit || size > UserLimit - address ||
		work.reads >= 8192 || size > 4 * 1024 * 1024 - work.bytes)
		return false;
	++work.reads;
	work.bytes += size;
	return memory.read(memory.context, address, data, size) == 0;
}

// All image table offsets refer to the main cache's first file mapping.
// Format: apple-oss-distributions/dyld/include/mach-o/dyld_cache_format.h.
inline bool findImage(Memory &memory, uint64_t base, Workspace &work,
	uint64_t &image, uint64_t &slide) {
	static constexpr char path[] =
		"/System/Library/PrivateFrameworks/AppleSystemInfo.framework/Versions/A/AppleSystemInfo";
	uint8_t header[456] {};
	if ((base & (PageSize - 1)) || !read(memory, work, base, header, 32) ||
		(memcmp(header, "dyld_v1 x86_64h", 16) != 0 &&
		 memcmp(header, "dyld_v1  x86_64", 16) != 0))
		return false;
	auto mappingOffset = word(header + 16), mappingCount = word(header + 20);
	if (mappingOffset < 248 || mappingOffset > 65536 || !mappingCount || mappingCount > 64)
		return false;
	auto headerSize = mappingOffset < sizeof(header) ? mappingOffset : sizeof(header);
	if (!read(memory, work, base + 32, header + 32, headerSize - 32))
		return false;
	auto unslid = wide(header + 224), cacheSize = wide(header + 232);
	if (!unslid || unslid > base || base - unslid > wide(header + 240) ||
		!cacheSize || cacheSize > 0x1000000000ULL || cacheSize > UserLimit - base)
		return false;
	slide = base - unslid;
	work.cache = {base, cacheSize};
	uint8_t mapping[32];
	if (!work.cache.contains(base + mappingOffset, sizeof(mapping)) ||
		!read(memory, work, base + mappingOffset, mapping, sizeof(mapping)) ||
		wide(mapping) != unslid || wide(mapping + 16) != 0 || !(word(mapping + 28) & Read))
		return false;
	auto metadataSize = wide(mapping + 8);
	if (!metadataSize || metadataSize > cacheSize)
		return false;
	Range metadata {base, metadataSize < 16 * 1024 * 1024 ? metadataSize : 16 * 1024 * 1024};
	if (!metadata.contains(base + mappingOffset, static_cast<uint64_t>(mappingCount) * 32))
		return false;
	auto imagesOffset = word(header + 24), imageCount = word(header + 28);
	if (!imagesOffset && !imageCount && headerSize >= sizeof(header)) {
		imagesOffset = word(header + 448);
		imageCount = word(header + 452);
	}
	if (!imageCount || imageCount > 8192 || imagesOffset < headerSize ||
		!metadata.contains(base + imagesOffset, static_cast<uint64_t>(imageCount) * 32))
		return false;
	Range pathWindow;
	for (uint32_t i = 0; i < imageCount;) {
		auto count = imageCount - i < 16 ? imageCount - i : 16;
		if (!read(memory, work, base + imagesOffset + static_cast<uint64_t>(i) * 32,
			work.entries, count * 32))
			return false;
		for (uint32_t j = 0; j < count; ++j) {
			const auto *entry = work.entries + j * 32;
			auto pathAddress = base + word(entry + 24);
			if (!metadata.contains(pathAddress, sizeof(path)))
				return false;
			if (!pathWindow.contains(pathAddress, sizeof(path))) {
				auto remaining = metadata.size - (pathAddress - base);
				auto length = remaining < PageSize ? static_cast<size_t>(remaining) : PageSize;
				if (!read(memory, work, pathAddress, work.scan, length))
					return false;
				pathWindow = {pathAddress, length};
			}
			if (memcmp(work.scan + (pathAddress - pathWindow.address), path, sizeof(path)) != 0)
				continue;
			auto address = wide(entry);
			if (image || address < unslid || address >= UserLimit ||
				slide > UserLimit - address || !work.cache.contains(address + slide, 32))
				return false;
			image = address + slide;
		}
		i += count;
	}
	return image != 0;
}

inline bool findSections(Memory &memory, uint64_t image, uint64_t slide, Workspace &work) {
	uint8_t header[32];
	if (!read(memory, work, image, header, sizeof(header)) ||
		word(header) != 0xFEEDFACF || word(header + 4) != 0x01000007 || word(header + 12) != 6)
		return false;
	auto count = word(header + 16), size = word(header + 20);
	if (!count || count > 128 || size > 65536 || !work.cache.contains(image + 32, size))
		return false;
	Range commands {image + 32, size};
	uint64_t cursor = commands.address;
	for (uint32_t i = 0; i < count; ++i) {
		uint8_t command[72];
		if (!commands.contains(cursor, 8) || !read(memory, work, cursor, command, 8))
			return false;
		auto commandSize = word(command + 4);
		if (commandSize < 8 || (commandSize & 7) || !commands.contains(cursor, commandSize))
			return false;
		if (word(command) == 0x19) {
			if (commandSize < sizeof(command) || !read(memory, work, cursor, command, sizeof(command)))
				return false;
			auto sectionCount = word(command + 64);
			if (sectionCount > 64 || commandSize != 72 + sectionCount * 80)
				return false;
			auto address = wide(command + 24), segmentSize = wide(command + 32);
			if (address >= UserLimit || slide > UserLimit - address ||
				!work.cache.contains(address + slide, segmentSize))
				return false;
			Range segment {address + slide, segmentSize};
			bool text = memcmp(command + 8, "__TEXT\0", 7) == 0;
			bool data = memcmp(command + 8, "__DATA_CONST\0", 13) == 0;
			if (text && (segment.address != image || !segment.contains(commands.address, commands.size)))
				return false;
			for (uint32_t j = 0; j < sectionCount && (text || data); ++j) {
				uint8_t section[80];
				if (!read(memory, work, cursor + 72 + j * 80, section, sizeof(section)) ||
					memcmp(section + 16, command + 8, 16) != 0)
					return false;
				auto sectionAddress = wide(section + 32), sectionSize = wide(section + 40);
				if (sectionAddress >= UserLimit || slide > UserLimit - sectionAddress ||
					!segment.contains(sectionAddress + slide, sectionSize))
					return false;
				Range range {sectionAddress + slide, sectionSize};
				if (text && memcmp(section, "__cstring\0", 10) == 0) {
					if (work.strings.size || !sectionSize || sectionSize > 1024 * 1024)
						return false;
					work.strings = range;
				} else if (memcmp(section, "__const\0", 8) == 0 && sectionSize) {
					if (work.constantCount == 8 || sectionSize > 1024 * 1024)
						return false;
					work.constants[work.constantCount++] = range;
				}
			}
		}
		cursor += commandSize;
	}
	return cursor == commands.address + commands.size && work.strings.size != 0;
}

// Overlapping reads detect strings crossing a VM page without scanning another
// image or counting the overlap twice. Ambiguous candidates are rejected.
inline bool scan(Memory &memory, Workspace &work, Range range, const void *pattern,
	size_t size, uint64_t &match) {
	if (!pattern || !size || size > MaxPatchSize)
		return false;
	for (uint64_t offset = 0; offset < range.size; offset += PageSize) {
		auto remaining = range.size - offset;
		size_t length = remaining < PageSize + size - 1 ?
			static_cast<size_t>(remaining) : PageSize + size - 1;
		if (!read(memory, work, range.address + offset, work.scan, length))
			return false;
		for (size_t i = 0; i < PageSize && i + size <= length; ++i) {
			if (memcmp(work.scan + i, pattern, size) != 0)
				continue;
			if (match) return false;
			match = range.address + offset + i;
		}
	}
	return true;
}

inline bool addSite(Memory &memory, Workspace &work, Range range, uint64_t address,
	const void *replacement, size_t size) {
	if (!size || size > MaxPatchSize || !range.contains(address, size) || work.siteCount == 2)
		return false;
	auto &site = work.sites[work.siteCount++];
	site.address = address;
	site.size = size;
	site.replacement = replacement;
	return read(memory, work, address, site.saved, size);
}

inline bool plan(Memory &memory, Workspace &work, const Request &request) {
	uint64_t name {0};
	if (!scan(memory, work, work.strings, request.find, request.findSize, name))
		return false;
	if (!name) {
		if (!scan(memory, work, work.strings, request.replace, request.replaceSize, name) || !name)
			return false;
	} else if (request.findSize != request.replaceSize ||
		memcmp(request.find, request.replace, request.findSize) != 0) {
		if (!addSite(memory, work, work.strings, name, request.replace, request.replaceSize))
			return false;
	}
	if (!request.coreFind)
		return true;
	uint64_t core {0};
	Range coreRange;
	for (size_t i = 0; i < work.constantCount; ++i) {
		if (!scan(memory, work, work.constants[i], request.coreFind, request.coreFindSize, core))
			return false;
		if (core && work.constants[i].contains(core, request.coreFindSize))
			coreRange = work.constants[i];
	}
	if (core)
		return addSite(memory, work, coreRange, core, request.coreReplace, request.coreReplaceSize);
	for (size_t i = 0; i < work.constantCount; ++i) {
		if (!scan(memory, work, work.constants[i], request.coreReplace, request.coreReplaceSize, core))
			return false;
	}
	return core != 0;
}

inline bool restore(Memory &memory, Workspace &work) {
	bool restored = true;
	for (size_t i = 0; i < work.pageCount; ++i) {
		auto &page = work.pages[i];
		if (!page.changed) continue;
		bool done = false;
		for (unsigned attempt = 0; attempt < 2 && !done; ++attempt) {
			done = memory.protect(memory.context, page.address, false, page.protection) == 0 &&
				memory.protect(memory.context, page.address, true, page.maximum) == 0;
		}
		if (done) page.changed = false;
		else restored = false;
	}
	return restored;
}

inline bool makeWritable(Memory &memory, Page &page) {
	page.changed = true;
	// VM_PROT_COPY creates a private shadow and adds write permission to its
	// maximum protection. Removing X temporarily avoids a W+X mapping.
	return memory.protect(memory.context, page.address, false, Read | Write | Copy) == 0;
}

inline Result commit(Memory &memory, Workspace &work) {
	for (size_t i = 0; i < work.siteCount; ++i) {
		const auto &site = work.sites[i];
		auto last = (site.address + site.size - 1) & ~(PageSize - 1ULL);
		for (auto address = site.address & ~(PageSize - 1ULL); address <= last; address += PageSize) {
			bool present = false;
			for (size_t j = 0; j < work.pageCount; ++j)
				if (work.pages[j].address == address) present = true;
			if (present) continue;
			if (work.pageCount == 4) return Result::Failed;
			auto &page = work.pages[work.pageCount++];
			page.address = address;
			if (!memory.query(memory.context, address, page.protection, page.maximum) ||
				!(page.protection & Read) || (page.protection & Write)) {
				work.error = "page protection";
				return Result::Failed;
			}
		}
	}
	for (size_t i = 0; i < work.pageCount; ++i) {
		if (!makeWritable(memory, work.pages[i])) {
			work.error = restore(memory, work) ? "private protection" : "protection restoration";
			return Result::Failed;
		}
	}
	size_t attempted = 0;
	bool verified = true;
	for (size_t i = 0; i < work.siteCount; ++i) {
		const auto &site = work.sites[i];
		attempted = i + 1; // A failed copyout can have written a prefix.
		if (memory.write(memory.context, site.address, site.replacement, site.size) != 0 ||
			!read(memory, work, site.address, work.scan, site.size) ||
			memcmp(work.scan, site.replacement, site.size) != 0) {
			work.error = "write or readback";
			verified = false;
			break;
		}
	}
	if (verified && restore(memory, work))
		return Result::Applied;
	if (verified) work.error = "protection restoration";

	// Some pages may already be RX again after a partial restore. Re-establish
	// private write access before restoring all attempted sites.
	bool writable = true;
	for (size_t i = 0; i < work.pageCount; ++i)
		if (!makeWritable(memory, work.pages[i])) writable = false;
	bool rolledBack = writable;
	if (writable) {
		for (size_t i = 0; i < attempted; ++i) {
			const auto &site = work.sites[i];
			if (memory.write(memory.context, site.address, site.saved, site.size) != 0 ||
				!read(memory, work, site.address, work.scan, site.size) ||
				memcmp(work.scan, site.saved, site.size) != 0)
				rolledBack = false;
		}
	}
	if (!restore(memory, work)) work.error = "protection restoration";
	else if (!rolledBack) work.error = "rollback";
	return Result::Failed;
}

inline Result apply(Memory &memory, uint64_t base, const Request &request, Workspace &work) {
	if (!request.find || !request.replace || !request.findSize || !request.replaceSize ||
		request.findSize > MaxPatchSize || request.replaceSize > MaxPatchSize ||
		(request.coreFind && (!request.coreReplace || !request.coreFindSize ||
		 !request.coreReplaceSize || request.coreFindSize > MaxPatchSize ||
		 request.coreReplaceSize > MaxPatchSize)))
		return Result::Unmatched;
	uint64_t image {0}, slide {0};
	if (!findImage(memory, base, work, image, slide) ||
		!findSections(memory, image, slide, work) || !plan(memory, work, request))
		return Result::Unmatched;
	if (!work.siteCount) return Result::AlreadyApplied;
	return commit(memory, work);
}
}

#endif
