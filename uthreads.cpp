#include <stdio.h>
#include <setjmp.h>
#include <csignal>
#include <iostream>
#include "uthreads.h"
#include <deque>
#include <sys/time.h>
#include <set>
#include <algorithm>

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

#define MAX_THREAD_NUM 100 /* maximal number of threads */
#define STACK_SIZE 4096 /* stack size per thread (in bytes) */

#define SECOND 1000000
#define SYSTEM_ERROR "system error: "
#define THREAD_ERROR "thread library error: "
#define QUANTUM_ERROR "Quantum_usecs is not positive"
#define MAX_THREAD_NUM_ERROR "Exceeds maximum number of threads"
#define INVALID_ID_ERROR "The thread id is invalid"
#define NO_THREAD_ERROR "The thread was not terminated, no such thread"
#define BLOCKING_MAIN_THREAD_ERROR "It isn't possible to block the main thread"
#define SIGACTION_ERROR "sigaction error"
#define SETTIME_ERROR "set-timer error"
#define QUANTUM_NUM_ERROR "The quantums number is illegal"

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
            : "=g" (ret)
            : "0" (addr));
    return ret;
}
enum State {
    RUNNING, READY, BLOCKED
};

class Thread {
public:
    int tid;
    State state;
    sigjmp_buf env;
    char* stack;
    thread_entry_point entry_point;
    int num_quantum;
    int wakeup_quantum;
    bool is_blocked = false;

    Thread(int tid, char *stack, thread_entry_point entry_point)
    {
        // initializes env[tid] to use the right stack,
        // and to run from the function 'entry_point', when we'll use
        // siglongjmp to jump into the thread.
        this->tid = tid;
        address_t sp = (address_t) stack + STACK_SIZE - sizeof(address_t);
        address_t pc = (address_t) entry_point;
        sigsetjmp(env, 1);
        num_quantum = 0;
        wakeup_quantum = 0;
        (env->__jmpbuf)[JB_SP] = translate_address(sp);
        (env->__jmpbuf)[JB_PC] = translate_address(pc);
        sigemptyset(&env->__saved_mask);
    }
    Thread(int tid): tid(tid), state(RUNNING), num_quantum(1){};

    ~Thread(){
        delete[] stack;
    }
};

class ThreadScheduler{
public:
    std::deque<Thread*> threads_queue;
    int total_quantum;
    std::set<int> avaliable_tids;
    int quantum;
    sigset_t* signals;
    Thread** all_threads;
    std::set<Thread*> sleeping_threads;
    ThreadScheduler(){
        total_quantum = 1;
        sigemptyset(signals);
        sigaddset(signals, SIGVTALRM);

        all_threads = new Thread*[MAX_THREAD_NUM];
        for (int i = 0; i < MAX_THREAD_NUM; i++) {
            all_threads[i] = nullptr;
        }
        threads_queue = *new std::deque<Thread*>();
        sleeping_threads = *new std::set<Thread*>();

    }
    ~ThreadScheduler(){

        for (int i = 0; i < MAX_THREAD_NUM; i++){
            if (all_threads[i] != nullptr){
                delete all_threads[i];
            }
        }
        delete all_threads;
    }
};
ThreadScheduler *scheduler = new ThreadScheduler();
struct sigaction sa = {0};
struct itimerval timer;


void wake_up_threads(){
    if(scheduler->sleeping_threads.empty()){
        return;
    }
    auto it = scheduler->sleeping_threads.begin();
    while (it != scheduler->sleeping_threads.end()){
        if ((*it)->wakeup_quantum == scheduler->total_quantum){
            (*it)->wakeup_quantum = 0;
            if (!((*it)->is_blocked)){
                (*it)->state = READY;
                scheduler->threads_queue.push_back(*it);
            }
            it = scheduler->sleeping_threads.erase(it);
        }
        else{
            it++;
        }
    }
}

void increase_quantum(Thread* next_thread) {
    scheduler->total_quantum++;
    next_thread->num_quantum++;
    wake_up_threads();
}

