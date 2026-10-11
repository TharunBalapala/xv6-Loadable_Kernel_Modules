#include "elfmod.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHT_RELA 4
#define R_RISCV_NONE 0
#define R_RISCV_BRANCH 16
#define R_RISCV_CALL_PLT 19
#define R_RISCV_RVC_BRANCH 44
#define R_RISCV_RVC_JUMP 45
#define R_RISCV_PCREL_HI20 23
#define R_RISCV_PCREL_LO12_I 24
#define R_RISCV_PCREL_LO12_S 25
#define R_RISCV_RELAX 51
#define TEST_TEXT_BASE 0x20000000UL
#define TEST_PRINTK_NEAR 0x20008000UL

static uchar input[ELFMOD_MAX_FILE_SIZE];
static uchar mutated[ELFMOD_MAX_FILE_SIZE];
static uchar staging[ELFMOD_MAX_IMAGE_SIZE];
static struct elfmod_context parsed;
static uint64 file_length;
static int tests_failed;
static uint64 printk_address = TEST_PRINTK_NEAR;

static uint16
rd16(const uchar *p)
{
  return (uint16)p[0] | ((uint16)p[1] << 8);
}

static uint32
rd32(const uchar *p)
{
  return (uint32)p[0] | ((uint32)p[1] << 8) |
         ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

static uint64
rd64(const uchar *p)
{
  return (uint64)rd32(p) | ((uint64)rd32(p + 4) << 32);
}

static void
wr16(uchar *p, uint16 value)
{
  p[0] = (uchar)value;
  p[1] = (uchar)(value >> 8);
}

static void
wr32(uchar *p, uint32 value)
{
  p[0] = (uchar)value;
  p[1] = (uchar)(value >> 8);
  p[2] = (uchar)(value >> 16);
  p[3] = (uchar)(value >> 24);
}

static void
wr64(uchar *p, uint64 value)
{
  wr32(p, (uint32)value);
  wr32(p + 4, (uint32)(value >> 32));
}

static int
read_file(const char *path, uchar *destination, uint64 *length)
{
  FILE *f = fopen(path, "rb");
  long file_size;

  if (!f)
    return -1;
  if (fseek(f, 0, SEEK_END) != 0 ||
      (file_size = ftell(f)) < 0 ||
      (uint64)file_size > ELFMOD_MAX_FILE_SIZE ||
      fseek(f, 0, SEEK_SET) != 0 ||
      fread(destination, 1, (size_t)file_size, f) != (size_t)file_size) {
    fclose(f);
    return -1;
  }
  fclose(f);
  *length = (uint64)file_size;
  return 0;
}

static void
check(int condition, const char *message)
{
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    tests_failed++;
  } else {
    printf("ok: %s\n", message);
  }
}

static void
expect_parse_error(const char *name, const uchar *bytes, uint64 length,
                   enum elfmod_error wanted)
{
  struct elfmod_context context;
  enum elfmod_error error = ELFMOD_OK;
  int result = elfmod_parse(&context, bytes, length, &error);

  check(result < 0 && error == wanted, name);
  if (result >= 0 || error != wanted)
    fprintf(stderr, "  got %s (%d), expected %s (%d)\n",
            elfmod_error_string(error), error,
            elfmod_error_string(wanted), wanted);
}

static int
copy_mutation(void)
{
  if (file_length > sizeof(mutated))
    return -1;
  memcpy(mutated, input, (size_t)file_length);
  return 0;
}

static uint64
section_header_offset(const struct elfmod_context *context, uint32 index)
{
  return context->section_table_offset + (uint64)index * 64;
}

static uint32
find_section(const struct elfmod_context *context, uint32 type,
             uint32 ordinal)
{
  uint32 i;

  for (i = 1; i < context->section_count; i++) {
    if (context->sections[i].type == type) {
      if (ordinal == 0)
        return i;
      ordinal--;
    }
  }
  return 0;
}

static int
find_relocation(const struct elfmod_context *context, uint32 wanted_type,
                uint32 *rel_section, uint32 *entry, uint64 *file_offset)
{
  uint32 i, j;

  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *section = &context->sections[i];
    if (section->type != SHT_RELA)
      continue;
    for (j = 0; j < section->size / 24; j++) {
      uint64 off = section->file_offset + (uint64)j * 24;
      if ((uint32)rd64(context->file + off + 8) == wanted_type) {
        *rel_section = i;
        *entry = j;
        *file_offset = off;
        return 0;
      }
    }
  }
  return -1;
}

