# Dynamic Kernel Extensibility in xv6

## Current implementation status (final readiness audit)

This status supersedes the phase snapshots below wherever they describe
features as proposed, deferred, or not implemented. Those passages record the
state at the time of each earlier phase.

- Runtime loading, initialization, command dispatch, listing, and restricted
  unloading are implemented for the strict ELF64 RISC-V ET_REL subset. The
  versioned interface in current source is `module_api_v2`; `hello`,
  `sysmon`, `procmon`, and `klogger` are separate filesystem objects.
- Modules require `MODULES=1` and one hart. Secondary harts are parked in
  M-mode in that build. No SMP module execution or remote fence/shootdown is
  supported. Module code is privileged and is not sandboxed.
- Module code pages are mapped RX after the writable direct-map aliases are
  removed; read-only data is R/NX, and mutable data/BSS is RW/NX. Permission
  changes use local `sfence.vma` and `fence.i`. The kernel ELF linker's
  RWX LOAD-segment warning describes the ELF load segment; runtime Sv39 PTEs
  still map kernel text RX and allocator RAM RW/NX.
- Unload is rejected when the module has an active command or dependencies;
  new commands are rejected after the slot enters UNLOADING. The busytest
  fixture checks those state transitions deterministically on one hart. It is
  not a simultaneous SMP race stress test. Async callbacks, workers, and
  interrupt registrations are not part of the API.
- During this audit, a fresh QEMU run exposed kernel stack exhaustion while
  loading a feature module. `module_load` had large per-call ELF metadata on
  xv6's one-page kernel stack. The metadata is now static scratch storage
  serialized by `module_operation_lock`; its measured function frame fell
  from 2848 bytes to 192 bytes. The test-mode QEMU integration suite passed
  after this correction.
- Use the build and demonstration commands in the README. Host tests do not
  establish SMP safety, module isolation, or compatibility with arbitrary ELF
  objects. Audit each new object's section layout and relocation types.


## 1. Objective

Design and implement a runtime kernel-module loading
framework for xv6-RISC-V.

## 2. Core Requirements

- Module loading
- Module initialization
- Module registration
- Module listing
- Module unloading
- Resource cleanup
- Invalid-module rejection

## 3. Architecture

- User-space module management commands
- System-call interface
- Module validation
- Module loader
- Module registry
- Module lifecycle management

## 4. Technical Constraints

- RISC-V 64-bit architecture
- xv6 kernel memory layout
- Kernel page allocation and mapping
- Supported module format and relocation types
- Synchronization during concurrent operations

## 5. Testing

- Load a valid module
- Execute its initialization routine
- List loaded modules
- Reject an invalid module
- Unload a module
- Verify that resources are released

## 6. Stretch Goals

- Support a restricted ELF relocatable module format
- Add dependency handling
- Measure loading and unloading overhead

## Phase 2 findings

### Scope and verification status

Phase 2 compiled and inspected a standalone hello module. No loader, system call, kernel page-table, or runtime execution changes were made. Facts below are marked **Verified** when observed in this repository or from the generated object; recommendations are **Proposed**.

### Toolchain and hello module

**Verified:** The xv6 Makefile uses `-march=rv64gc` and `-mcmodel=medany`; it does not explicitly pass `-mabi`. The installed `riscv64-linux-gnu-gcc` reports LP64D as the effective ABI for this target, with XLEN 64 and a double-float ABI. The experiment was compiled with the Makefile's compiler flags and explicit `-mabi=lp64d` to make that effective setting explicit. It was compiled as an object only and was not linked into xv6.

The hello module exports `module_init(void)` and `module_exit(void)`. It imports `printk` to demonstrate an external call. The declaration was checked against `kernel/defs.h` and corrected to match the kernel interface:

```c
int printk(char *, ...) __attribute__((format (printf, 1, 2)));
```

### Object properties

**Verified:** `experiments/hello_module.o` is ELF64, little-endian, RISC-V `ET_REL`, with RVC and double-float ABI flags. It has no program headers. Its RISC-V attributes declare 16-byte stack alignment and an rv64gc-compatible architecture (including zicsr, zifencei, and zmmul).

The relevant sections are:

| Section | Size | Flags / role |
| --- | ---: | --- |
| `.text` | 0x40 | AX; contains both module functions |
| `.rodata.str1.8` | 0x42 | mergeable string constants |
| `.data` | 0 | empty |
| `.bss` | 0 | empty |
| `.eh_frame` | 0x68 | allocatable unwind data |
| debug sections | present | non-runtime debug information |

The symbol table defines global functions `module_init` at `.text+0` (32 bytes) and `module_exit` at `.text+0x20` (32 bytes). `printk` is an undefined global symbol. The format strings are local symbols in the string section.

### Relocations observed

**Verified:** The runtime code has two instances of each primary relocation below in `.rela.text`; each primary relocation is accompanied by an `R_RISCV_RELAX` marker.

| Type | Offset(s) | Referenced symbol | Meaning and loader implication |
| --- | --- | --- | --- |
| `R_RISCV_PCREL_HI20` | 0x08, 0x28 | `.LC0`, `.LC1` | High 20 bits of a PC-relative address, applied to each `auipc` that begins loading a format-string address. Must be paired with the matching low relocation through its anchor label. |
| `R_RISCV_PCREL_LO12_I` | 0x0c, 0x2c | `.L0` anchors at 0x08 and 0x28 | Low 12 bits for the corresponding I-type instruction (here the address-building `addi`). The loader must resolve the anchor to its associated HI20 relocation and apply both parts consistently. |
| `R_RISCV_CALL_PLT` | 0x10, 0x30 | undefined `printk` | PC-relative function call encoded across the `auipc ra` / `jalr ra, ra` pair. The loader must resolve `printk` through the module API and patch the pair with range checks. |
| `R_RISCV_RELAX` | at each primary relocation site | paired relocation | Linker relaxation hint. A loader that does not relax instructions can treat it as a no-op while preserving the associated primary relocation. |

The disassembly confirms the relocation sites: each function loads its string address using `auipc` plus the low-part instruction, then calls `printk` using `auipc` plus `jalr`. The relocation at the call's AUIPC covers the two-instruction sequence.

