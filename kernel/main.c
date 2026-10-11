#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

#ifdef MODULE_MEMTEST
void modmemtest(void);
#endif
#ifdef MODULES
void module_hart_online(int hartid);
void module_system_init(void);
#endif

volatile static int started = 0;

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
#ifdef MODULES
  module_hart_online(cpuid());
#endif
  if (cpuid() == 0) {
    consoleinit();
    printkinit();
    printk("\n");
    printk("xv6 kernel is booting\n");
    printk("\n");
    kinit();        // physical page allocator
    kvminit();      // create kernel page table
    kvminithart();  // turn on paging
    procinit();     // process table
    trapinit();     // trap vectors
    trapinithart(); // install kernel trap vector
#ifdef MODULE_MEMTEST
    modmemtest(); // test executable page permissions on this hart
#endif
    plicinit();         // set up interrupt controller
    plicinithart();     // ask PLIC for device interrupts
    binit();            // buffer cache
    iinit();            // inode table
    fileinit();         // file table
    virtio_disk_init(); // emulated hard disk
#ifdef MODULES
    module_system_init();
#endif
    userinit(); // first user process

    __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
  } else {
    while (__atomic_load_n(&started, __ATOMIC_ACQUIRE) == 0)
      ;

    printk("hart %d starting\n", cpuid());
    kvminithart();  // turn on paging
    trapinithart(); // install kernel trap vector
    plicinithart(); // ask PLIC for device interrupts
  }

  scheduler();
}
