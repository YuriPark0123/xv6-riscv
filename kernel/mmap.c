#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fcntl.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

struct {
  struct spinlock lock;
  struct mmap_area entries[NMMAPAREA];
} mmap_table;

void
mmap_init(void)
{
  initlock(&mmap_table.lock, "mmap");
}

static int
mmap_alloc_page(pagetable_t pagetable, uint64 va, int prot,
                struct file *f, int offset, int is_anon)
{
  char *mem;
  int perm;

  mem = kalloc();
  if(mem == 0)
    return -1;

  memset(mem, 0, PGSIZE);

  if(!is_anon && f){
    ilock(f->ip);
    readi(f->ip, 0, (uint64)mem, offset, PGSIZE);
    iunlock(f->ip);
  }

  perm = PTE_R | PTE_U;
  if(prot & PROT_WRITE)
    perm |= PTE_W;

  if(mappages(pagetable, va, PGSIZE, (uint64)mem, perm) != 0){
    kfree(mem);
    return -1;
  }

  return 0;
}

uint64
sys_mmap(void)
{
  uint64 addr;
  int length, prot, flags, fd, offset;
  struct proc *p = myproc();
  struct file *f = 0;
  uint64 va;

  argaddr(0, &addr);
  argint(1, &length);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argint(5, &offset);

  va = MMAPBASE + addr;

  if(va % PGSIZE != 0)
    return 0;
  if(length <= 0 || (length % PGSIZE) != 0)
    return 0;

  int is_anon = (flags & MAP_ANONYMOUS) != 0;

  if(!is_anon){
    if(fd < 0 || fd >= NOFILE || p->ofile[fd] == 0)
      return 0;
    f = p->ofile[fd];

    if((prot & PROT_READ) && !f->readable)
      return 0;
    if((prot & PROT_WRITE) && !f->writable)
      return 0;
  }

  acquire(&mmap_table.lock);
  int slot = -1;
  for(int i = 0; i < NMMAPAREA; i++){
    if(mmap_table.entries[i].p == 0){
      slot = i;
      break;
    }
  }
  if(slot == -1){
    release(&mmap_table.lock);
    return 0;
  }

  mmap_table.entries[slot].f = f;
  mmap_table.entries[slot].addr = va;
  mmap_table.entries[slot].length = length;
  mmap_table.entries[slot].offset = offset;
  mmap_table.entries[slot].prot = prot;
  mmap_table.entries[slot].flags = flags;
  mmap_table.entries[slot].p = p;
  release(&mmap_table.lock);

  if(f)
    filedup(f);

  if(flags & MAP_POPULATE){
    for(int i = 0; i < length; i += PGSIZE){
      int file_off = offset + i;
      if(mmap_alloc_page(p->pagetable, va + i, prot, f, file_off, is_anon) < 0){
        for(int j = 0; j < i; j += PGSIZE)
          uvmunmap(p->pagetable, va + j, 1, 1);
        acquire(&mmap_table.lock);
        mmap_table.entries[slot].p = 0;
        mmap_table.entries[slot].f = 0;
        release(&mmap_table.lock);
        if(f) fileclose(f);
        return 0;
      }
    }
  }

  return va;
}

uint64
sys_munmap(void)
{
  uint64 addr;
  struct proc *p = myproc();

  argaddr(0, &addr);

  if(addr % PGSIZE != 0)
    return -1;

  acquire(&mmap_table.lock);
  for(int i = 0; i < NMMAPAREA; i++){
    if(mmap_table.entries[i].p == p && mmap_table.entries[i].addr == addr){
      struct mmap_area area = mmap_table.entries[i];
      mmap_table.entries[i].p = 0;
      mmap_table.entries[i].f = 0;
      release(&mmap_table.lock);

      for(int j = 0; j < area.length; j += PGSIZE){
        uint64 va = addr + j;
        pte_t *pte = walk(p->pagetable, va, 0);
        if(pte && (*pte & PTE_V))
          uvmunmap(p->pagetable, va, 1, 1);
      }
      if(area.f)
        fileclose(area.f);
      return 1;
    }
  }
  release(&mmap_table.lock);

  return -1;
}

uint64
sys_freemem(void)
{
  return freemem() / PGSIZE;
}

