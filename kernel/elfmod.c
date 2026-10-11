#include "elfmod.h"

typedef signed long int64;
typedef char elfmod_u64_width_check[(sizeof(uint64) == 8) ? 1 : -1];
typedef char elfmod_s64_width_check[(sizeof(int64) == 8) ? 1 : -1];

/* ELF gABI and RISC-V psABI values used by the deliberately narrow loader. */
#define EI_NIDENT       16
#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EV_CURRENT      1
#define ET_REL          1
#define EM_RISCV        243
#define EF_RISCV_RVC    0x0001
#define EF_RISCV_FLOAT  0x0006
#define EF_RISCV_DOUBLE 0x0004

#define SHT_NULL             0
#define SHT_PROGBITS         1
#define SHT_SYMTAB           2
#define SHT_STRTAB           3
#define SHT_RELA             4
#define SHT_NOBITS           8
#define SHT_RISCV_ATTRIBUTES 0x70000003

#define SHF_WRITE      0x1
#define SHF_ALLOC      0x2
#define SHF_EXECINSTR  0x4
#define SHF_MERGE      0x10
#define SHF_STRINGS    0x20
#define SHF_INFO_LINK  0x40
#define SHF_TLS        0x400
#define SHF_COMPRESSED 0x800

#define STB_LOCAL     0
#define STB_GLOBAL    1
#define STB_WEAK      2
#define STT_NOTYPE    0
#define STT_OBJECT    1
#define STT_FUNC      2
#define STT_SECTION   3
#define STT_FILE      4
#define SHN_UNDEF     0
#define SHN_LORESERVE 0xff00
#define SHN_ABS       0xfff1

#define R_RISCV_NONE         0
#define R_RISCV_BRANCH       16
#define R_RISCV_CALL_PLT     19
#define R_RISCV_RVC_BRANCH   44
#define R_RISCV_RVC_JUMP     45
#define R_RISCV_PCREL_HI20   23
#define R_RISCV_PCREL_LO12_I 24
#define R_RISCV_PCREL_LO12_S 25
#define R_RISCV_RELAX        51

#define ELF_HEADER_SIZE  64
#define ELF_SECTION_SIZE 64
#define ELF_SYMBOL_SIZE  24
#define ELF_RELA_SIZE    24
#define ELFMOD_COOKIE    0x454c464d4f445631UL
#define INT64_MAX_U      0x7fffffffffffffffUL
#define INT64_MIN_MAG    0x8000000000000000UL

struct elfmod_rela {
  uint64 offset;
  uint32 symbol;
  uint32 type;
  int64 addend;
};

static int
set_error(enum elfmod_error *error, enum elfmod_error value)
{
  if (error)
    *error = value;
  return -1;
}

static int
range_ok(uint64 total, uint64 offset, uint64 length)
{
  return offset <= total && length <= total - offset;
}

static int
add_u64(uint64 a, uint64 b, uint64 *result)
{
  if (a > ~(uint64)0 - b)
    return -1;
  *result = a + b;
  return 0;
}

static int
mul_u64(uint64 a, uint64 b, uint64 *result)
{
  if (a != 0 && b > ~(uint64)0 / a)
    return -1;
  *result = a * b;
  return 0;
}

static int
is_power_of_two(uint64 value)
{
  return value != 0 && (value & (value - 1)) == 0;
}

static int
align_up(uint64 value, uint64 alignment, uint64 *result)
{
  uint64 mask;

  if (!is_power_of_two(alignment))
    return -1;
  mask = alignment - 1;
  if (value > ~(uint64)0 - mask)
    return -1;
  *result = (value + mask) & ~mask;
  return 0;
}

static uint16
get16(const uchar *p)
{
  return (uint16)p[0] | ((uint16)p[1] << 8);
}

static uint32
get32(const uchar *p)
{
  return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
         ((uint32)p[3] << 24);
}

static uint64
get64(const uchar *p)
{
  return (uint64)get32(p) | ((uint64)get32(p + 4) << 32);
}

static void
put16(uchar *p, uint16 value)
{
  p[0] = (uchar)value;
  p[1] = (uchar)(value >> 8);
}

static void
put32(uchar *p, uint32 value)
{
  p[0] = (uchar)value;
  p[1] = (uchar)(value >> 8);
  p[2] = (uchar)(value >> 16);
  p[3] = (uchar)(value >> 24);
}

static int64
get_s64(const uchar *p)
{
  uint64 value = get64(p);

  if (value <= INT64_MAX_U)
    return (int64)value;
  return -1 - (int64)(~value);
}

static int
same_bytes(const uchar *a, const char *b, uint64 length)
{
  uint64 i;

  for (i = 0; i < length; i++)
    if (a[i] != (uchar)b[i])
      return 0;
  return b[length] == 0;
}

static int
name_at(const struct elfmod_context *context, uint32 string_section,
        uint32 offset, const uchar **name, uint64 *length)
{
  const struct elfmod_section *section;
  uint64 i;
  const uchar *base;

  if (string_section >= context->section_count)
    return -1;
  section = &context->sections[string_section];
  if (section->type != SHT_STRTAB || offset >= section->size)
    return -1;
  base = context->file + section->file_offset + offset;
  for (i = 0; i < section->size - offset; i++) {
    if (base[i] == 0) {
      if (name)
        *name = base;
      if (length)
        *length = i;
      return 0;
    }
  }
  return -1;
}

static int
name_is(const struct elfmod_context *context, uint32 string_section,
        uint32 offset, const char *wanted)
{
  const uchar *name;
  uint64 length, wanted_length = 0;

  while (wanted[wanted_length])
    wanted_length++;
  if (name_at(context, string_section, offset, &name, &length) < 0)
    return 0;
  return length == wanted_length && same_bytes(name, wanted, length);
}

