#ifndef ELFMOD_H
#define ELFMOD_H

#include "types.h"

/*
 * Bounded ELF64/RISC-V ET_REL validation and relocation engine.
 *
 * The context is caller-owned and intentionally fixed-size so the parser
 * needs no allocator and never places attacker-sized metadata on the stack.
 * Keep the input buffer immutable and alive until relocation completes.
 */
#define ELFMOD_MAX_FILE_SIZE   (1024UL * 1024UL)
#define ELFMOD_MAX_IMAGE_SIZE  (2UL * 1024UL * 1024UL)
#define ELFMOD_MAX_SECTIONS    32
#define ELFMOD_MAX_SYMBOLS     2048
#define ELFMOD_MAX_RELOCATIONS 4096
#define ELFMOD_PAGE_SIZE       4096UL

enum elfmod_error {
  ELFMOD_OK = 0,
  ELFMOD_E_ARGUMENT,
  ELFMOD_E_LIMIT,
  ELFMOD_E_HEADER,
  ELFMOD_E_BOUNDS,
  ELFMOD_E_SECTION,
  ELFMOD_E_UNSUPPORTED_SECTION,
  ELFMOD_E_ATTRIBUTE,
  ELFMOD_E_SYMBOL,
  ELFMOD_E_IMPORT,
  ELFMOD_E_RELOCATION,
  ELFMOD_E_UNSUPPORTED_RELOCATION,
  ELFMOD_E_PAIR,
  ELFMOD_E_INSTRUCTION,
  ELFMOD_E_ALIGNMENT,
  ELFMOD_E_OVERFLOW,
  ELFMOD_E_RANGE,
  ELFMOD_E_EXPORT
};

enum elfmod_perm_class {
  ELFMOD_PERM_TEXT = 0,
  ELFMOD_PERM_READONLY = 1,
  ELFMOD_PERM_DATA = 2
};

struct elfmod_section {
  uint32 name_offset;
  uint32 type;
  uint64 flags;
  uint64 file_offset;
  uint64 size;
  uint32 link;
  uint32 info;
  uint64 alignment;
  uint64 entry_size;
  uint64 staging_offset;
  uint8 perm_class;
  uint8 allocated;
};

struct elfmod_context {
  uint64 cookie;
  const uchar *file;
  uint64 file_size;
  uint64 section_table_offset;
  uint64 staging_size;
  uint32 section_count;
  uint32 section_name_index;
  uint32 symbol_table_index;
  uint32 symbol_string_index;
  uint32 symbol_count;
  uint32 first_global_symbol;
  uint32 relocation_count;
  struct elfmod_section sections[ELFMOD_MAX_SECTIONS];
};

/* Return 1 and write a stable address only for an explicitly permitted import. */
typedef int (*elfmod_import_resolver)(void *opaque, const char *name,
                                      uint8 symbol_type, uint64 *address);

struct elfmod_export {
  uint32 section_index;
  uint64 section_offset;
  uint64 size;
};

int elfmod_parse(struct elfmod_context *context, const void *file,
                 uint64 file_size, enum elfmod_error *error);

/*
 * Copy allocated PROGBITS into the bounded staging image, zero NOBITS, and
 * apply supported RELA records. section_addresses[i] is the final runtime
 * address of allocated section i; non-allocated entries are ignored.
 *
 * The resolver is an explicit import allowlist (for tests, the only permitted
 * name is printk). Passing null rejects every undefined import. This function
 * never calls relocated code and never changes page-table permissions.
 */
int elfmod_relocate(const struct elfmod_context *context, uchar *staging,
                    uint64 staging_capacity,
                    const uint64 section_addresses[ELFMOD_MAX_SECTIONS],
                    elfmod_import_resolver resolver, void *resolver_opaque,
                    enum elfmod_error *error);

int elfmod_find_export(const struct elfmod_context *context, const char *name,
                       struct elfmod_export *result,
                       enum elfmod_error *error);

const char *elfmod_error_string(enum elfmod_error error);

#endif
