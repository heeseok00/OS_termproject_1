/*
[run queue / wait queue 전체 로직]

1. Parent가 CPU를 준다. 
2. Child가 CPU burst를 소모한다.
3. CPU burst 끝난 child는 Parent에게 IO burst를 보낸다. 
4. Parent는 IO burst 메시지를 받는다.  (여기까지 진행함.)
5. Parent는 child를 run queue에서 wait queue로 이동시킨다.
6. Parent는 wait queue에서 IO burst를 감소시킨다.
7. IO burst가 끝나면 다시 run queue로 보내서 schedule 반복한다.
*/



// Round Robin, 타이머, IPC, fork 등에 필요한 헤더들
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/types.h>
#include <sys/time.h>
#include <string.h>
#include <sys/wait.h>

// 부모가 자식에게 보내는 메시지 구조체

struct msgbuf {
    long mtype; // 메시지 타입입
    int pid; // 전달자 PID
    int value; //burst time
};

// wait queue 구조체
typedef struct {
    int pid;
    int io_burst;
} WaitProc;

WaitProc waitQ[100];
int wait_front = 0, wait_rear = 0;

void move_to_waitQ(int pid, int io_burst) {
    waitQ[wait_rear].pid = pid;
    waitQ[wait_rear].io_burst = io_burst;
    wait_rear++;
}

void send_timeslice(int msgid, pid_t child_pid) {
struct msgbuf msg;

msg.mtype = child_pid; // 이 메시지는 child_pid에게 배달된다.
msg.value = 1; //CPU time slice = 1 tick
msgsnd(msgid,&msg,0,0);
}

// Child 코드
void exe_child(int msgid) {
    struct msgbuf msg;

    int cpu_burst = rand() % 5 + 1;  // CPU burst 랜덤 시작값 (1~5)

    while (1) {
        // 1) 부모가 보내는 메시지를 기다림
        msgrcv(msgid, &msg, 0 , getpid(), 0);

        // 2) 메시지를 받으면 CPU burst를 1 감소
        cpu_burst--;

        // 3) CPU burst가 끝났으면 부모에게 IO 요청 메시지 보내기
        if (cpu_burst <= 0) {
            struct msgbuf send;
            send.mtype = 1;      // 부모가 받을 타입 (IO 보고용)
            send.pid = getpid();  // 누가 보냈는지 알려주기  
            send.value = rand() % 5 + 1;   // IO burst 랜덤 생성 (1~5)
            msgsnd(msgid, &send, sizeof(send)  - sizeof(long), 0);  // 메시지 안에서 mtype(=long) 제외한 나머지 (pid + value) 데이터 크기만 보내라는 뜻. 

            cpu_burst = rand() % 5 + 1; // IO 후에 다시 새로운 CPU burst 시작
        }
    }
}


int main() {

    // 1) 메시지 큐 생성
    int msgid = msgget((key_t)1234, IPC_CREAT | 0666);
    if (msgid == -1) {
        perror("msgget");
        exit(1);
    }

    // 2) child PID 저장 배열
    pid_t pids[10];

    // 3) 10개 child 생성
    for (int i = 0; i < 10; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            exit(1);
        }

        if (pid == 0) {
            // ---- child code ----
            exe_child(msgid);
            exit(0);
        } else {
            // parent에 저장
            pids[i] = pid;
        }
    }

    // ---- 부모가 여기서 스케줄링 루프 돌게 될 예정 ----
    while (1) {
        // 1) child 0에게 CPU 1 tick 부여 (임시 테스트용)
        send_timeslice(msgid, pids[0]);

        // 2) IO 메시지 받기 (child -> parent)
        struct msgbuf recv;
        while (msgrcv(msgid, &recv, sizeof(recv) - sizeof(long), 1, IPC_NOWAIT) != -1) {
            printf("[Parent] Child %d requested IO. IO burst = %d\n",
                    recv.pid, recv.value);

            move_to_waitQ(recv.pid,recv.value); 
                
        }

        
        sleep(1);
    }

}