static int
find_symbol(const struct elfmod_context *context, const char *wanted,
            uint32 *symbol_index)
{
  const struct elfmod_section *symtab =
    &context->sections[context->symbol_table_index];
  const struct elfmod_section *strtab =
    &context->sections[context->symbol_string_index];
  uint32 i;

  for (i = 0; i < context->symbol_count; i++) {
    uint64 symoff = symtab->file_offset + (uint64)i * 24;
    uint32 name_offset = rd32(context->file + symoff);
    const char *name = (const char *)(context->file +
                                     strtab->file_offset + name_offset);
    if (name_offset < strtab->size && strcmp(name, wanted) == 0) {
      *symbol_index = i;
      return 0;
    }
  }
  return -1;
}

static int
resolve_test_import(void *opaque, const char *name, uint8 type,
                    uint64 *address)
{
  (void)opaque;
  if (strcmp(name, "printk") != 0 || type != 0)
    return 0;
  *address = printk_address;
  return 1;
}

static void
section_addresses(const struct elfmod_context *context,
                  uint64 addresses[ELFMOD_MAX_SECTIONS], uint64 text_base,
                  uint64 rodata_base, uint64 data_base)
{
  uint64 class_start[3] = {
    ~(uint64)0, ~(uint64)0, ~(uint64)0
  };
  uint32 i;

  memset(addresses, 0, sizeof(uint64) * ELFMOD_MAX_SECTIONS);
  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *section = &context->sections[i];
    if (section->allocated &&
        section->staging_offset < class_start[section->perm_class])
      class_start[section->perm_class] = section->staging_offset;
  }
  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *section = &context->sections[i];
    uint64 base;
    if (!section->allocated)
      continue;
    if (section->perm_class == ELFMOD_PERM_TEXT)
      base = text_base;
    else if (section->perm_class == ELFMOD_PERM_READONLY)
      base = rodata_base;
    else
      base = data_base;
    addresses[i] = base + section->staging_offset -
                   class_start[section->perm_class];
  }
}

static int64_t
sext20(uint32 value)
{
  uint32 immediate = (value >> 12) & 0xfffff;
  return (immediate & 0x80000) ? (int64_t)immediate - 0x100000
                               : (int64_t)immediate;
}

static int64_t
sext12(uint32 value)
{
  uint32 immediate = (value >> 20) & 0xfff;
  return (immediate & 0x800) ? (int64_t)immediate - 0x1000
                             : (int64_t)immediate;
}

static int64_t
sext_s12(uint32 value)
{
  uint32 immediate = ((value >> 25) << 5) | ((value >> 7) & 0x1f);
  immediate &= 0xfff;
  return (immediate & 0x800) ? (int64_t)immediate - 0x1000
                             : (int64_t)immediate;
}

