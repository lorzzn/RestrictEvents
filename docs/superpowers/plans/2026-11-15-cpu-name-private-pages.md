# CPU name private page repair

The observed macOS 15.8.3 crash reports show SIGBUS, page-in error 94 and an
APFS compressed data hash mismatch. Replacing the original CPU string with
the 47-byte CPUID brand in a copy of the affected 16 KB block reproduces the
reported bad SHA-256 exactly.

The repair keeps CPU naming options and uses the official master commit
9265e787bbd288ef6f7d5cee2cb77e751377b4ef as its base. On Big Sur and newer,
CPU naming moves out of the code signature validation callback. After the
native shared-region check succeeds, the current process's dyld image table
and AppleSystemInfo Mach-O sections identify the string. Writes use private
copy-on-write pages, with readback, rollback and protection restoration.
Earlier systems retain their existing implementation.

Assumptions and limits

- Support the two Intel 64-bit cache magic strings and both legacy and modern
  image tables. Reject malformed, ambiguous or unsupported layouts.
- Preserve `revcpu`, `revcpuname`, `revpatch` and core-count selection.
- Preserve the original shared cache bytes and normal APFS validation.
- Bluetooth changes are outside this repair.
- Automated tests and builds do not establish reboot or long-running runtime
  success on the affected computer.

Implementation sequence

- [x] Add regression cases for private writes, unchanged backing bytes, image
  scoping, cross-page names, repeated calls and transaction failures.
- [x] Implement bounded cache/image parsing and the private page transaction.
- [x] Connect the shared-region hook and remove modern CPU writes from the
  validation callback.
- [ ] Run the portable suite, build Debug and Release, review the change and
  publish the fork branch and usable artifacts.
- [ ] Verify the VM operation on macOS and document remaining runtime checks.

Verification on November 15, 2026

- GCC portable regression suite passed.
- Apple Clang portable suite passed with ASan and UBSan, recovery disabled.
- Removing VM_PROT_COPY in an isolated mutation test failed the unchanged
  backing-bytes assertion as expected.
- Real cross-page Mach VM writes on macOS 15.8.3 preserved file contents, an
  independent shared mapping and current/maximum protections.
- Read-only inspection parsed the running AppleSystemInfo image and its
  2532-byte cstring section. The existing driver had already renamed its CPU
  string, so the planner correctly produced zero additional writes.
- Kernel-hook installation, reboot and sustained crash-log checks remain
  unverified at this point.

Primary references

- [RestrictEvents](https://github.com/acidanthera/RestrictEvents/blob/9265e787bbd288ef6f7d5cee2cb77e751377b4ef/RestrictEvents/RestrictEvents.cpp)
- [dyld cache format](https://github.com/apple-oss-distributions/dyld/blob/main/include/mach-o/dyld_cache_format.h)
- [Mach-O format](https://github.com/apple-oss-distributions/xnu/blob/main/EXTERNAL_HEADERS/mach-o/loader.h)
- [VM_PROT_COPY](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/vm_prot.h)
- [Shared-region syscall](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/vm/vm_unix.c)
