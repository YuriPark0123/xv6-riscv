#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define N 3

void busy_loop() {
    volatile int x = 0;
    while(1) {
        x++; // CPU 사용
    }
}

int
main(int argc, char *argv[])
{
    printf("===== EEVDF SCHEDULING TEST START =====\n");

    int pids[N];

    // -------------------------------
    // 1. 서로 다른 nice 값으로 fork
    // -------------------------------
    for(int i = 0; i < N; i++){
        int pid = fork();
        if(pid == 0){
            int mypid = getpid();

            if(i == 0){
                setnice(mypid, 5);   // 높은 우선순위
                printf("Child %d: nice=5 (HIGH PRIORITY)\n", mypid);
            }
            else if(i == 1){
                setnice(mypid, 20);  // 기본
                printf("Child %d: nice=20 (NORMAL)\n", mypid);
            }
            else{
                setnice(mypid, 35);  // 낮은 우선순위
                printf("Child %d: nice=35 (LOW PRIORITY)\n", mypid);
            }

            busy_loop(); // CPU-bound
            exit(0);
        }
        else{
            pids[i] = pid;
        }
    }

    // -------------------------------
    // 2. 일정 시간 동안 스케줄링 관찰
    // -------------------------------
    printf("\n[INFO] Letting scheduler run...\n");

    // sleep 없으므로 간단한 delay loop
    for(volatile int i = 0; i < 100000000; i++);

    // -------------------------------
    // 3. ps 출력 (핵심 검증)
    // -------------------------------
    printf("\n===== PROCESS STATUS (ps) =====\n");
    ps(0);

    printf("\n[CHECK POINT]\n");
    printf("1. runtime: nice 낮을수록 커야 함\n");
    printf("2. vruntime: 서로 비슷해야 함 (fairness)\n");
    printf("3. vdeadline: 낮은 nice가 더 빠름\n");
    printf("4. eligibility: runnable 중 일부만 eligible\n");

    // -------------------------------
    // 4. cleanup
    // -------------------------------
    for(int i = 0; i < N; i++){
        kill(pids[i]);
    }
    for(int i = 0; i < N; i++){
        wait(0);
    }

    printf("\n===== EEVDF SCHEDULING TEST END =====\n");
    exit(0);
}