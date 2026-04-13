#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Spin for ~tick units of busy work to give the scheduler something to track.
static void
burn_cpu(void)
{
  while(1)
    ;
}

static void
banner(const char *msg)
{
  printf("\n========================================\n");
  printf(" %s\n", msg);
  printf("========================================\n");
}

static void
reap(int *pids, int n)
{
  for(int i = 0; i < n; i++)
    kill(pids[i]);
  for(int i = 0; i < n; i++)
    wait(0);
}

// Test 1 ─ Two children with very different priorities.
// Goal: verify runtime ratio matches weight ratio,
//       and runtime/weight values converge (fairness).
static void
test_two_priorities(void)
{
  banner("Test 1: nice=0 vs nice=10  (weights 88761 vs 9548)");

  int pids[2];

  pids[0] = fork();
  if(pids[0] == 0){
    setnice(getpid(), 0);
    burn_cpu();
  }

  pids[1] = fork();
  if(pids[1] == 0){
    setnice(getpid(), 10);
    burn_cpu();
  }

  pause(500);
  ps(0);

  printf("\nExpected weight ratio  : 88761 / 9548  ~= 9.3\n");
  printf("Expected runtime ratio : ~9.3 (high priority gets more)\n");
  printf("Expected runtime/weight: should be roughly equal (fairness)\n");

  reap(pids, 2);
}

// Test 2 ─ Three children with the same nice value.
// Goal: verify equal runtime distribution among same-priority tasks.
static void
test_equal_priority(void)
{
  banner("Test 2: three children, all nice=20");

  int pids[3];
  for(int i = 0; i < 3; i++){
    pids[i] = fork();
    if(pids[i] == 0)
      burn_cpu();
  }

  pause(500);
  ps(0);

  printf("\nExpected: runtime values within a few percent of each other\n");
  printf("Expected: vruntime values nearly identical\n");

  reap(pids, 3);
}

// Test 3 ─ Three children spanning the nice range.
// Goal: see clear hierarchy of runtimes.
static void
test_wide_range(void)
{
  banner("Test 3: nice=0, 20, 39  (extreme weight spread)");

  int pids[3];
  int nices[3] = {0, 20, 39};

  for(int i = 0; i < 3; i++){
    pids[i] = fork();
    if(pids[i] == 0){
      setnice(getpid(), nices[i]);
      burn_cpu();
    }
  }

  pause(500);
  ps(0);

  printf("\nExpected: nice=0 dominates (weight 88761)\n");
  printf("Expected: nice=39 (weight 15) gets ~1 time slice and stalls\n");
  printf("          since its vruntime explodes after running\n");

  reap(pids, 3);
}

// Test 4 ─ setnice() in the middle of execution.
// Goal: verify that vdeadline is recalculated when nice changes,
//       so the priority change actually takes effect.
static void
test_dynamic_nice(void)
{
  banner("Test 4: dynamic setnice during runtime");

  int pids[2];

  pids[0] = fork();
  if(pids[0] == 0){
    // start as low priority
    setnice(getpid(), 30);
    burn_cpu();
  }

  pids[1] = fork();
  if(pids[1] == 0){
    // start as high priority
    setnice(getpid(), 5);
    burn_cpu();
  }

  pause(300);
  printf("\n--- before swap (pid %d nice=30, pid %d nice=5) ---\n",
         pids[0], pids[1]);
  ps(0);

  // Swap priorities mid-flight
  setnice(pids[0], 5);
  setnice(pids[1], 30);

  pause(300);
  printf("\n--- after swap  (pid %d nice=5,  pid %d nice=30) ---\n",
         pids[0], pids[1]);
  ps(0);

  printf("\nExpected: after the swap, pid %d's runtime growth accelerates\n", pids[0]);
  printf("          and pid %d's growth slows down\n", pids[1]);

  reap(pids, 2);
}

// Test 5 ─ fork() vruntime inheritance.
// Goal: verify a newly forked child inherits parent's vruntime,
//       so it can't unfairly cut in line by starting at 0.
static void
test_fork_inheritance(void)
{
  banner("Test 5: fork inherits parent vruntime");

  // First, give the parent some vruntime by spinning briefly.
  int warmer = fork();
  if(warmer == 0){
    burn_cpu();
  }
  pause(200);
  kill(warmer);
  wait(0);

  printf("--- after parent warm-up ---\n");
  ps(getpid());

  // Now fork a child and immediately observe.
  int child = fork();
  if(child == 0){
    burn_cpu();
  }
  pause(50);

  printf("\n--- child should start near parent's vruntime, not 0 ---\n");
  ps(0);

  kill(child);
  wait(0);
}

