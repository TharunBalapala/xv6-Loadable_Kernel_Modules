#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "module.h"
#include "defs.h"
static struct spinlock log_lock;
static struct module_log_record ring[MODULE_LOG_CAPACITY];
static uint32 head, count, dropped;
static uint64 next_sequence;
void modulelog_init(void) {
  initlock(&log_lock, "module log");
  head = count = dropped = 0; next_sequence = 0;
  memset(ring, 0, sizeof(ring));
}
int modulelog_append(uint32 level, const char *message) {
  struct module_log_record r; uint i, index;
  memset(&r, 0, sizeof(r));
  if (level < MODULE_LOG_INFO || level > MODULE_LOG_ERROR || message == 0) return -1;
  for (i=0; i<MODULE_LOG_MESSAGE_MAX-1 && message[i]; i++) r.message[i]=message[i];
  if (message[i]) return -1;
  r.message[i]=0;
  acquire(&tickslock); r.tick=ticks; release(&tickslock);
  r.level=level;
  acquire(&log_lock); r.sequence=++next_sequence;
  if (count<MODULE_LOG_CAPACITY) { index=(head+count)%MODULE_LOG_CAPACITY; count++; }
  else { index=head; head=(head+1)%MODULE_LOG_CAPACITY; if (dropped != ~(uint32)0) dropped++; }
  ring[index]=r; release(&log_lock); return 0;
}
int modulelog_snapshot(struct module_log_snapshot *out) {
  uint i; if (!out) return -1;
  acquire(&log_lock); out->count=count; out->dropped=dropped;
  for (i=0;i<count;i++) out->records[i]=ring[(head+i)%MODULE_LOG_CAPACITY];
  release(&log_lock); return 0;
}
int modulelog_clear(void) {
  acquire(&log_lock); memset(ring,0,sizeof(ring)); head=count=dropped=0;
  release(&log_lock); return 0;
}
