/*
Round-Robin 스케줄링 구현

[전체 실행 흐름]

1. 초기화 단계
- 메시지 큐 생성 및 로그 파일 열기
- 10개 child 프로세스 생성 및 run queue 초기화
- 타이머 설정 (setitimer) 및 SIGALRM 핸들러 등록

2. 스케줄링 루프 (각 time tick마다 반복)
a) 이전 틱에서 실행 중이던 프로세스 처리
    - 타임 퀀텀 감소 및 CPU burst 추적
    - 타임 퀀텀 남아있으면 run queue에 재등록

b) 새 프로세스 선택 및 CPU 할당
    - run queue에서 다음 프로세스 선택 (Round-Robin)
    - IPC 메시지로 time slice 전송

c) Child 프로세스 동작
    - time slice 수신 시 CPU burst 감소
    - CPU burst 완료 시 IO 요청 메시지 전송

d) IO 요청 처리
    - child의 IO 요청 메시지 수신
    - run queue에서 해당 프로세스 제거
    - wait queue로 이동

e) Wait queue 처리
    - 모든 프로세스의 IO burst 감소
    - IO 완료된 프로세스는 run queue로 복귀

f) 로그 출력
    - 현재 시간, 실행 중인 프로세스, 남은 CPU burst
    - run queue 및 wait queue 상태를 파일에 기록

3. 종료 단계
- 모든 child 프로세스 종료 및 정리
- 메시지 큐 및 로그 파일 닫기
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
#include <time.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>

#define MAX_TICKS 10000
#define TICK_INTERVAL_USEC 10000 // 10ms tick -> 100s total runtime for 10,000 ticks
#define TIME_QUANTUM 3 // 타임 퀀텀 (ticks)
#define MAX_CHILDREN 10
#define QUEUE_SIZE 100

volatile sig_atomic_t tick_flag = 0;
volatile sig_atomic_t tick_count = 0;

void handle_tick(int signo) {
    (void)signo;
    tick_flag = 1;
    tick_count++;
}

// 부모가 자식에게 보내는 메시지 구조체
struct msgbuf {
    long mtype; // 메시지 타입
    int pid; // 전달자 PID
    int value; //burst time
};

// Child 상태 추적 구조체
typedef struct {
    int pid;
    int remaining_quantum;      // 남은 타임 퀀텀
    int remaining_cpu_burst;     // 남은 CPU burst 
    int waiting_time;            // 대기 시간
    int total_runtime;            // 총 실행 시간
} ChildState;

// 5단계: wait queue 구조체 추가
typedef struct {
    int pid;
    int io_burst;
} WaitProc;

// 전역 변수
ChildState child_states[MAX_CHILDREN];
int child_count = 0;
WaitProc waitQ[QUEUE_SIZE];
int wait_front = 0, wait_rear = 0;
int runQ[QUEUE_SIZE]; // run queue 배열
int run_front = 0, run_rear = 0;
int current_running_pid = -1; // 현재 실행 중인 프로세스
int log_fd = -1; // 로그 파일 디스크립터

// ChildState 찾기
ChildState* find_child_state(int pid) {
    for (int i = 0; i < child_count; i++) {
        if (child_states[i].pid == pid) {
            return &child_states[i];
        }
    }
    return NULL;
}

// run queue가 비었는지 확인
int is_runQ_empty() {
    return run_front == run_rear;
}

// run queue에 pid 넣기
void enqueue_runQ(int pid) {
    if (run_rear >= QUEUE_SIZE) {
        // 앞부분에 여유가 있으면 인덱스를 재사용
        if (run_front > 0) {
            int j = 0;
            for (int i = run_front; i < run_rear; i++) {
                runQ[j++] = runQ[i];
            }
            run_front = 0;
            run_rear = j;
        }
        if (run_rear >= QUEUE_SIZE) {
            fprintf(stderr, "Error: run queue overflow!\n");
            return;
        }
    }
    runQ[run_rear] = pid;
    run_rear++;
}

// run queue에서 pid 하나 꺼내기
int dequeue_runQ() {
    if (is_runQ_empty()) return -1;
    int pid = runQ[run_front];
    run_front++;
    if (run_front == run_rear) {
        run_front = run_rear = 0;
    }
    return pid;
}

// run queue에서 특정 pid 제거 (IO 요청 시 사용)
void remove_from_runQ(int pid) {
    int write_idx = run_front;
    for (int i = run_front; i < run_rear; i++) {
        if (runQ[i] != pid) {
            runQ[write_idx] = runQ[i];
            write_idx++;
        }
    }
    run_rear = write_idx;
}

// 5단계: run queue에서 wait queue로 이동시키는 함수 추가
void move_to_waitQ(int pid, int io_burst) {
    // wait queue가 가득 찼는지 확인 (배열 인덱스는 0부터 QUEUE_SIZE-1까지)
    // wait_rear가 QUEUE_SIZE이면 이미 가득 찬 상태
    // process_waitQ()가 매 틱마다 먼저 호출되므로 이론적으로는 발생하지 않아야 함
    if (wait_rear >= QUEUE_SIZE) {
        fprintf(stderr, "Error: wait queue overflow! wait_rear=%d, QUEUE_SIZE=%d\n", wait_rear, QUEUE_SIZE);
        return;
    }
    // run queue에서 제거
    remove_from_runQ(pid);
    // wait queue에 추가
    waitQ[wait_rear].pid = pid;
    waitQ[wait_rear].io_burst = io_burst;
    wait_rear++;
    
    // ChildState 업데이트
    ChildState* state = find_child_state(pid);
    if (state) {
        state->waiting_time = 0; // 대기 시작
    }
}

// 6단계: wait queue IO 감소 함수 추가 + 7단계: IO 끝난 프로세스는 run queue로 복귀
void process_waitQ() {
    // 1) IO burst 1씩 감소시키기
    for (int i = wait_front; i < wait_rear; i++) {
        waitQ[i].io_burst--;
        
        // ChildState 업데이트
        ChildState* state = find_child_state(waitQ[i].pid);
        if (state) {
            state->waiting_time++;
        }
    }

    // 2) IO가 끝난 프로세스들을 run queue로 보내고, waitQ 재구성
    int write_idx = 0;
    for (int i = wait_front; i < wait_rear; i++) {
        if (waitQ[i].io_burst <= 0) {
            // 7단계: IO 끝 → run queue로 이동
            enqueue_runQ(waitQ[i].pid);
            
            // ChildState 업데이트: 새로운 CPU burst 시작
            ChildState* state = find_child_state(waitQ[i].pid);
            if (state) {
                state->remaining_cpu_burst = rand() % 5 + 1;
                state->remaining_quantum = TIME_QUANTUM;
            }
        } else {
            waitQ[write_idx] = waitQ[i];
            write_idx++;
        }
    }
    wait_front = 0;
    wait_rear = write_idx;
}

// run queue 덤프 문자열 생성
void dump_runQ(char* buf, size_t buf_size) {
    int pos = 0;
    pos += snprintf(buf + pos, buf_size - pos, "runQ:[");
    if (is_runQ_empty()) {
        pos += snprintf(buf + pos, buf_size - pos, "empty");
    } else {
        for (int i = run_front; i < run_rear; i++) {
            if (i > run_front) pos += snprintf(buf + pos, buf_size - pos, ",");
            pos += snprintf(buf + pos, buf_size - pos, "%d", runQ[i]);
        }
    }
    pos += snprintf(buf + pos, buf_size - pos, "]");
}

// wait queue 덤프 문자열 생성
void dump_waitQ(char* buf, size_t buf_size) {
    int pos = 0;
    pos += snprintf(buf + pos, buf_size - pos, "waitQ:[");
    if (wait_front == wait_rear) {
        pos += snprintf(buf + pos, buf_size - pos, "empty");
    } else {
        for (int i = wait_front; i < wait_rear; i++) {
            if (i > wait_front) pos += snprintf(buf + pos, buf_size - pos, ",");
            pos += snprintf(buf + pos, buf_size - pos, "(%d,%d)", waitQ[i].pid, waitQ[i].io_burst);
        }
    }
    pos += snprintf(buf + pos, buf_size - pos, "]");
}

// CPU timeslice 1 tick 보내기
void send_timeslice(int msgid, pid_t child_pid) {
    struct msgbuf msg;
    msg.mtype = child_pid;
    msg.pid = child_pid; // 보낼 대상 PID 기록 (일관성 유지용)
    msg.value = 1; //CPU time slice = 1 tick
    msgsnd(msgid, &msg, sizeof(msg) - sizeof(long), 0);
}

// child용 SIGTERM 핸들러
void child_term_handler(int signo) {
    (void)signo;
    exit(0);
}

// Child 코드
void exe_child(int msgid) {
    struct msgbuf msg;
    int cpu_burst = rand() % 5 + 1;  // CPU burst 랜덤 시작값 (1~5)

    // SIGTERM 시 정상 종료
    signal(SIGTERM, child_term_handler);

    while (1) {
        // 1단계: Parent가 CPU를 준다
        msgrcv(msgid, &msg, sizeof(msg) - sizeof(long), getpid(), 0);

        // 2단계: Child가 CPU burst를 소모한다
        cpu_burst--;

        // 3단계: CPU burst 끝난 child는 Parent에게 IO burst를 보낸다
        if (cpu_burst <= 0) {
            struct msgbuf send;
            send.mtype = 1;      // 부모가 받을 타입 (IO 보고용)
            send.pid = getpid();
            send.value = rand() % 5 + 1;   // IO burst 랜덤 생성 (1~5)
            msgsnd(msgid, &send, sizeof(send) - sizeof(long), 0);

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

    // 2) 로그 파일 열기
    log_fd = open("schedule_dump.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd == -1) {
        perror("open schedule_dump.txt");
        exit(1);
    }

    // 3) child PID 저장 배열
    pid_t pids[MAX_CHILDREN];

    // 4) 10개 child 생성 + run queue에 추가
    srand(time(NULL));
    for (int i = 0; i < MAX_CHILDREN; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            exit(1);
        }

        if (pid == 0) {
            // child code
            exe_child(msgid);
            exit(0);
        } else {
            // parent에 저장
            pids[i] = pid;
            
            // ChildState 초기화
            child_states[child_count].pid = pid;
            child_states[child_count].remaining_quantum = TIME_QUANTUM;
            child_states[child_count].remaining_cpu_burst = rand() % 5 + 1; // 초기값 추정
            child_states[child_count].waiting_time = 0;
            child_states[child_count].total_runtime = 0;
            child_count++;
            
            // 초기에 모든 child를 run queue에 추가
            enqueue_runQ(pid);
        }
    }

    // 5) 타이머 설정 및 SIGALRM 핸들러 등록
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_tick;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = TICK_INTERVAL_USEC;
    timer.it_interval = timer.it_value;
    if (setitimer(ITIMER_REAL, &timer, NULL) == -1) {
        perror("setitimer");
        exit(1);
    }

    //부모가 여기서 스케줄링 루프 돌게 될 예정
    while (tick_count < MAX_TICKS) {
        while (!tick_flag) {
            pause();
        }
        tick_flag = 0;

        // a) 이전 틱에서 실행 중이던 프로세스 처리
        if (current_running_pid != -1) {
            ChildState* state = find_child_state(current_running_pid);
            if (state) {
                state->remaining_quantum--;
                state->total_runtime++;
                
                // 타임 퀀텀 소진 여부 체크
                // 타임 퀀텀 소진되었으면 리셋 후 재enqueue (Round-Robin)
                if (state->remaining_quantum <= 0) {
                    // 타임 퀀텀 소진 시 리셋
                    state->remaining_quantum = TIME_QUANTUM;
                }
                enqueue_runQ(current_running_pid);
                // CPU burst 완료는 IO 요청으로 처리됨
            }
            current_running_pid = -1;
        }

        // d) run queue에서 dequeue → timeslice 전송
        int next_pid = -1;
        if (!is_runQ_empty()) {
            next_pid = dequeue_runQ();
            send_timeslice(msgid, next_pid);
            current_running_pid = next_pid;
        }

        // f) wait queue IO 감소 및 완료된 프로세스 run queue 복귀 (먼저 처리하여 공간 확보)
        process_waitQ();

        // e) 자식 IO 요청 수신 → run queue에서 제거 → wait queue 이동
        struct msgbuf recv;
        while (msgrcv(msgid, &recv, sizeof(recv) - sizeof(long), 1, IPC_NOWAIT) != -1) {
            // wait queue가 가득 찼으면 process_waitQ() 호출하여 공간 확보
            if (wait_rear >= QUEUE_SIZE) {
                process_waitQ();
            }
            
            // run queue에서 제거하고 wait queue로 이동
            if (current_running_pid == recv.pid) {
                current_running_pid = -1;
            }
            // run queue에서 제거 (move_to_waitQ 내부에서 처리됨)
            move_to_waitQ(recv.pid, recv.value);
            
            // ChildState 업데이트: CPU burst 완료
            ChildState* state = find_child_state(recv.pid);
            if (state) {
                state->remaining_cpu_burst = 0; // IO 요청 시 CPU burst 완료
                state->remaining_quantum = TIME_QUANTUM; // 다음 실행을 위해 리셋
            }
        }

        // 로그 파일 출력
        char log_buf[1024];
        char runQ_buf[512];
        char waitQ_buf[512];
        dump_runQ(runQ_buf, sizeof(runQ_buf));
        dump_waitQ(waitQ_buf, sizeof(waitQ_buf));
        
        int remaining_cpu = -1;
        if (current_running_pid != -1) {
            ChildState* state = find_child_state(current_running_pid);
            if (state) {
                remaining_cpu = state->remaining_cpu_burst;
            }
        }
        
        int len = snprintf(log_buf, sizeof(log_buf),
            "t=%d, pid=%d gets CPU, remaining CPU-burst=%d, %s, %s\n",
            (int)tick_count,
            current_running_pid != -1 ? current_running_pid : -1,
            remaining_cpu,
            runQ_buf,
            waitQ_buf);
        
        if (len > 0 && len < (int)sizeof(log_buf)) {
            write(log_fd, log_buf, len);
        }
    }

    // 모든 child 정리
    for (int i = 0; i < MAX_CHILDREN; i++) {
        kill(pids[i], SIGTERM);
    }
    
    // 모든 child 종료 대기
    for (int i = 0; i < MAX_CHILDREN; i++) {
        waitpid(pids[i], NULL, 0);
    }
    
    // 메시지 큐 정리
    msgctl(msgid, IPC_RMID, NULL);
    
    // 로그 파일 닫기
    close(log_fd);
    
    return 0;
}