void timer_handler(int sig){
    sigprocmask(SIG_BLOCK, scheduler->signals, nullptr);
    if(!scheduler->threads_queue.empty()){
        scheduler->total_quantum++;
        wake_up_threads();
        Thread* cur_thread = scheduler->threads_queue.front();
        scheduler->threads_queue.pop_front();
        cur_thread->state = READY;
        scheduler->threads_queue.push_back(cur_thread);
        Thread* next_thread = scheduler->threads_queue.front();
        next_thread->state = RUNNING;
        next_thread->num_quantum++;
        int ret_val = sigsetjmp(cur_thread->env, 1);
        bool did_just_save_bookmark = ret_val == 0;
        if(did_just_save_bookmark){
            sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
            siglongjmp(next_thread->env, 1);
        }
    }
    sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
}



void reset_timer(int quantum_usecs){
    sa.sa_handler = &timer_handler;
    if (sigaction(SIGVTALRM, &sa, NULL) < 0)
    {
        std::cerr << SYSTEM_ERROR << SIGACTION_ERROR << std::endl;
        exit(1);
    }
    timer.it_value.tv_sec = quantum_usecs / SECOND;
    timer.it_value.tv_usec = quantum_usecs % SECOND;

    timer.it_interval.tv_sec = quantum_usecs / SECOND;
    timer.it_interval.tv_usec = quantum_usecs % SECOND;
    if (setitimer(ITIMER_VIRTUAL, &timer, NULL))
    {
        std::cerr << SYSTEM_ERROR << SETTIME_ERROR << std::endl;
        exit(1);
    }
}

int uthread_init(int quantum_usecs){
    if(quantum_usecs <= 0){
        std::cerr << THREAD_ERROR << QUANTUM_ERROR << std::endl;
        return -1;
    }
    Thread *main_thread = new Thread(0);
    scheduler->threads_queue.push_back(main_thread);
    scheduler->all_threads[main_thread->tid] = main_thread;
    scheduler->quantum = quantum_usecs;
    reset_timer(quantum_usecs);
    return 0;
}

int uthread_spawn(thread_entry_point entry_point){
    sigprocmask(SIG_BLOCK, scheduler->signals, nullptr);
    for (int i = 1; i < MAX_THREAD_NUM; ++i) {
        if (scheduler->all_threads[i] == nullptr){
            char* stack_pointer = new char[STACK_SIZE];
            scheduler->all_threads[i] = new Thread(i, stack_pointer,
                                                   entry_point);
            scheduler->all_threads[i]->state = READY;
            scheduler->threads_queue.push_back(scheduler->all_threads[i]);
            sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
            return i;
        }
    }
    std::cerr << THREAD_ERROR << MAX_THREAD_NUM_ERROR << std::endl;
    sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
    return -1;
}

int uthread_terminate(int tid){
    sigprocmask(SIG_BLOCK, scheduler->signals, nullptr);
    if(tid < 0 || tid >= MAX_THREAD_NUM){
        // Invalid id
        std::cerr << THREAD_ERROR << INVALID_ID_ERROR << std::endl;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return -1;
    }
    if (tid == 0){
        // Terminate the main thread, ends the whole process.
        delete scheduler;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        exit(0);
    }
    Thread* cur_thread = scheduler->all_threads[tid];

    if (cur_thread == nullptr){
        // No such thread.
        std::cerr << THREAD_ERROR << NO_THREAD_ERROR << std::endl;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return -1;
    }

    if(tid == scheduler->threads_queue.front()->tid){
        // Thread is Running now
        scheduler->threads_queue.pop_front();
        delete cur_thread;
        scheduler->all_threads[tid] = nullptr;
        reset_timer(scheduler->quantum);
        scheduler->threads_queue.front()->state = RUNNING;
        increase_quantum(scheduler->threads_queue.front());
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        siglongjmp(scheduler->threads_queue.front()->env, 1);
    }
    else{
        for (auto it = scheduler->threads_queue.begin();
        it != scheduler->threads_queue.end(); it++){
            // Thread is not running now
            if((*it)->tid == tid){
                scheduler->threads_queue.erase(it);
                break;
            }
        }
        for (auto it = scheduler->sleeping_threads.begin();
        it != scheduler->sleeping_threads.end(); it++){
            // Thread is not running now
            if((*it)->tid == tid){
                scheduler->sleeping_threads.erase(it);
                break;
            }
        }
        scheduler->all_threads[tid] = nullptr;
        delete cur_thread;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return 0;
    }
}