static int
read_uleb(const uchar *bytes, uint64 end, uint64 *cursor, uint32 *value)
{
  uint32 result = 0;
  uint shift = 0;
  uchar byte;

  do {
    if (*cursor >= end || shift >= 35)
      return -1;
    byte = bytes[(*cursor)++];
    if (shift == 28 && (byte & 0x70) != 0)
      return -1;
    result |= (uint32)(byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);
  *value = result;
  return 0;
}

static int
arch_name_compatible(const uchar *arch, uint64 length)
{
  static const char *const required[] = {"m", "a",     "f",        "d",
                                         "c", "zicsr", "zifencei", "zmmul"};
  uint32 found = 0;
  uint64 pos = 5;
  uint i;

  if (length < 5 || !same_bytes(arch, "rv64i", 5))
    return 0;
  while (pos < length && arch[pos] != '_') {
    uchar c = arch[pos++];
    if (!((c >= '0' && c <= '9') || c == 'p'))
      return 0;
  }
  while (pos < length) {
    uint64 start, end, n;
    uint64 name_end;
    int matched = 0;

    if (arch[pos++] != '_' || pos >= length)
      return 0;
    start = pos;
    while (pos < length && arch[pos] != '_')
      pos++;
    end = pos;
    name_end = start;
    while (name_end < end && !(arch[name_end] >= '0' && arch[name_end] <= '9'))
      name_end++;
    if (name_end == start)
      return 0;
    n = name_end - start;
    for (i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
      uint64 wanted = 0;
      while (required[i][wanted])
        wanted++;
      if (n == wanted && same_bytes(arch + start, required[i], n)) {
        if (found & (1U << i))
          return 0;
        found |= 1U << i;
        matched = 1;
        break;
      }
    }
    if (!matched)
      return 0;
    while (name_end < end) {
      uchar c = arch[name_end++];
      if (!((c >= '0' && c <= '9') || c == 'p'))
        return 0;
    }
  }
  return found == 0xff;
}

static int
validate_attributes(const struct elfmod_context *context, uint32 index)
{
  const struct elfmod_section *section = &context->sections[index];
  const uchar *bytes = context->file + section->file_offset;
  uint64 length = section->size;
  uint64 pos, vendor_end, subsection_end, arch_length = 0;
  uint32 vendor_length, subsection_tag, subsection_length, tag, value;
  int have_stack = 0, have_arch = 0;
  const uchar *arch = 0;

  if (length < 12 || bytes[0] != 'A' || get32(bytes + 1) != length - 1)
    return -1;
  pos = 5;
  while (pos < length && bytes[pos] != 0)
    pos++;
  if (pos >= length || !same_bytes(bytes + 5, "riscv", 5))
    return -1;
  vendor_length = (uint32)(pos - 5);
  (void)vendor_length;
  pos++;
  vendor_end = length;

  if (pos >= vendor_end)
    return -1;
  if (read_uleb(bytes, vendor_end, &pos, &subsection_tag) < 0 ||
      subsection_tag != 1 || !range_ok(vendor_end, pos, 4))
    return -1;
  subsection_length = get32(bytes + pos);
  if (subsection_length < 5 ||
      !range_ok(vendor_end, pos - 1, subsection_length))
    return -1;
  subsection_end = pos - 1 + subsection_length;
  pos += 4;

  while (pos < subsection_end) {
    if (read_uleb(bytes, subsection_end, &pos, &tag) < 0)
      return -1;
    if (tag == 4) { /* Tag_RISCV_stack_align */
      if (read_uleb(bytes, subsection_end, &pos, &value) < 0 || have_stack ||
          value != 16)
        return -1;
      have_stack = 1;
    } else if (tag == 5) { /* Tag_RISCV_arch */
      uint64 start = pos;
      while (pos < subsection_end && bytes[pos] != 0)
        pos++;
      if (pos >= subsection_end || have_arch)
        return -1;
      arch = bytes + start;
      arch_length = pos - start;
      pos++;
      have_arch = 1;
    } else {
      return -1;
    }
  }
  if (pos != subsection_end || subsection_end != vendor_end || !have_stack ||
      !have_arch || !arch_name_compatible(arch, arch_length))
    return -1;
  return 0;
}

static int
read_section_header(const struct elfmod_context *context, uint32 index,
                    struct elfmod_section *section, uint64 *address)
{
  uint64 delta, offset;
  const uchar *p;

  if (index >= context->section_count ||
      mul_u64(index, ELF_SECTION_SIZE, &delta) < 0 ||
      add_u64(context->section_table_offset, delta, &offset) < 0 ||
      !range_ok(context->file_size, offset, ELF_SECTION_SIZE))
    return -1;
  p = context->file + offset;
  section->name_offset = get32(p);
  section->type = get32(p + 4);
  section->flags = get64(p + 8);
  if (address)
    *address = get64(p + 16);
  section->file_offset = get64(p + 24);
  section->size = get64(p + 32);
  section->link = get32(p + 40);
  section->info = get32(p + 44);
  section->alignment = get64(p + 48);
  section->entry_size = get64(p + 56);
  section->staging_offset = 0;
  section->perm_class = 0;
  section->allocated = 0;
  return 0;
}

static int
validate_section_shape(struct elfmod_context *context, uint32 index,
                       enum elfmod_error *error, uint32 *attribute_count)
{
  struct elfmod_section *section = &context->sections[index];
  uint64 address = 0;
  uint64 allowed_flags = 0;
  uint64 entries;
  int allocated;

  if (read_section_header(context, index, section, &address) < 0)
    return set_error(error, ELFMOD_E_BOUNDS);
  if (address != 0)
    return set_error(error, ELFMOD_E_SECTION);
  if (section->alignment > ELFMOD_PAGE_SIZE ||
      (section->alignment > 1 && !is_power_of_two(section->alignment)))
    return set_error(error, ELFMOD_E_ALIGNMENT);
  if (section->type != SHT_NULL && section->alignment == 0)
    return set_error(error, ELFMOD_E_ALIGNMENT);
  if (section->type == SHT_NOBITS) {
    if (section->file_offset > context->file_size)
      return set_error(error, ELFMOD_E_BOUNDS);
  } else if (!range_ok(context->file_size, section->file_offset,
                       section->size)) {
    return set_error(error, ELFMOD_E_BOUNDS);
  }
  if (section->alignment > 1 && section->file_offset % section->alignment != 0)
    return set_error(error, ELFMOD_E_ALIGNMENT);

  allocated = (section->flags & SHF_ALLOC) != 0;
  if (section->flags & (SHF_TLS | SHF_COMPRESSED))
    return set_error(error, ELFMOD_E_UNSUPPORTED_SECTION);

  switch (section->type) {
  case SHT_NULL:
    if (index != 0 || section->name_offset != 0 || section->flags != 0 ||
        section->file_offset != 0 || section->size != 0 || section->link != 0 ||
        section->info != 0 || section->alignment != 0 ||
        section->entry_size != 0)
      return set_error(error, ELFMOD_E_SECTION);
    break;
  case SHT_PROGBITS:
    if (allocated) {
      allowed_flags =
        SHF_WRITE | SHF_ALLOC | SHF_EXECINSTR | SHF_MERGE | SHF_STRINGS;
      if ((section->flags & ~allowed_flags) != 0 ||
          (section->flags & SHF_ALLOC) == 0)
        return set_error(error, ELFMOD_E_UNSUPPORTED_SECTION);
      if ((section->flags & SHF_EXECINSTR) &&
          (section->flags & (SHF_WRITE | SHF_MERGE | SHF_STRINGS)))
        return set_error(error, ELFMOD_E_SECTION);
      if ((section->flags & SHF_WRITE) &&
          (section->flags & (SHF_EXECINSTR | SHF_MERGE | SHF_STRINGS)))
        return set_error(error, ELFMOD_E_SECTION);
      if ((section->flags & SHF_STRINGS) &&
          ((section->flags & SHF_MERGE) == 0 || section->entry_size != 1))
        return set_error(error, ELFMOD_E_SECTION);
      if ((section->flags & SHF_MERGE) &&
          ((section->flags & (SHF_WRITE | SHF_EXECINSTR)) ||
           section->entry_size == 0 ||
           section->size % section->entry_size != 0))
        return set_error(error, ELFMOD_E_SECTION);
      if ((section->flags & SHF_STRINGS) && section->size != 0 &&
          context->file[section->file_offset + section->size - 1] != 0)
        return set_error(error, ELFMOD_E_SECTION);
      section->allocated = 1;
      section->perm_class =
        (section->flags & SHF_EXECINSTR)
          ? ELFMOD_PERM_TEXT
          : ((section->flags & SHF_WRITE) ? ELFMOD_PERM_DATA
                                          : ELFMOD_PERM_READONLY);
    } else {
      if ((section->flags & ~(SHF_MERGE | SHF_STRINGS)) != 0)
        return set_error(error, ELFMOD_E_UNSUPPORTED_SECTION);
      if ((!name_is(context, context->section_name_index, section->name_offset,
                    ".comment") &&
           !name_is(context, context->section_name_index, section->name_offset,
                    ".note.GNU-stack")) ||
          section->link != 0 || section->info != 0 ||
          section->entry_size != ((section->flags & SHF_MERGE) ? 1 : 0) ||
          ((section->flags & SHF_STRINGS) &&
           ((section->flags & SHF_MERGE) == 0 ||
            (section->size != 0 &&
             context->file[section->file_offset + section->size - 1] != 0))))
        return set_error(error, ELFMOD_E_UNSUPPORTED_SECTION);
    }
    if (section->link != 0 || section->info != 0)
      return set_error(error, ELFMOD_E_SECTION);
    break;
  case SHT_NOBITS:
    if (!allocated || section->flags != (SHF_ALLOC | SHF_WRITE) ||
        section->link != 0 || section->info != 0 || section->entry_size != 0)
      return set_error(error, ELFMOD_E_UNSUPPORTED_SECTION);
    section->allocated = 1;
    section->perm_class = ELFMOD_PERM_DATA;
    break;
  case SHT_STRTAB:
    if (section->flags != 0 || section->link != 0 || section->info != 0 ||
        section->entry_size != 0 || section->size == 0 ||
        context->file[section->file_offset] != 0)
      return set_error(error, ELFMOD_E_SECTION);
    break;
  case SHT_SYMTAB:
    if (section->flags != 0 || section->entry_size != ELF_SYMBOL_SIZE ||
        section->size == 0 || section->size % ELF_SYMBOL_SIZE != 0 ||
        section->link >= context->section_count ||
        section->info > section->size / ELF_SYMBOL_SIZE)
      return set_error(error, ELFMOD_E_SECTION);
    if (section->alignment < 8 || section->file_offset % 8 != 0)
      return set_error(error, ELFMOD_E_ALIGNMENT);
    entries = section->size / ELF_SYMBOL_SIZE;
    if (entries > ELFMOD_MAX_SYMBOLS)
      return set_error(error, ELFMOD_E_LIMIT);
    if (context->symbol_table_index != 0)
      return set_error(error, ELFMOD_E_UNSUPPORTED_SECTION);
    context->symbol_table_index = index;
    break;
  case SHT_RELA:
    if (section->flags != SHF_INFO_LINK ||
        section->entry_size != ELF_RELA_SIZE ||
        section->size % ELF_RELA_SIZE != 0 ||
        section->link >= context->section_count ||
        section->info >= context->section_count || section->alignment < 8 ||
        section->file_offset % 8 != 0)
      return set_error(error, ELFMOD_E_SECTION);
    entries = section->size / ELF_RELA_SIZE;
    if (entries > ELFMOD_MAX_RELOCATIONS ||
        context->relocation_count > ELFMOD_MAX_RELOCATIONS - entries)
      return set_error(error, ELFMOD_E_LIMIT);
    context->relocation_count += (uint32)entries;
    break;
  case SHT_RISCV_ATTRIBUTES:
    if (section->flags != 0 || section->link != 0 || section->info != 0 ||
        section->entry_size != 0 || ++*attribute_count != 1)
      return set_error(error, ELFMOD_E_SECTION);
    break;
  default:
    return set_error(error, ELFMOD_E_UNSUPPORTED_SECTION);
  }
  return 0;
}

static int
layout_alloc_sections(struct elfmod_context *context, enum elfmod_error *error)
{
  uint64 cursor = 0;
  uint64 class_start;
  uint64 aligned;
  uint32 perm, i;
  int have_text = 0;

  for (perm = ELFMOD_PERM_TEXT; perm <= ELFMOD_PERM_DATA; perm++) {
    if (perm != ELFMOD_PERM_TEXT) {
      if (align_up(cursor, ELFMOD_PAGE_SIZE, &cursor) < 0)
        return set_error(error, ELFMOD_E_OVERFLOW);
    }
    class_start = cursor;
    for (i = 1; i < context->section_count; i++) {
      struct elfmod_section *section = &context->sections[i];
      if (!section->allocated || section->perm_class != perm)
        continue;
      if (align_up(cursor, section->alignment ? section->alignment : 1,
                   &aligned) < 0 ||
          add_u64(aligned, section->size, &cursor) < 0)
        return set_error(error, ELFMOD_E_OVERFLOW);
      section->staging_offset = aligned;
      if (perm == ELFMOD_PERM_TEXT && section->size != 0)
        have_text = 1;
      if (cursor > ELFMOD_MAX_IMAGE_SIZE)
        return set_error(error, ELFMOD_E_LIMIT);
    }
    if (cursor < class_start)
      return set_error(error, ELFMOD_E_OVERFLOW);
  }
  if (!have_text || cursor == 0)
    return set_error(error, ELFMOD_E_SECTION);
  context->staging_size = cursor;
  return 0;
}

static int
validate_sections(struct elfmod_context *context, enum elfmod_error *error)
{
  uint32 i, attribute_count = 0;
  const struct elfmod_section *names;

  uint64 table_size, table_end;

  if (context->section_count < 4 ||
      context->section_count > ELFMOD_MAX_SECTIONS ||
      context->section_name_index == 0 ||
      context->section_name_index >= context->section_count)
    return set_error(error, ELFMOD_E_LIMIT);
  if (mul_u64(context->section_count, ELF_SECTION_SIZE, &table_size) < 0 ||
      add_u64(context->section_table_offset, table_size, &table_end) < 0 ||
      context->section_table_offset < ELF_HEADER_SIZE)
    return set_error(error, ELFMOD_E_BOUNDS);

  for (i = 0; i < context->section_count; i++)
    if (read_section_header(context, i, &context->sections[i], 0) < 0)
      return set_error(error, ELFMOD_E_BOUNDS);
  for (i = 0; i < context->section_count; i++) {
    struct elfmod_section *section = &context->sections[i];
    uint64 section_end;
    if (validate_section_shape(context, i, error, &attribute_count) < 0)
      return -1;
    if (section->type == SHT_NULL || section->type == SHT_NOBITS ||
        section->size == 0)
      continue;
    if (section->file_offset < ELF_HEADER_SIZE ||
        add_u64(section->file_offset, section->size, &section_end) < 0 ||
        (section->file_offset < table_end &&
         context->section_table_offset < section_end))
      return set_error(error, ELFMOD_E_SECTION);
  }

  names = &context->sections[context->section_name_index];
  if (names->type != SHT_STRTAB)
    return set_error(error, ELFMOD_E_SECTION);
  for (i = 0; i < context->section_count; i++) {
    if (name_at(context, context->section_name_index,
                context->sections[i].name_offset, 0, 0) < 0)
      return set_error(error, ELFMOD_E_SECTION);
  }
  if (names->size == 0 ||
      context->file[names->file_offset + names->size - 1] != 0)
    return set_error(error, ELFMOD_E_SECTION);
  if (context->symbol_table_index == 0 || attribute_count != 1)
    return set_error(error, ELFMOD_E_SECTION);

  {
    struct elfmod_section *symbols =
      &context->sections[context->symbol_table_index];
    if (symbols->link >= context->section_count ||
        context->sections[symbols->link].type != SHT_STRTAB)
      return set_error(error, ELFMOD_E_SECTION);
    context->symbol_string_index = symbols->link;
    context->symbol_count = (uint32)(symbols->size / ELF_SYMBOL_SIZE);
    context->first_global_symbol = symbols->info;
  }
  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *strings = &context->sections[i];
    if (strings->type == SHT_STRTAB &&
        (strings->size == 0 ||
         context->file[strings->file_offset + strings->size - 1] != 0))
      return set_error(error, i == context->symbol_string_index
                                ? ELFMOD_E_SYMBOL
                                : ELFMOD_E_SECTION);
  }

  for (i = 1; i < context->section_count; i++) {
    struct elfmod_section *section = &context->sections[i];
    if (section->type == SHT_RELA) {
      const struct elfmod_section *target;
      if (section->link != context->symbol_table_index)
        return set_error(error, ELFMOD_E_SECTION);
      target = &context->sections[section->info];
      if (!target->allocated ||
          (target->type != SHT_PROGBITS && target->type != SHT_NOBITS))
        return set_error(error, ELFMOD_E_RELOCATION);
    }
    if (section->type == SHT_RISCV_ATTRIBUTES &&
        validate_attributes(context, i) < 0)
      return set_error(error, ELFMOD_E_ATTRIBUTE);
  }
  if (layout_alloc_sections(context, error) < 0)
    return -1;
  return 0;
}

