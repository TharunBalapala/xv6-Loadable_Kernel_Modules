#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "file.h"
#include "stat.h"
#include "elfmod.h"
#include "module.h"
#include "defs.h"

#define MODULE_SLOTS 4
#define MODULE_FILE_MAX (64 * 1024)
#define MODULE_ARENA_PAGES 2048
#define MODULE_ARENA_BASE (PHYSTOP + PGSIZE)
#define MODULE_IMAGE_PAGES (ELFMOD_MAX_IMAGE_SIZE / PGSIZE)

enum module_state {
  MODULE_FREE = 0, MODULE_LOADING, MODULE_LIVE, MODULE_UNLOADING,
  MODULE_QUARANTINED
};

struct module_slot {
  enum module_state state;
  char name[MODULE_NAME_MAX + 1];
  uint64 arena_base;
  uint32 arena_pages;
  uint64 page_pa[MODULE_IMAGE_PAGES];
  uchar page_perm[MODULE_IMAGE_PAGES];
  uint32 active_calls;
  uint32 dependent_refs;
  uint8 arena_reserved;
  uint8 aliases_removed;
  uint8 mappings_ready;
  module_command_fn command;
  module_exit_fn exit;
};

static struct module_slot modules[MODULE_SLOTS];
static struct spinlock module_lock;
static struct sleeplock module_operation_lock;
static volatile uint module_online_harts;
static uchar module_arena_used[MODULE_ARENA_PAGES];
static uchar module_file[MODULE_FILE_MAX];
static uchar module_staging[ELFMOD_MAX_IMAGE_SIZE];
extern pagetable_t kernel_pagetable;

static int
module_print(const char *message)
{
  if (message == 0)
    return -1;
  return printk("%s\n", (char *)message);
}

static const struct module_api_v1 api_v1 = {
  MODULE_API_V1, sizeof(struct module_api_v1), MODULE_CAP_PRINT, module_print
};

void
module_hart_online(int hartid)
{
  if (hartid < 0 || hartid >= NCPU)
    panic("module: bad hart id");
  __atomic_add_fetch(&module_online_harts, 1, __ATOMIC_RELEASE);
}

void
module_system_init(void)
{
  initlock(&module_lock, "modules");
  initsleeplock(&module_operation_lock, "module operation");
}

static int
module_single_hart_allowed(void)
{
  return cpuid() == 0 &&
         __atomic_load_n(&module_online_harts, __ATOMIC_ACQUIRE) == 1;
}

static int
module_name_equal(const char *left, const char *right)
{
  uint i = 0;
  while (left[i] != 0 && right[i] != 0) {
    if (left[i] != right[i])
      return 0;
    i++;
  }
  return left[i] == 0 && right[i] == 0;
}

