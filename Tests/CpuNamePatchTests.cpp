// Regression tests for CPU naming in private dyld cache mappings.
#include "../RestrictEvents/CpuNamePatch.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

namespace {
constexpr uint64_t Base = 0x7FF810000000ULL;
constexpr uint64_t Unslid = 0x7FF800000000ULL;
constexpr size_t NameOffset = 0x5FF0;
constexpr char Original[] = "\0Dual-Core Intel Core i5";
constexpr char Brand[] = "\0AMD Athlon PRO 300U w/ Radeon Vega Mobile Gfx  ";
constexpr char ImagePath[] =
	"/System/Library/PrivateFrameworks/AppleSystemInfo.framework/Versions/A/AppleSystemInfo";
constexpr uint8_t CoreFind[24] {0, 0, 0, 0, 0, 0, 0, 0, 15, 0, 0, 0, 2, 0, 0, 0, 28};
constexpr uint8_t CoreReplace[17] {0, 0, 0, 0, 0, 0, 0, 0, 15, 0, 0, 0, 2, 0, 0, 0, 32};

struct Fixture {
	std::vector<uint8_t> backing = std::vector<uint8_t>(0x10000);
	std::vector<uint8_t> local;
	int protection[16] {};
	int maximum[16] {};
	bool copied[16] {};
	unsigned writes {0}, protections {0}, failProtect {0}, failWrite {0}, failRestores {0};

	template<typename T>
	void put(size_t offset, T value) { memcpy(backing.data() + offset, &value, sizeof(value)); }

	void bytes(size_t offset, const void *value, size_t size) {
		memcpy(backing.data() + offset, value, size);
	}

	Fixture() {
		bytes(0, "dyld_v1 x86_64h", 16);
		put<uint32_t>(16, 0x200);
		put<uint32_t>(20, 1);
		put<uint64_t>(224, Unslid);
		put<uint64_t>(232, 0x10000);
		put<uint64_t>(240, 0x100000000ULL);
		put<uint32_t>(448, 0x400);
		put<uint32_t>(452, 2);
		put<uint64_t>(0x200, Unslid);
		put<uint64_t>(0x208, 0x10000);
		put<uint32_t>(0x218, 5);
		put<uint32_t>(0x21C, 5);
		put<uint64_t>(0x400, Unslid + 0x4000);
		put<uint32_t>(0x418, 0x800);
		put<uint64_t>(0x420, Unslid + 0x9000);
		put<uint32_t>(0x438, 0x900);
		bytes(0x800, ImagePath, sizeof(ImagePath));
		bytes(0x900, "/Other.dylib", 13);
		put<uint32_t>(0x4000, 0xFEEDFACF);
		put<uint32_t>(0x4004, 0x01000007);
		put<uint32_t>(0x400C, 6);
		put<uint32_t>(0x4010, 1);
		put<uint32_t>(0x4014, 232);
		put<uint32_t>(0x4020, 0x19);
		put<uint32_t>(0x4024, 232);
		bytes(0x4028, "__TEXT", 7);
		put<uint64_t>(0x4038, Unslid + 0x4000);
		put<uint64_t>(0x4040, 0x4000);
		put<uint32_t>(0x4060, 2);
		bytes(0x4068, "__cstring", 10);
		bytes(0x4078, "__TEXT", 7);
		put<uint64_t>(0x4088, Unslid + 0x5000);
		put<uint64_t>(0x4090, 0x2000);
		bytes(0x40B8, "__const", 8);
		bytes(0x40C8, "__TEXT", 7);
		put<uint64_t>(0x40D8, Unslid + 0x7000);
		put<uint64_t>(0x40E0, 0x100);
		bytes(NameOffset, Original, sizeof(Original));
		bytes(0x9020, Original, sizeof(Original));
		bytes(0x7020, CoreFind, sizeof(CoreFind));
		for (size_t i = 0; i < 16; ++i) protection[i] = maximum[i] = 5;
		reset();
	}

	void reset() { local = backing; }

	bool range(uint64_t address, size_t size) const {
		return address >= Base && address - Base <= local.size() &&
			size <= local.size() - (address - Base);
	}

	static int read(void *context, uint64_t address, void *data, size_t size) {
		auto &self = *static_cast<Fixture *>(context);
		if (!self.range(address, size)) return 1;
		memcpy(data, self.local.data() + (address - Base), size);
		return 0;
	}

	static bool query(void *context, uint64_t address, int &protection, int &maximum) {
		auto &self = *static_cast<Fixture *>(context);
		if (!self.range(address, 4096)) return false;
		auto page = (address - Base) / 4096;
		protection = self.protection[page];
		maximum = self.maximum[page];
		return true;
	}

	static int protect(void *context, uint64_t address, bool setMaximum, int protection) {
		auto &self = *static_cast<Fixture *>(context);
		assert(self.range(address, 4096));
		auto page = (address - Base) / 4096;
		if (++self.protections == self.failProtect) return 1;
		if (protection == 5 && self.failRestores > 0) {
			--self.failRestores;
			return 1;
		}
		if (protection & 0x10) {
			self.copied[page] = true;
			self.maximum[page] |= 2;
		}
		if (setMaximum) self.maximum[page] = protection;
		else self.protection[page] = protection & 7;
		return 0;
	}