static int validate_symbols(struct elfmod_context *, enum elfmod_error *);
static int validate_relocations(struct elfmod_context *, enum elfmod_error *);

int
elfmod_parse(struct elfmod_context *context, const void *file, uint64 file_size,
             enum elfmod_error *error)
{
  const uchar *bytes = (const uchar *)file;
  uint64 section_table_size;
  uint64 section_table_offset;
  uint16 section_count, section_name_index;
  uint32 flags;

  if (error)
    *error = ELFMOD_OK;
  if (context == 0 || file == 0)
    return set_error(error, ELFMOD_E_ARGUMENT);
  if (file_size > ELFMOD_MAX_FILE_SIZE)
    return set_error(error, ELFMOD_E_LIMIT);
  if (file_size < ELF_HEADER_SIZE)
    return set_error(error, ELFMOD_E_BOUNDS);

  {
    uint i;
    for (i = 0; i < sizeof(*context); i++)
      ((uchar *)context)[i] = 0;
  }
  context->file = bytes;
  context->file_size = file_size;

  if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' ||
      bytes[3] != 'F' || bytes[4] != ELFCLASS64 || bytes[5] != ELFDATA2LSB ||
      bytes[6] != EV_CURRENT || bytes[7] != 0 || bytes[8] != 0)
    return set_error(error, ELFMOD_E_HEADER);
  {
    uint i;
    for (i = 9; i < EI_NIDENT; i++)
      if (bytes[i] != 0)
        return set_error(error, ELFMOD_E_HEADER);
  }
  if (get16(bytes + 16) != ET_REL || get16(bytes + 18) != EM_RISCV ||
      get32(bytes + 20) != EV_CURRENT || get64(bytes + 24) != 0 ||
      get64(bytes + 32) != 0 || get16(bytes + 52) != ELF_HEADER_SIZE ||
      get16(bytes + 54) != 0 || get16(bytes + 56) != 0 ||
      get16(bytes + 58) != ELF_SECTION_SIZE)
    return set_error(error, ELFMOD_E_HEADER);
  flags = get32(bytes + 48);
  if (flags != (EF_RISCV_RVC | EF_RISCV_DOUBLE))
    return set_error(error, ELFMOD_E_HEADER);

  section_table_offset = get64(bytes + 40);
  section_count = get16(bytes + 60);
  section_name_index = get16(bytes + 62);
  if (section_count == 0 || section_count == 0xffff ||
      section_count > ELFMOD_MAX_SECTIONS || section_name_index == 0xffff ||
      section_name_index >= section_count || section_table_offset % 8 != 0 ||
      mul_u64(section_count, ELF_SECTION_SIZE, &section_table_size) < 0 ||
      !range_ok(file_size, section_table_offset, section_table_size))
    return set_error(error, ELFMOD_E_BOUNDS);

  context->section_table_offset = section_table_offset;
  context->section_count = section_count;
  context->section_name_index = section_name_index;
  if (validate_sections(context, error) < 0 ||
      validate_symbols(context, error) < 0 ||
      validate_relocations(context, error) < 0)
    return -1;
  context->cookie = ELFMOD_COOKIE;
  return 0;
}

struct elfmod_symbol_record {
  uint32 name_offset;
  uint8 info;
  uint8 other;
  uint16 section_index;
  uint64 value;
  uint64 size;
};

static int
read_symbol(const struct elfmod_context *context, uint32 index,
            struct elfmod_symbol_record *symbol)
{
  const struct elfmod_section *table;
  uint64 delta, offset;
  const uchar *p;