int uthread_block(int tid){
    sigprocmask(SIG_BLOCK, scheduler->signals, nullptr);
    if(tid < 0 || tid >= MAX_THREAD_NUM){
        // Invalid id
        std::cerr << THREAD_ERROR << INVALID_ID_ERROR << std::endl;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return -1;
    }
    if(tid == 0){
        std::cerr << THREAD_ERROR << BLOCKING_MAIN_THREAD_ERROR << std::endl;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return -1;
    }
    Thread* cur_thread = scheduler->all_threads[tid];
    if(cur_thread == nullptr){
        // No such thread
        std::cerr << THREAD_ERROR << NO_THREAD_ERROR << std::endl;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return -1;
    }
    if(tid == scheduler->threads_queue.front()->tid){
        // Thread is running now
        cur_thread->state = BLOCKED;
        cur_thread->is_blocked = true;
        scheduler->threads_queue.pop_front();
        Thread* next_thread = scheduler->threads_queue.front();
        next_thread->state = RUNNING;
        int ret_val = sigsetjmp(cur_thread->env, 1);
        bool did_just_save_bookmark = ret_val == 0;
        if(did_just_save_bookmark){
            reset_timer(scheduler->quantum);
            increase_quantum(next_thread);
            sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
            siglongjmp(next_thread->env, 1);
        }
    }
    else {
        // Thread is not running now
        cur_thread->state = BLOCKED;
        cur_thread->is_blocked = true;
        for (auto it = scheduler->threads_queue.begin();
        it != scheduler->threads_queue.end(); it++) {
            if (tid == (*it)->tid) {
                scheduler->threads_queue.erase(it);
                sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
                break;
            }
        }
    }
    sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
    return 0;
}

int uthread_resume(int tid){
    sigprocmask(SIG_BLOCK, scheduler->signals, nullptr);
    if(tid < 0 || tid >= MAX_THREAD_NUM){
        // Invalid id
        std::cerr << THREAD_ERROR << INVALID_ID_ERROR << std::endl;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return -1;
    }
    Thread* cur_thread = scheduler->all_threads[tid];
    if (cur_thread == nullptr){
        // No such thread
        std::cerr << THREAD_ERROR << NO_THREAD_ERROR << std::endl;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return -1;
    }
    if (cur_thread->state == BLOCKED){
        // Thread is blocked
        if (cur_thread->wakeup_quantum > 0){
            // Thread is sleeping
            cur_thread->is_blocked = false;
            sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
            return 0;
        }
        cur_thread->state = READY;
        cur_thread->is_blocked = false;
        scheduler->threads_queue.push_back(cur_thread);
    }
    sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
    return 0;
}


int uthread_sleep(int num_quantums){
    sigprocmask(SIG_BLOCK, scheduler->signals, nullptr);
    if(num_quantums <= 0){
        std::cerr << THREAD_ERROR << QUANTUM_NUM_ERROR << std::endl;
        return -1;
    }
    Thread *cur_thread = scheduler->threads_queue.front();
    if (cur_thread->tid == 0){
        std::cerr << THREAD_ERROR << BLOCKING_MAIN_THREAD_ERROR << std::endl;
        return -1;
    }
    cur_thread->wakeup_quantum = scheduler->total_quantum + num_quantums + 1;
    cur_thread->state = BLOCKED;
    scheduler->sleeping_threads.insert(cur_thread);
    scheduler->threads_queue.pop_front();
    Thread* next_thread = scheduler->threads_queue.front();
    next_thread->state = RUNNING;
    int ret_val = sigsetjmp(cur_thread->env, 1);
    bool did_just_save_bookmark = ret_val == 0;
    if(did_just_save_bookmark) {
        reset_timer(scheduler->quantum);
        increase_quantum(next_thread);
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        siglongjmp(next_thread->env, 1);
    }
    sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
    return 0;
}

int uthread_get_tid(){
    return scheduler->threads_queue.front()->tid;
}

int uthread_get_total_quantums(){
    return scheduler->total_quantum;
}

int uthread_get_quantums(int tid){
    sigprocmask(SIG_BLOCK, scheduler->signals, nullptr);
    if(tid < 0 || tid >= MAX_THREAD_NUM){
        // Invalid id
        std::cerr << THREAD_ERROR << INVALID_ID_ERROR << std::endl;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return -1;
    }
    Thread* cur_thread = scheduler->all_threads[tid];
    if (cur_thread == nullptr){
        std::cerr << THREAD_ERROR << NO_THREAD_ERROR << std::endl;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return -1;
    }
    sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
    return cur_thread->num_quantum;
}
