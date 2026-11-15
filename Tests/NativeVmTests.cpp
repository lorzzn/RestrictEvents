// Run on Intel macOS. Exercise the production transaction with real Mach VM,
// then inspect the running OS's cache using the production parser.
#include "../RestrictEvents/CpuNamePatch.hpp"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
int readCurrent(void *, uint64_t address, void *data, size_t size) {
	mach_vm_size_t copied = 0;
	auto result = mach_vm_read_overwrite(mach_task_self(), address, size,
		reinterpret_cast<mach_vm_address_t>(data), &copied);
	return result == KERN_SUCCESS && copied == size ? 0 : 1;
}

bool queryCurrent(void *, uint64_t address, int &protection, int &maximum) {
	natural_t depth = 0;
	for (unsigned i = 0; i < 16; ++i) {
		mach_vm_address_t region = address;
		mach_vm_size_t size = 0;
		vm_region_submap_short_info_data_64_t info {};
		mach_msg_type_number_t count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
		if (mach_vm_region_recurse(mach_task_self(), &region, &size, &depth,
			reinterpret_cast<vm_region_recurse_info_t>(&info), &count) != KERN_SUCCESS ||
			count < VM_REGION_SUBMAP_SHORT_INFO_COUNT_64 || region > address ||
			address - region > size || 4096 > size - (address - region))
			return false;
		if (!info.is_submap) {
			protection = info.protection;
			maximum = info.max_protection;
			return true;
		}
		++depth;
	}
	return false;
}

int protectCurrent(void *, uint64_t address, bool maximum, int protection) {
	return mach_vm_protect(mach_task_self(), address, 4096, maximum, protection);
}

int writeCurrent(void *, uint64_t address, const void *data, size_t size) {
	return mach_vm_write(mach_task_self(), address,
		reinterpret_cast<vm_offset_t>(data), static_cast<mach_msg_type_number_t>(size));
}

CpuNamePatch::Memory memory {nullptr, readCurrent, queryCurrent, protectCurrent, writeCurrent};

void realPrivateMapping() {
	assert(getpagesize() == 4096);
	char path[] = "/tmp/restrict-events-vm-XXXXXX";
	int fd = mkstemp(path);
	assert(fd >= 0);
	std::vector<uint8_t> clean(8192, 0xA5), observed(8192);
	assert(write(fd, clean.data(), clean.size()) == static_cast<ssize_t>(clean.size()));
	auto mapping = mmap(nullptr, clean.size(), PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
	assert(mapping != MAP_FAILED);
	auto other = mmap(nullptr, clean.size(), PROT_READ, MAP_SHARED, fd, 0);
	assert(other != MAP_FAILED);
	auto base = reinterpret_cast<uint64_t>(mapping);
	int protection[2], maximum[2];
	for (size_t i = 0; i < 2; ++i)
		assert(queryCurrent(nullptr, base + i * 4096, protection[i], maximum[i]));
	static constexpr char brand[] = "\0AMD Athlon PRO 300U w/ Radeon Vega Mobile Gfx  ";
	CpuNamePatch::Workspace work {};
	work.siteCount = 1;
	auto &site = work.sites[0];
	site.address = base + 4096 - 10;
	site.size = sizeof(brand);
	site.replacement = brand;
	assert(readCurrent(nullptr, site.address, site.saved, site.size) == 0);
	assert(CpuNamePatch::commit(memory, work) == CpuNamePatch::Result::Applied);
	assert(memcmp(reinterpret_cast<void *>(site.address), brand, sizeof(brand)) == 0);
	assert(pread(fd, observed.data(), observed.size(), 0) == static_cast<ssize_t>(observed.size()));
	assert(clean == observed);
	assert(memcmp(other, clean.data(), clean.size()) == 0);
	for (size_t i = 0; i < 2; ++i) {
		int current, max;
		assert(queryCurrent(nullptr, base + i * 4096, current, max));
		assert(current == protection[i] && max == maximum[i]);
	}
	munmap(mapping, clean.size());
	munmap(other, clean.size());
	close(fd);
	unlink(path);
	std::puts("Mach VM cross-page private writes preserve file, other mapping and original protections");
}

void runningCacheLayout() {
	using CacheRange = const void *(*)(size_t *);
	auto cacheRange = reinterpret_cast<CacheRange>(dlsym(RTLD_DEFAULT, "_dyld_get_shared_cache_range"));
	assert(cacheRange);
	size_t size = 0;
	auto base = reinterpret_cast<uint64_t>(cacheRange(&size));
	assert(base && size);
	CpuNamePatch::Workspace work {};
	uint64_t image = 0, slide = 0;
	assert(CpuNamePatch::findImage(memory, base, work, image, slide));
	assert(CpuNamePatch::findSections(memory, image, slide, work));
	static constexpr char original[] = "\0Dual-Core Intel Core i5";
	static constexpr char brand[] = "\0AMD Athlon PRO 300U w/ Radeon Vega Mobile Gfx  ";
	CpuNamePatch::Request request {original, sizeof(original), brand, sizeof(brand), nullptr, 0, nullptr, 0};
	assert(CpuNamePatch::plan(memory, work, request));
	std::printf("Running cache AppleSystemInfo parsed, cstring size %llu, planned writes %zu (read-only inspection)\n",
		static_cast<unsigned long long>(work.strings.size), work.siteCount);
}
}

int main(int argc, char **argv) {
	realPrivateMapping();
	if (argc == 2 && strcmp(argv[1], "--inspect-running-cache") == 0)
		runningCacheLayout();
}
