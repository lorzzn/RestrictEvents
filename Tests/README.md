# CPU naming regression tests

The portable suite runs the production cache parser and page transaction.
It checks private replacement, unchanged backing bytes, cross-page names,
image scoping, legacy and modern image tables, idempotence, malformed input,
partial-write rollback and restoration of current and maximum protections.
The VM adapter models the boundary of the kernel VM operations.

Run with Clang or GCC and assertions enabled.

```sh
c++ -std=c++14 -Wall -Wextra -Werror Tests/CpuNamePatchTests.cpp -o /tmp/cpu-name-tests
/tmp/cpu-name-tests
```

On Intel macOS, also run the production transaction against real file-backed
Mach VM mappings. It verifies both pages of a cross-page write, an independent
mapping, file contents and original protections.

```sh
c++ -std=c++14 -Wall -Wextra -Werror Tests/NativeVmTests.cpp -o /tmp/cpu-name-vm-tests
/tmp/cpu-name-vm-tests
```

`--inspect-running-cache` adds a read-only parser/plan check against the running
OS cache for the AMD Athlon PRO 300U diagnosis. It requires that cache to contain
the original dual-core i5 name or the diagnosed replacement. The generic VM
test above has no CPU model requirement.

These checks do not exercise the installed kernel hook, code-signing rules of
individual system applications, or cache eviction during sustained use.