  if (index >= context->symbol_count)
    return -1;
  table = &context->sections[context->symbol_table_index];
  if (mul_u64(index, ELF_SYMBOL_SIZE, &delta) < 0 ||
      add_u64(table->file_offset, delta, &offset) < 0 ||
      !range_ok(context->file_size, offset, ELF_SYMBOL_SIZE))
    return -1;
  p = context->file + offset;
  symbol->name_offset = get32(p);
  symbol->info = p[4];
  symbol->other = p[5];
  symbol->section_index = get16(p + 6);
  symbol->value = get64(p + 8);
  symbol->size = get64(p + 16);
  return 0;
}

static uint8
symbol_type(const struct elfmod_symbol_record *symbol)
{
  return symbol->info & 0x0f;
}

static uint8
symbol_binding(const struct elfmod_symbol_record *symbol)
{
  return symbol->info >> 4;
}

static int
validate_symbols(struct elfmod_context *context, enum elfmod_error *error)
{
  struct elfmod_symbol_record symbol;
  uint32 i;
  const struct elfmod_section *strings;

  strings = &context->sections[context->symbol_string_index];
  if (context->first_global_symbol == 0 ||
      context->first_global_symbol >= context->symbol_count ||
      strings->size == 0)
    return set_error(error, ELFMOD_E_SYMBOL);

  for (i = 0; i < context->symbol_count; i++) {
    const uchar *name;
    uint64 name_length;
    uint8 type, binding;

    if (read_symbol(context, i, &symbol) < 0 ||
        name_at(context, context->symbol_string_index, symbol.name_offset,
                &name, &name_length) < 0)
      return set_error(error, ELFMOD_E_SYMBOL);
    type = symbol_type(&symbol);
    binding = symbol_binding(&symbol);
    if (symbol.other != 0 || binding == STB_WEAK ||
        (binding != STB_LOCAL && binding != STB_GLOBAL) ||
        (i < context->first_global_symbol && binding != STB_LOCAL) ||
        (i >= context->first_global_symbol && binding == STB_LOCAL))
      return set_error(error, ELFMOD_E_SYMBOL);

    if (i == 0) {
      if (symbol.name_offset != 0 || symbol.info != 0 || symbol.other != 0 ||
          symbol.section_index != SHN_UNDEF || symbol.value != 0 ||
          symbol.size != 0 || name_length != 0)
        return set_error(error, ELFMOD_E_SYMBOL);
      continue;
    }

    if (symbol.section_index == SHN_UNDEF) {
      if (binding != STB_GLOBAL || (type != STT_FUNC && type != STT_NOTYPE) ||
          name_length == 0 || symbol.value != 0 || symbol.size != 0)
        return set_error(error, ELFMOD_E_SYMBOL);
      continue;
    }
    if (symbol.section_index == SHN_ABS) {
      if (type != STT_FILE || binding != STB_LOCAL || name_length == 0 ||
          symbol.value != 0 || symbol.size != 0)
        return set_error(error, ELFMOD_E_SYMBOL);
      continue;
    }
    if (symbol.section_index >= SHN_LORESERVE ||
        symbol.section_index >= context->section_count)
      return set_error(error, ELFMOD_E_SYMBOL);

    {
      const struct elfmod_section *section =
        &context->sections[symbol.section_index];
      if (type == STT_SECTION) {
        if (binding != STB_LOCAL || symbol.name_offset != 0 ||
            symbol.value != 0 || symbol.size != 0)
          return set_error(error, ELFMOD_E_SYMBOL);
      } else if (type == STT_NOTYPE) {
        if (binding != STB_LOCAL || symbol.size != 0 ||
            symbol.value > section->size)
          return set_error(error, ELFMOD_E_SYMBOL);
      } else if (type == STT_FUNC) {
        if (!section->allocated || (section->flags & SHF_EXECINSTR) == 0 ||
            symbol.value > section->size ||
            symbol.size > section->size - symbol.value ||
            (symbol.size != 0 && symbol.value == section->size) ||
            (symbol.value & 1) != 0)
          return set_error(error, ELFMOD_E_SYMBOL);
      } else if (type == STT_OBJECT) {
        if (!section->allocated || (section->flags & SHF_EXECINSTR) != 0 ||
            symbol.value > section->size ||
            symbol.size > section->size - symbol.value)
          return set_error(error, ELFMOD_E_SYMBOL);
      } else {
        return set_error(error, ELFMOD_E_SYMBOL);
      }
    }
  }
  return 0;
}

static int
read_relocation(const struct elfmod_context *context, uint32 section_index,
                uint32 ordinal, struct elfmod_rela *relocation)
{
  const struct elfmod_section *section;
  uint64 delta, offset, count, info;

  if (section_index >= context->section_count)
    return -1;
  section = &context->sections[section_index];
  if (section->type != SHT_RELA)
    return -1;
  count = section->size / ELF_RELA_SIZE;
  if (ordinal >= count || mul_u64(ordinal, ELF_RELA_SIZE, &delta) < 0 ||
      add_u64(section->file_offset, delta, &offset) < 0 ||
      !range_ok(context->file_size, offset, ELF_RELA_SIZE))
    return -1;
  relocation->offset = get64(context->file + offset);
  info = get64(context->file + offset + 8);
  relocation->symbol = (uint32)(info >> 32);
  relocation->type = (uint32)info;
  relocation->addend = get_s64(context->file + offset + 16);
  return 0;
}

static int
read_instruction(const struct elfmod_context *context, uint32 target_index,
                 uint64 offset, const uchar *staging, uint32 *instruction)
{
  const struct elfmod_section *target;
  uint64 file_offset, stage_offset;

  if (target_index >= context->section_count)
    return -1;
  target = &context->sections[target_index];
  if (target->type != SHT_PROGBITS || !(target->flags & SHF_EXECINSTR) ||
      !range_ok(target->size, offset, 4))
    return -1;
  if (staging) {
    if (add_u64(target->staging_offset, offset, &stage_offset) < 0 ||
        !range_ok(context->staging_size, stage_offset, 4))
      return -1;
    *instruction = get32(staging + stage_offset);
  } else {
    if (add_u64(target->file_offset, offset, &file_offset) < 0 ||
        !range_ok(context->file_size, file_offset, 4))
      return -1;
    *instruction = get32(context->file + file_offset);
  }
  return 0;
}

static int
read_halfword(const struct elfmod_context *context, uint32 target_index,
              uint64 offset, const uchar *staging, uint16 *instruction)
{
  const struct elfmod_section *target;
  uint64 file_offset, stage_offset;

  if (target_index >= context->section_count)
    return -1;
  target = &context->sections[target_index];
  if (target->type != SHT_PROGBITS || !(target->flags & SHF_EXECINSTR) ||
      !range_ok(target->size, offset, 2))
    return -1;
  if (staging) {
    if (add_u64(target->staging_offset, offset, &stage_offset) < 0 ||
        !range_ok(context->staging_size, stage_offset, 2))
      return -1;
    *instruction = get16(staging + stage_offset);
  } else {
    if (add_u64(target->file_offset, offset, &file_offset) < 0 ||
        !range_ok(context->file_size, file_offset, 2))
      return -1;
    *instruction = get16(context->file + file_offset);
  }
  return 0;
}

static int
find_hi_for_anchor(const struct elfmod_context *context, uint32 target_index,
                   const struct elfmod_symbol_record *anchor,
                   struct elfmod_rela *result, uint32 *result_section)
{
  struct elfmod_rela candidate;
  uint32 i, j, matches = 0;

  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *relsec = &context->sections[i];
    if (relsec->type != SHT_RELA || relsec->info != target_index)
      continue;
    for (j = 0; j < relsec->size / ELF_RELA_SIZE; j++) {
      if (read_relocation(context, i, j, &candidate) < 0)
        return -1;
      if (candidate.type == R_RISCV_PCREL_HI20 &&
          candidate.offset == anchor->value) {
        if (matches == 0) {
          *result = candidate;
          *result_section = i;
        }
        matches++;
      }
    }
  }
  return matches == 1 ? 0 : -1;
}

static int
count_lows_for_hi(const struct elfmod_context *context, uint32 target_index,
                  const struct elfmod_rela *hi, uint32 *count)
{
  struct elfmod_rela low, found_hi;
  struct elfmod_symbol_record anchor;
  uint32 i, j, total = 0, hi_section;

  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *relsec = &context->sections[i];
    if (relsec->type != SHT_RELA || relsec->info != target_index)
      continue;
    for (j = 0; j < relsec->size / ELF_RELA_SIZE; j++) {
      if (read_relocation(context, i, j, &low) < 0)
        return -1;
      if (low.type != R_RISCV_PCREL_LO12_I && low.type != R_RISCV_PCREL_LO12_S)
        continue;
      if (read_symbol(context, low.symbol, &anchor) < 0 ||
          anchor.section_index != target_index ||
          symbol_type(&anchor) != STT_NOTYPE || anchor.value != hi->offset ||
          low.addend != 0)
        continue;
      if (find_hi_for_anchor(context, target_index, &anchor, &found_hi,
                             &hi_section) == 0 &&
          found_hi.offset == hi->offset && found_hi.symbol == hi->symbol)
        total++;
    }
  }
  *count = total;
  return 0;
}