**Verified:** Other relocation types occur in non-code metadata: debug sections contain `R_RISCV_32`, `R_RISCV_64`, `R_RISCV_ADD16`, `R_RISCV_SUB16`, `R_RISCV_ADD64`, and `R_RISCV_SUB64`. The allocatable `.eh_frame` section has `R_RISCV_32_PCREL`, `R_RISCV_ADD32`, `R_RISCV_SUB32`, `R_RISCV_SET6`, and `R_RISCV_SUB6`. These are not all runtime instruction relocations: a loader must still validate section flags and decide whether to reject or discard unsupported allocatable metadata. The official [RISC-V ELF psABI relocation specification](https://docs.riscv.org/reference/abi/v1.0/riscv-elf-object-files.html) describes the PC-relative pairs, call sequence, and relaxation marker.

### Proposed ELF subset and module ABI

**Proposed:** Use a restricted ELF64 RISC-V `ET_REL` loader for the first version. The hello object's small executable relocation set is a practical starting point, while ELF supplies established sections, symbols, and relocation records. Support at minimum:

- `R_RISCV_PCREL_HI20` paired with `R_RISCV_PCREL_LO12_I`, including anchor-label resolution.
- `R_RISCV_PCREL_LO12_S` as well, since real modules may store through PC-relative addresses.
- `R_RISCV_CALL_PLT` for calls to API functions, with a deliberate range policy.
- `R_RISCV_RELAX` as an ignored hint; do not perform relaxation initially.

Compile module objects with unwind tables disabled (for example, `-fno-asynchronous-unwind-tables -fno-unwind-tables`) so the allocatable `.eh_frame` relocations are absent. Require a supported section set and reject unsupported allocatable sections or relocations. Debug-only sections may be omitted from the loaded image after validating their bounds. Disable small-data assumptions with `-msmall-data-limit=0`; initially avoid TLS, GOT-based references, absolute function pointers, and other relocation forms until real module objects demonstrate the need.

Use a small, explicit, versioned kernel API table rather than resolving arbitrary kernel symbols. A production module would receive that table through `int module_init(const struct module_api *api)`, export `int module_command(int argc, const char *const argv[])`, and export `void module_exit(void)`. The loader locates these exact global function names through the ELF symbol and string tables, verifies that each defined entry belongs to executable allocated code, relocates the image, then invokes init. It publishes a module only after successful initialization. Command dispatch uses the registered command entry and tracks active calls; unload stops new dispatch, waits for or rejects active calls/dependents, calls exit, and then releases memory. The current hello experiment has the no-argument init/exit shape only; it does not yet implement this proposed production ABI.

**Uncertainty:** The hello object is deliberately tiny and does not establish the relocation needs of the planned sysmon, procmon, or klogger modules. Inspect each real module object before freezing the supported subset. Static data, global data accesses, switch tables, function pointers, compiler-generated helpers, and module-to-module references may add requirements.

### Proposed executable-memory strategy

**Verified from xv6 source:** `KERNBASE` is 0x80000000 and `PHYSTOP` is 0x88000000. The kernel page table maps the kernel text from `KERNBASE` through `etext` as readable/executable and maps the remaining RAM through `PHYSTOP` as readable/writable and non-executable. `kalloc()` returns a pointer through xv6's direct/identity-mapped RAM mapping. Copying code into a normal allocator page and calling through that alias therefore cannot execute it under the current permissions.

**Proposed:** Reserve a bounded module virtual-address arena in an unused Sv39 range near the kernel's direct map and within the reach of the `medany` PC-relative model. Allocate backing pages with `kalloc`; populate and relocate them through the existing writable direct map, then map module text RX, read-only data R, and data/BSS RW/NX in the module arena. Do not leave a writable direct-map alias to text pages, since a second writable alias defeats W^X. Keep data pages NX. This avoids making all RAM executable and requires only a restricted set of module mappings, but it requires careful page-table ownership, unmapping, and reclamation.

After writing or relocating code, execute `fence.i` on every hart that may run the module; the instruction-cache fence is local to a hart. TLB invalidation is likewise local, so any mapping changes visible to other harts need a remote shootdown and acknowledgement protocol or a carefully enforced quiescent, single-hart policy. The inspected kernel has local `sfence_vma` use and no established remote TLB shootdown mechanism. A one-hart QEMU prototype may fit the two-week schedule; supporting concurrent module use and unload across all harts is a significant schedule and correctness risk. No page-table changes or module execution have been attempted.

### Remaining risks and gates

- Relocation pairing, overflow/range checks, malformed ELF rejection, section alignment, and symbol validation need a narrow, testable loader design.
- Real module objects may need relocations beyond this hello sample.
- The `.eh_frame` section is allocatable in this build; the module build flags or loader policy must address it explicitly.
- W^X requires removing writable aliases for executable backing pages, not merely adding an RX alias.
- Cross-hart instruction-cache synchronization, TLB shootdown, safe command dispatch, and unload quiescence remain unresolved design work.
- No evidence yet demonstrates runtime loading or execution. Phase 2 stops before implementation and before Phase 3.

## Phase 3 Stage 1 - finalized minimal-loader design

This section records the selected design after inspecting the current xv6 source. It is a design decision only. No loader, syscall, page-table, interrupt, or module code has been implemented. The Phase 2 proposal for a separate module virtual-address arena was exploratory and is superseded here by the smaller direct-map permission-transition design.

### Source facts that constrain the design

**Verified in this tree:**

- `kernel/vm.c:kvmmake()` maps kernel text from `KERNBASE` to `etext` as R-X. It maps RAM from `etext` through `PHYSTOP` at the same virtual and physical addresses as R-W and non-executable. `KERNBASE` is 0x80000000 and `PHYSTOP` is 0x88000000.
- `kernel/kalloc.c` frees pages in `[end, PHYSTOP)`; `kalloc()` returns the identity-mapped address and initially fills the page through that mapping. It does not grant execute permission. `kfree()` writes to the page with `memset`, so a module text page must be writable again before it can be returned to the allocator.
- All harts use the shared `kernel_pagetable`. `walk()` exposes its leaf PTEs and the existing page size is 4096 bytes.
- `sfence_vma()` and `icache_fence()` exist in `kernel/riscv.h`, but each executes only on the calling hart. `kernel/start.c` delegates interrupt causes with `w_mideleg(0xffff)`, but enables only supervisor external and timer interrupts. `kernel/trap.c:devintr()` handles those two sources; a supervisor software interrupt is currently unhandled. The `CLINT(hart)` address macro exists, but `kvmmake()` does not map CLINT and the kernel has no software-IPI protocol.
- `NCPU` is 8 and the Makefile's QEMU `CPUS` default is 3. The current kernel is therefore multi-hart by default; a local-only TLB or instruction-cache fence is insufficient.
- The kernel context and trapframe save integer registers, not floating-point registers. Initial modules must not use floating-point or vector state.

### Selected object format and validation policy

Use a restricted, independently compiled ELF64 little-endian RISC-V `ET_REL` object with the kernel's rv64gc / LP64D ABI and `medany` code model. The object is read from a filesystem path at runtime, copied into allocator pages, relocated, permissioned, and invoked. A compiled-in registry of functions is not the loader.

The module build recipe should use the kernel's freestanding C flags, including `-march=rv64gc -mabi=lp64d -mcmodel=medany -ffreestanding -fno-pie -fno-stack-protector -fno-common -msmall-data-limit=0 -g0 -fno-asynchronous-unwind-tables -fno-unwind-tables`, then compile with `-c` and no link. Audit the symbol table and reject any object with unresolved runtime helpers. Keep module code integer-only because xv6 does not preserve FPU state. Check the exact recipe against real module outputs before freezing it.

The initial parser must bound the input length, section/symbol/relocation counts, all table offsets and sizes, arithmetic, and allocations. Validate the ELF identity, class, endianness, machine, type, ABI flags, rv64gc-compatible architecture attributes, exact header and entry sizes, and the absence of program headers required for this ET_REL subset. Validate the section-name table, section links, symbol string table, string termination, symbol indices, section indices, alignments, and relocation target ranges before copying or patching anything. Reject arithmetic overflow, malformed or unsupported section kinds and flags, TLS sections, W+X sections, `SHF_COMPRESSED` sections, common/TLS/IFUNC symbols, bad entry symbols, unexpected ABI flags, and every unsupported relocation. Require the RVC and double-float ABI flags used by the kernel toolchain and attributes compatible with rv64gc; reject unsupported architecture attributes. Permit `SHF_MERGE` / `SHF_STRINGS` on read-only data by copying the bytes without merge semantics. Permit the ordinary local `STT_SECTION`, `STT_FILE`, and `STT_NOTYPE` entries used by the compiler, and `STT_OBJECT` / `STT_FUNC` definitions in allocated sections; require module entrypoints and callable relocation targets to be `STT_FUNC`. Validate special section indices by symbol type, allowing the null symbol and the `STT_FILE` absolute symbol; reject `SHN_COMMON`, unsupported referenced symbol types, other undefined names under ABI v1, and every unsupported relocation. Ignore no relocation merely because the loader does not understand it.

Require separate page-aligned RX text, R/NX read-only data, and RW/NX data/BSS groups; never pack sections with different permissions into the same page. Zero BSS. Compile without debug and unwind sections so their metadata relocations are not part of the loadable object. The Phase 2 object is a relocation probe, not yet a valid object for the final API ABI: it contains debug and allocatable `.eh_frame` relocations and an undefined `printk` import.

### Relocation subset and pair rules

Initially accept only `R_RISCV_NONE`, `R_RISCV_PCREL_HI20`, `R_RISCV_PCREL_LO12_I`, `R_RISCV_PCREL_LO12_S`, `R_RISCV_CALL_PLT`, and `R_RISCV_RELAX`. The [RISC-V ELF psABI](https://docs.riscv.org/reference/abi/v1.0/riscv-elf-object-files.html) defines these fields and the label-based HI20/LO12 relationship.

- For a HI20 relocation, resolve its target symbol to the final loaded address `S`, use its addend `A`, and let `P_hi` be the runtime address of the AUIPC. Compute `D = S + A - P_hi`; reject values outside the signed 32-bit PC-relative range. Encode the rounded high part as `(D + 0x800) >> 12`.
- A LO12 relocation's symbol is an anchor label in the same section, not the original target. Require its addend to be zero. Find exactly one HI20 relocation at that anchor's section offset, resolve the HI20's target, and compute the low part from that same `D` as the signed low 12 bits. Do not compute the low part from the LO instruction's own PC. Validate the AUIPC and I-type/S-type encodings and that the low instruction uses the AUIPC destination as its base register. One HI20 may serve more than one LO12 use; reject missing or ambiguous anchors and orphan HI20 entries.
- For `R_RISCV_CALL_PLT`, resolve only a defined module function, validate the AUIPC/JALR pair and register relationship, compute `D = S + A - P`, range-check it, and patch the rounded U+I immediates. Kernel services are reached through the API table, not by searching kernel symbols.
- `R_RISCV_RELAX` is accepted only as a marker paired at the same offset with a supported primary relocation. The first loader performs no relaxation and leaves the instruction sequence intact while applying the primary relocation.
- Treat `R_RISCV_NONE` as an explicit no-op. Reject all other relocation types, including those seen in Phase 2 metadata, until a real module demonstrates a need and a correct implementation is added.

For the first ABI, reject undefined ELF symbols. Module code obtains kernel services through the versioned API pointer passed to init. If a later ABI permits named imports, resolve only exact names deliberately exported by that API table; never search or expose the general kernel symbol table. The current Phase 2 `printk` import must therefore be changed to an API-table call before the test object can load.

### Versioned API and entry points

Define a stable `module_api_v1` prefix containing `abi_version`, `struct_size`, a capability field, and only services actually implemented by the kernel. Its initial layout is:

```c
struct module_api_v1 {
  uint abi_version;
  uint struct_size;
  uint64 capabilities;
  int (*printk)(char *, ...);
};
```

Here `uint` and `uint64` use xv6 kernel typedefs, and the `printk` signature matches `kernel/defs.h`. V1 needs only this printing service for the hello module. Later API revisions can append process-snapshot, system-statistics, and logging services; snapshot APIs copy fixed-layout data and do not return pointers to private kernel structures. Modules use no user-supplied function pointers.

Required module exports are:

```c
int module_init(const struct module_api_v1 *api);
int module_command(int argc, const char *const argv[]);
void module_exit(void);
```

The loader finds these exact global function symbols in the validated symbol/string tables, requires each to be defined, nonempty, in an allocated executable section, and within that section's bounds. Init receives the stable kernel API table and returns success or failure. A failed init must clean up its own partial state and is never published. Command arguments are bounded kernel-owned copies; user space selects a module command, never a kernel function address.

### Load, invocation, dependencies, and cleanup

Use a registry state machine `LOADING -> LIVE -> UNLOADING -> DEAD`. Reserve the module's normalized name during loading so a duplicate cannot race initialization. The kernel reads the object from the filesystem; after validation, section placement, relocation, and permission synchronization, it calls init. Publish the module only after init succeeds. Listing reports registry metadata, not a table of built-in callable functions.

For invocation, under the registry lock require `LIVE`, increment `active_calls`, copy the entry pointer, then release the lock before calling module code. Decrement the reference and wake unload waiters on return, including after a module command sleeps or yields. Never hold the registry spinlock across module code.

Unload changes state to `UNLOADING` under the registry lock, which denies new invocations, then waits without holding a spinlock for `active_calls == 0`. Refuse unload while `dependent_refs != 0`. Call module_exit only after command calls have drained. V1 forbids module-created kernel threads, interrupt handlers, timers, callbacks, and retained cross-module function pointers; these would require additional lifetime references. V1 has no module-to-module imports, so dependencies are absent and the loader rejects such references. Keep a dependent-reference field and add explicit get/put accounting before a later version adds dependencies.

### Selected executable-memory and W^X strategy

Use selected allocator pages in xv6's existing identity/direct map; do not add a second alias or a separate executable arena in the first version. Since the direct-map virtual address equals the physical address, a relocated section's runtime address is the allocated page address. The entire allocator range is within 128 MiB of the kernel text, within the range needed by the selected `medany` PC-relative relocations.

Allocate separate page groups for text, read-only data, and writable data. Fill and relocate them while all pages are in their initial RW/NX direct-map state. Use this ordered transition:

1. Change the selected text and read-only-data leaf PTEs from RW/NX to R/NX.
2. Perform a system-wide TLB shootdown and wait for every online hart to acknowledge. This revokes cached writable translations before any executable permission is installed.
3. Change only the text PTEs from R/NX to R/X.
4. Shoot down TLBs and execute `fence.i` on every online hart; wait for acknowledgements before publishing the module or calling init.

Thus only allocated module text pages become executable, and a page has no writable alias while executable. Data remains RW/NX. On unload, after references drain and exit returns, change text R/X to R/NX and wait for all-hart invalidation before changing text and read-only-data pages to RW/NX. Flush and acknowledge that change before calling `kfree()`, whose poison write requires a writable direct mapping. If any hart fails to acknowledge a permission transition, do not execute or reuse the affected pages; quarantine them rather than violating W^X or freeing memory under a stale translation.

This in-place PTE transition avoids a second virtual alias and does not make all RAM executable. It is feasible within the existing direct-map layout, but safe SMP operation depends on adding and testing the missing remote synchronization path.

### Multi-hart TLB/I-cache protocol (Stage 1 proposal superseded)

The Stage 1 suggestion that CLINT machine-software interrupts could simply be delegated to a supervisor software-interrupt handler was not verified and is not valid for the project's tested QEMU configuration. Phase 3 Stage 3 records the source inspection and a QEMU readback probe below.

**Verified in the source:** kernel/memlayout.h defines CLINT(hart), but kvmmake() does not map CLINT. start() enters S-mode after writing mideleg=0xffff and enabling only supervisor external and timer interrupts. It does not install an M-mode trap vector or M-mode IPI handler. trapinithart() installs only the S-mode stvec; devintr() handles only supervisor external and timer interrupts. There is no online-hart bitmap or IPI acknowledgement protocol. sfence_vma() and icache_fence() execute only on the calling hart.

A CLINT MSIP write raises machine-software-interrupt cause 3. mideleg support is implementation-dependent; merely writing 0xffff is not evidence that cause 3 is delegated. In the tested QEMU virt setup, a separate M-mode probe wrote 0xffff to mideleg and read bit 3 back as zero. Therefore a CLINT MSIP would not reach the current S-mode devintr() path on that configuration. Even on a platform that delegates the cause, the current kernel neither enables that interrupt nor dispatches it, maps the CLINT page, tracks online harts, or waits for completion acknowledgements.

Before any SMP module execution, choose and implement one complete mechanism:

- A small M-mode IPI shim can install a per-hart M-mode trap vector, enable MSIE, map CLINT for the S-mode sender, service the local MSIP, run the local fences, publish an acknowledgement, and return with mret. Its handler must preserve interrupted state and avoid all xv6 spinlocks and allocation paths. Startup, trap entry/exit, and lock/interrupt interactions need dedicated multi-hart tests.
- An SBI-based approach can use SBI IPI/RFENCE services, but the current -bios none/M-mode startup is not an SBI kernel entry path: start() accesses M-mode CSRs directly. Switching to firmware entry requires startup/timer changes. Also, an SBI success result must not be treated as proof that all target harts completed their fences until the selected SBI contract has been verified; missing or failed acknowledgements require quarantine, never page reuse.

The safe fallback for the current project is a documented single-hart build and test. Local fences are sufficient only when no other hart can hold or use the affected translation or execute the page. Do not advertise the default multi-hart build as safe for loading or unloading modules.

### Two-week implementation boundary and remaining risks

The smallest credible two-week deliverable is one genuine independently compiled hello ET_REL object read from the filesystem at runtime, validated and relocated, given the page permissions above, and initialized, invoked, listed, and unloaded with active-call tracking. It must execute its allocated module code; a built-in function registry does not meet the goal. The permission-transition stage has passed a single-hart test; SMP invocation remains gated on a complete, acknowledged IPI or SBI synchronization implementation and multi-hart tests.

Do not include sysmon, procmon, klogger, cross-module dependencies, module threads/callbacks, GOT/TLS, arbitrary imports, or a general-purpose ELF linker in this first deliverable. Add snapshot and logging API services only as required for later real modules. All module code is kernel-privileged trusted code; ELF validation is not a sandbox, so the initial filesystem image and module-loading policy must not imply that arbitrary user-created objects are safe to execute.

Remaining risks are the CLINT IPI delivery/acknowledgement design, permission-transition rollback and quarantine, ELF parser attack surface, compiler-emitted relocations beyond this subset, the FPU-state restriction, and active-call leaks from a buggy module. Stage 2 implements bounded ELF validation and relocation processing without executing code. Phase 3 Stage 3 records the PTE transition prototype and corrects the unverified CLINT proposal; it does not complete the SMP synchronization gate or establish module execution.


## Phase 3 Stage 2 - bounded ELF validation and relocation core

### Implemented boundary

**Implemented and host-tested:** `kernel/elfmod.c` and `kernel/elfmod.h` provide a freestanding parser and relocation core. The caller owns a bounded input byte buffer, a fixed-size parser context, a non-executable staging buffer, and the final runtime address for each allocated section. The implementation does not use xv6's allocator or filesystem interfaces yet. Source inspection confirmed that `kalloc()` supplies one 4096-byte page and that the filesystem exposes `readi()`; neither is called from this parser.

The parser source is intentionally not added to `Makefile`'s `OBJS`. The test runner compiles that same source for both the host harness and an RV64 freestanding relocatable object. No kernel code calls it, and no system call, page-table, interrupt, allocator, filesystem, or module-manager path was changed. Relocated bytes stay in a caller-provided byte array. The parser has no function that invokes an export.

**Not implemented:** Runtime file loading, module registration, the production `module_api_v1` handoff, executable-page installation, W^X enforcement, TLB or instruction-cache synchronization, module invocation, and unloading. This stage does not establish that modules can execute.

### Verified object fixtures

The clean hello fixture was compiled from the existing `experiments/hello_module.c` with the xv6 compiler's `rv64gc`, `medany`, freestanding, no-PIE, and no-stack-protector settings; the installed compiler reports `lp64d` as its default ABI, which the fixture command specifies explicitly. It also disables debug and unwind data so the fixture matches the selected loadable-section subset.

**Verified with `riscv64-linux-gnu-readelf` and `objdump`:** That clean fixture is ELF64, little-endian, `ET_REL`, `EM_RISCV`, has no program headers, uses `e_flags = 0x5` (RVC and double-float ABI), and contains 12 sections, 17 symbols, and 12 `.rela.text` entries. Its `.text` is 0x40 bytes with AX flags; `.rodata.str1.8` is 0x42 bytes with mergeable string flags; `.data` and `.bss` are empty. The RISC-V attributes report 16-byte stack alignment and the toolchain's rv64gc-compatible ISA string.

The 12 text relocations are two each of `R_RISCV_PCREL_HI20`, `R_RISCV_PCREL_LO12_I`, and `R_RISCV_CALL_PLT`, plus six `R_RISCV_RELAX` markers. Disassembly shows an `AUIPC` plus `ADDI` address sequence for each string, and an `AUIPC ra` plus `JALR ra, ra` sequence for each `printk` call. The undefined `printk` symbol is global `STT_NOTYPE` in this object. The host test resolver permits exactly that name and returns a fixed test address; it is not a kernel symbol table.

The existing Phase 2 `experiments/hello_module.o` was also passed to the parser. It is rejected with `unsupported ELF section type or flags` because it contains debug sections excluded from this subset. Its allocatable `.eh_frame` also has relocation types outside the supported set. The test does not discard those records.

A separate 9-section assembler fixture, `experiments/elfmod_store.S`, emits one `R_RISCV_PCREL_HI20` / `R_RISCV_PCREL_LO12_S` pair and two `R_RISCV_RELAX` records. It exercises the S-type immediate patch path. These fixtures demonstrate only the cases they contain; they do not establish the complete relocation needs of sysmon, procmon, or klogger.

The existing hello C source still has `void module_init(void)` and `void module_exit(void)`, imports `printk`, and has no `module_command`. The parser can identify the two defined function exports and reports the missing command export, but it cannot infer or validate C prototypes from ELF. No entrypoint was called.

### Accepted ELF and section subset

The parser accepts only ELF64, little-endian, current-version, System V `ET_REL` for `EM_RISCV`; a 64-byte ELF header; 64-byte section headers; no program headers; a normal, non-extended section table; and exactly the rv64gc / LP64D flags observed for this kernel toolchain. It requires one valid RISC-V attributes section with stack alignment 16 and the supported rv64gc extension set. Unknown architecture attributes, ABI flags, or attribute tags are rejected.

Bounds are fixed at 1 MiB of input, 32 sections, 2048 symbols, 4096 relocation records, and 2 MiB of staging image. All table products, file ranges, layout alignment, section addresses, and relocation arithmetic use checked operations. The parser reads ELF fields byte-by-byte as little-endian values; it does not cast file offsets to native ELF structs or use unchecked file-provided pointer arithmetic. It rejects a section table that overlaps the ELF header or file-backed section data.

Supported sections are deliberately narrow:

- Section zero must be the all-zero `SHT_NULL` entry.
- Allocated `SHT_PROGBITS` sections are classified as executable text, read-only data, or writable data. W+X and unrecognized flag combinations are rejected. Read-only `SHF_MERGE` / `SHF_STRINGS` data is copied without merging; its element size and string termination are checked.
- Allocated `SHT_NOBITS` is accepted only as writable, non-executable data and is zeroed in staging.
- Exactly one `SHT_SYMTAB` is accepted with 24-byte entries and a valid linked `SHT_STRTAB`. String tables, section names, and symbol names must be bounded and terminated.
- `SHT_RELA` is accepted with 24-byte entries, `SHF_INFO_LINK`, the one supported symbol table, and an allocated target section.
- One `SHT_RISCV_ATTRIBUTES` section is required. Non-allocated `SHT_PROGBITS` is accepted only for `.comment` and `.note.GNU-stack`.

Debug sections, `SHT_REL`, dynamic sections, groups, TLS, compressed sections, unknown section types or flags, and other non-allocated data sections are rejected. An `.eh_frame` section by itself is not interpreted specially; its unsupported relocation records cause rejection. No unsupported relocation is skipped because it targets metadata.

### Symbol and import policy

The parser validates every symbol, name, binding, type, visibility byte, value, size, and section index before relocation. It accepts the null symbol; local `STT_FILE` in `SHN_ABS`; local `STT_SECTION` and `STT_NOTYPE` labels; and defined `STT_FUNC` / `STT_OBJECT` symbols in compatible allocated sections. Undefined global `STT_FUNC` or `STT_NOTYPE` symbols are represented as imports. Undefined weak symbols and all other weak symbols are rejected. `SHN_COMMON`, `SHN_XINDEX`, TLS, IFUNC, processor-reserved indices, non-default `st_other`, and unsupported symbol types are rejected.

Imports are resolved only through an optional caller-supplied callback. The test callback is an explicit one-name allowlist for `printk`; passing no resolver rejects every undefined import. The callback must return a stable address for the duration of one relocation operation. The parser contains no general kernel-symbol lookup. The eventual v1 module ABI remains the Stage 1 API pointer; production loading can pass no resolver and require modules to use that API pointer instead.

`elfmod_find_export()` locates one unambiguous global, nonempty `STT_FUNC` in executable allocated text. A later manager must require `module_init`, `module_command`, and `module_exit` and enforce their documented ABI contract. ELF symbol tables do not carry C function prototypes, so the parser cannot prove that an object uses the correct parameter list.

### Implemented relocation semantics

The implementation accepts only `R_RISCV_NONE`, `R_RISCV_PCREL_HI20`, `R_RISCV_PCREL_LO12_I`, `R_RISCV_PCREL_LO12_S`, `R_RISCV_CALL_PLT`, and `R_RISCV_RELAX`. The field definitions and pair semantics follow the [RISC-V ELF psABI](https://docs.riscv.org/reference/abi/v1.0/riscv-elf-object-files.html).

- For a HI20 relocation, the engine resolves the final target `S`, reads addend `A`, and uses the runtime address `P_hi` of the AUIPC. It computes `D = S + A - P_hi`, then `H = floor((D + 0x800) / 4096)` and `L = D - 4096*H`. It first requires `D` to fit signed 32-bit range, then requires a signed 20-bit `H` and signed 12-bit `L`; it does not truncate an out-of-range displacement.
- A LO12 relocation must have addend zero and name a local `STT_NOTYPE` anchor in the same section. The anchor value must identify exactly one HI20 relocation site. The engine resolves the HI20 target and computes the same `D` from the HI20 instruction address, never from the low instruction's address. It rejects missing or ambiguous anchors, orphan HI20 entries, bad instruction encodings, misalignment, and a low instruction whose base register does not match the AUIPC destination. Multiple low uses of one HI20 are permitted. I-type support is limited to the observed ADDI form plus validated load/JALR forms; S-type support is a validated store form.
- `R_RISCV_CALL_PLT` is limited to the verified call sequence `AUIPC ra; JALR ra, ra`. The engine resolves a defined function or allowlisted import, computes `D = S + A - P` at the AUIPC, validates the register/opcode pair and even target alignment, then applies the same checked U+I split. Tail-call forms are outside this subset.
- `R_RISCV_RELAX` is metadata only and never changes instruction bytes. It must share the target section and offset with exactly one supported primary relocation. The marker accepts the all-zero symbol/addend form emitted by the current assembler or the same symbol/addend as its paired primary record.
- `R_RISCV_NONE` is an explicit no-op. Every other relocation returns an unsupported-relocation error.

Relocation target sections for these instruction relocations must be allocated executable `.text`. The engine checks every relocation symbol index and target range before applying patches. It preflights every relocation against the original bytes, then copies allocated section bytes into staging, zeros `.bss`, and applies patches there. Any second-pass failure clears the staging image before returning an error. Final section addresses are supplied separately from staging offsets and are checked for alignment, overflow, overlap, and cross-permission page sharing.

### Staging and executable-memory boundary

Allocated sections are assigned bounded staging offsets in text, read-only, and writable groups; each group starts on a 4096-byte boundary so sections with different intended permissions do not share a staging page. Relocation math uses the caller's final runtime address for each section, not the host address of the staging buffer. This is sufficient to test the relocation engine independently of the eventual allocator and page-table installer.

The staging array is ordinary writable data and is never marked executable or called. The selected Stage 1 direct-map permission-transition strategy, writable-alias revocation, W^X, instruction-cache fencing, all-hart TLB shootdown, CLINT IPI delivery, and safe unload protocol remain designs only. The current kernel still lacks that synchronization path. No executable memory was allocated or installed in this stage.

### Tests and verification performed

`experiments/run_elfmod_tests.sh` builds the clean hello and S-type fixtures in a temporary directory, compiles the same `kernel/elfmod.c` for the host and as a freestanding RV64 object, then runs the host harness against those fixtures and the existing Phase 2 object. The suite passed for truncated headers, invalid magic/machine, section-table bounds and name index, unterminated section/symbol strings, invalid symbol-table bounds, bad relocation symbol index/target section, unsupported relocation, malformed HI/LO anchor, misaligned relocation sites and final addresses, out-of-range calls, BSS layout overflow, relocation/file-offset overflow, unmatched RELAX markers, explicit NONE handling, and I-type and S-type relocation reconstruction. The existing debug/unwind object was rejected as expected. The test callback's denied and out-of-range imports were rejected before staging writes.

The same suite passed with `HOST_SANITIZE=1` using AddressSanitizer and UndefinedBehaviorSanitizer. GCC `-fanalyzer` completed without diagnostics for `kernel/elfmod.c`. The RV64 parser object compiled with the repository's compiler and compatible `-march=rv64gc -mabi=lp64d -mcmodel=medany` settings. `make kernel/kernel` reported that the existing target was up to date; it did not rebuild because the new parser is not in `OBJS`. No QEMU run was performed, and no module code was executed.

### Remaining risks and next gate

The accepted subset is intentionally strict and may reject legitimate objects from a different assembler or compiler version. The test object does not cover data-pointer relocations, global-data access sequences, absolute relocations, GOT, TLS, alignment relocations, branch/jump relocations, compiler helper calls, or module-to-module references. The planned sysmon, procmon, and klogger objects still need independent relocation audits before the subset can be frozen. The parser validates ELF structure and relocation instruction forms, not module safety; a valid kernel module remains privileged code. It also does not prove that compiler output avoids floating-point instructions even though the current kernel does not preserve FPU state.

The parser keeps its input immutable by contract, and the import resolver must be stable. Phase 3 Stage 3 records a single-hart direct-map PTE transition prototype without module invocation. Multi-hart synchronization remains unimplemented, and Stage 2 does not establish runtime loading or execution.



## Phase 3 Stage 3 - executable-memory and synchronization feasibility

### Stage 2 review and hello-object compatibility

**Verified:** The approved Stage 2 artifacts were inspected: kernel/elfmod.c, kernel/elfmod.h, experiments/test_elfmod.c, experiments/elfmod_store.S, experiments/run_elfmod_tests.sh, and this design document. The parser remains outside the normal kernel OBJS list and has no filesystem, allocator, page-table, trap, or module-invocation integration. No Stage 2 source was changed for this stage.

The existing experiments/hello_module.o remains a debug/unwind object. The parser rejects it with its unchanged unsupported-section validation; the Stage 2 test also confirms that its unsupported sections/relocations are not skipped. The accepted clean fixture is reproducible using the experiments/run_elfmod_tests.sh build recipe. The xv6 Makefile specifies -march=rv64gc and -mcmodel=medany, but no ABI; riscv64-linux-gnu-gcc -Q --help=target reports default -mabi=lp64d and -march=rv64imafdc_zicsr_zifencei. The fixture makes ABI explicit with -mabi=lp64d and uses -march=rv64gc, -mcmodel=medany, freestanding/no-PIE/no-stack-protector/small-data-limit-zero settings, -g0, -fno-asynchronous-unwind-tables, and -fno-unwind-tables. The last three flags prevent debug and unwind sections from being emitted; parser validation was not weakened.

**Verified with the accepted object:** ELF64, little-endian, ET_REL, EM_RISCV, e_flags=0x5 (RVC and double-float ABI), no program headers, 12 sections, 17 symbols, and 12 .rela.text records. .text is 0x40 bytes with AX flags; .rodata.str1.8 is 0x42 bytes with mergeable-string flags; .data and .bss are empty. There are two each of R_RISCV_PCREL_HI20, R_RISCV_PCREL_LO12_I, and R_RISCV_CALL_PLT, with six R_RISCV_RELAX markers. Disassembly shows AUIPC/ADDI string-address pairs and AUIPC ra/JALR ra,ra calls. The undefined global symbol remains printk; the host test resolver permits it only as a fixture. This is not yet the proposed API-table module source, and no relocated code was invoked.

### Verified memory map and proposed section placement

kernel/vm.c:kvmmake() maps kernel text [KERNBASE, etext) as R-X and maps [etext, PHYSTOP) at identical virtual and physical addresses as R-W/NX. Here KERNBASE=0x80000000 and PHYSTOP=0x88000000. kalloc() returns a page from the allocator and writes through that identity mapping; it does not make pages executable. kfree() poisons the page with memset, so a text page must be writable again before it is returned. The kernel page table is shared by harts. The test below creates no extra mapping for its allocated page.

**Proposed:** Allocate distinct page groups for text, read-only data, and writable data/BSS. Copy and relocate them through their existing identity mapping while RW/NX. Then update only those allocator-owned identity PTEs: text to R-X, read-only data to R/NX, and data/BSS to R-W/NX. No separate writable staging alias may remain after publication. Module code and data addresses are the direct-map addresses returned by kalloc(); the loader must reject overlap, page sharing across permission classes, and any extra writable alias. Only selected allocated text pages become executable; the RAM mapping is not globally changed to executable.

On unload, first prevent new entry and wait for all active calls/dependencies to drain. Revoke X (R-X to R/NX), synchronize stale translations, then restore R-W/NX, synchronize again, and only then call kfree(). If synchronization fails, quarantine the pages and do not free them. A production loader still needs transactional rollback for partial permission changes.

### Single-hart permission-transition prototype

**Implemented for feasibility only:** kernel/modmemtest.c is linked and called only when building with MODULE_MEMTEST=1. The normal kernel build does not link or call it. At boot, after the primary hart enables the kernel page table and before secondary harts are released, the test allocates one page, checks its identity PTE, populates a byte while RW/NX, changes permissions through R/NX and R-X, executes local sfence.vma and fence.i, returns through R/NX to R-W/NX, checks the flags and saved byte, then calls kfree() only after write permission is restored. It immediately reallocates the same page, verifies that kalloc() can write through the restored mapping, and frees it again. It does not branch to the page, invoke a module entrypoint, change vm.c, or alter system calls.

This test demonstrates that the current Sv39 direct-map PTE can represent the desired permission classes and that an allocator page can be restored for kfree() on one hart. PTE-flag checks and a successful boot do not prove hardware fault isolation, cross-hart synchronization, or executable code fetch; the test deliberately does not try a forbidden write or execute the page. W^X for future modules depends on the full loader ensuring there are no other aliases.

### Multi-hart feasibility findings

**Verified on the actual target configuration:** QEMU 8.2.2 was run with -machine virt -bios none -smp 1. A temporary M-mode probe wrote 0xffff to mideleg, read it back, and printed N because bit 3 (machine-software-interrupt cause) read as zero. Thus the Stage 1 direct CLINT-MSIP-to-S-mode proposal does not work on this QEMU CPU configuration. QEMU virt has a CLINT, but that does not imply that its MSIP cause is delegated to S-mode. The RISC-V privileged specification describes MSIP as a machine-level software interrupt and leaves delegation support platform-dependent; the probe resolves that question for this project's tested QEMU configuration. See the [RISC-V machine interrupt specification](https://docs.riscv.org/reference/isa/priv/machine.html), [QEMU virt platform documentation](https://www.qemu.org/docs/master/system/riscv/virt.html), [SBI RFENCE specification](https://docs.riscv.org/reference/sbi/v3.0/ext-rfence.html), and [Zifencei specification](https://docs.riscv.org/reference/isa/unpriv/zifencei.html) for platform and fence contracts.

The existing kernel has no S-mode software-interrupt enable/dispatch, no CLINT PTE mapping, no M-mode mtvec handler, and no online-hart/ack state. main.c's started flag gates secondary startup but is not a completed-online bitmap. Consequently, there is no safe multi-hart shootdown today. A future handler must run sfence.vma on every hart using the shared kernel page table and fence.i on every hart that may fetch new code. After writing code, the source hart must execute a data FENCE before sending the remote request; each target executes the required local sfence.vma and fence.i, then publishes its acknowledgement. The sender uses release/acquire ordering for request and acknowledgement state, waits for all required acknowledgements after the actual fences, uses a timeout, and quarantines pages if any hart does not respond. IPI delivery alone is not completion. Hart IDs must be bounded by NCPU, online state must be published only after per-hart VM/trap setup, and a late-starting hart must perform a full local synchronization before it can run modules. Handlers must not take allocator/registry locks; senders must wait without holding a spinlock or disabling interrupts in a way that prevents progress.

An M-mode IPI shim or conversion to an SBI firmware boot path is a real kernel change, not just enabling a supervisor interrupt bit. SBI RFENCE is a potential alternative after such a boot-path change, but its API contract and completion/ack behavior must be verified before a successful SBI return is used to free or execute pages. The current evidence supports only the single-hart prototype; no multi-hart synchronization was implemented or tested.

### Verification and remaining risks

Commands and results: experiments/run_elfmod_tests.sh passed; HOST_SANITIZE=1 experiments/run_elfmod_tests.sh passed with ASan/UBSan; the host parser compiled with GCC -fanalyzer without diagnostics; the clean RV64 hello object compiled and was inspected with riscv64-linux-gnu-readelf and riscv64-linux-gnu-objdump. make -B MODULE_MEMTEST=1 CPUS=1 kernel/kernel built the test kernel, and timeout -k 2s 18s make MODULE_MEMTEST=1 CPUS=1 qemu booted to init: starting sh after printing the memory self-test success. The wrapper exited 124 because the timeout stopped the interactive shell. make -B kernel/kernel then restored the default kernel, and nm confirmed that modmemtest is absent. A separate M-mode probe on QEMU 8.2.2 with -machine virt -bios none -smp 1 printed N for mideleg bit 3. The builds emitted a linker warning that kernel/kernel has a LOAD segment with RWX permissions; xv6's runtime kernel page table still maps text R-X and allocator RAM R-W/NX. No module loader, system call, runtime object read, code execution, module invocation, unload, or multi-hart fence was implemented.

The two-week scope should remain a single-hart, controlled demonstration until an acknowledged SMP synchronization path exists. A default three-hart QEMU run cannot safely load or unload modules with the present kernel. Before the hello module is invoked, its printk import must be converted to the finalized versioned API-table ABI, and the loader must integrate file input, bounded parsing/relocation, no-alias permission transitions, active-call lifetime tracking, and single-hart enforcement. Actual machine permission faults and all-hart behavior remain unverified.


## Single-hart runtime-loader prototype (current status)

This section records the current prototype and supersedes earlier phase snapshots that said runtime loading, module invocation, or executable-page testing had not been implemented. Those statements were accurate when written. This update does **not** claim SMP safety or a security sandbox.

### Verified ELF object and relocation behavior

The MODULES=1 build compiles experiments/hello_module.c as a separate object using the Makefile's -march=rv64gc and -mcmodel=medany flags. The installed GCC's LP64D ABI is passed explicitly as -mabi=lp64d. The recipe also uses -msmall-data-limit=0, -g0, -fno-asynchronous-unwind-tables, and -fno-unwind-tables. The object is not linked into the kernel: hello.o is installed as a filesystem file, and modctl load hello.o reads and parses it at runtime. The older Phase 2 debug/unwind object remains rejected by the unchanged strict parser.

**Verified with riscv64-linux-gnu-readelf and objdump:** hello.o is 3624 bytes, ELF64 little-endian ET_REL for EM_RISCV, has no program headers, has e_flags=0x5 (RVC and double-float ABI), 12 sections, 38 symbols, and 44 .rela.text records. .text is 0x104 bytes with AX flags, .rodata.str1.8 is 0x5d bytes with allocated mergeable strings, .data is empty, and .bss is 8 bytes. The symbol table defines module_init, module_command, and module_exit and has no undefined external imports. The relocated staging image uses 8200 bytes.

| Observed relocation | Count | Use in this object |
| --- | ---: | --- |
| R_RISCV_PCREL_HI20 | 7 | AUIPC high parts for local data/string addresses. |
| R_RISCV_PCREL_LO12_I | 6 | I-type low parts paired to the matching HI20 anchor. |
| R_RISCV_PCREL_LO12_S | 1 | S-type low part for storing the API pointer; paired to its HI20 anchor. |
| R_RISCV_BRANCH | 6 | 32-bit conditional branches to local text labels. |
| R_RISCV_RVC_BRANCH | 6 | Compressed conditional branches to local text labels. |
| R_RISCV_RVC_JUMP | 4 | Compressed C.J branches to local text labels. |
| R_RISCV_RELAX | 14 | Paired linker relaxation markers; treated as no-op markers. |

The parser accepts those forms plus R_RISCV_NONE and R_RISCV_CALL_PLT. PCREL low relocations must resolve through their local anchor to the correct HI20; malformed or ambiguous pairs fail. Branch relocations require a valid instruction, same-section local target, valid alignment, and in-range displacement. CALL_PLT validates its AUIPC/JALR instruction pair and range, but was not present in this hello object. RELAX is ignored only when it matches exactly one primary relocation at the same offset. Other relocation types are rejected. The module manager passes no import resolver, so undefined symbols are rejected. These findings do not establish the relocation needs of sysmon, procmon, or klogger; each future object needs its own audit.

### Versioned API and lifecycle

The module uses struct module_api_v1 from kernel/module.h: abi_version, struct_size, capability bits, and int (*print)(const char *). The loader passes this table to module_init(); the module stores the pointer and calls the service. It imports no printk symbol and the loader does not resolve arbitrary kernel symbols. The loader finds module_init(const struct module_api_v1 *), module_command(int, const char *const []), and module_exit(void) by their exact ELF symbol names and requires defined function exports in allocated executable sections.

The registry reserves names while loading and publishes a module only after init succeeds. Calls require LIVE state and increment active_calls under the registry lock, then execute without holding that lock. Unload rejects a non-live module, active calls, or nonzero dependency references; it sets UNLOADING under the same lock so later calls fail, calls module_exit only after that check, then tears down the image. V1 offers no worker, timer, callback, or interrupt-registration service and has no dependency support; the dependency counter is reserved. Modules are privileged kernel code and this loader is not a sandbox.

### Executable memory and single-hart restriction

**Verified in xv6 and the QEMU prototype:** allocator pages are RW/NX through xv6's identity/direct map; kalloc() does not make them executable. The loader relocates into a bounded non-executable staging buffer, copies into kalloc() pages while their direct aliases are RW/NX, removes those aliases, then maps only the selected arena at PHYSTOP + PGSIZE (0x88001000). Module text is RX, read-only data is R/NX, and data/BSS is RW/NX. Different permission classes cannot share a page. No writable alias remains for executable text, and ordinary RAM remains NX.

After alias removal and permission changes the loader performs local sfence.vma and fence.i before publishing the module. On unload it removes module mappings and fences, restores identity aliases as RW/NX, fences again, and calls kfree() only after write permission is restored. Uncertain page-table cleanup quarantines pages rather than freeing them. The MODULES build parks nonzero harts in M-mode before S-mode startup; module operations require hart 0 and exactly one S-mode kernel hart online; the QEMU target rejects MODULES=1 with CPUS other than 1. This is a single-hart design. There is no remote TLB shootdown or remote fence.i and no multi-hart correctness claim.

### Tests and remaining limits

- HOST_SANITIZE=1 experiments/run_elfmod_tests.sh passed with AddressSanitizer and UndefinedBehaviorSanitizer, covering malformed ELF, relocation pairing/range, branches, API exports, and rejection of the debug/unwind fixture.
- make -B MODULES=1 MODULE_MEMTEST=1 CPUS=1 kernel/kernel fs.img completed successfully. hello.o is in the filesystem image, separate from kernel/kernel.
- QEMU used -smp 1. The boot test executed a fixed trusted addi a0,zero,42; ret sequence from an RX page, returned 42, checked the absent writable alias and PTE permissions, restored RW/NX before kfree(), and verified allocator reuse.
- Separate trusted-code QEMU builds produced the expected store fault on RX (scause=0xf) and instruction fault on R/NX (scause=0xc). These tests intentionally panic after observing the expected fault.
- The one-hart QEMU lifecycle loaded hello.o at runtime, ran module_init and module_command with two arguments, listed it, ran module_exit, unloaded it, and showed an empty registry.
- make MODULES=1 CPUS=2 qemu was rejected by the single-hart gate. No two-hart boot was tested.
- Active-call rejection and UNLOADING checks are implemented, but a concurrent busy-unload race test remains outstanding. Dependencies are unsupported and untested.

Remaining risks: privileged modules are not sandboxed; module code must remain integer-only because xv6 does not save floating-point state; the three feature modules may need additional relocations; the accepted sections and API need broader tests; intermediate page-table pages allocated under the shared kernel page table are not reclaimed by uvmunmap; and SMP synchronization is absent. Keep the two-week scope to a one-hart ET_REL validation, relocation, load, init, invocation, list, and restricted unload demonstration. Defer SMP, dependencies, callbacks/workers, arbitrary imports, sysmon, procmon, and klogger until review and further lifecycle testing.

## Completed module implementation and one-hart integration

This section records the completed feature modules and the runtime checks performed after the earlier prototype notes. It supersedes earlier statements that sysmon, procmon, and klogger were deferred. The system remains an educational, single-hart prototype; it is not an SMP-safe production loader or a module sandbox.

### Versioned module API v2

The loader now passes `struct module_api_v2` from `kernel/module.h` to each module's `module_init`. The table carries `abi_version`, `struct_size`, capability bits, and function pointers. It provides:

- A bounded console print service. Messages must terminate within 192 bytes.
- A system snapshot with uptime ticks, allocator page capacity/free-page count, process-slot count, and counts by process state.
- A process snapshot containing pid, state, and a copied 16-byte name for each non-unused process.
- A kernel log service with append, chronological snapshot, and clear operations.

No module imports arbitrary kernel symbols. The objects have no undefined symbols; internal helper functions use the supported local CALL_PLT relocations. The kernel checks process states under each process lock, reads uptime under tickslock, and counts allocator free pages under kmem.lock. These are individually protected snapshots, not one globally atomic snapshot. The process table is not locked while module output is printed.

`module_init`, `module_command`, and `module_exit` are found by those exact ELF symbol names. Init receives the API table; command receives argc/argv through `modctl run` (the prior `call` spelling remains an alias); exit has no arguments. The manager increments active_calls before entering command code and decrements it afterward. Unload first marks the module UNLOADING, which rejects new calls, invokes exit, removes mappings, restores allocator aliases and permissions, then frees pages. The existing one-hart gate remains mandatory.

The v2 interface has no callback registration, workers, interrupt handlers, dependency imports, or inter-module symbol resolution. The dependency counter continues to be zero because module dependencies are outside this implementation.

### Feature modules and object compatibility

The filesystem now contains separate ET_REL objects for `hello`, `sysmon`, `procmon`, and `klogger`. The Makefile builds them with the kernel's `-march=rv64gc` and `-mcmodel=medany`, explicit `-mabi=lp64d`, `-msmall-data-limit=0`, and no debug/unwind sections. The three feature objects additionally use `-fno-jump-tables -mno-relax`. This avoids compiler-generated ADD32/SUB32 jump-table relocations and relaxation markers with addends outside the parser's strict accepted form. The parser itself was not weakened. The hello fixture keeps the normal assembler relaxation records so its RELAX validation remains covered.

Verified with `riscv64-linux-gnu-readelf` and `nm`: each feature object is ELF64, little-endian, ET_REL, EM_RISCV, and has no undefined symbols. All three currently use the same relocation types:

- `R_RISCV_PCREL_HI20` and paired `R_RISCV_PCREL_LO12_I/S` for local code/data addresses.
- `R_RISCV_CALL_PLT` for calls to local module helper functions.
- `R_RISCV_BRANCH`, `R_RISCV_RVC_BRANCH`, and `R_RISCV_RVC_JUMP` for local control flow.

The expanded host test runner compiles each production module and both test fixtures with reproducible flags, validates the required exports, and runs the strict parser and relocator with no import resolver. All those objects pass. Existing malformed ELF, bad relocation type, malformed HI20/LO12 pair, range, alignment, and debug/unwind rejection cases remain covered. A small-object result does not prove compatibility for arbitrary future module sources; keep checking the relocations and section layout of each new object.

### Module behavior

- `sysmon` prints uptime, total/free allocator pages, active processes, and process slots.
- `procmon` snapshots and prints pid, state, and name for each allocated process slot.
- `klogger` accepts `add INFO|WARN|ERROR message...`, `show`, and `clear`. The kernel ring holds 32 records with 63-byte maximum messages, sequence number, tick, and level. On overflow it overwrites the oldest entry and increments a saturating dropped counter. Clear resets entries and dropped count while preserving the monotonic sequence.
- `hello` now uses API v2 consistently and no longer imports printk.

The test-only `fail.o` and `busytest.o` objects are installed only with `MODULE_TESTS=1`. The test API hooks are compiled only in that configuration. fail.o returns an init error to verify rollback and confirm exit is not called after failed init. busytest invokes a test-only unload attempt from inside its active command, then attempts a new call from exit while the slot is UNLOADING. These are deterministic lifecycle checks; neither test hook is present in a normal `MODULES=1` build.

### Runtime and memory verification

The end-to-end harness `experiments/run_module_qemu_tests.py` boots QEMU with `-smp 1` and checks missing/malformed objects, unknown calls, duplicate loads, hello init/call/exit, all three feature modules, failed-init cleanup, active-call unload rejection, call rejection during UNLOADING, simultaneous registration of the three modules, and final empty-registry state. It adds 35 records after clearing klogger, verifies the 32 retained records are entries 003–034 and that dropped=3, then verifies clear returns entries=0 and dropped=0. The harness completed successfully.

Executable placement and permission handling remain as in the verified single-hart prototype: allocate ordinary kalloc pages, populate them through RW/NX direct-map aliases before publication, remove those aliases, map text RX, rodata R/NX, and data/BSS RW/NX, then perform local sfence.vma and fence.i. Unload removes the module mapping, fences, restores RW/NX allocator aliases, and only then calls kfree. Uncertain teardown still quarantines pages. No general RAM mapping was made executable.

### Verification status and remaining risks

Verified in this pass:
- The expanded host parser/relocation suite passes, including feature objects. The ASan/UBSan invocation also passed the parser suite; the subsequent ELF audit command in that combined shell invocation had a quoting error, so its output is not used as evidence. A separate readelf/nm audit confirmed the object headers, relocation type sets, and empty undefined-symbol lists.
- `make -B MODULES=1 MODULE_TESTS=1 CPUS=1 kernel/kernel fs.img` completed, with the repository's existing linker warning that the kernel ELF has an RWX LOAD segment. xv6's runtime PTE mapping policy remains independently RX for kernel text and RW/NX for allocator RAM.
- The one-hart QEMU integration harness passed actual runtime load, initialization, invocation, listing, unload, failed-init rollback, busy-unload rejection, and klogger wrap/clear checks. The production-mode run passed with test hooks omitted.
- The default non-module build completed, and \`experiments/run_usertests_qemu.py\` ran the standard \`usertests -q\` suite to \`ALL TESTS PASSED\` on one QEMU hart. Its expected user page-fault probes printed trap diagnostics before succeeding.

Not verified or supported:
- No multi-hart module loading, calls, TLB shootdown, or remote instruction-cache fencing. The build parks secondary harts in M-mode and refuses module operations unless exactly one hart is online. Demonstrations must use `CPUS=1`.
- Module code is privileged kernel code and is not isolated from the kernel. Malicious or buggy modules can corrupt kernel state.
- There is no asynchronous callback/worker/interrupt API, dependency loading, general symbol resolver, concurrent module command service, or unload of executing code from multiple harts.
- New feature code may require new relocation types or larger parser limits; inspect each object's full ELF metadata before accepting it.
- The test-only API changes the API table size at compile time. Test modules and the test kernel must be built with the same `MODULE_TESTS=1` setting. Normal builds omit those hooks.

The next practical step is a review of this implementation and a production-mode `MODULES=1 CPUS=1` demonstration without test hooks, followed by the normal non-module xv6 regression build. No SMP or security claim should be made.
