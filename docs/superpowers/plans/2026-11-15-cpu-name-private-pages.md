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
On macOS 10.8 through 10.15, CPU naming uses the validation callback.

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
- [x] Run the portable suite, build Debug and Release, review the change and
  publish the fork branch and usable artifacts.
- [x] Verify the VM operation on macOS and document remaining runtime checks.

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
- Debug and Release builds and both Clang static analysis configurations passed
  in [CI run 43692573054](https://github.com/lorzzn/RestrictEvents/actions/runs/43692573054).
  The expanded regression suite and the same build/analysis jobs also passed in
  [CI run 43693706307](https://github.com/lorzzn/RestrictEvents/actions/runs/43693706307).
- A subsequent real-cache probe failed while reading that string section, before
  any CPU write. Mach reported KERN_INVALID_ADDRESS at `0x7ff8293c2754`.
  At 16:08:14 and 16:09:38 +0800, the kernel logged the same compressed-block
  hash mismatch as the original reports. RestrictEvents 1.1.6 was still loaded.
  The extended real-cache write probe therefore remains unverified.
- Kernel-hook installation, reboot and sustained crash-log checks remain
  unverified at this point.

The affected block starts at file offset `240893952` (`0xE5BC000`), with size
16384. Its expected SHA-256 is
`eb1ff268f54b45b15b2d744262b07efc8719b4306beb94133995859d710cc87e`.
The hash after the diagnosed CPU replacement is
`6832de8ff4a66814ed387c3ff9754a8e1c779c16ffb24e4d24f48c79768cf690`.

Remaining runtime validation

Boot the candidate with a fresh shared cache, confirm the requested CPU name,
and repeatedly open System Settings and About This Mac. Exercise the Chinese
input method and Notification Center. Check for the same compressed-block
hash and page-in failures during several hours of use, including page reloads.

Primary references

- [RestrictEvents](https://github.com/acidanthera/RestrictEvents/blob/9265e787bbd288ef6f7d5cee2cb77e751377b4ef/RestrictEvents/RestrictEvents.cpp)
- [dyld cache format](https://github.com/apple-oss-distributions/dyld/blob/main/include/mach-o/dyld_cache_format.h)
- [Mach-O format](https://github.com/apple-oss-distributions/xnu/blob/main/EXTERNAL_HEADERS/mach-o/loader.h)
- [VM_PROT_COPY](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/vm_prot.h)
- [Shared-region syscall](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/vm/vm_unix.c)