static int
validate_hi_instruction(const struct elfmod_context *context,
                        uint32 target_index, uint64 offset, uint32 *rd)
{
  uint32 instruction;

  if ((offset & 1) != 0 ||
      read_instruction(context, target_index, offset, 0, &instruction) < 0 ||
      (instruction & 0x7f) != 0x17)
    return -1;
  *rd = (instruction >> 7) & 0x1f;
  return *rd != 0 ? 0 : -1;
}

static int
validate_lo_instruction(const struct elfmod_context *context,
                        uint32 target_index, uint64 offset, uint32 type,
                        uint32 hi_rd)
{
  uint32 instruction, opcode, funct3, rs1;

  if ((offset & 1) != 0 ||
      read_instruction(context, target_index, offset, 0, &instruction) < 0)
    return -1;
  opcode = instruction & 0x7f;
  funct3 = (instruction >> 12) & 7;
  rs1 = (instruction >> 15) & 0x1f;
  if (rs1 != hi_rd)
    return -1;
  if (type == R_RISCV_PCREL_LO12_I) {
    if (opcode == 0x13)
      return funct3 == 0 ? 0 : -1; /* ADDI address materialization */
    if (opcode == 0x03)
      return funct3 >= 1 && funct3 <= 6 ? 0 : -1;
    if (opcode == 0x67)
      return funct3 == 0 ? 0 : -1;
    return -1;
  }
  if (type == R_RISCV_PCREL_LO12_S)
    return opcode == 0x23 && funct3 <= 3 ? 0 : -1;
  return -1;
}

static int
validate_call_instructions(const struct elfmod_context *context,
                           uint32 target_index, uint64 offset)
{
  uint32 auipc, jalr, rd;

  if ((offset & 1) != 0 ||
      read_instruction(context, target_index, offset, 0, &auipc) < 0 ||
      read_instruction(context, target_index, offset + 4, 0, &jalr) < 0 ||
      (auipc & 0x7f) != 0x17)
    return -1;
  rd = (auipc >> 7) & 0x1f;
  if (rd != 1 || (jalr & 0x7f) != 0x67 || ((jalr >> 12) & 7) != 0 ||
      ((jalr >> 15) & 0x1f) != rd || ((jalr >> 7) & 0x1f) != 1)
    return -1;
  return 0;
}

static int
find_hi_for_low(const struct elfmod_context *context, uint32 target_index,
                const struct elfmod_rela *low, struct elfmod_rela *high,
                uint32 *high_section)
{
  struct elfmod_symbol_record anchor;

  if (low->addend != 0 || read_symbol(context, low->symbol, &anchor) < 0 ||
      anchor.section_index != target_index ||
      symbol_type(&anchor) != STT_NOTYPE ||
      symbol_binding(&anchor) != STB_LOCAL)
    return -1;
  return find_hi_for_anchor(context, target_index, &anchor, high, high_section);
}

static int
symbol_is_supported_relocation_target(const struct elfmod_context *context,
                                      uint32 symbol_index, int is_call)
{
  struct elfmod_symbol_record symbol;
  const struct elfmod_section *section;
  uint8 type;

  if (read_symbol(context, symbol_index, &symbol) < 0)
    return -1;
  type = symbol_type(&symbol);
  if (symbol.section_index == SHN_UNDEF)
    return symbol_binding(&symbol) == STB_GLOBAL &&
               (type == STT_FUNC || type == STT_NOTYPE)
             ? 0
             : -1;
  if (symbol.section_index >= context->section_count)
    return -1;
  section = &context->sections[symbol.section_index];
  if (!section->allocated)
    return -1;
  if (is_call)
    return type == STT_FUNC && (section->flags & SHF_EXECINSTR) != 0 ? 0 : -1;
  return type == STT_FUNC || type == STT_OBJECT || type == STT_NOTYPE ||
             type == STT_SECTION
           ? 0
           : -1;
}

static int
count_primary_at(const struct elfmod_context *context, uint32 target_index,
                 uint64 offset, uint32 *primary_count)
{
  struct elfmod_rela relocation;
  uint32 i, j, count = 0;

  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *relsec = &context->sections[i];
    if (relsec->type != SHT_RELA || relsec->info != target_index)
      continue;
    for (j = 0; j < relsec->size / ELF_RELA_SIZE; j++) {
      if (read_relocation(context, i, j, &relocation) < 0)
        return -1;
      if (relocation.offset == offset && relocation.type != R_RISCV_NONE &&
          relocation.type != R_RISCV_RELAX)
        count++;
    }
  }
  *primary_count = count;
  return 0;
}

static int
find_primary_at(const struct elfmod_context *context, uint32 target_index,
                uint64 offset, struct elfmod_rela *primary)
{
  struct elfmod_rela relocation;
  uint32 i, j, count = 0;

  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *relsec = &context->sections[i];
    if (relsec->type != SHT_RELA || relsec->info != target_index)
      continue;
    for (j = 0; j < relsec->size / ELF_RELA_SIZE; j++) {
      if (read_relocation(context, i, j, &relocation) < 0)
        return -1;
      if (relocation.offset == offset && relocation.type != R_RISCV_NONE &&
          relocation.type != R_RISCV_RELAX) {
        *primary = relocation;
        count++;
      }
    }
  }
  return count == 1 ? 0 : -1;
}

static int
count_relax_at(const struct elfmod_context *context, uint32 target_index,
               uint64 offset, uint32 *relax_count)
{
  struct elfmod_rela relocation;
  uint32 i, j, count = 0;

  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *relsec = &context->sections[i];
    if (relsec->type != SHT_RELA || relsec->info != target_index)
      continue;
    for (j = 0; j < relsec->size / ELF_RELA_SIZE; j++) {
      if (read_relocation(context, i, j, &relocation) < 0)
        return -1;
      if (relocation.offset == offset && relocation.type == R_RISCV_RELAX)
        count++;
    }
  }
  *relax_count = count;
  return 0;
}

static int
validate_branch_target_symbol(const struct elfmod_context *context,
                              uint32 target_index,
                              const struct elfmod_rela *relocation)
{
  struct elfmod_symbol_record symbol;
  const struct elfmod_section *target;

  if (target_index >= context->section_count ||
      read_symbol(context, relocation->symbol, &symbol) < 0 ||
      symbol.section_index != target_index ||
      (symbol_type(&symbol) != STT_NOTYPE &&
       symbol_type(&symbol) != STT_FUNC) ||
      (symbol_binding(&symbol) != STB_LOCAL &&
       symbol_binding(&symbol) != STB_GLOBAL) ||
      relocation->addend != 0)
    return -1;
  target = &context->sections[target_index];
  if (!(target->flags & SHF_EXECINSTR) || symbol.value >= target->size)
    return -1;
  return 0;
}

static int
validate_branch_instruction(const struct elfmod_context *context,
                            uint32 target_index, uint64 offset, uint32 type)
{
  uint32 instruction, funct3;
  uint16 compressed;

  if (type == R_RISCV_BRANCH) {
    if (read_instruction(context, target_index, offset, 0, &instruction) < 0 ||
        (instruction & 0x7f) != 0x63)
      return -1;
    funct3 = (instruction >> 12) & 7;
    return (funct3 == 0 || funct3 == 1 || (funct3 >= 4 && funct3 <= 7)) ? 0
                                                                        : -1;
  }
  if (read_halfword(context, target_index, offset, 0, &compressed) < 0 ||
      (compressed & 3) != 1)
    return -1;
  funct3 = compressed >> 13;
  if (type == R_RISCV_RVC_BRANCH)
    return (funct3 == 6 || funct3 == 7) ? 0 : -1;
  if (type == R_RISCV_RVC_JUMP)
    return funct3 == 5 ? 0 : -1; /* RV64 C.J */
  return -1;
}