// Test 6 ─ ps invariants.
// Goal: spot-check ps output requirements.
static void
test_ps_invariants(void)
{
  banner("Test 6: ps invariants");

  int pids[2];
  pids[0] = fork();
  if(pids[0] == 0){
    setnice(getpid(), 0);
    burn_cpu();
  }
  pids[1] = fork();
  if(pids[1] == 0){
    setnice(getpid(), 20);
    burn_cpu();
  }
  pause(200);

  printf("ps(0): all processes\n");
  ps(0);

  printf("\nps(%d): only this child\n", pids[0]);
  ps(pids[0]);

  printf("\nps(99999): nonexistent  -- should print nothing below\n");
  ps(99999);
  printf("(end of ps(99999))\n");

  printf("\nChecklist:\n");
  printf("  [ ] header has a single 'tick NNN' value (global, not per-proc)\n");
  printf("  [ ] runtime/weight values are similar across runnable children\n");
  printf("  [ ] vruntime values are similar across runnable children\n");
  printf("  [ ] high-priority child has earlier vdeadline\n");
  printf("  [ ] at least one runnable child is is_eligible=true\n");

  reap(pids, 2);
}

static void
test_vruntime_correctness(void)
{
  banner("Test 7: vruntime correctness (mathematical)");

  int pids[2];

  // 서로 다른 nice
  pids[0] = fork();
  if(pids[0] == 0){
    setnice(getpid(), 0);  // weight 큼
    burn_cpu();
  }

  pids[1] = fork();
  if(pids[1] == 0){
    setnice(getpid(), 20); // baseline
    burn_cpu();
  }

  pause(400);
  ps(0);

  printf("\n[CRITICAL CHECK]\n");
  printf("vruntime = runtime * (1024 / weight)\n");
  printf("→ weight 큰 프로세스는 vruntime 증가가 느려야 함\n");

  printf("\nExpected:\n");
  printf("  high priority: runtime ↑ but vruntime ≈ others\n");
  printf("  low  priority: runtime ↓ but vruntime ≈ others\n");

  reap(pids, 2);
}

static void
test_deadline_ordering(void)
{
  banner("Test 8: virtual deadline ordering");

  int pids[3];
  int nices[3] = {0, 20, 30};

  for(int i = 0; i < 3; i++){
    pids[i] = fork();
    if(pids[i] == 0){
      setnice(getpid(), nices[i]);
      burn_cpu();
    }
  }

  pause(400);
  ps(0);

  printf("\n[CRITICAL CHECK]\n");
  printf("scheduler picks smallest vdeadline\n");

  printf("\nExpected order (small → large):\n");
  printf("  nice=0  (earliest deadline)\n");
  printf("  nice=20\n");
  printf("  nice=30 (latest deadline)\n");

  reap(pids, 3);
}

static void
test_eligibility(void)
{
  banner("Test 9: eligibility correctness");

  int pids[3];

  for(int i = 0; i < 3; i++){
    pids[i] = fork();
    if(pids[i] == 0){
      burn_cpu();
    }
  }

  pause(300);
  ps(0);

  printf("\n[CRITICAL CHECK]\n");
  printf("eligibility:\n");
  printf("  not all processes should be eligible at same time\n");

  printf("\nExpected:\n");
  printf("  some processes: eligible=1\n");
  printf("  others        : eligible=0\n");

  printf("\nIf ALL are eligible → WRONG implementation\n");

  reap(pids, 3);
}

static void
test_timeslice(void)
{
  banner("Test 10: timeslice enforcement");

  int pid = fork();
  if(pid == 0){
    setnice(getpid(), 0);
    burn_cpu();
  }

  pause(200);
  ps(pid);

  printf("\n[CRITICAL CHECK]\n");
  printf("process should not run continuously\n");
  printf("→ runtime should grow in chunks (~5 ticks)\n");

  kill(pid);
  wait(0);
}

int
main(int argc, char *argv[])
{
  printf("###### EEVDF SCHEDULER TEST ######\n");

  test_two_priorities();
  test_equal_priority();
  test_wide_range();
  test_dynamic_nice();
  test_fork_inheritance();
  test_ps_invariants();
  test_vruntime_correctness();
  test_deadline_ordering();
  test_eligibility();
  test_timeslice();

  printf("\n###### ALL TESTS COMPLETE ######\n");
  exit(0);
}