static int
module_name_from_path(const char *path, char name[MODULE_NAME_MAX + 1])
{
  const char *base = path;
  uint length = 0, i;

  if (path == 0 || path[0] == 0)
    return -1;
  for (i = 0; path[i] != 0; i++)
    if (path[i] == '/')
      base = path + i + 1;
  while (base[length] != 0)
    length++;
  if (length >= 2 && base[length - 2] == '.' && base[length - 1] == 'o')
    length -= 2;
  if (length == 0 || length > MODULE_NAME_MAX)
    return -1;
  for (i = 0; i < length; i++) {
    char c = base[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return -1;
    name[i] = c;
  }
  name[length] = 0;
  return 0;
}

static struct module_slot *
module_find_locked(const char *name)
{
  uint i;
  for (i = 0; i < MODULE_SLOTS; i++)
    if (modules[i].state != MODULE_FREE &&
        module_name_equal(modules[i].name, name))
      return &modules[i];
  return 0;
}

static struct module_slot *
module_reserve(const char *name)
{
  struct module_slot *slot = 0;
  uint i;

  acquire(&module_lock);
  if (module_find_locked(name) != 0) {
    release(&module_lock);
    return 0;
  }
  for (i = 0; i < MODULE_SLOTS; i++)
    if (modules[i].state == MODULE_FREE) {
      slot = &modules[i];
      break;
    }
  if (slot) {
    memset(slot, 0, sizeof(*slot));
    safestrcpy(slot->name, (char *)name, sizeof(slot->name));
    slot->state = MODULE_LOADING;
  }
  release(&module_lock);
  return slot;
}

static void
module_forget(struct module_slot *slot)
{
  acquire(&module_lock);
  memset(slot, 0, sizeof(*slot));
  release(&module_lock);
}

static int
module_arena_reserve(struct module_slot *slot, uint32 pages)
{
  uint32 first, i;
  int free_run;

  if (pages == 0 || pages > MODULE_IMAGE_PAGES ||
      pages > MODULE_ARENA_PAGES)
    return -1;
  for (first = 0; first + pages <= MODULE_ARENA_PAGES; first++) {
    free_run = 1;
    for (i = 0; i < pages; i++)
      if (module_arena_used[first + i]) {
        free_run = 0;
        first += i;
        break;
      }
    if (!free_run)
      continue;
    for (i = 0; i < pages; i++)
      module_arena_used[first + i] = 1;
    slot->arena_base = MODULE_ARENA_BASE + (uint64)first * PGSIZE;
    slot->arena_pages = pages;
    slot->arena_reserved = 1;
    return 0;
  }
  return -1;
}

static void
module_arena_release(struct module_slot *slot)
{
  uint32 first, i;
  if (!slot->arena_reserved)
    return;
  first = (uint32)((slot->arena_base - MODULE_ARENA_BASE) / PGSIZE);
  if (first + slot->arena_pages > MODULE_ARENA_PAGES)
    panic("module: bad arena");
  for (i = 0; i < slot->arena_pages; i++)
    module_arena_used[first + i] = 0;
  slot->arena_reserved = 0;
  slot->arena_base = 0;
  slot->arena_pages = 0;
}

static int
module_section_addresses(const struct elfmod_context *ctx,
                         struct module_slot *slot,
                         uint64 addresses[ELFMOD_MAX_SECTIONS])
{
  uint32 i;
  int has_text = 0;

  memset(addresses, 0, sizeof(uint64) * ELFMOD_MAX_SECTIONS);
  memset(slot->page_perm, 0, sizeof(slot->page_perm));
  for (i = 1; i < ctx->section_count; i++) {
    const struct elfmod_section *s = &ctx->sections[i];
    uint64 end;
    uint32 first, last, p;
    uchar permission;

    if (!s->allocated)
      continue;
    if (s->perm_class > ELFMOD_PERM_DATA ||
        s->staging_offset > ctx->staging_size ||
        s->size > ctx->staging_size - s->staging_offset)
      return -1;
    addresses[i] = slot->arena_base + s->staging_offset;
    if (s->size == 0)
      continue;
    end = s->staging_offset + s->size;
    first = (uint32)(s->staging_offset / PGSIZE);
    last = (uint32)((end - 1) / PGSIZE);
    if (last >= slot->arena_pages)
      return -1;
    permission = s->perm_class + 1;
    for (p = first; p <= last; p++) {
      if (slot->page_perm[p] != 0 &&
          slot->page_perm[p] != permission)
        return -1;
      slot->page_perm[p] = permission;
    }
    if (s->perm_class == ELFMOD_PERM_TEXT)
      has_text = 1;
  }
  return has_text ? 0 : -1;
}

static int
module_restore_direct_aliases(struct module_slot *slot)
{
  uint32 i;
  pte_t *pte;

  for (i = 0; i < slot->arena_pages; i++) {
    uint64 pa = slot->page_pa[i];
    if (pa == 0)
      continue;
    pte = walk(kernel_pagetable, pa, 0);
    if (pte == 0 || (*pte & PTE_V) != 0)
      return -1;
  }
  for (i = 0; i < slot->arena_pages; i++) {
    uint64 pa = slot->page_pa[i];
    if (pa == 0)
      continue;
    pte = walk(kernel_pagetable, pa, 0);
    *pte = PA2PTE(pa) | PTE_V | PTE_R | PTE_W;
  }
  sfence_vma();
  slot->aliases_removed = 0;
  return 0;
}

static void
module_free_direct_pages(struct module_slot *slot)
{
  uint32 i;
  for (i = 0; i < slot->arena_pages; i++)
    if (slot->page_pa[i]) {
      kfree((void *)slot->page_pa[i]);
      slot->page_pa[i] = 0;
    }
}

static int
module_release_image(struct module_slot *slot)
{
  if (slot->mappings_ready) {
    uvmunmap(kernel_pagetable, slot->arena_base, slot->arena_pages, 0);
    sfence_vma();
    icache_fence();
    slot->mappings_ready = 0;
  }
  if (slot->aliases_removed && module_restore_direct_aliases(slot) < 0)
    return -1;
  module_free_direct_pages(slot);
  module_arena_release(slot);
  return 0;
}

static int
module_prepare_pages(const struct elfmod_context *ctx,
                    struct module_slot *slot)
{
  uint32 i;
  pte_t *pte;

  for (i = 0; i < slot->arena_pages; i++) {
    uint64 offset, amount;
    char *page;
    if (slot->page_perm[i] == 0)
      continue;
    page = kalloc();
    if (page == 0)
      goto alloc_failed;
    slot->page_pa[i] = (uint64)page;
    memset(page, 0, PGSIZE);
    offset = (uint64)i * PGSIZE;
    if (offset < ctx->staging_size) {
      amount = ctx->staging_size - offset;
      if (amount > PGSIZE)
        amount = PGSIZE;
      memmove(page, (char *)module_staging + offset, amount);
    }
  }

  // Preflight all identity mappings before removing any writable alias.
  for (i = 0; i < slot->arena_pages; i++) {
    uint64 pa = slot->page_pa[i];
    if (pa == 0)
      continue;
    pte = walk(kernel_pagetable, pa, 0);
    if (pte == 0 || (*pte & PTE_V) == 0 || PTE2PA(*pte) != pa ||
        (PTE_FLAGS(*pte) & (PTE_R | PTE_W | PTE_X | PTE_U)) !=
          (PTE_R | PTE_W))
      return -2;
  }

  // Remove all writable aliases before creating any final mapping.
  for (i = 0; i < slot->arena_pages; i++) {
    uint64 pa = slot->page_pa[i];
    if (pa != 0) {
      pte = walk(kernel_pagetable, pa, 0);
      *pte = 0;
    }
  }
  slot->aliases_removed = 1;
  sfence_vma();

  for (i = 0; i < slot->arena_pages; i++) {
    int flags;
    if (slot->page_pa[i] == 0)
      continue;
    switch (slot->page_perm[i] - 1) {
    case ELFMOD_PERM_TEXT: flags = PTE_R | PTE_X; break;
    case ELFMOD_PERM_READONLY: flags = PTE_R; break;
    case ELFMOD_PERM_DATA: flags = PTE_R | PTE_W; break;
    default: goto map_failed;
    }
    if (mappages(kernel_pagetable,
                 slot->arena_base + (uint64)i * PGSIZE,
                 PGSIZE, slot->page_pa[i], flags) < 0)
      goto map_failed;
  }
  sfence_vma();
  icache_fence();
  slot->mappings_ready = 1;
  return 0;

map_failed:
  uvmunmap(kernel_pagetable, slot->arena_base, slot->arena_pages, 0);
  sfence_vma();
  icache_fence();
  if (module_restore_direct_aliases(slot) < 0)
    return -2;
  module_free_direct_pages(slot);
  return -1;

alloc_failed:
  module_free_direct_pages(slot);
  return -1;
}

static int
module_read_file(char *path, uint64 *length)
{
  struct inode *ip;
  uint size;
  int n;

  begin_op();
  ip = namei(path);
  if (ip == 0) {
    end_op();
    return -1;
  }
  ilock(ip);
  size = ip->size;
  if (ip->type != T_FILE || size == 0 || size > sizeof(module_file)) {
    iunlockput(ip);
    end_op();
    return -1;
  }
  n = readi(ip, 0, (uint64)module_file, 0, size);
  iunlockput(ip);
  end_op();
  if (n != (int)size)
    return -1;
  *length = size;
  return 0;
}

static void
module_set_quarantined(struct module_slot *slot)
{
  acquire(&module_lock);
  slot->state = MODULE_QUARANTINED;
  release(&module_lock);
}

int
module_load(char *path)
{
  struct module_slot *slot;
  struct elfmod_context ctx;
  struct elfmod_export init_export, command_export, exit_export;
  enum elfmod_error error = ELFMOD_OK;
  uint64 section_addresses[ELFMOD_MAX_SECTIONS], file_length, rounded;
  uint32 arena_pages;
  char name[MODULE_NAME_MAX + 1];
  int result = -1, map_result;
  module_init_fn init_entry;

  if (!module_single_hart_allowed() ||
      module_name_from_path(path, name) < 0)
    return -1;
  acquiresleep(&module_operation_lock);
  slot = module_reserve(name);
  if (slot == 0)
    goto done;
  if (module_read_file(path, &file_length) < 0)
    goto fail;
  if (elfmod_parse(&ctx, module_file, file_length, &error) < 0) {
    printk("module %s: ELF rejected: %s\n", name,
           (char *)elfmod_error_string(error));
    goto fail;
  }
  if (elfmod_find_export(&ctx, "module_init", &init_export, &error) < 0 ||
      elfmod_find_export(&ctx, "module_command", &command_export,
                         &error) < 0 ||
      elfmod_find_export(&ctx, "module_exit", &exit_export, &error) < 0) {
    printk("module %s: required entry point missing\n", name);
    goto fail;
  }
  if (ctx.staging_size == 0 ||
      ctx.staging_size > sizeof(module_staging) ||
      ctx.staging_size > ~(uint64)0 - (PGSIZE - 1))
    goto fail;
  rounded = (ctx.staging_size + PGSIZE - 1) & ~(uint64)(PGSIZE - 1);
  arena_pages = (uint32)(rounded / PGSIZE);
  if (module_arena_reserve(slot, arena_pages) < 0 ||
      module_section_addresses(&ctx, slot, section_addresses) < 0)
    goto fail;
  if (elfmod_relocate(&ctx, module_staging, sizeof(module_staging),
                      section_addresses, 0, 0, &error) < 0) {
    printk("module %s: relocation rejected: %s\n", name,
           (char *)elfmod_error_string(error));
    goto fail;
  }
  map_result = module_prepare_pages(&ctx, slot);
  if (map_result == -2) {
    module_set_quarantined(slot);
    printk("module %s: uncertain page state; memory quarantined\n", name);
    goto done;
  }
  if (map_result < 0)
    goto fail;

  slot->command = (module_command_fn)
    (section_addresses[command_export.section_index] +
     command_export.section_offset);
  slot->exit = (module_exit_fn)
    (section_addresses[exit_export.section_index] +
     exit_export.section_offset);
  init_entry = (module_init_fn)
    (section_addresses[init_export.section_index] +
     init_export.section_offset);
  if (init_entry(&api_v1) != 0) {
    printk("module %s: initialization failed\n", name);
    goto fail;
  }
  acquire(&module_lock);
  slot->state = MODULE_LIVE;
  release(&module_lock);
  printk("module %s loaded\n", name);
  result = 0;
  goto done;

fail:
  if (module_release_image(slot) < 0) {
    module_set_quarantined(slot);
    printk("module %s: cleanup uncertain; memory quarantined\n", name);
    goto done;
  }
  module_forget(slot);
done:
  releasesleep(&module_operation_lock);
  return result;
}

int
module_unload(char *requested_name)
{
  struct module_slot *slot;
  module_exit_fn exit_entry;
  char name[MODULE_NAME_MAX + 1];

  if (!module_single_hart_allowed() ||
      module_name_from_path(requested_name, name) < 0)
    return -1;
  acquiresleep(&module_operation_lock);
  acquire(&module_lock);
  slot = module_find_locked(name);
  if (slot == 0 || slot->state != MODULE_LIVE ||
      slot->active_calls != 0 || slot->dependent_refs != 0) {
    release(&module_lock);
    releasesleep(&module_operation_lock);
    return -1;
  }
  slot->state = MODULE_UNLOADING;
  exit_entry = slot->exit;
  release(&module_lock);

  // V1 has no callbacks, workers, or interrupt handlers.
  exit_entry();
  if (module_release_image(slot) < 0) {
    module_set_quarantined(slot);
    printk("module %s: unload cleanup uncertain; memory quarantined\n",
           name);
    releasesleep(&module_operation_lock);
    return -1;
  }
  printk("module %s unloaded\n", name);
  module_forget(slot);
  releasesleep(&module_operation_lock);
  return 0;
}

int
module_call(char *requested_name, int argc, const char *const argv[])
{
  struct module_slot *slot;
  module_command_fn command;
  char name[MODULE_NAME_MAX + 1];
  int result;

  if (!module_single_hart_allowed() || argc < 0 ||
      argc > MODULE_MAX_ARGS ||
      module_name_from_path(requested_name, name) < 0 ||
      (argc != 0 && argv == 0))
    return -1;
  acquire(&module_lock);
  slot = module_find_locked(name);
  if (slot == 0 || slot->state != MODULE_LIVE ||
      slot->active_calls == ~(uint32)0) {
    release(&module_lock);
    return -1;
  }
  slot->active_calls++;
  command = slot->command;
  release(&module_lock);

  result = command(argc, argv);

  acquire(&module_lock);
  if (slot->active_calls == 0)
    panic("module: active call underflow");
  slot->active_calls--;
  release(&module_lock);
  return result;
}

int
module_list(void)
{
  struct module_view {
    char name[MODULE_NAME_MAX + 1];
    enum module_state state;
    uint32 active_calls;
    uint32 dependent_refs;
    uint32 pages;
  } view[MODULE_SLOTS];
  uint count = 0, i;
  const char *state_name;

  acquire(&module_lock);
  for (i = 0; i < MODULE_SLOTS; i++) {
    if (modules[i].state == MODULE_FREE)
      continue;
    safestrcpy(view[count].name, modules[i].name, sizeof(view[count].name));
    view[count].state = modules[i].state;
    view[count].active_calls = modules[i].active_calls;
    view[count].dependent_refs = modules[i].dependent_refs;
    view[count].pages = modules[i].arena_pages;
    count++;
  }
  release(&module_lock);
  if (count == 0) {
    printk("no modules loaded\n");
    return 0;
  }
  for (i = 0; i < count; i++) {
    switch (view[i].state) {
    case MODULE_LOADING: state_name = "loading"; break;
    case MODULE_LIVE: state_name = "live"; break;
    case MODULE_UNLOADING: state_name = "unloading"; break;
    case MODULE_QUARANTINED: state_name = "quarantined"; break;
    default: state_name = "unknown"; break;
    }
    printk("%s state=%s active=%d dependencies=%d pages=%d\n",
           view[i].name, (char *)state_name, view[i].active_calls,
           view[i].dependent_refs, view[i].pages);
  }
  return (int)count;
}