static int
validate_relocations(struct elfmod_context *context, enum elfmod_error *error)
{
  struct elfmod_rela relocation, high;
  struct elfmod_symbol_record anchor;
  uint32 i, j, target_index;

  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *relsec = &context->sections[i];
    const struct elfmod_section *target;

    if (relsec->type != SHT_RELA)
      continue;
    target_index = relsec->info;
    target = &context->sections[target_index];
    for (j = 0; j < relsec->size / ELF_RELA_SIZE; j++) {
      uint64 width;
      uint32 rd, high_section, primary_count, relax_count;
      struct elfmod_rela paired_primary;

      if (read_relocation(context, i, j, &relocation) < 0 ||
          relocation.symbol >= context->symbol_count)
        return set_error(error, ELFMOD_E_RELOCATION);
      switch (relocation.type) {
      case R_RISCV_NONE:
      case R_RISCV_RELAX:
        width = 0;
        break;
      case R_RISCV_BRANCH:
      case R_RISCV_PCREL_HI20:
      case R_RISCV_PCREL_LO12_I:
      case R_RISCV_PCREL_LO12_S:
        width = 4;
        break;
      case R_RISCV_RVC_BRANCH:
      case R_RISCV_RVC_JUMP:
        width = 2;
        break;
      case R_RISCV_CALL_PLT:
        width = 8;
        break;
      default:
        return set_error(error, ELFMOD_E_UNSUPPORTED_RELOCATION);
      }
      if (relocation.offset > target->size ||
          width > target->size - relocation.offset)
        return set_error(error, ELFMOD_E_BOUNDS);
      if (relocation.type == R_RISCV_NONE)
        continue;
      if (relocation.type == R_RISCV_RELAX) {
        if (count_primary_at(context, target_index, relocation.offset,
                             &primary_count) < 0 ||
            primary_count != 1 ||
            find_primary_at(context, target_index, relocation.offset,
                            &paired_primary) < 0 ||
            count_relax_at(context, target_index, relocation.offset,
                           &relax_count) < 0 ||
            relax_count != 1 ||
            !((relocation.symbol == 0 && relocation.addend == 0) ||
              (relocation.symbol == paired_primary.symbol &&
               relocation.addend == paired_primary.addend)))
          return set_error(error, ELFMOD_E_RELOCATION);
        continue;
      }
      if (target->type != SHT_PROGBITS || !(target->flags & SHF_ALLOC) ||
          !(target->flags & SHF_EXECINSTR))
        return set_error(error, ELFMOD_E_RELOCATION);
      if ((relocation.offset & 1) != 0)
        return set_error(error, ELFMOD_E_ALIGNMENT);
      if (count_primary_at(context, target_index, relocation.offset,
                           &primary_count) < 0 ||
          primary_count != 1)
        return set_error(error, ELFMOD_E_RELOCATION);

      if (relocation.type == R_RISCV_PCREL_HI20) {
        uint32 low_count;
        if (symbol_is_supported_relocation_target(context, relocation.symbol,
                                                  0) < 0 ||
            validate_hi_instruction(context, target_index, relocation.offset,
                                    &rd) < 0 ||
            count_lows_for_hi(context, target_index, &relocation, &low_count) <
              0 ||
            low_count == 0)
          return set_error(error, ELFMOD_E_PAIR);
      } else if (relocation.type == R_RISCV_PCREL_LO12_I ||
                 relocation.type == R_RISCV_PCREL_LO12_S) {
        if (find_hi_for_low(context, target_index, &relocation, &high,
                            &high_section) < 0 ||
            read_symbol(context, relocation.symbol, &anchor) < 0 ||
            symbol_is_supported_relocation_target(context, high.symbol, 0) <
              0 ||
            validate_hi_instruction(context, target_index, high.offset, &rd) <
              0 ||
            validate_lo_instruction(context, target_index, relocation.offset,
                                    relocation.type, rd) < 0)
          return set_error(error, ELFMOD_E_PAIR);
      } else if (relocation.type == R_RISCV_CALL_PLT) {
        if (symbol_is_supported_relocation_target(context, relocation.symbol,
                                                  1) < 0 ||
            validate_call_instructions(context, target_index,
                                       relocation.offset) < 0)
          return set_error(error, ELFMOD_E_INSTRUCTION);
      } else if (relocation.type == R_RISCV_BRANCH ||
                 relocation.type == R_RISCV_RVC_BRANCH ||
                 relocation.type == R_RISCV_RVC_JUMP) {
        if (validate_branch_target_symbol(context, target_index, &relocation) <
            0)
          return set_error(error, ELFMOD_E_PAIR);
        if (validate_branch_instruction(context, target_index,
                                        relocation.offset, relocation.type) < 0)
          return set_error(error, ELFMOD_E_INSTRUCTION);
      }
    }
  }
  return 0;
}

static int
add_signed(uint64 base, int64 addend, uint64 *result)
{
  uint64 magnitude;

  if (addend >= 0) {
    if (add_u64(base, (uint64)addend, result) < 0)
      return -1;
    return 0;
  }
  magnitude = (uint64)(-(addend + 1)) + 1;
  if (base < magnitude)
    return -1;
  *result = base - magnitude;
  return 0;
}

static int
signed_difference(uint64 target, uint64 place, int64 *difference)
{
  uint64 magnitude;

  if (target >= place) {
    magnitude = target - place;
    if (magnitude > INT64_MAX_U)
      return -1;
    *difference = (int64)magnitude;
    return 0;
  }
  magnitude = place - target;
  if (magnitude > INT64_MIN_MAG)
    return -1;
  if (magnitude == INT64_MIN_MAG)
    *difference = (int64)(-INT64_MAX_U - 1);
  else
    *difference = -(int64)magnitude;
  return 0;
}

static int64
floor_div_4096(int64 value)
{
  uint64 magnitude, quotient;

  if (value >= 0)
    return value / 4096;
  magnitude = (uint64)(-(value + 1)) + 1;
  quotient = (magnitude + 4095) / 4096;
  return -(int64)quotient;
}

static int
split_pcrel(int64 difference, int64 *high, int64 *low)
{
  int64 rounded;

  if (difference < -2147483648L || difference > 2147483647L)
    return -1;
  if (difference > (int64)INT64_MAX_U - 0x800)
    return -1;
  rounded = difference + 0x800;
  *high = floor_div_4096(rounded);
  if (*high < -524288 || *high > 524287)
    return -1;
  *low = difference - (*high * 4096);
  if (*low < -2048 || *low > 2047)
    return -1;
  return 0;
}

static int
resolve_symbol_value(const struct elfmod_context *context, uint32 index,
                     const uint64 section_addresses[ELFMOD_MAX_SECTIONS],
                     elfmod_import_resolver resolver, void *opaque,
                     uint64 *value)
{
  struct elfmod_symbol_record symbol;
  uint64 base;
  const uchar *name;

  if (read_symbol(context, index, &symbol) < 0)
    return -1;
  if (symbol.section_index == SHN_UNDEF) {
    if (resolver == 0 ||
        name_at(context, context->symbol_string_index, symbol.name_offset,
                &name, 0) < 0 ||
        resolver(opaque, (const char *)name, symbol_type(&symbol), value) !=
          1 ||
        *value == 0)
      return -1;
    return 0;
  }
  if (symbol.section_index >= context->section_count ||
      !context->sections[symbol.section_index].allocated)
    return -1;
  base = section_addresses[symbol.section_index];
  if (base == 0 || add_u64(base, symbol.value, value) < 0)
    return -1;
  return 0;
}

static int
relocation_target_in_section(
  const struct elfmod_context *context, uint32 symbol_index, uint64 target,
  const uint64 section_addresses[ELFMOD_MAX_SECTIONS], int is_call)
{
  struct elfmod_symbol_record symbol;
  const struct elfmod_section *section;
  uint64 base, end;

  if (read_symbol(context, symbol_index, &symbol) < 0)
    return -1;
  if (symbol.section_index == SHN_UNDEF)
    return 0;
  if (symbol.section_index >= context->section_count)
    return -1;
  section = &context->sections[symbol.section_index];
  if (!section->allocated || section->size == 0 ||
      (is_call && !(section->flags & SHF_EXECINSTR)))
    return -1;
  base = section_addresses[symbol.section_index];
  if (add_u64(base, section->size, &end) < 0 || target < base || target > end ||
      (is_call && target == end))
    return -1;
  return 0;
}

static int
get_hi_target_difference(const struct elfmod_context *context,
                         uint32 target_index, const struct elfmod_rela *high,
                         const uint64 section_addresses[ELFMOD_MAX_SECTIONS],
                         elfmod_import_resolver resolver, void *opaque,
                         int64 *difference)
{
  uint64 symbol_value, place, target;

  if (target_index >= context->section_count ||
      add_u64(section_addresses[target_index], high->offset, &place) < 0 ||
      resolve_symbol_value(context, high->symbol, section_addresses, resolver,
                           opaque, &symbol_value) < 0 ||
      add_signed(symbol_value, high->addend, &target) < 0 ||
      relocation_target_in_section(context, high->symbol, target,
                                   section_addresses, 0) < 0 ||
      signed_difference(target, place, difference) < 0)
    return -1;
  return 0;
}

static uint32
patch_u_imm(uint32 instruction, int64 high)
{
  uint32 immediate = (uint32)high & 0xfffff;
  return (instruction & 0x00000fff) | (immediate << 12);
}

static uint32
patch_i_imm(uint32 instruction, int64 low)
{
  uint32 immediate = (uint32)low & 0xfff;
  return (instruction & 0x000fffff) | (immediate << 20);
}