// Page fault handler for mmap regions.
// Returns 1 on success, -1 on failure.
int
mmap_page_fault(uint64 va, int is_write)
{
  struct proc *p = myproc();
  va = PGROUNDDOWN(va);

  acquire(&mmap_table.lock);
  for(int i = 0; i < NMMAPAREA; i++){
    if(mmap_table.entries[i].p != p)
      continue;
    uint64 start = mmap_table.entries[i].addr;
    uint64 end = start + mmap_table.entries[i].length;
    if(va >= start && va < end){
      if(is_write && !(mmap_table.entries[i].prot & PROT_WRITE)){
        release(&mmap_table.lock);
        return -1;
      }

      // Snapshot before releasing lock; the slot itself remains held
      // by this process's mmap_area, so it stays valid for our use.
      int prot = mmap_table.entries[i].prot;
      int flags = mmap_table.entries[i].flags;
      int offset = mmap_table.entries[i].offset;
      struct file *f = mmap_table.entries[i].f;
      release(&mmap_table.lock);

      if(ismapped(p->pagetable, va))
        return 1;

      int is_anon = (flags & MAP_ANONYMOUS) != 0;
      int file_off = offset + (va - start);

      if(mmap_alloc_page(p->pagetable, va, prot, f, file_off, is_anon) < 0)
        return -1;

      return 1;
    }
  }
  release(&mmap_table.lock);

  return -1;
}

// Copy mmap_areas from parent to child process during fork.
// Called with parent's process lock-free state but child not yet running.
void
mmap_fork(struct proc *parent, struct proc *child)
{
  // Two-phase: snapshot parent entries under lock, then materialize
  // child copies (allocating new slots) with lock held; finally copy
  // pages without holding the lock (kalloc/mappages can be slow).
  struct mmap_area copies[NMMAPAREA];
  int ncopies = 0;

  acquire(&mmap_table.lock);
  for(int i = 0; i < NMMAPAREA; i++){
    if(mmap_table.entries[i].p != parent)
      continue;

    int slot = -1;
    for(int j = 0; j < NMMAPAREA; j++){
      if(mmap_table.entries[j].p == 0){
        slot = j;
        break;
      }
    }
    if(slot == -1)
      continue;

    mmap_table.entries[slot] = mmap_table.entries[i];
    mmap_table.entries[slot].p = child;
    copies[ncopies++] = mmap_table.entries[slot];
  }
  release(&mmap_table.lock);

  for(int k = 0; k < ncopies; k++){
    if(copies[k].f)
      filedup(copies[k].f);

    uint64 start = copies[k].addr;
    int length = copies[k].length;
    for(int j = 0; j < length; j += PGSIZE){
      uint64 va = start + j;
      pte_t *pte = walk(parent->pagetable, va, 0);
      if(pte && (*pte & PTE_V)){
        uint64 pa = PTE2PA(*pte);
        uint flags = PTE_FLAGS(*pte);
        char *mem = kalloc();
        if(mem == 0)
          continue;
        memmove(mem, (char*)pa, PGSIZE);
        if(mappages(child->pagetable, va, PGSIZE, (uint64)mem, flags) != 0)
          kfree(mem);
      }
    }
  }
}

// Cleanup mmap_areas when a process exits.
void
mmap_cleanup(struct proc *p)
{
  // Snapshot entries to free under lock, then perform expensive
  // unmap/file-close work without the lock held.
  struct mmap_area to_free[NMMAPAREA];
  int n = 0;

  acquire(&mmap_table.lock);
  for(int i = 0; i < NMMAPAREA; i++){
    if(mmap_table.entries[i].p != p)
      continue;
    to_free[n++] = mmap_table.entries[i];
    mmap_table.entries[i].p = 0;
    mmap_table.entries[i].f = 0;
  }
  release(&mmap_table.lock);

  for(int k = 0; k < n; k++){
    for(int j = 0; j < to_free[k].length; j += PGSIZE){
      uint64 va = to_free[k].addr + j;
      pte_t *pte = walk(p->pagetable, va, 0);
      if(pte && (*pte & PTE_V))
        uvmunmap(p->pagetable, va, 1, 1);
    }
    if(to_free[k].f)
      fileclose(to_free[k].f);
  }
}