static void
check_hello_pairs(const struct elfmod_context *context,
                  const uint64 addresses[ELFMOD_MAX_SECTIONS],
                  int store_type)
{
  uint32 i, j;
  uint32 checked = 0, wanted_low = store_type
                                      ? R_RISCV_PCREL_LO12_S
                                      : R_RISCV_PCREL_LO12_I;

  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *relsec = &context->sections[i];
    if (relsec->type != SHT_RELA)
      continue;
    for (j = 0; j < relsec->size / 24; j++) {
      uint64 hi_off = relsec->file_offset + (uint64)j * 24;
      uint64 info = rd64(context->file + hi_off + 8);
      uint32 type = (uint32)info;
      uint32 target_index = relsec->info;
      uint64 target_offset, target_symbol_value = 0;
      uint32 target_symbol_section = 0;
      uint32 k, found_low = 0;

      if (type != R_RISCV_PCREL_HI20)
        continue;
      target_offset = rd64(context->file + hi_off);
      for (k = 1; k < context->section_count; k++) {
        const struct elfmod_section *lowsec = &context->sections[k];
        uint32 n;
        if (lowsec->type != SHT_RELA || lowsec->info != target_index)
          continue;
        for (n = 0; n < lowsec->size / 24; n++) {
          uint64 low_off = lowsec->file_offset + (uint64)n * 24;
          uint64 low_info = rd64(context->file + low_off + 8);
          uint32 low_type = (uint32)low_info;
          uint32 anchor_index = (uint32)(low_info >> 32);
          const struct elfmod_section *symtab =
            &context->sections[context->symbol_table_index];
          uint64 anchor_off = symtab->file_offset +
                              (uint64)anchor_index * 24;
          uint32 anchor_section = rd16(context->file + anchor_off + 6);
          uint64 anchor_value = rd64(context->file + anchor_off + 8);
          uint32 high_symbol;

          if (low_type != wanted_low || anchor_section != target_index ||
              anchor_value != target_offset)
            continue;
          found_low = 1;
          high_symbol = (uint32)(info >> 32);
          {
            uint64 symbol_off = symtab->file_offset +
                                (uint64)high_symbol * 24;
            target_symbol_section =
              rd16(context->file + symbol_off + 6);
            target_symbol_value = rd64(context->file + symbol_off + 8);
          }
          {
            uint64 text_stage = context->sections[target_index].staging_offset;
            uint64 hi_instr = rd32(staging + text_stage + target_offset);
            uint64 lo_instr = rd32(staging + text_stage +
                                   rd64(context->file + low_off));
            int64_t actual = sext20((uint32)hi_instr) * 4096 +
              (store_type ? sext_s12((uint32)lo_instr)
                          : sext12((uint32)lo_instr));
            int64_t expected =
              (int64_t)(addresses[target_symbol_section] +
                        target_symbol_value) -
              (int64_t)(addresses[target_index] + target_offset);
            check(actual == expected,
                  store_type ? "PCREL_HI20/LO12_S reconstructs S-P"
                             : "PCREL_HI20/LO12_I reconstructs S-P");
            checked++;
          }
          break;
        }
        if (found_low)
          break;
      }
    }
  }
  check(checked != 0, store_type ? "S-type relocation pair was applied"
                                 : "I-type relocation pair was applied");
}

static int64_t
sign_extend(uint32 value, uint bits)
{
  uint32 sign = 1U << (bits - 1);
  uint32 mask = (1U << bits) - 1;
  value &= mask;
  return (value & sign) ? (int64_t)value - ((int64_t)1 << bits)
                        : (int64_t)value;
}

static void
check_control_flow_relocations(const struct elfmod_context *context)
{
  uint32 i, j, branch_count = 0, cbranch_count = 0, cjump_count = 0;

  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *relsec = &context->sections[i];
    uint32 target_index = relsec->info;
    const struct elfmod_section *target;
    if (relsec->type != SHT_RELA)
      continue;
    target = &context->sections[target_index];
    for (j = 0; j < relsec->size / 24; j++) {
      uint64 entry = relsec->file_offset + (uint64)j * 24;
      uint64 info = rd64(context->file + entry + 8);
      uint32 type = (uint32)info, sym = (uint32)(info >> 32);
      uint64 offset = rd64(context->file + entry), symoff;
      uint64 symbol_value, stage;
      uint32 symbol_section;
      int64_t expected, actual;

      if (type != R_RISCV_BRANCH && type != R_RISCV_RVC_BRANCH &&
          type != R_RISCV_RVC_JUMP)
        continue;
      symoff = context->sections[context->symbol_table_index].file_offset +
               (uint64)sym * 24;
      symbol_section = rd16(context->file + symoff + 6);
      symbol_value = rd64(context->file + symoff + 8);
      stage = target->staging_offset + offset;
      if (symbol_section != target_index) {
        check(0, "local branch target stays in its text section");
        continue;
      }
      expected = (int64_t)symbol_value - (int64_t)offset;
      if (type == R_RISCV_BRANCH) {
        uint32 insn = rd32(staging + stage);
        uint32 imm = (((insn >> 31) & 1) << 12) |
                     (((insn >> 25) & 0x3f) << 5) |
                     (((insn >> 8) & 0xf) << 1) |
                     (((insn >> 7) & 1) << 11);
        actual = sign_extend(imm, 13);
        branch_count++;
      } else {
        uint16 insn = rd16(staging + stage);
        uint32 imm;
        if (type == R_RISCV_RVC_BRANCH) {
          imm = (((insn >> 12) & 1) << 8) |
                (((insn >> 10) & 3) << 3) |
                (((insn >> 5) & 3) << 6) |
                (((insn >> 3) & 3) << 1) |
                (((insn >> 2) & 1) << 5);
          actual = sign_extend(imm, 9);
          cbranch_count++;
        } else {
          imm = (((insn >> 12) & 1) << 11) |
                (((insn >> 11) & 1) << 4) |
                (((insn >> 9) & 3) << 8) |
                (((insn >> 8) & 1) << 10) |
                (((insn >> 7) & 1) << 6) |
                (((insn >> 6) & 1) << 7) |
                (((insn >> 3) & 7) << 1) |
                (((insn >> 2) & 1) << 5);
          actual = sign_extend(imm, 12);
          cjump_count++;
        }
      }
      check(actual == expected, "local branch relocation reconstructs S-P");
    }
  }
  check(branch_count > 0, "32-bit branch relocations are present");
  check(cbranch_count > 0, "compressed branch relocations are present");
  check(cjump_count > 0, "compressed jump relocations are present");
}