static uint32
patch_s_imm(uint32 instruction, int64 low)
{
  uint32 immediate = (uint32)low & 0xfff;
  uint32 high = (immediate >> 5) & 0x7f;
  uint32 low_bits = immediate & 0x1f;
  return (instruction & ~((uint32)0xfe000f80)) | (high << 25) | (low_bits << 7);
}

static int
branch_target_difference(const struct elfmod_context *context,
                         uint32 target_index,
                         const struct elfmod_rela *relocation,
                         const uint64 section_addresses[ELFMOD_MAX_SECTIONS],
                         int64 *difference)
{
  struct elfmod_symbol_record symbol;
  uint64 target, place;

  if (read_symbol(context, relocation->symbol, &symbol) < 0 ||
      symbol.section_index != target_index ||
      (symbol_type(&symbol) != STT_NOTYPE &&
       symbol_type(&symbol) != STT_FUNC) ||
      relocation->addend != 0 ||
      resolve_symbol_value(context, relocation->symbol, section_addresses, 0, 0,
                           &target) < 0 ||
      relocation_target_in_section(context, relocation->symbol, target,
                                   section_addresses, 0) < 0 ||
      add_u64(section_addresses[target_index], relocation->offset, &place) <
        0 ||
      signed_difference(target, place, difference) < 0)
    return -1;
  return 0;
}

static uint32
patch_b_imm(uint32 instruction, int64 difference)
{
  uint32 immediate = (uint32)difference;
  instruction &= ~((uint32)0xfe000f80);
  instruction |= ((immediate >> 12) & 1) << 31;
  instruction |= ((immediate >> 5) & 0x3f) << 25;
  instruction |= ((immediate >> 1) & 0xf) << 8;
  instruction |= ((immediate >> 11) & 1) << 7;
  return instruction;
}

static uint16
patch_cb_imm(uint16 instruction, int64 difference)
{
  uint16 immediate = (uint16)difference;
  instruction &= (uint16)~0x1c7c;
  instruction |= ((immediate >> 8) & 1) << 12;
  instruction |= ((immediate >> 3) & 3) << 10;
  instruction |= ((immediate >> 6) & 3) << 5;
  instruction |= ((immediate >> 1) & 3) << 3;
  instruction |= ((immediate >> 5) & 1) << 2;
  return instruction;
}

static uint16
patch_cj_imm(uint16 instruction, int64 difference)
{
  uint16 immediate = (uint16)difference;
  instruction &= (uint16)~0x1ffc;
  instruction |= ((immediate >> 11) & 1) << 12;
  instruction |= ((immediate >> 4) & 1) << 11;
  instruction |= ((immediate >> 8) & 3) << 9;
  instruction |= ((immediate >> 10) & 1) << 8;
  instruction |= ((immediate >> 6) & 1) << 7;
  instruction |= ((immediate >> 7) & 1) << 6;
  instruction |= ((immediate >> 1) & 7) << 3;
  instruction |= ((immediate >> 5) & 1) << 2;
  return instruction;
}

static int
write_halfword(const struct elfmod_context *context, uint32 target_index,
               uint64 offset, uchar *staging, uint16 instruction)
{
  uint64 stage_offset;

  if (staging == 0 || target_index >= context->section_count ||
      add_u64(context->sections[target_index].staging_offset, offset,
              &stage_offset) < 0 ||
      !range_ok(context->staging_size, stage_offset, 2))
    return -1;
  put16(staging + stage_offset, instruction);
  return 0;
}

static int
write_instruction(const struct elfmod_context *context, uint32 target_index,
                  uint64 offset, uchar *staging, uint32 instruction)
{
  uint64 stage_offset;

  if (staging == 0 || target_index >= context->section_count ||
      add_u64(context->sections[target_index].staging_offset, offset,
              &stage_offset) < 0 ||
      !range_ok(context->staging_size, stage_offset, 4))
    return -1;
  put32(staging + stage_offset, instruction);
  return 0;
}

static int
apply_relocation(const struct elfmod_context *context,
                 uint32 relocation_section,
                 const struct elfmod_rela *relocation, uchar *staging,
                 const uint64 section_addresses[ELFMOD_MAX_SECTIONS],
                 elfmod_import_resolver resolver, void *opaque)
{
  const struct elfmod_section *relsec = &context->sections[relocation_section];
  uint32 target_index = relsec->info;
  uint32 instruction, second, hi_section;
  struct elfmod_rela high;
  int64 difference, high_part, low_part;
  uint64 symbol_value, place, target;

  if (relocation->type == R_RISCV_NONE || relocation->type == R_RISCV_RELAX)
    return 0;

  if (relocation->type == R_RISCV_PCREL_HI20) {
    if (read_instruction(context, target_index, relocation->offset, staging,
                         &instruction) < 0 ||
        get_hi_target_difference(context, target_index, relocation,
                                 section_addresses, resolver, opaque,
                                 &difference) < 0 ||
        split_pcrel(difference, &high_part, &low_part) < 0)
      return -1;
    (void)low_part;
    if (staging &&
        write_instruction(context, target_index, relocation->offset, staging,
                          patch_u_imm(instruction, high_part)) < 0)
      return -1;
    return 0;
  }

  if (relocation->type == R_RISCV_PCREL_LO12_I ||
      relocation->type == R_RISCV_PCREL_LO12_S) {
    if (find_hi_for_low(context, target_index, relocation, &high, &hi_section) <
          0 ||
        read_instruction(context, target_index, relocation->offset, staging,
                         &instruction) < 0 ||
        read_instruction(context, target_index, high.offset, staging, &second) <
          0 ||
        get_hi_target_difference(context, target_index, &high,
                                 section_addresses, resolver, opaque,
                                 &difference) < 0 ||
        split_pcrel(difference, &high_part, &low_part) < 0)
      return -1;
    (void)second;
    (void)hi_section;
    (void)high_part;
    if (staging) {
      if (relocation->type == R_RISCV_PCREL_LO12_I) {
        if (write_instruction(context, target_index, relocation->offset,
                              staging, patch_i_imm(instruction, low_part)) < 0)
          return -1;
      } else if (write_instruction(context, target_index, relocation->offset,
                                   staging,
                                   patch_s_imm(instruction, low_part)) < 0) {
        return -1;
      }
    }
    return 0;
  }

  if (relocation->type == R_RISCV_BRANCH ||
      relocation->type == R_RISCV_RVC_BRANCH ||
      relocation->type == R_RISCV_RVC_JUMP) {
    int64 minimum, maximum;
    if (branch_target_difference(context, target_index, relocation,
                                 section_addresses, &difference) < 0 ||
        (difference & 1) != 0)
      return -1;
    if (relocation->type == R_RISCV_BRANCH) {
      minimum = -4096;
      maximum = 4094;
      if (minimum > difference || difference > maximum ||
          read_instruction(context, target_index, relocation->offset, staging,
                           &instruction) < 0)
        return -1;
      if (staging &&
          write_instruction(context, target_index, relocation->offset, staging,
                            patch_b_imm(instruction, difference)) < 0)
        return -1;
    } else {
      uint16 compressed;
      if (relocation->type == R_RISCV_RVC_BRANCH) {
        minimum = -256;
        maximum = 254;
      } else {
        minimum = -2048;
        maximum = 2046;
      }
      if (minimum > difference || difference > maximum ||
          read_halfword(context, target_index, relocation->offset, staging,
                        &compressed) < 0)
        return -1;
      if (staging) {
        compressed = relocation->type == R_RISCV_RVC_BRANCH
                       ? patch_cb_imm(compressed, difference)
                       : patch_cj_imm(compressed, difference);
        if (write_halfword(context, target_index, relocation->offset, staging,
                           compressed) < 0)
          return -1;
      }
    }
    return 0;
  }

  if (relocation->type == R_RISCV_CALL_PLT) {
    if (read_instruction(context, target_index, relocation->offset, staging,
                         &instruction) < 0 ||
        read_instruction(context, target_index, relocation->offset + 4, staging,
                         &second) < 0 ||
        add_u64(section_addresses[target_index], relocation->offset, &place) <
          0 ||
        resolve_symbol_value(context, relocation->symbol, section_addresses,
                             resolver, opaque, &symbol_value) < 0 ||
        add_signed(symbol_value, relocation->addend, &target) < 0 ||
        relocation_target_in_section(context, relocation->symbol, target,
                                     section_addresses, 1) < 0 ||
        signed_difference(target, place, &difference) < 0 ||
        split_pcrel(difference, &high_part, &low_part) < 0 || (target & 1) != 0)
      return -1;
    if (staging) {
      if (write_instruction(context, target_index, relocation->offset, staging,
                            patch_u_imm(instruction, high_part)) < 0 ||
          write_instruction(context, target_index, relocation->offset + 4,
                            staging, patch_i_imm(second, low_part)) < 0)
        return -1;
    }
    return 0;
  }
  return -1;
}

