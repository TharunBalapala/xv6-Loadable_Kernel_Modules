# Dynamic Kernel Extensibility in xv6

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

