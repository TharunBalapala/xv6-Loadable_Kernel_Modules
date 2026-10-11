#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

extern pagetable_t kernel_pagetable;

#define MODMEMTEST_VA    (PHYSTOP + PGSIZE)
#define MODMEMTEST_FLAGS (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U)

typedef int (*trusted_probe_fn)(void);

static void
check_pte(pte_t *pte, uint64 pa, uint64 flags)
{
  if (pte == 0 || (PTE_FLAGS(*pte) & MODMEMTEST_FLAGS) != flags ||
      PTE2PA(*pte) != pa)
    panic("modmemtest: PTE mismatch");
}

// A fixed, trusted instruction sequence: addi a0,zero,42; ret.
// The test verifies the final module mapping and never consumes file input.
void
modmemtest(void)
{
  static const uint32 code[2] = {0x02a00513, 0x00008067};
  char *page = kalloc();
  uint64 pa;
  pte_t *pte;
  trusted_probe_fn probe;

  if (page == 0)
    panic("modmemtest: kalloc");
  pa = (uint64)page;
  pte = walk(kernel_pagetable, pa, 0);
  check_pte(pte, pa, PTE_V | PTE_R | PTE_W);
  memset(page, 0, PGSIZE);
  memmove(page, (char *)code, sizeof(code));

  // Remove the writable identity alias before installing executable memory.
  *pte = 0;
  sfence_vma();
  if (mappages(kernel_pagetable, MODMEMTEST_VA, PGSIZE, pa, PTE_R | PTE_X) < 0)
    panic("modmemtest: map RX");
  sfence_vma();
  icache_fence();
  pte = walk(kernel_pagetable, MODMEMTEST_VA, 0);
  check_pte(pte, pa, PTE_V | PTE_R | PTE_X);
  if (walk(kernel_pagetable, pa, 0) == 0 ||
      (PTE_FLAGS(*walk(kernel_pagetable, pa, 0)) & PTE_V) != 0)
    panic("modmemtest: writable alias remains");

  probe = (trusted_probe_fn)MODMEMTEST_VA;
  if (probe() != 42)
    panic("modmemtest: RX execution failed");
  printk("module memory selftest: trusted RX code returned 42\n");

#ifdef MODULE_MEMTEST_WRITE_FAULT
  printk("module memory selftest: expecting store fault on RX page\n");
  *(volatile uint32 *)MODMEMTEST_VA = 0;
  panic("modmemtest: RX write unexpectedly succeeded");
#endif

  // R/NX must remain readable but cannot be used for instruction fetch.
  *pte = PA2PTE(pa) | PTE_V | PTE_R;
  sfence_vma();
  icache_fence();
  check_pte(pte, pa, PTE_V | PTE_R);
#ifdef MODULE_MEMTEST_NX_FAULT
  printk("module memory selftest: expecting instruction fault on R/NX page\n");
  (void)probe();
  panic("modmemtest: NX execution unexpectedly succeeded");
#endif

  uvmunmap(kernel_pagetable, MODMEMTEST_VA, 1, 0);
  sfence_vma();
  icache_fence();
  pte = walk(kernel_pagetable, pa, 0);
  if (pte == 0 || (*pte & PTE_V) != 0)
    panic("modmemtest: direct alias unexpectedly valid");
  *pte = PA2PTE(pa) | PTE_V | PTE_R | PTE_W;
  sfence_vma();
  check_pte(pte, pa, PTE_V | PTE_R | PTE_W);
  if (*(volatile uint32 *)page != code[0])
    panic("modmemtest: restored direct map");
  kfree(page);
  page = kalloc();
  if (page != (char *)pa)
    panic("modmemtest: allocator reuse");
  ((volatile uchar *)page)[16] = 0x5a;
  if (((volatile uchar *)page)[16] != 0x5a)
    panic("modmemtest: reuse write");
  kfree(page);
  printk(
    "module memory selftest: alias removal, permissions, and reuse passed\n");
}
