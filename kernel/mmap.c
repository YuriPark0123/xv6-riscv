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

struct mmap_area mmap_areas[NMMAPAREA];

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
    // Read file content into the physical page
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

  // Check page alignment
  if(va % PGSIZE != 0)
    return 0;

  int is_anon = (flags & MAP_ANONYMOUS) != 0;

  if(!is_anon){
    // File mapping: validate fd
    if(fd < 0 || fd >= NOFILE || p->ofile[fd] == 0)
      return 0;
    f = p->ofile[fd];

    // Check prot matches file open mode
    if((prot & PROT_READ) && !f->readable)
      return 0;
    if((prot & PROT_WRITE) && !f->writable)
      return 0;
  }

  // Find a free mmap_area slot
  int slot = -1;
  for(int i = 0; i < NMMAPAREA; i++){
    if(mmap_areas[i].p == 0){
      slot = i;
      break;
    }
  }
  if(slot == -1)
    return 0;

  // Record the mapping
  mmap_areas[slot].f = f;
  mmap_areas[slot].addr = va;
  mmap_areas[slot].length = length;
  mmap_areas[slot].offset = offset;
  mmap_areas[slot].prot = prot;
  mmap_areas[slot].flags = flags;
  mmap_areas[slot].p = p;

  // Increase file reference count for file mappings
  if(f)
    filedup(f);

  // If MAP_POPULATE, allocate pages and create page table entries now
  if(flags & MAP_POPULATE){
    for(int i = 0; i < length; i += PGSIZE){
      int file_off = offset + i;
      if(mmap_alloc_page(p->pagetable, va + i, prot, f, file_off, is_anon) < 0){
        // Cleanup on failure: unmap already mapped pages
        for(int j = 0; j < i; j += PGSIZE){
          uvmunmap(p->pagetable, va + j, 1, 1);
        }
        mmap_areas[slot].p = 0;
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

  // Check page alignment
  if(addr % PGSIZE != 0)
    return -1;

  // Find the mmap_area for this process with matching addr
  for(int i = 0; i < NMMAPAREA; i++){
    if(mmap_areas[i].p == p && mmap_areas[i].addr == addr){
      // Free any allocated physical pages and page table entries
      for(int j = 0; j < mmap_areas[i].length; j += PGSIZE){
        uint64 va = addr + j;
        pte_t *pte = walk(p->pagetable, va, 0);
        if(pte && (*pte & PTE_V)){
          uvmunmap(p->pagetable, va, 1, 1);
        }
      }
      // Close file reference
      if(mmap_areas[i].f)
        fileclose(mmap_areas[i].f);
      // Clear the mmap_area
      mmap_areas[i].p = 0;
      mmap_areas[i].f = 0;
      return 1;
    }
  }

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

  // Find the mmap_area containing this address
  for(int i = 0; i < NMMAPAREA; i++){
    if(mmap_areas[i].p != p)
      continue;
    uint64 start = mmap_areas[i].addr;
    uint64 end = start + mmap_areas[i].length;
    if(va >= start && va < end){
      // Found the mapping
      // Check write permission
      if(is_write && !(mmap_areas[i].prot & PROT_WRITE))
        return -1;

      // Check if page is already mapped
      if(ismapped(p->pagetable, va))
        return 1;

      int is_anon = (mmap_areas[i].flags & MAP_ANONYMOUS) != 0;
      int file_off = mmap_areas[i].offset + (va - start);

      if(mmap_alloc_page(p->pagetable, va, mmap_areas[i].prot,
                         mmap_areas[i].f, file_off, is_anon) < 0)
        return -1;

      return 1;
    }
  }

  return -1;
}

// Copy mmap_areas from parent to child process during fork.
void
mmap_fork(struct proc *parent, struct proc *child)
{
  for(int i = 0; i < NMMAPAREA; i++){
    if(mmap_areas[i].p != parent)
      continue;

    // Find a free slot for child
    int slot = -1;
    for(int j = 0; j < NMMAPAREA; j++){
      if(mmap_areas[j].p == 0){
        slot = j;
        break;
      }
    }
    if(slot == -1)
      continue;

    // Copy mmap_area entry
    mmap_areas[slot] = mmap_areas[i];
    mmap_areas[slot].p = child;

    // Increase file reference count
    if(mmap_areas[slot].f)
      filedup(mmap_areas[slot].f);

    // Copy any already-mapped pages
    uint64 start = mmap_areas[i].addr;
    int length = mmap_areas[i].length;
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
        if(mappages(child->pagetable, va, PGSIZE, (uint64)mem, flags) != 0){
          kfree(mem);
        }
      }
    }
  }
}

// Cleanup mmap_areas when a process exits.
void
mmap_cleanup(struct proc *p)
{
  for(int i = 0; i < NMMAPAREA; i++){
    if(mmap_areas[i].p != p)
      continue;

    // Free any allocated physical pages
    for(int j = 0; j < mmap_areas[i].length; j += PGSIZE){
      uint64 va = mmap_areas[i].addr + j;
      pte_t *pte = walk(p->pagetable, va, 0);
      if(pte && (*pte & PTE_V)){
        uvmunmap(p->pagetable, va, 1, 1);
      }
    }

    // Close file reference
    if(mmap_areas[i].f)
      fileclose(mmap_areas[i].f);

    // Clear entry
    mmap_areas[i].p = 0;
    mmap_areas[i].f = 0;
  }
}