static int
validate_section_addresses(const struct elfmod_context *context,
                           const uint64 section_addresses[ELFMOD_MAX_SECTIONS])
{
  uint32 i, j;

  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *section = &context->sections[i];
    uint64 address, end, page_start, page_end;

    if (!section->allocated || section->size == 0)
      continue;
    address = section_addresses[i];
    if (address == 0 ||
        address % (section->alignment ? section->alignment : 1) != 0 ||
        add_u64(address, section->size, &end) < 0)
      return -1;
    if (align_up(end, ELFMOD_PAGE_SIZE, &page_end) < 0)
      return -1;
    page_start = address & ~(ELFMOD_PAGE_SIZE - 1);
    for (j = 1; j < i; j++) {
      const struct elfmod_section *other = &context->sections[j];
      uint64 other_address, other_end, other_page_end, other_page_start;

      if (!other->allocated || other->size == 0)
        continue;
      other_address = section_addresses[j];
      if (add_u64(other_address, other->size, &other_end) < 0 ||
          align_up(other_end, ELFMOD_PAGE_SIZE, &other_page_end) < 0)
        return -1;
      other_page_start = other_address & ~(ELFMOD_PAGE_SIZE - 1);
      if (address < other_end && other_address < end)
        return -1;
      if (section->perm_class != other->perm_class &&
          page_start < other_page_end && other_page_start < page_end)
        return -1;
    }
  }
  return 0;
}

static void
zero_bytes(uchar *destination, uint64 length)
{
  uint64 i;
  for (i = 0; i < length; i++)
    destination[i] = 0;
}

static int
copy_allocated_sections(const struct elfmod_context *context, uchar *staging)
{
  uint32 i;

  zero_bytes(staging, context->staging_size);
  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *section = &context->sections[i];
    uint64 j;

    if (!section->allocated || section->type == SHT_NOBITS)
      continue;
    if (!range_ok(context->staging_size, section->staging_offset,
                  section->size) ||
        !range_ok(context->file_size, section->file_offset, section->size))
      return -1;
    for (j = 0; j < section->size; j++)
      staging[section->staging_offset + j] =
        context->file[section->file_offset + j];
  }
  return 0;
}

static int
validate_imports(const struct elfmod_context *context,
                 elfmod_import_resolver resolver, void *opaque)
{
  struct elfmod_symbol_record symbol;
  const uchar *name;
  uint64 address;
  uint32 i;

  for (i = 1; i < context->symbol_count; i++) {
    if (read_symbol(context, i, &symbol) < 0)
      return -1;
    if (symbol.section_index != SHN_UNDEF)
      continue;
    if (resolver == 0 ||
        name_at(context, context->symbol_string_index, symbol.name_offset,
                &name, 0) < 0 ||
        resolver(opaque, (const char *)name, symbol_type(&symbol), &address) !=
          1 ||
        address == 0)
      return -1;
  }
  return 0;
}

int
elfmod_relocate(const struct elfmod_context *context, uchar *staging,
                uint64 staging_capacity,
                const uint64 section_addresses[ELFMOD_MAX_SECTIONS],
                elfmod_import_resolver resolver, void *resolver_opaque,
                enum elfmod_error *error)
{
  struct elfmod_rela relocation;
  uint32 i, j;

  if (error)
    *error = ELFMOD_OK;
  if (context == 0 || context->cookie != ELFMOD_COOKIE || context->file == 0 ||
      staging == 0 || section_addresses == 0)
    return set_error(error, ELFMOD_E_ARGUMENT);
  uint64 file_begin, file_end, stage_begin, stage_end;

  if (context->staging_size > ELFMOD_MAX_IMAGE_SIZE ||
      staging_capacity < context->staging_size)
    return set_error(error, ELFMOD_E_LIMIT);
  file_begin = (uint64)context->file;
  stage_begin = (uint64)staging;
  if (add_u64(file_begin, context->file_size, &file_end) < 0 ||
      add_u64(stage_begin, context->staging_size, &stage_end) < 0 ||
      (file_begin < stage_end && stage_begin < file_end))
    return set_error(error, ELFMOD_E_ARGUMENT);
  if (validate_section_addresses(context, section_addresses) < 0)
    return set_error(error, ELFMOD_E_ALIGNMENT);
  if (validate_imports(context, resolver, resolver_opaque) < 0)
    return set_error(error, ELFMOD_E_IMPORT);

  /* Preflight every relocation against the original bytes before staging. */
  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *relsec = &context->sections[i];
    if (relsec->type != SHT_RELA)
      continue;
    for (j = 0; j < relsec->size / ELF_RELA_SIZE; j++) {
      if (read_relocation(context, i, j, &relocation) < 0 ||
          apply_relocation(context, i, &relocation, 0, section_addresses,
                           resolver, resolver_opaque) < 0)
        return set_error(error, ELFMOD_E_RANGE);
    }
  }

  if (copy_allocated_sections(context, staging) < 0) {
    zero_bytes(staging, context->staging_size);
    return set_error(error, ELFMOD_E_BOUNDS);
  }
  for (i = 1; i < context->section_count; i++) {
    const struct elfmod_section *relsec = &context->sections[i];
    if (relsec->type != SHT_RELA)
      continue;
    for (j = 0; j < relsec->size / ELF_RELA_SIZE; j++) {
      if (read_relocation(context, i, j, &relocation) < 0 ||
          apply_relocation(context, i, &relocation, staging, section_addresses,
                           resolver, resolver_opaque) < 0) {
        zero_bytes(staging, context->staging_size);
        return set_error(error, ELFMOD_E_RANGE);
      }
    }
  }
  return 0;
}

int
elfmod_find_export(const struct elfmod_context *context, const char *name,
                   struct elfmod_export *result, enum elfmod_error *error)
{
  struct elfmod_symbol_record symbol;
  uint32 i;
  int found = 0;

  if (error)
    *error = ELFMOD_OK;
  if (context == 0 || context->cookie != ELFMOD_COOKIE || name == 0 ||
      result == 0)
    return set_error(error, ELFMOD_E_ARGUMENT);
  for (i = 1; i < context->symbol_count; i++) {
    const uchar *candidate;
    uint64 length, wanted = 0;

    if (read_symbol(context, i, &symbol) < 0 ||
        symbol.section_index == SHN_UNDEF ||
        symbol_binding(&symbol) != STB_GLOBAL ||
        symbol_type(&symbol) != STT_FUNC ||
        symbol.section_index >= context->section_count ||
        !(context->sections[symbol.section_index].flags & SHF_EXECINSTR) ||
        symbol.size == 0 ||
        symbol.value >= context->sections[symbol.section_index].size)
      continue;
    if (name_at(context, context->symbol_string_index, symbol.name_offset,
                &candidate, &length) < 0)
      return set_error(error, ELFMOD_E_SYMBOL);
    while (name[wanted])
      wanted++;
    if (length != wanted || !same_bytes(candidate, name, length))
      continue;
    if (found)
      return set_error(error, ELFMOD_E_EXPORT);
    result->section_index = symbol.section_index;
    result->section_offset = symbol.value;
    result->size = symbol.size;
    found = 1;
  }
  if (!found)
    return set_error(error, ELFMOD_E_EXPORT);
  return 0;
}

const char *
elfmod_error_string(enum elfmod_error error)
{
  switch (error) {
  case ELFMOD_OK:
    return "ok";
  case ELFMOD_E_ARGUMENT:
    return "invalid argument or parser context";
  case ELFMOD_E_LIMIT:
    return "configured input, section, symbol, or image limit exceeded";
  case ELFMOD_E_HEADER:
    return "invalid or unsupported ELF header";
  case ELFMOD_E_BOUNDS:
    return "file offset or size is out of bounds";
  case ELFMOD_E_SECTION:
    return "malformed section table or section";
  case ELFMOD_E_UNSUPPORTED_SECTION:
    return "unsupported ELF section type or flags";
  case ELFMOD_E_ATTRIBUTE:
    return "unsupported or malformed RISC-V attributes";
  case ELFMOD_E_SYMBOL:
    return "malformed or unsupported symbol";
  case ELFMOD_E_IMPORT:
    return "undefined import is not in the explicit allowlist";
  case ELFMOD_E_RELOCATION:
    return "malformed relocation record or target";
  case ELFMOD_E_UNSUPPORTED_RELOCATION:
    return "unsupported RISC-V relocation type";
  case ELFMOD_E_PAIR:
    return "malformed or unmatched PC-relative relocation pair";
  case ELFMOD_E_INSTRUCTION:
    return "unexpected instruction encoding for relocation";
  case ELFMOD_E_ALIGNMENT:
    return "invalid section, instruction, or placement alignment";
  case ELFMOD_E_OVERFLOW:
    return "integer overflow in section layout";
  case ELFMOD_E_RANGE:
    return "relocation target is out of range";
  case ELFMOD_E_EXPORT:
    return "required module export is missing or ambiguous";
  }
  return "unknown ELF module error";
}