static int
run_relocation(const struct elfmod_context *context,
               uint64 addresses[ELFMOD_MAX_SECTIONS],
               uint64 printk_target, enum elfmod_error *error)
{
  printk_address = printk_target;
  memset(staging, 0xcc, sizeof(staging));
  return elfmod_relocate(context, staging, sizeof(staging), addresses,
                         resolve_test_import, 0, error);
}

static void
test_malformed_inputs(void)
{
  enum elfmod_error error;
  expect_parse_error("truncated ELF header", input, 63, ELFMOD_E_BOUNDS);

  copy_mutation();
  mutated[0] = 0;
  expect_parse_error("invalid ELF magic", mutated, file_length,
                     ELFMOD_E_HEADER);

  copy_mutation();
  wr16(mutated + 18, 62);
  expect_parse_error("non-RISC-V machine", mutated, file_length,
                     ELFMOD_E_HEADER);

  copy_mutation();
  wr64(mutated + 40, 0xfffffffffffffff8UL);
  expect_parse_error("section-table offset/size overflow", mutated,
                     file_length, ELFMOD_E_BOUNDS);

  copy_mutation();
  wr16(mutated + 62, 0xffff);
  expect_parse_error("invalid section-name table index", mutated,
                     file_length, ELFMOD_E_BOUNDS);

  copy_mutation();
  {
    const struct elfmod_section *strings =
      &parsed.sections[parsed.section_name_index];
    mutated[strings->file_offset + strings->size - 1] = 'X';
  }
  expect_parse_error("unterminated section-name table", mutated,
                     file_length, ELFMOD_E_SECTION);

  copy_mutation();
  wr64(mutated + section_header_offset(&parsed,
                parsed.symbol_table_index) + 24, 0xfffffffffffffff0UL);
  expect_parse_error("invalid symbol-table file bounds", mutated,
                     file_length, ELFMOD_E_BOUNDS);

  copy_mutation();
  {
    const struct elfmod_section *strings =
      &parsed.sections[parsed.symbol_string_index];
    mutated[strings->file_offset + strings->size - 1] = 'X';
  }
  expect_parse_error("unterminated symbol string table/name", mutated,
                     file_length, ELFMOD_E_SYMBOL);

  copy_mutation();
  {
    uint32 relsec, entry;
    uint64 off, info;
    find_relocation(&parsed, R_RISCV_PCREL_HI20, &relsec, &entry, &off);
    info = rd64(mutated + off + 8);
    wr64(mutated + off + 8,
         ((uint64)0xffffffff << 32) | (uint32)info);
  }
  expect_parse_error("invalid relocation symbol index", mutated,
                     file_length, ELFMOD_E_RELOCATION);

  copy_mutation();
  {
    uint32 relsec, entry;
    uint64 off;
    find_relocation(&parsed, R_RISCV_PCREL_HI20, &relsec, &entry, &off);
    wr32(mutated + section_header_offset(&parsed, relsec) + 44,
         parsed.section_count + 1);
  }
  expect_parse_error("invalid relocation target section", mutated,
                     file_length, ELFMOD_E_SECTION);

  copy_mutation();
  {
    uint32 relsec, entry;
    uint64 off, info;
    find_relocation(&parsed, R_RISCV_PCREL_HI20, &relsec, &entry, &off);
    info = rd64(mutated + off + 8);
    wr64(mutated + off + 8, (info & 0xffffffff00000000UL) | 99);
  }
  expect_parse_error("unsupported relocation type", mutated,
                     file_length, ELFMOD_E_UNSUPPORTED_RELOCATION);

  copy_mutation();
  {
    uint32 relsec, entry, module_init;
    uint64 off, info;
    find_relocation(&parsed, R_RISCV_PCREL_LO12_I,
                    &relsec, &entry, &off);
    check(find_symbol(&parsed, "module_init", &module_init) == 0,
          "test found module_init symbol");
    info = rd64(mutated + off + 8);
    wr64(mutated + off + 8,
         ((uint64)module_init << 32) | (uint32)info);
  }
  expect_parse_error("malformed PCREL HI20/LO12 anchor pair", mutated,
                     file_length, ELFMOD_E_PAIR);

  copy_mutation();
  {
    uint32 relsec, entry;
    uint64 off, site;
    uint32 target_index;
    find_relocation(&parsed, R_RISCV_BRANCH, &relsec, &entry, &off);
    target_index = parsed.sections[relsec].info;
    site = rd64(mutated + off);
    wr32(mutated + parsed.sections[target_index].file_offset + site,
         0x00000013);
  }
  expect_parse_error("branch relocation site is not a branch instruction",
                     mutated, file_length, ELFMOD_E_INSTRUCTION);

  copy_mutation();
  {
    uint32 relsec, entry;
    uint64 off;
    find_relocation(&parsed, R_RISCV_RVC_BRANCH, &relsec, &entry, &off);
    wr64(mutated + off + 16, 2);
  }
  expect_parse_error("compressed branch addend is rejected", mutated,
                     file_length, ELFMOD_E_PAIR);

  copy_mutation();
  {
    uint32 relsec, entry;
    uint64 off;
    find_relocation(&parsed, R_RISCV_PCREL_HI20, &relsec, &entry, &off);
    wr64(mutated + off, 9);
  }
  expect_parse_error("misaligned relocation instruction site", mutated,
                     file_length, ELFMOD_E_ALIGNMENT);

  copy_mutation();
  {
    uint32 relsec, entry;
    uint64 off;
    find_relocation(&parsed, R_RISCV_PCREL_HI20, &relsec, &entry, &off);
    wr64(mutated + off, 0xffffffffffffffffUL);
  }
  expect_parse_error("relocation offset/width overflow", mutated,
                     file_length, ELFMOD_E_BOUNDS);

  copy_mutation();
  {
    uint32 relsec, entry;
    uint64 off;
    find_relocation(&parsed, R_RISCV_RELAX, &relsec, &entry, &off);
    wr64(mutated + off, rd64(mutated + off) + 2);
  }
  expect_parse_error("RELAX marker without a same-site primary relocation",
                     mutated, file_length, ELFMOD_E_RELOCATION);

  copy_mutation();
  {
    uint32 relsec, entry;
    uint64 off, info;
    struct elfmod_context none_context;
    uint64 addresses[ELFMOD_MAX_SECTIONS];
    find_relocation(&parsed, R_RISCV_RELAX, &relsec, &entry, &off);
    info = rd64(mutated + off + 8);
    wr64(mutated + off + 8,
         (info & 0xffffffff00000000UL) | R_RISCV_NONE);
    check(elfmod_parse(&none_context, mutated, file_length, &error) == 0,
          "R_RISCV_NONE parses as an explicit no-op");
    if (none_context.cookie != 0) {
      section_addresses(&none_context, addresses, TEST_TEXT_BASE,
                        TEST_TEXT_BASE + 0x1000, TEST_TEXT_BASE + 0x2000);
      check(elfmod_relocate(&none_context, staging, sizeof(staging),
                            addresses, resolve_test_import, 0, &error) == 0,
            "R_RISCV_NONE leaves relocation processing valid");
    }
  }

  copy_mutation();
  {
    uint32 bss = 0, i;
    for (i = 1; i < parsed.section_count; i++) {
      if (parsed.sections[i].type == 8 &&
          parsed.sections[i].perm_class == ELFMOD_PERM_DATA) {
        bss = i;
        break;
      }
    }
    check(bss != 0, "test found BSS section");
    wr64(mutated + section_header_offset(&parsed, bss) + 32,
         0xffffffffffffffffUL);
  }
  expect_parse_error("BSS layout arithmetic overflow", mutated,
                     file_length, ELFMOD_E_OVERFLOW);

  copy_mutation();
  wr64(mutated + section_header_offset(&parsed, 1) + 24,
       0xfffffffffffffff0UL);
  expect_parse_error("section offset/size bounds overflow", mutated,
                     file_length, ELFMOD_E_BOUNDS);


}

