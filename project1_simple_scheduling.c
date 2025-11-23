/*
[run queue / wait queue 전체 로직 구현 순서]

1. Parent가 CPU를 준다. 
2. Child가 CPU burst를 소모한다.
3. CPU burst 끝난 child는 Parent에게 IO burst를 보낸다. 
4. Parent는 IO burst 메시지를 받는다.  
5. Parent는 child를 run queue에서 wait queue로 이동시킨다.(여기까지 진행함.)
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

// 5단계: wait queue 구조체 추가
typedef struct {
    int pid;
    int io_burst;
} WaitProc;

WaitProc waitQ[100];
int wait_front = 0, wait_rear = 0;

// 7단계: run queue 구조체 추가 (Round Robin용)
int runQ[100]; // run queue 배열 (최대 100개 프로세스 저장 가능)
int run_front = 0 ; // run queue의 앞쪽 인덱스 (dequeue할 위치)
int run_rear = 0; // run queue의 뒤쪽 인덱스 (enqueue할 위치)

// run queue가 비었는지 확인
int is_runQ_empty() {
    return run_front == run_rear;
}

// run queue에 pid 넣기
void enqueue_runQ(int pid) {
    runQ[run_rear] = pid; // rear 위치에 pid 저장
    run_rear++; // rear를 다음 위치로 이동
}

// run queue에서 pid 하나 꺼내기
int dequeue_runQ() {
    if (is_runQ_empty()) return -1; // 큐가 비어있으면 -1 반환
    int pid = runQ[run_front];  // front 위치의 pid 가져오기
    run_front++;   // front를 다음 위치로 이동
    // 너무 많이 커지는 걸 방지하기 위해 앞쪽으로 땡기기 (간단 리셋)
    if (run_front == run_rear) {
        run_front = run_rear = 0;
    }
    return pid;
}

// 5단계: run queue에서 wait queue로 이동시키는 함수 추가
void move_to_waitQ(int pid, int io_burst) {
    waitQ[wait_rear].pid = pid;
    waitQ[wait_rear].io_burst = io_burst;
    wait_rear++;
}

// 6단계: wait queue IO 감소 함수 추가 + 7단계: IO 끝난 프로세스는 run queue로 복귀
void process_waitQ() {
    // 1) IO burst 1씩 감소시키기
    for (int i = wait_front; i < wait_rear; i++) {
        waitQ[i].io_burst--;

        // IO 작업 중인 child 상태 출력
        printf("[Parent] Processing IO... Child %d (remaining IO: %d)\n",
            waitQ[i].pid, waitQ[i].io_burst);
    }

    // 2) IO가 끝난 프로세스들을 run queue로 보내고, waitQ 압축
    int write_idx = wait_front;  // 살아남은 애들만 앞으로 땡기기
    for (int i = wait_front; i < wait_rear; i++) {
        if (waitQ[i].io_burst <= 0) {
            // 7단계: IO 끝 → run queue로 이동
            printf("[Parent] IO complete. Child %d moved back to RUN queue.\n",
                    waitQ[i].pid);
            enqueue_runQ(waitQ[i].pid);
        } else {
            // 아직 IO 남은 애들은 waitQ에 유지
            waitQ[write_idx] = waitQ[i];
            write_idx++;
        }
    }
    // wait_rear 갱신 (front는 0만 쓴다고 보면 됨)
    wait_rear = write_idx;
}


// CPU timeslice 1 tick 보내기
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
        // 1단계: Parent가 CPU를 준다
        msgrcv(msgid, &msg, 0 , getpid(), 0);

        // 2단계: Child가 CPU burst를 소모한다
        cpu_burst--;

        // 3단계: CPU burst 끝난 child는 Parent에게 IO burst를 보낸다
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

    // 3) 10개 child 생성 + run queue에 추가
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
            // 초기에 모든 child를 run queue에 추가
            enqueue_runQ(pid);
        }
    }

    // ---- 부모가 여기서 스케줄링 루프 돌게 될 예정 ----
    while (1) {
        // 1단계: run queue에서 다음 프로세스를 선택하여 CPU를 준다 (Round Robin)
        if (!is_runQ_empty()) {
            int next_pid = dequeue_runQ();
            send_timeslice(msgid, next_pid);
            
            // CPU를 받은 프로세스는 다시 run queue 맨 뒤로 (Round Robin)
            enqueue_runQ(next_pid);
        }

        // 4단계: Parent는 IO burst 메시지를 받는다
        struct msgbuf recv;
        while (msgrcv(msgid, &recv, sizeof(recv) - sizeof(long), 1, IPC_NOWAIT) != -1) {
            printf("[Parent] Child %d requested IO. IO burst = %d\n",
                    recv.pid, recv.value);

            // 5단계: run queue에서 wait queue로 이동
            move_to_waitQ(recv.pid,recv.value);
                
        }
        // 6단계: wait queue 처리
        process_waitQ(); //wait queue IO 감소 함수 호출
        
        sleep(1);
    }

}