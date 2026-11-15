//
// Apply CPU naming to the current task after dyld's shared region is mapped.
//
#include <Headers/kern_api.hpp>
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <sys/proc.h>

#include "CpuNamePatch.hpp"

namespace {
struct SharedRegionCheckArgs {
	user_addr_t startAddress;
};
static_assert(sizeof(SharedRegionCheckArgs) == 8, "64-bit syscall arguments required");
static_assert(PAGE_SIZE == CpuNamePatch::PageSize, "Unexpected x86 VM page size");
static_assert(VM_PROT_COPY == CpuNamePatch::Copy, "Unexpected VM_PROT_COPY");

using SharedRegionCheck = int (*)(proc_t, SharedRegionCheckArgs *, int *);
using GetTaskMap = vm_map_t (*)(task_t);
using RegionRecurse = kern_return_t (*)(vm_map_t, mach_vm_address_t *,
	mach_vm_size_t *, natural_t *, vm_region_recurse_info_t, mach_msg_type_number_t *);
using CopyIn = int (*)(user_addr_t, void *, size_t);
using CopyOut = int (*)(const void *, user_addr_t, size_t);

mach_vm_address_t originalCheck;
GetTaskMap getTaskMap;
RegionRecurse regionRecurse;
CopyIn copyIn;
CopyOut copyOut;
IOLock *patchLock;
CpuNamePatch::Request request {};
uint32_t appliedLogs, failureLogs, unmatchedLogs;

int readCurrent(void *, uint64_t address, void *data, size_t size) {
	return copyIn(address, data, size);
}

int writeCurrent(void *, uint64_t address, const void *data, size_t size) {
	return copyOut(data, address, size);
}

bool queryCurrent(void *context, uint64_t address, int &protection, int &maximum) {
	auto map = static_cast<vm_map_t>(context);
	natural_t depth = 0;
	for (unsigned attempt = 0; attempt < 16; ++attempt) {
		mach_vm_address_t region = address;
		mach_vm_size_t size = 0;
		vm_region_submap_short_info_data_64_t info {};
		mach_msg_type_number_t count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
		if (regionRecurse(map, &region, &size, &depth,
			reinterpret_cast<vm_region_recurse_info_t>(&info), &count) != KERN_SUCCESS ||
			count < VM_REGION_SUBMAP_SHORT_INFO_COUNT_64 ||
			region > address || address - region > size || PAGE_SIZE > size - (address - region))
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

int protectCurrent(void *context, uint64_t address, bool maximum, int protection) {
	return vm_protect(static_cast<vm_map_t>(context), address, PAGE_SIZE, maximum, protection);
}

int sharedRegionCheck(proc_t process, SharedRegionCheckArgs *args, int *returnValue) {
	auto result = reinterpret_cast<SharedRegionCheck>(originalCheck)(process, args, returnValue);
	if (result || !args || !args->startAddress)
		return result;
	uint64_t base = 0;
	if (copyIn(args->startAddress, &base, sizeof(base)) != 0 || !base)
		return result;
	auto map = getTaskMap(current_task());
	if (!map) return result;
	auto work = static_cast<CpuNamePatch::Workspace *>(IOMalloc(sizeof(CpuNamePatch::Workspace)));
	if (!work) return result;
	memset(work, 0, sizeof(*work));
	CpuNamePatch::Memory memory {map, readCurrent, queryCurrent, protectCurrent, writeCurrent};

	// Serialize only our private-page transaction. The native syscall runs
	// without this lock, and no per-process address or task is retained.
	IOLockLock(patchLock);
	auto status = CpuNamePatch::apply(memory, base, request, *work);
	IOLockUnlock(patchLock);
	if (status == CpuNamePatch::Result::Applied &&
		__atomic_fetch_add(&appliedLogs, 1, __ATOMIC_RELAXED) < 8) {
		SYSLOG("rev", "private CPU name patch applied for pid %d", proc_pid(process));
	} else if (status == CpuNamePatch::Result::Failed &&
		__atomic_fetch_add(&failureLogs, 1, __ATOMIC_RELAXED) < 8) {
		SYSLOG("rev", "private CPU name patch failed for pid %d: %s", proc_pid(process),
			work->error ? work->error : "page discovery");
	} else if (status == CpuNamePatch::Result::Unmatched &&
		__atomic_fetch_add(&unmatchedLogs, 1, __ATOMIC_RELAXED) < 4) {
		DBGLOG("rev", "private CPU name patch unmatched for pid %d, reads %lu", proc_pid(process), work->reads);
	}
	IOFree(work, sizeof(*work));
	return result;
}
}

void initCpuNamePatcher(KernelPatcher &patcher, const CpuNamePatch::Request &patch) {
	getTaskMap = reinterpret_cast<GetTaskMap>(patcher.solveSymbol(KernelPatcher::KernelID, "_get_task_map"));
	patcher.clearError();
	regionRecurse = reinterpret_cast<RegionRecurse>(patcher.solveSymbol(KernelPatcher::KernelID, "_mach_vm_region_recurse"));
	patcher.clearError();
	copyIn = reinterpret_cast<CopyIn>(patcher.solveSymbol(KernelPatcher::KernelID, "_copyin"));
	patcher.clearError();
	copyOut = reinterpret_cast<CopyOut>(patcher.solveSymbol(KernelPatcher::KernelID, "_copyout"));
	patcher.clearError();
	if (!getTaskMap || !regionRecurse || !copyIn || !copyOut) {
		SYSLOG("rev", "private CPU name patch unavailable: missing VM symbols");
		return;
	}
	patchLock = IOLockAlloc();
	if (!patchLock) {
		SYSLOG("rev", "private CPU name patch unavailable: lock allocation");
		return;
	}
	request = patch;
	KernelPatcher::RouteRequest route {"_shared_region_check_np", sharedRegionCheck, originalCheck};
	if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &route, 1)) {
		patcher.clearError();
		IOLockFree(patchLock);
		patchLock = nullptr;
		SYSLOG("rev", "private CPU name patch unavailable: shared-region hook");
		return;
	}
	SYSLOG("rev", "private CPU name patch enabled (COW revision 1)");
}