static void
test_success_and_range(const char *object_path, int store_type)
{
  uint64 addresses[ELFMOD_MAX_SECTIONS];
  enum elfmod_error error;
  struct elfmod_export exported;
  uint32 text = find_section(&parsed, 1, 0);
  int result;

  section_addresses(&parsed, addresses, TEST_TEXT_BASE,
                    TEST_TEXT_BASE + 0x1000, TEST_TEXT_BASE + 0x2000);
  result = run_relocation(&parsed, addresses, TEST_PRINTK_NEAR, &error);
  check(result == 0, "bounded parse and relocation succeeds");
  if (result == 0) {
    check_hello_pairs(&parsed, addresses, store_type);
    if (!store_type) {
      check_control_flow_relocations(&parsed);
      section_addresses(&parsed, addresses, 0x21001000UL,
                        0x21000000UL, 0x21002000UL);
      result = run_relocation(&parsed, addresses, 0x21000000UL, &error);
      check(result == 0, "negative PC-relative fixture relocates");
      if (result == 0) {
        check_hello_pairs(&parsed, addresses, 0);
        check_control_flow_relocations(&parsed);
      }
      section_addresses(&parsed, addresses, TEST_TEXT_BASE,
                        TEST_TEXT_BASE + 0x1000, TEST_TEXT_BASE + 0x2000);
    }
  } else {
    fprintf(stderr, "  %s: %s\n", object_path,
            elfmod_error_string(error));
  }

  check(elfmod_find_export(&parsed, "module_init", &exported, &error) == 0,
        "module_init export is identified as a defined function");
  if (!store_type) {
    check(elfmod_find_export(&parsed, "module_exit", &exported, &error) == 0,
          "module_exit export is identified as a defined function");
    check(elfmod_find_export(&parsed, "module_command", &exported, &error) == 0,
          "module_command export is identified as a defined function");

    memset(staging, 0xcc, sizeof(staging));
    result = elfmod_relocate(&parsed, staging, sizeof(staging), addresses,
                             0, 0, &error);
    check(result == 0,
          "API-table module relocates without arbitrary kernel imports");

    {
      uint64 far_addresses[ELFMOD_MAX_SECTIONS];
      section_addresses(&parsed, far_addresses, TEST_TEXT_BASE,
                        TEST_TEXT_BASE + 0x90000000UL,
                        TEST_TEXT_BASE + 0x2000);
      memset(staging, 0xcc, sizeof(staging));
      result = elfmod_relocate(&parsed, staging, sizeof(staging),
                               far_addresses, 0, 0, &error);
      check(result < 0 && error == ELFMOD_E_RANGE,
            "out-of-range section-relative PCREL relocation is rejected");
      check(staging[0] == 0xcc,
            "out-of-range relocation is rejected before staging writes");
    }
    memset(staging, 0xcc, sizeof(staging));
    addresses[text] += 1;
    result = elfmod_relocate(&parsed, staging, sizeof(staging), addresses,
                             resolve_test_import, 0, &error);
    check(result < 0 && error == ELFMOD_E_ALIGNMENT,
          "misaligned final section address is rejected");
  }
}

