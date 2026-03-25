#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NCHILD 5

// ============================
// 1. NICE (boundary + invalid)
// ============================
void test_nice(int pid) {
    printf("\n[TEST] NICE STRESS\n");

    if(setnice(pid, 0) == 0)
        printf("setnice 0 OK\n");

    if(setnice(pid, 39) == 0)
        printf("setnice 39 OK\n");

    if(setnice(pid, 40) == -1)
        printf("invalid high nice rejected\n");

    if(setnice(pid, -5) == -1)
        printf("invalid low nice rejected\n");

    int val = getnice(pid);
    printf("final nice: %d (expected 39)\n", val);
}

// ============================
// 2. PS (multi-process)
// ============================
void test_ps_multi() {
    printf("\n[TEST] PS MULTI PROCESS\n");

    int pids[NCHILD];

    for(int i = 0; i < NCHILD; i++){
        int pid = fork();
        if(pid == 0){
            setnice(getpid(), 5 + i);
            while(1);
        } else {
            pids[i] = pid;
        }
    }

    printf("=== ALL PROCESS LIST ===\n");
    ps(0);

    printf("\n=== EACH CHILD ===\n");
    for(int i = 0; i < NCHILD; i++){
        ps(pids[i]);
    }

    // cleanup
    for(int i = 0; i < NCHILD; i++){
        kill(pids[i]);
        wait(0);
    }
}

// ============================
// 3. MEMINFO
// ============================
void test_meminfo() {
    printf("\n[TEST] MEMINFO\n");

    uint64 before = meminfo();
    printf("Before: %d\n", (int)before);

    int pid = fork();
    if(pid == 0){
        for(int i = 0; i < 50; i++)
            sbrk(4096);
        while(1);
    } else {
        uint64 after = meminfo();
        printf("After alloc: %d\n", (int)after);

        if(after < before)
            printf("memory decreased OK\n");

        kill(pid);
        wait(0);

        uint64 restored = meminfo();
        printf("After free: %d\n", (int)restored);

        if(restored >= after)
            printf("memory restored OK\n");
    }
}

// ============================
// 4. WAITPID (strict)
// ============================
void test_waitpid() {
    printf("\n[TEST] WAITPID\n");

    int pid = fork();
    if(pid == 0){
        exit(0);
    } else {
        if(waitpid(pid) == 0)
            printf("waitpid normal OK\n");

        if(waitpid(pid) == -1)
            printf("double wait rejected\n");
    }

    // non-parent test
    int pid2 = fork();
    if(pid2 == 0){
        while(1);
    } else {
        int fake = fork();
        if(fake == 0){
            if(waitpid(pid2) == -1)
                printf("non-parent wait rejected\n");
            exit(0);
        } else {
            wait(0);
            kill(pid2);
            wait(0);
        }
    }

    if(waitpid(99999) == -1)
        printf("invalid pid rejected\n");
}

// ============================
// 5. UNUSED PROCESS TEST
// ============================
void test_unused() {
    printf("\n[TEST] UNUSED PROCESS\n");

    int dead_pid = fork();
    if(dead_pid == 0){
        exit(0);
    } else {
        wait(0); // -> UNUSED 상태로 돌아감
    }

    // getnice
    if(getnice(dead_pid) == -1)
        printf("getnice UNUSED rejected OK\n");
    else
        printf("ERROR: getnice accepted UNUSED\n");

    // setnice
    if(setnice(dead_pid, 10) == -1)
        printf("setnice UNUSED rejected OK\n");
    else
        printf("ERROR: setnice accepted UNUSED\n");

    // ps 단일
    printf("ps(dead_pid) should print NOTHING:\n");
    ps(dead_pid);

    // ps 전체
    printf("ps(0) should NOT include dead_pid:\n");
    ps(0);
}

// ============================
// MAIN
// ============================
int
main(int argc, char *argv[])
{
    printf("===== TEST START =====\n");

    int pid = getpid();

    test_nice(pid);
    test_ps_multi();
    test_meminfo();
    test_waitpid();
    test_unused();

    printf("\n===== TEST END =====\n");

    exit(0);
}