	static int write(void *context, uint64_t address, const void *data, size_t size) {
		auto &self = *static_cast<Fixture *>(context);
		assert(self.range(address, size));
		bool fail = ++self.writes == self.failWrite;
		size_t length = fail ? size / 2 : size;
		for (size_t i = 0; i < length; ++i) {
			auto offset = static_cast<size_t>(address - Base) + i;
			auto page = offset / 4096;
			assert(self.protection[page] & 2);
			self.local[offset] = static_cast<const uint8_t *>(data)[i];
			if (!self.copied[page]) self.backing[offset] = self.local[offset];
		}
		return fail ? 1 : 0;
	}

	CpuNamePatch::Memory memory() { return {this, read, query, protect, write}; }

	void restored() const {
		for (size_t i = 0; i < 16; ++i) {
			assert(protection[i] == 5);
			assert(maximum[i] == 5);
		}
	}
};

CpuNamePatch::Request request(const char *brand = Brand, size_t size = sizeof(Brand), bool cores = false) {
	return {Original, sizeof(Original), brand, size,
		cores ? CoreFind : nullptr, sizeof(CoreFind), CoreReplace, sizeof(CoreReplace)};
}

CpuNamePatch::Result apply(Fixture &fixture, const CpuNamePatch::Request &patch) {
	auto memory = fixture.memory();
	CpuNamePatch::Workspace workspace {};
	return CpuNamePatch::apply(memory, Base, patch, workspace);
}

void privateNameAndIdempotence() {
	Fixture fixture;
	auto clean = fixture.backing;
	assert(apply(fixture, request()) == CpuNamePatch::Result::Applied);
	assert(memcmp(fixture.local.data() + NameOffset, Brand, sizeof(Brand)) == 0);
	assert(fixture.backing == clean);
	assert(memcmp(fixture.local.data() + 0x9020, Original, sizeof(Original)) == 0);
	fixture.restored();
	auto writes = fixture.writes;
	assert(apply(fixture, request()) == CpuNamePatch::Result::AlreadyApplied);
	assert(fixture.writes == writes);
	fixture.restored();
}

void shortAndEqualNames() {
	const char *names[] = {"\0AMD 300U", Original, "\0Dual-Core AMD PRO 300U"};
	const size_t sizes[] = {sizeof("\0AMD 300U"), sizeof(Original), sizeof("\0Dual-Core AMD PRO 300U")};
	for (size_t i = 0; i < 3; ++i) {
		Fixture fixture;
		auto clean = fixture.backing;
		auto status = apply(fixture, request(names[i], sizes[i]));
		assert(status == CpuNamePatch::Result::Applied || status == CpuNamePatch::Result::AlreadyApplied);
		assert(memcmp(fixture.local.data() + NameOffset, names[i], sizes[i]) == 0);
		assert(fixture.backing == clean);
		fixture.restored();
	}
}

void legacyTableAndPlainX64() {
	Fixture fixture;
	fixture.bytes(0, "dyld_v1  x86_64", 16);
	fixture.put<uint32_t>(24, 0x400);
	fixture.put<uint32_t>(28, 2);
	fixture.put<uint32_t>(448, 0);
	fixture.put<uint32_t>(452, 0);
	fixture.reset();
	assert(apply(fixture, request()) == CpuNamePatch::Result::Applied);
	fixture.restored();
}

void rejectAmbiguousOrMalformedImages() {
	for (unsigned scenario = 0; scenario < 7; ++scenario) {
		Fixture fixture;
		switch (scenario) {
			case 0: fixture.bytes(0x5700, Original, sizeof(Original)); break;
			case 1: fixture.put<uint32_t>(448, 0xFFFFFFF0); break;
			case 2: fixture.put<uint32_t>(0x4024, 0); break;
			case 3: fixture.put<uint64_t>(0x4088, UINT64_MAX - 5); break;
			case 4: fixture.put<uint64_t>(232, UINT64_MAX); break;
			case 5: fixture.put<uint32_t>(0x418, 0xFFFFFFF0); break;
			case 6: fixture.bytes(0x900, ImagePath, sizeof(ImagePath)); break;
		}
		fixture.reset();
		auto clean = fixture.backing;
		assert(apply(fixture, request()) != CpuNamePatch::Result::Applied);
		assert(fixture.writes == 0);
		assert(fixture.local == clean && fixture.backing == clean);
		fixture.restored();
	}
}

void transactionFailures() {
	for (unsigned scenario = 0; scenario < 4; ++scenario) {
		Fixture fixture;
		auto clean = fixture.backing;
		switch (scenario) {
			case 0: fixture.failProtect = 2; break;
			case 1: fixture.failWrite = 1; break;
			case 2: fixture.failWrite = 2; break;
			case 3: fixture.failRestores = 2; break;
		}
		assert(apply(fixture, request(Brand, sizeof(Brand), true)) == CpuNamePatch::Result::Failed);
		assert(fixture.local == clean && fixture.backing == clean);
		fixture.restored();
	}
}

void coreCountAndReadOnlyMaximum() {
	Fixture fixture;
	auto clean = fixture.backing;
	fixture.protection[7] = 1;
	fixture.maximum[7] = 3;
	assert(apply(fixture, request(Brand, sizeof(Brand), true)) == CpuNamePatch::Result::Applied);
	assert(memcmp(fixture.local.data() + 0x7020, CoreReplace, sizeof(CoreReplace)) == 0);
	assert(fixture.backing == clean);
	assert(fixture.protection[7] == 1 && fixture.maximum[7] == 3);
}
}

int main() {
	privateNameAndIdempotence();
	shortAndEqualNames();
	legacyTableAndPlainX64();
	rejectAmbiguousOrMalformedImages();
	transactionFailures();
	coreCountAndReadOnlyMaximum();
	std::puts("CPU name private-page regression tests passed");
}