static void
test_existing_phase2_object(const char *path)
{
  uchar old_bytes[ELFMOD_MAX_FILE_SIZE];
  uint64 old_length;
  struct elfmod_context context;
  enum elfmod_error error;

  if (read_file(path, old_bytes, &old_length) < 0) {
    fprintf(stderr, "FAIL: could not read existing Phase 2 object: %s\n",
            path);
    tests_failed++;
    return;
  }
  if (elfmod_parse(&context, old_bytes, old_length, &error) == 0 ||
      error != ELFMOD_E_UNSUPPORTED_SECTION) {
    fprintf(stderr, "FAIL: existing debug/unwind object was not rejected "
                    "as outside the subset (%s)\n",
            elfmod_error_string(error));
    tests_failed++;
  } else {
    printf("ok: existing Phase 2 debug/unwind object rejected: %s\n",
           elfmod_error_string(error));
  }
}

static int
load_primary_fixture(const char *path)
{
  enum elfmod_error error;

  if (read_file(path, input, &file_length) < 0) {
    fprintf(stderr, "FAIL: could not read fixture: %s\n", path);
    tests_failed++;
    return -1;
  }
  if (elfmod_parse(&parsed, input, file_length, &error) < 0) {
    uint32 i;
    fprintf(stderr, "FAIL: fixture parse failed: %s (%d)\n",
            elfmod_error_string(error), error);
    for (i = 0; i < parsed.section_count; i++)
      fprintf(stderr, "  sec %u type=%u flags=%lx off=%lu size=%lu nameoff=%u\n",
              i, parsed.sections[i].type,
              (unsigned long)parsed.sections[i].flags,
              (unsigned long)parsed.sections[i].file_offset,
              (unsigned long)parsed.sections[i].size,
              parsed.sections[i].name_offset);
    tests_failed++;
    return -1;
  }
  return 0;
}


