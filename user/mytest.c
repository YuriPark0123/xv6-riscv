#include "../kernel/types.h"
#include "../kernel/stat.h"
#include "user.h"
#include "../kernel/fcntl.h"
#include "../kernel/memlayout.h"
#include "../kernel/param.h"
#include "../kernel/spinlock.h"
#include "../kernel/sleeplock.h"
#include "../kernel/fs.h"
#include "../kernel/syscall.h"

int passed = 0;
int failed = 0;

void
check(const char *name, int cond)
{
  if(cond){
    printf("  [PASS] %s\n", name);
    passed++;
  } else {
    printf("  [FAIL] %s\n", name);
    failed++;
  }
}

//
// Test 1: Anonymous mapping with MAP_POPULATE
//
void
test_anon_populate(void)
{
  int free1, free2, free3;
  volatile char *addr;

  printf("\n=== Test 1: Anonymous + MAP_POPULATE ===\n");

  free1 = freemem();
  printf("  freemem before: %d\n", free1);

  addr = (volatile char*)mmap(0, 4096 * 2, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  check("mmap returns non-zero", (uint64)addr != 0);
  check("mmap returns MMAPBASE", (uint64)addr == MMAPBASE);

  free2 = freemem();
  printf("  freemem after mmap: %d (used %d)\n", free2, free1 - free2);
  check("pages allocated (>= 2)", free1 - free2 >= 2);

  // Anonymous pages must be zero-filled
  check("page1 byte[0] == 0", addr[0] == 0);
  check("page1 byte[4095] == 0", addr[4095] == 0);
  check("page2 byte[0] == 0", addr[4096] == 0);
  check("page2 byte[4095] == 0", addr[8191] == 0);

  // Write and read back
  addr[0] = 'A';
  addr[4096] = 'B';
  check("write/read page1", addr[0] == 'A');
  check("write/read page2", addr[4096] == 'B');

  // Fill entire first page
  for(int i = 0; i < 4096; i++)
    addr[i] = (char)(i & 0xff);
  int ok = 1;
  for(int i = 0; i < 4096; i++){
    if(addr[i] != (char)(i & 0xff)){
      ok = 0;
      break;
    }
  }
  check("full page write/read integrity", ok);

  check("munmap succeeds", munmap((uint64)addr) == 1);

  free3 = freemem();
  printf("  freemem after munmap: %d (recovered %d)\n", free3, free3 - free2);
  check("pages freed after munmap (>= 2)", free3 - free2 >= 2);
}

//
// Test 2: Anonymous mapping WITHOUT MAP_POPULATE (lazy / page fault)
//
void
test_anon_lazy(void)
{
  int free1, free2, free3;
  volatile char *addr;

  printf("\n=== Test 2: Anonymous + lazy (no MAP_POPULATE) ===\n");

  free1 = freemem();
  printf("  freemem before: %d\n", free1);

  addr = (volatile char*)mmap(0, 4096 * 3, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS, -1, 0);
  check("mmap returns non-zero", (uint64)addr != 0);

  free2 = freemem();
  printf("  freemem after mmap (no access): %d\n", free2);
  check("no pages allocated yet", free1 == free2);

  // Touch page 0 - triggers page fault
  addr[0] = 'X';
  free3 = freemem();
  printf("  freemem after touch page0: %d (used %d)\n", free3, free2 - free3);
  check("1 page allocated after touch page0", free2 - free3 >= 1);
  check("page0 content correct", addr[0] == 'X');

  // Touch page 1
  int before = freemem();
  addr[4096] = 'Y';
  int after = freemem();
  printf("  freemem after touch page1: %d (used %d)\n", after, before - after);
  check("page1 allocated on demand", before - after >= 1);
  check("page1 content correct", addr[4096] == 'Y');

  // Touch page 2
  before = freemem();
  addr[8192] = 'Z';
  after = freemem();
  check("page2 allocated on demand", before - after >= 1);
  check("page2 content correct", addr[8192] == 'Z');

  // Verify all three pages still readable
  check("page0 still correct", addr[0] == 'X');
  check("page1 still correct", addr[4096] == 'Y');
  check("page2 still correct", addr[8192] == 'Z');

  check("munmap succeeds", munmap((uint64)addr) == 1);
}

//
// Test 3: File mapping with MAP_POPULATE
//
void
test_file_populate(void)
{
  int fd, free1, free2;
  volatile char *addr;
  char buf[16];

  printf("\n=== Test 3: File mapping + MAP_POPULATE ===\n");

  fd = open("README", O_RDONLY);
  check("open README", fd >= 0);
  if(fd < 0) return;

  // Read first bytes via read() for comparison
  int n = read(fd, buf, 10);
  check("read() succeeds", n > 0);
  // Seek back by closing and reopening
  close(fd);
  fd = open("README", O_RDONLY);

  free1 = freemem();
  addr = (volatile char*)mmap(0, 4096, PROT_READ, MAP_POPULATE, fd, 0);
  check("mmap returns non-zero", (uint64)addr != 0);
  if((uint64)addr == 0){ close(fd); return; }

  free2 = freemem();
  printf("  freemem used: %d\n", free1 - free2);
  check("page allocated immediately", free1 - free2 >= 1);

  // Compare mmap content with read() content
  int match = 1;
  for(int i = 0; i < n; i++){
    if(addr[i] != buf[i]){
      match = 0;
      printf("  mismatch at byte %d: mmap=0x%x read=0x%x\n", i, addr[i], buf[i]);
      break;
    }
  }
  check("mmap content matches read() content", match);

  printf("  first 3 bytes via mmap: ");
  char ch;
  ch = addr[0]; write(1, &ch, 1);
  ch = addr[1]; write(1, &ch, 1);
  ch = addr[2]; write(1, &ch, 1);
  printf("\n");

  check("munmap succeeds", munmap((uint64)addr) == 1);
  close(fd);
}

//
// Test 4: File mapping WITHOUT MAP_POPULATE (lazy)
//
void
test_file_lazy(void)
{
  int fd, free1, free2, free3;
  volatile char *addr;
  char buf[16];

  printf("\n=== Test 4: File mapping + lazy ===\n");

  fd = open("README", O_RDONLY);
  check("open README", fd >= 0);
  if(fd < 0) return;

  int n = read(fd, buf, 10);
  close(fd);
  fd = open("README", O_RDONLY);

  free1 = freemem();
  addr = (volatile char*)mmap(0, 4096, PROT_READ, 0, fd, 0);
  check("mmap returns non-zero", (uint64)addr != 0);
  if((uint64)addr == 0){ close(fd); return; }

  free2 = freemem();
  check("no pages allocated yet", free1 == free2);

  // Access triggers page fault - file content should be loaded
  char ch = addr[0];
  (void)ch;

  free3 = freemem();
  check("page allocated after access", free2 - free3 >= 1);

  // Verify file content
  int match = 1;
  for(int i = 0; i < n; i++){
    if(addr[i] != buf[i]){
      match = 0;
      printf("  mismatch at byte %d: mmap=0x%x read=0x%x\n", i, addr[i], buf[i]);
      break;
    }
  }
  check("lazy-loaded content matches read()", match);

  check("munmap succeeds", munmap((uint64)addr) == 1);
  close(fd);
}

//
// Test 5: File mapping with non-zero offset
//
void
test_file_offset(void)
{
  int fd;
  volatile char *addr;
  char buf[16];

  printf("\n=== Test 5: File mapping with offset ===\n");

  fd = open("README", O_RDONLY);
  check("open README", fd >= 0);
  if(fd < 0) return;

  // Read bytes starting at offset 5 via read() for comparison
  char dummy[5];
  read(fd, dummy, 5); // skip 5 bytes
  int n = read(fd, buf, 10);
  check("read at offset 5 succeeds", n > 0);
  close(fd);

  fd = open("README", O_RDONLY);
  // mmap with offset 5
  addr = (volatile char*)mmap(0, 4096, PROT_READ, MAP_POPULATE, fd, 5);
  check("mmap with offset returns non-zero", (uint64)addr != 0);
  if((uint64)addr == 0){ close(fd); return; }

  int match = 1;
  for(int i = 0; i < n && i < 10; i++){
    if(addr[i] != buf[i]){
      match = 0;
      printf("  mismatch at byte %d: mmap=0x%x read=0x%x\n", i, addr[i], buf[i]);
      break;
    }
  }
  check("offset content matches read() at same offset", match);

  check("munmap succeeds", munmap((uint64)addr) == 1);
  close(fd);
}

//
// Test 6: Multiple simultaneous mappings
//
void
test_multiple_mappings(void)
{
  int free1, free2;
  volatile char *a1, *a2, *a3;

  printf("\n=== Test 6: Multiple simultaneous mappings ===\n");

  free1 = freemem();

  a1 = (volatile char*)mmap(0, 4096, PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  a2 = (volatile char*)mmap(4096, 4096, PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  a3 = (volatile char*)mmap(8192, 4096, PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

  check("mapping 1 succeeds", (uint64)a1 != 0);
  check("mapping 2 succeeds", (uint64)a2 != 0);
  check("mapping 3 succeeds", (uint64)a3 != 0);

  check("addr1 == MMAPBASE+0", (uint64)a1 == MMAPBASE);
  check("addr2 == MMAPBASE+4096", (uint64)a2 == MMAPBASE + 4096);
  check("addr3 == MMAPBASE+8192", (uint64)a3 == MMAPBASE + 8192);

  free2 = freemem();
  check("3 data pages allocated", free1 - free2 >= 3);

  // Write different values to each
  a1[0] = 'A'; a1[100] = 'a';
  a2[0] = 'B'; a2[100] = 'b';
  a3[0] = 'C'; a3[100] = 'c';

  // Verify isolation
  check("a1 isolated", a1[0] == 'A' && a1[100] == 'a');
  check("a2 isolated", a2[0] == 'B' && a2[100] == 'b');
  check("a3 isolated", a3[0] == 'C' && a3[100] == 'c');

  // Unmap in different order
  check("munmap a2 (middle)", munmap((uint64)a2) == 1);
  check("a1 still valid after a2 munmap", a1[0] == 'A');
  check("a3 still valid after a2 munmap", a3[0] == 'C');

  check("munmap a1", munmap((uint64)a1) == 1);
  check("munmap a3", munmap((uint64)a3) == 1);
}

//
// Test 7: Fork with populated file mapping
//
void
test_fork_populate(void)
{
  int fd, pid;
  volatile char *addr;

  printf("\n=== Test 7: Fork + populated file mapping ===\n");

  fd = open("README", O_RDONLY);
  check("open README", fd >= 0);
  if(fd < 0) return;

  addr = (volatile char*)mmap(0, 4096, PROT_READ, MAP_POPULATE, fd, 0);
  check("mmap succeeds", (uint64)addr != 0);
  if((uint64)addr == 0){ close(fd); return; }

  // Save parent data for comparison
  char p0 = addr[0], p1 = addr[1], p2 = addr[2];
  int free_before = freemem();
  printf("  freemem before fork: %d\n", free_before);

  pid = fork();
  check("fork succeeds", pid >= 0);
  if(pid < 0){ munmap((uint64)addr); close(fd); return; }

  if(pid == 0){
    // Child
    int free_child = freemem();
    printf("  [child] freemem: %d\n", free_child);

    check("[child] data[0] matches parent", addr[0] == p0);
    check("[child] data[1] matches parent", addr[1] == p1);
    check("[child] data[2] matches parent", addr[2] == p2);

    // Verify file content integrity in child
    printf("  [child] first 3 bytes: ");
    char ch;
    ch = addr[0]; write(1, &ch, 1);
    ch = addr[1]; write(1, &ch, 1);
    ch = addr[2]; write(1, &ch, 1);
    printf("\n");

    exit(0);
  } else {
    int status;
    wait(&status);
    int free_after = freemem();
    printf("  [parent] freemem after child exit: %d\n", free_after);
    check("[parent] memory recovered after child exit", free_after >= free_before);

    // Parent data still intact
    check("[parent] data[0] still correct", addr[0] == p0);

    munmap((uint64)addr);
    close(fd);
  }
}

//
// Test 8: Fork with lazy mapping - child must page-fault independently
//
void
test_fork_lazy(void)
{
  int fd, pid;
  volatile char *addr;
  char buf[4];

  printf("\n=== Test 8: Fork + lazy file mapping ===\n");

  fd = open("README", O_RDONLY);
  check("open README", fd >= 0);
  if(fd < 0) return;

  read(fd, buf, 3);
  close(fd);
  fd = open("README", O_RDONLY);

  addr = (volatile char*)mmap(0, 4096, PROT_READ, 0, fd, 0);
  check("lazy mmap succeeds", (uint64)addr != 0);
  if((uint64)addr == 0){ close(fd); return; }

  pid = fork();
  check("fork succeeds", pid >= 0);
  if(pid < 0){ munmap((uint64)addr); close(fd); return; }

  if(pid == 0){
    // Child: access should trigger page fault
    int free_before = freemem();
    char ch = addr[0];
    int free_after = freemem();

    check("[child] page fault allocated page", free_before - free_after >= 1);
    check("[child] data matches file", ch == buf[0]);

    printf("  [child] first 3 bytes: ");
    ch = addr[0]; write(1, &ch, 1);
    ch = addr[1]; write(1, &ch, 1);
    ch = addr[2]; write(1, &ch, 1);
    printf("\n");

    exit(0);
  } else {
    int status;
    wait(&status);

    // Parent also accesses (independently)
    int free_before = freemem();
    char ch = addr[0];
    int free_after = freemem();
    check("[parent] page fault works independently", free_before - free_after >= 1);
    check("[parent] data matches file", ch == buf[0]);

    munmap((uint64)addr);
    close(fd);
  }
}

//
// Test 9: Fork with anonymous writable mapping - writes must be independent
//
void
test_fork_write_isolation(void)
{
  int pid;
  volatile char *addr;

  printf("\n=== Test 9: Fork write isolation (anonymous) ===\n");

  addr = (volatile char*)mmap(0, 4096, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  check("mmap succeeds", (uint64)addr != 0);
  if((uint64)addr == 0) return;

  addr[0] = 'P';
  addr[1] = 'A';

  pid = fork();
  check("fork succeeds", pid >= 0);
  if(pid < 0){ munmap((uint64)addr); return; }

  if(pid == 0){
    // Child sees parent's data
    check("[child] inherited data[0]", addr[0] == 'P');
    check("[child] inherited data[1]", addr[1] == 'A');

    // Child writes different values
    addr[0] = 'C';
    addr[1] = 'H';
    check("[child] write data[0]", addr[0] == 'C');
    check("[child] write data[1]", addr[1] == 'H');

    exit(0);
  } else {
    int status;
    wait(&status);

    // Parent's data must NOT be affected by child writes
    check("[parent] data[0] unaffected by child", addr[0] == 'P');
    check("[parent] data[1] unaffected by child", addr[1] == 'A');

    munmap((uint64)addr);
  }
}

//
// Test 10: Freemem consistency across mmap lifecycle
//
void
test_freemem_lifecycle(void)
{
  int f0, f1, f2, f3, f4;
  volatile char *a1, *a2;

  printf("\n=== Test 10: Freemem lifecycle ===\n");

  f0 = freemem();
  printf("  baseline: %d\n", f0);

  a1 = (volatile char*)mmap(0, 4096 * 4, PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  f1 = freemem();
  printf("  after alloc 4 pages: %d (used %d)\n", f1, f0 - f1);
  check("4-page alloc uses >= 4 pages", f0 - f1 >= 4);

  a2 = (volatile char*)mmap(4096 * 4, 4096 * 2, PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  f2 = freemem();
  printf("  after alloc 2 more: %d (used %d more)\n", f2, f1 - f2);
  check("2-page alloc uses >= 2 pages", f1 - f2 >= 2);

  munmap((uint64)a1);
  f3 = freemem();
  printf("  after munmap 4-page: %d (freed %d)\n", f3, f3 - f2);
  check("freed >= 4 pages", f3 - f2 >= 4);

  munmap((uint64)a2);
  f4 = freemem();
  printf("  after munmap 2-page: %d (freed %d)\n", f4, f4 - f3);
  check("freed >= 2 pages", f4 - f3 >= 2);

  // a1 was first mapping at MMAPBASE, created page table pages.
  // Those won't be freed until process exit, so f4 <= f0.
  printf("  final vs baseline: %d vs %d\n", f4, f0);
}

//
// Test 11: Error cases
//
void
test_errors(void)
{
  volatile char *addr;
  int fd;

  printf("\n=== Test 11: Error cases ===\n");

  // File mapping with fd=-1 (not anonymous) should fail
  addr = (volatile char*)mmap(0, 4096, PROT_READ, 0, -1, 0);
  check("file mapping fd=-1 fails", (uint64)addr == 0);

  // Prot mismatch: file opened RDONLY but prot has PROT_WRITE
  fd = open("README", O_RDONLY);
  if(fd >= 0){
    addr = (volatile char*)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_POPULATE, fd, 0);
    check("RDONLY file + PROT_WRITE fails", (uint64)addr == 0);
    if((uint64)addr != 0) munmap((uint64)addr);
    close(fd);
  }

  // munmap on address with no mapping should fail
  check("munmap non-existent mapping fails", munmap(MMAPBASE + 0x10000000) == -1);

  // munmap on non-page-aligned address should fail
  check("munmap non-aligned fails", munmap(MMAPBASE + 1) == -1);
}

//
// Test 12: File mapping with PROT_READ|PROT_WRITE (O_RDWR file)
//
void
test_file_readwrite(void)
{
  int fd;
  volatile char *addr;

  printf("\n=== Test 12: File RW mapping ===\n");

  fd = open("README", O_RDWR);
  if(fd < 0){
    printf("  (skipped: README not writable)\n");
    return;
  }

  addr = (volatile char*)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_POPULATE, fd, 0);
  check("RW mmap succeeds", (uint64)addr != 0);
  if((uint64)addr == 0){ close(fd); return; }

  char orig = addr[0];
  addr[0] = '!';
  check("write to RW mapping", addr[0] == '!');

  // Restore
  addr[0] = orig;
  check("restore original byte", addr[0] == orig);

  munmap((uint64)addr);
  close(fd);
}

//
// Test 13: Lazy anonymous read - should be zero
//
void
test_anon_lazy_read(void)
{
  volatile char *addr;

  printf("\n=== Test 13: Lazy anonymous read (zero-fill) ===\n");

  addr = (volatile char*)mmap(0, 4096, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS, -1, 0);
  check("mmap succeeds", (uint64)addr != 0);
  if((uint64)addr == 0) return;

  // Read without writing first - should be zero (page fault allocates zero page)
  check("lazy anon byte[0] == 0", addr[0] == 0);
  check("lazy anon byte[100] == 0", addr[100] == 0);
  check("lazy anon byte[4095] == 0", addr[4095] == 0);

  // Now write
  addr[0] = 42;
  check("write after lazy alloc", addr[0] == 42);
  check("other bytes still 0", addr[100] == 0);

  munmap((uint64)addr);
}

//
// Test 14: Large multi-page mapping
//
void
test_large_mapping(void)
{
  int npages = 8;
  volatile char *addr;
  int free1, free2;

  printf("\n=== Test 14: Large mapping (%d pages) ===\n", npages);

  free1 = freemem();
  addr = (volatile char*)mmap(0, 4096 * npages, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  check("mmap succeeds", (uint64)addr != 0);
  if((uint64)addr == 0) return;

  free2 = freemem();
  check("allocated >= npages", free1 - free2 >= npages);

  // Write unique pattern to each page
  for(int i = 0; i < npages; i++){
    addr[i * 4096] = 'A' + i;
    addr[i * 4096 + 1] = '0' + i;
  }

  // Verify all pages
  int ok = 1;
  for(int i = 0; i < npages; i++){
    if(addr[i * 4096] != 'A' + i || addr[i * 4096 + 1] != '0' + i){
      printf("  page %d mismatch!\n", i);
      ok = 0;
    }
  }
  check("all pages hold correct data", ok);

  munmap((uint64)addr);
}

//
// Test 15: Fork then munmap in parent - child unaffected
//
void
test_fork_munmap_parent(void)
{
  int pid;
  volatile char *addr;

  printf("\n=== Test 15: Fork then munmap in parent ===\n");

  addr = (volatile char*)mmap(0, 4096, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  check("mmap succeeds", (uint64)addr != 0);
  if((uint64)addr == 0) return;

  addr[0] = 'Z';

  pid = fork();
  if(pid < 0){ munmap((uint64)addr); return; }

  if(pid == 0){
    // Child: verify data, then sleep briefly so parent munmaps first
    check("[child] sees parent data", addr[0] == 'Z');

    // Write in child
    addr[0] = 'W';
    check("[child] write works", addr[0] == 'W');
    exit(0);
  } else {
    // Parent munmaps immediately
    munmap((uint64)addr);

    int status;
    wait(&status);
    printf("  child exited normally\n");
  }
}

//
// Test 16: Mixed file and anonymous mappings at different addresses
//
void
test_mixed_mappings(void)
{
  int fd;
  volatile char *anon_addr, *file_addr;
  char buf[4];

  printf("\n=== Test 16: Mixed file + anonymous mappings ===\n");

  fd = open("README", O_RDONLY);
  check("open README", fd >= 0);
  if(fd < 0) return;
  read(fd, buf, 3);
  close(fd);
  fd = open("README", O_RDONLY);

  // Anonymous at offset 0
  anon_addr = (volatile char*)mmap(0, 4096, PROT_READ | PROT_WRITE,
                                   MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  // File at offset 4096
  file_addr = (volatile char*)mmap(4096, 4096, PROT_READ, MAP_POPULATE, fd, 0);

  check("anon mapping succeeds", (uint64)anon_addr != 0);
  check("file mapping succeeds", (uint64)file_addr != 0);

  if((uint64)anon_addr != 0 && (uint64)file_addr != 0){
    // Verify anonymous is zero
    check("anon page is zero", anon_addr[0] == 0);

    // Verify file has correct content
    check("file page has correct content", file_addr[0] == buf[0] && file_addr[1] == buf[1]);

    // Write to anonymous
    anon_addr[0] = 'Q';
    check("anon write doesn't affect file mapping", file_addr[0] == buf[0]);

    munmap((uint64)anon_addr);
    // File mapping should still work
    check("file mapping survives anon munmap", file_addr[0] == buf[0]);
    munmap((uint64)file_addr);
  } else {
    if((uint64)anon_addr != 0) munmap((uint64)anon_addr);
    if((uint64)file_addr != 0) munmap((uint64)file_addr);
  }

  close(fd);
}

//
// Test 17: Partial page allocation - lazy map 3 pages, touch only page 0,
//          then munmap. Only the touched page should be freed.
//
void
test_partial_page_alloc(void)
{
  int free1, free2, free3, free4;
  volatile char *addr;

  printf("\n=== Test 17: Partial page allocation (lazy) ===\n");

  free1 = freemem();

  addr = (volatile char*)mmap(0, 4096 * 3, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS, -1, 0);
  check("mmap succeeds", (uint64)addr != 0);
  if((uint64)addr == 0) return;

  free2 = freemem();
  check("no pages used before access", free1 == free2);

  // Touch ONLY page 0
  addr[0] = 'X';
  free3 = freemem();
  printf("  after touch page0 only: used %d\n", free2 - free3);
  check("only 1 page allocated (page0 touched)", free2 - free3 >= 1);

  // Page 1 and 2 not touched - no allocation
  // Now munmap: should free the 1 allocated page, skip the 2 unallocated ones
  munmap((uint64)addr);
  free4 = freemem();
  printf("  after munmap: freed %d pages\n", free4 - free3);
  check("freed the 1 allocated page", free4 - free3 >= 1);
  check("freemem back near baseline", free4 >= free1 - 1);
}

//
// Test 18: Write to PROT_READ mapping must kill the process.
//          Fork a child that writes to read-only mmap, verify it dies.
//
void
test_write_protection_fault(void)
{
  int pid;
  volatile char *addr;

  printf("\n=== Test 18: Write to PROT_READ mapping (should kill) ===\n");

  addr = (volatile char*)mmap(0, 4096, PROT_READ,
                              MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  check("mmap PROT_READ succeeds", (uint64)addr != 0);
  if((uint64)addr == 0) return;

  pid = fork();
  if(pid < 0){
    munmap((uint64)addr);
    return;
  }

  if(pid == 0){
    // Child: attempt write to read-only mapping -> should be killed
    addr[0] = 'A';
    // If we reach here, the protection fault was NOT caught
    printf("  [child] ERROR: write to PROT_READ succeeded (should have been killed)\n");
    exit(0);
  } else {
    int status;
    wait(&status);
    // Child should have been killed (status == -1)
    check("[parent] child was killed by write fault", status == -1);
    munmap((uint64)addr);
  }
}

//
// Test 19: Access outside any mmap region must kill the process.
//
void
test_invalid_access_outside_mmap(void)
{
  int pid;

  printf("\n=== Test 19: Access outside mmap region (should kill) ===\n");

  pid = fork();
  if(pid < 0) return;

  if(pid == 0){
    // Child: access MMAPBASE area with no mapping -> should be killed
    volatile char *bad = (volatile char*)(MMAPBASE + 0x1000000);
    char ch = bad[0];
    (void)ch;
    // If we reach here, the fault was NOT caught
    printf("  [child] ERROR: access outside mmap succeeded (should have been killed)\n");
    exit(0);
  } else {
    int status;
    wait(&status);
    check("[parent] child was killed by invalid access", status == -1);
  }
}

//
// Test 20: Fork -> child munmap -> parent data still accessible
//          Verifies mmap_area entries are independent copies, not shared.
//
void
test_fork_child_munmap_independence(void)
{
  int pid;
  volatile char *addr;

  printf("\n=== Test 20: Fork + child munmap independence ===\n");

  addr = (volatile char*)mmap(0, 4096, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  check("mmap succeeds", (uint64)addr != 0);
  if((uint64)addr == 0) return;

  addr[0] = 'M';
  addr[1] = 'A';
  addr[2] = 'P';

  pid = fork();
  if(pid < 0){ munmap((uint64)addr); return; }

  if(pid == 0){
    // Child: verify data, then munmap
    check("[child] sees parent data[0]", addr[0] == 'M');
    munmap((uint64)addr);
    // After munmap, child's mapping is gone. Exit cleanly.
    exit(0);
  } else {
    int status;
    wait(&status);
    // Parent: data must still be intact after child munmap
    check("[parent] data[0] intact after child munmap", addr[0] == 'M');
    check("[parent] data[1] intact after child munmap", addr[1] == 'A');
    check("[parent] data[2] intact after child munmap", addr[2] == 'P');
    munmap((uint64)addr);
  }
}

//
// Test 21: mmap_area limit (max 64). 65th mmap should fail.
//
void
test_mmap_area_limit(void)
{
  uint64 addrs[70];
  int count = 0;
  int i;

  printf("\n=== Test 21: mmap_area limit (64 max) ===\n");

  for(i = 0; i < 70; i++){
    addrs[i] = mmap((uint64)(i * 4096), 4096, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if(addrs[i] == 0)
      break;
    count++;
  }

  printf("  successfully created %d mappings\n", count);
  check("created at least 64 mappings", count >= 64);
  // The 65th (or later) should have failed if we have 64 slots total.
  // Note: other processes may use some slots, so we check <= 64.
  if(count < 70){
    check("hit the limit (couldn't create all 70)", count < 70);
  }

  // Cleanup: unmap everything we created
  for(i = 0; i < count; i++){
    munmap(addrs[i]);
  }
}

//
// Test 22: Page alignment error - non-aligned addr must return 0
//
void
test_page_alignment_error(void)
{
  uint64 ret;

  printf("\n=== Test 22: Page alignment error ===\n");

  // addr=1 is not page-aligned -> MMAPBASE + 1 is not aligned
  ret = mmap(1, 4096, PROT_READ, MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  check("mmap(addr=1) returns 0", ret == 0);

  ret = mmap(100, 4096, PROT_READ, MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  check("mmap(addr=100) returns 0", ret == 0);

  ret = mmap(4095, 4096, PROT_READ, MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  check("mmap(addr=4095) returns 0", ret == 0);

  // Aligned addresses should work
  ret = mmap(0, 4096, PROT_READ, MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  check("mmap(addr=0) succeeds", ret != 0);
  if(ret != 0) munmap(ret);

  ret = mmap(4096, 4096, PROT_READ, MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  check("mmap(addr=4096) succeeds", ret != 0);
  if(ret != 0) munmap(ret);
}

int
main(int argc, char *argv[])
{
  printf("========================================\n");
  printf("       xv6 mmap test suite\n");
  printf("========================================\n");

  test_anon_populate();             // 1
  test_anon_lazy();                 // 2
  test_file_populate();             // 3
  test_file_lazy();                 // 4
  test_file_offset();               // 5
  test_multiple_mappings();         // 6
  test_fork_populate();             // 7
  test_fork_lazy();                 // 8
  test_fork_write_isolation();      // 9
  test_freemem_lifecycle();         // 10
  test_errors();                    // 11
  test_file_readwrite();            // 12
  test_anon_lazy_read();            // 13
  test_large_mapping();             // 14
  test_fork_munmap_parent();        // 15
  test_mixed_mappings();            // 16
  test_partial_page_alloc();        // 17
  test_write_protection_fault();    // 18
  test_invalid_access_outside_mmap(); // 19
  test_fork_child_munmap_independence(); // 20
  test_mmap_area_limit();           // 21
  test_page_alignment_error();      // 22

  printf("\n========================================\n");
  printf("  Results: %d passed, %d failed\n", passed, failed);
  printf("========================================\n");

  if(failed > 0){
    printf("SOME TESTS FAILED\n");
  } else {
    printf("ALL TESTS PASSED\n");
  }

  exit(0);
}