static void
test_module_object(const char *path)
{
  uchar bytes[ELFMOD_MAX_FILE_SIZE];
  uint64 length, addresses[ELFMOD_MAX_SECTIONS];
  struct elfmod_context context;
  struct elfmod_export exported;
  enum elfmod_error error = ELFMOD_OK;
  int result;
  const char *required[] = {"module_init", "module_command", "module_exit"};
  uint i;

  if (read_file(path, bytes, &length) < 0) {
    fprintf(stderr, "FAIL: could not read module object: %s\n", path);
    tests_failed++;
    return;
  }
  result = elfmod_parse(&context, bytes, length, &error);
  check(result == 0, "feature module passes strict ELF validation");
  if (result < 0) {
    fprintf(stderr, "  %s: %s\n", path, elfmod_error_string(error));
    return;
  }
  for (i = 0; i < sizeof(required) / sizeof(required[0]); i++)
    check(elfmod_find_export(&context, required[i], &exported, &error) == 0,
          "feature module exports required entry point");
  section_addresses(&context, addresses, TEST_TEXT_BASE,
                    TEST_TEXT_BASE + 0x1000, TEST_TEXT_BASE + 0x2000);
  memset(staging, 0, sizeof(staging));
  result = elfmod_relocate(&context, staging, sizeof(staging), addresses,
                           0, 0, &error);
  check(result == 0, "feature module relocates without kernel imports");
  if (result < 0)
    fprintf(stderr, "  %s: relocation: %s\n", path,
            elfmod_error_string(error));
}

int
main(int argc, char **argv)
{
  uint32 relsec, entry;
  uint64 reloff;
  uint32 store_type = 0;

  if (argc < 4) {
    fprintf(stderr, "usage: %s clean-hello.o store-fixture.o old-hello.o\n",
            argv[0]);
    return 2;
  }
  if (load_primary_fixture(argv[1]) < 0)
    return 1;
  printf("parsed clean fixture (%lu bytes, %u sections, %u symbols, "
         "%u relocations, %lu staging bytes)\n",
         (unsigned long)file_length, parsed.section_count,
         parsed.symbol_count, parsed.relocation_count,
         (unsigned long)parsed.staging_size);
  test_success_and_range(argv[1], 0);
  test_malformed_inputs();
  test_existing_phase2_object(argv[3]);

  if (load_primary_fixture(argv[2]) == 0) {
    if (find_relocation(&parsed, R_RISCV_PCREL_LO12_S,
                        &relsec, &entry, &reloff) == 0)
      store_type = 1;
    else
      check(0, "S-type relocation fixture contains LO12_S");
    if (store_type)
      test_success_and_range(argv[2], 1);
  }
  if (!store_type)
    fprintf(stderr, "FAIL: S-type relocation fixture unavailable\n");

  {
    int i;
    for (i = 4; i < argc; i++)
      test_module_object(argv[i]);
  }

  if (tests_failed) {
    fprintf(stderr, "%d test(s) failed\n", tests_failed);
    return 1;
  }
  printf("all ELF module parser tests passed\n");
  return 0;
}
