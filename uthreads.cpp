#include <stdio.h>
#include <setjmp.h>
#include <csignal>
#include <iostream>
#include "uthreads.h"
#include <queue>
#include <deque>
#include <sys/time.h>
#include <map>
#include <unordered_map>
#include <set>

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

#define MAX_THREAD_NUM 100 /* maximal number of threads */
#define STACK_SIZE 4096 /* stack size per thread (in bytes) */

#define SECOND 1000000
#define SYSTEM_ERROR "system error: "
#define THREAD_ERROR "thread library error: "

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
    int savemask;
    char* stack;
    thread_entry_point entry_point;
    int num_quantum;

    Thread(int tid, char *stack, thread_entry_point entry_point)
    {
        // initializes env[tid] to use the right stack, and to run from the function 'entry_point', when we'll use
        // siglongjmp to jump into the thread.
        this->tid = tid;
        address_t sp = (address_t) stack + STACK_SIZE - sizeof(address_t);
        address_t pc = (address_t) entry_point;
        sigsetjmp(env, 1);
        num_quantum = 0;
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
    std::unordered_map<int, Thread*> blocked_threads;
    int total_quantum;
    int largest_index;
    int threads_num;
    std::set<int> avaliable_tids;
    int quantum;
    sigset_t* signals;
    Thread** all_threads;
    std::set<Thread*> sleeping_threads;
    ThreadScheduler(){
        total_quantum = 1;
        largest_index = 0;
        threads_num = 0;
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
        for (int i = 1; i < threads_queue.size(); ++i) {

            Thread* cur_thread = threads_queue[i];
            if(cur_thread->tid == 0){

                delete &cur_thread;
            }
            else{
                delete[] cur_thread->stack;
                delete cur_thread;
            }
        }
        delete &threads_queue;
        for (auto & blocked_thread : blocked_threads) {
            delete[] blocked_thread.second->stack;
            delete &blocked_thread.second;
        }
        delete &blocked_threads;
    }
};
ThreadScheduler *scheduler = new ThreadScheduler();
struct sigaction sa = {0};
struct itimerval timer;

void increase_quantum(Thread* next_thread) {
    scheduler->total_quantum++;
    next_thread->num_quantum++;
//    if(scheduler->sleeping_threads.empty()){
//        return;
//    }
    auto it = scheduler->sleeping_threads.begin();
    while (it != scheduler->sleeping_threads.end()){
        if ((*it)->wakeup_quantum == scheduler->total_quantum){
            (*it)->wakeup_quantum = 0;
            uthread_resume((*it)->tid);
            scheduler->sleeping_threads.erase(it);
            break;
        }
        it++;
    }
}

void timer_handler(int sig){
    sigprocmask(SIG_BLOCK, scheduler->signals, nullptr);
    if(!scheduler->threads_queue.empty()){
        scheduler->total_quantum ++;
        Thread* cur_thread = scheduler->threads_queue.front();
        scheduler->threads_queue.pop_front();
        cur_thread->state = READY;
        scheduler->threads_queue.push_back(cur_thread);
        Thread* next_thread = scheduler->threads_queue.front();
        next_thread->state = RUNNING;
        cur_thread->num_quantum++;
        std::cout << "In handler, thread num: " << cur_thread->tid << "\n";

        int ret_val = sigsetjmp(cur_thread->env, 1);
        bool did_just_save_bookmark = ret_val == 0;
        if(did_just_save_bookmark){
            siglongjmp(next_thread->env, 1);
        }
    }
    sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
}



void reset_timer(int quantum_usecs){
    sa.sa_handler = &timer_handler;
    if (sigaction(SIGVTALRM, &sa, NULL) < 0)
    {
        std::cerr << SYSTEM_ERROR << "sigaction error" << std::endl;
        exit(1);
    }
    timer.it_value.tv_sec = quantum_usecs / SECOND;
    timer.it_value.tv_usec = quantum_usecs % SECOND;

    timer.it_interval.tv_sec = quantum_usecs / SECOND;
    timer.it_interval.tv_usec = quantum_usecs % SECOND;
    if (setitimer(ITIMER_VIRTUAL, &timer, NULL))
    {
        std::cerr << SYSTEM_ERROR << "setitimer error" << std::endl;
        exit(1);
    }
}

int uthread_init(int quantum_usecs){
    if(quantum_usecs <= 0){
        std::cerr << THREAD_ERROR <<"Quantum_usecs is not positive" << std::endl;
        return -1;
    }
    Thread *main_thread = new Thread(0);
//    scheduler->threads_num++;
    scheduler->threads_queue.push_back(main_thread);
    scheduler->all_threads[main_thread->tid] = main_thread;
    scheduler->quantum = quantum_usecs;
    reset_timer(quantum_usecs);

    return 0;
}

int uthread_spawn(thread_entry_point entry_point){

    sigprocmask(SIG_BLOCK, scheduler->signals, nullptr);
//    if(scheduler->threads_num >= MAX_THREAD_NUM){
//        std::cerr << THREAD_ERROR << "Exceeds maximum number of threads" << std::endl;
//        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
//        return -1;
//    }
    for (int i = 1; i < MAX_THREAD_NUM; ++i) {
        if (scheduler->all_threads[i] == nullptr){
            char* stack_pointer = new char[STACK_SIZE];
            scheduler->all_threads[i] = new Thread(i, stack_pointer, entry_point);
            scheduler->all_threads[i]->state = READY;
//            scheduler->threads_num++;
            scheduler->threads_queue.push_back(scheduler->all_threads[i]);
            sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
            return i;
        }
    }
    std::cerr << THREAD_ERROR << "Exceeds maximum number of threads" << std::endl;
    sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
    return -1;
}

int uthread_terminate(int tid){
    sigprocmask(SIG_BLOCK, scheduler->signals, nullptr);
    if(tid < 0 || tid >= MAX_THREAD_NUM){
        // Invalid id
        std::cerr << THREAD_ERROR << "The thread id is invalid" << std::endl;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return -1;
    }
    if (tid == 0){
//        for (int i = 0; i < scheduler->threads_queue.size(); ++i) {
//            delete[] scheduler->threads_queue[i]->stack;
//            delete scheduler->threads_queue[i];
//        }
        delete scheduler;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        exit(0);
    }
    if(tid == scheduler->threads_queue.front()->tid){
        Thread* cur_thread = scheduler->threads_queue.front();
        scheduler->threads_queue.pop_front();
        delete[] cur_thread->stack;
        delete cur_thread;
        scheduler->all_threads[tid] = nullptr;
        reset_timer(scheduler->quantum);
        scheduler->threads_queue.front()->state = RUNNING;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        siglongjmp(scheduler->threads_queue.front()->env, 1);
    }
    else{
        for (auto it = scheduler->threads_queue.begin(); it != scheduler->threads_queue.end(); it++){
            // Thread is not running now
            if((*it)->tid == tid){
                scheduler->threads_queue.erase(it);
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
        std::cerr << THREAD_ERROR << "The thread id is invalid" << std::endl;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return -1;
    }
    if(tid == 0){
        std::cerr << THREAD_ERROR << "It isn't possible to block the main thread" << std::endl;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return -1;
    }
    Thread* cur_thread = scheduler->threads_queue.front();
    if(tid == cur_thread->tid){
        cur_thread->state = BLOCKED;
        scheduler->threads_queue.pop_front();
        Thread* next_thread = scheduler->threads_queue.front();
        next_thread->state = RUNNING;
        int ret_val = sigsetjmp(cur_thread->env, 1);
        bool did_just_save_bookmark = ret_val == 0;
        if(did_just_save_bookmark){
            reset_timer(scheduler->quantum);
            sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
//            next_thread->num_quantum++;
//            scheduler->total_quantum++;
            increase_quantum(next_thread);
            siglongjmp(next_thread->env, 1);
        }
    }
    else {
        // Thread is not running now
        cur_thread->state = BLOCKED;
        for (auto it = scheduler->threads_queue.begin(); it != scheduler->threads_queue.end(); it++) {
            if (tid == (*it)->tid) {
                //std::cout << "Blocked thread: " << (*it)->tid << std::endl;
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
    if (scheduler->blocked_threads.find(tid) == scheduler->blocked_threads.end()){
        for (auto &thread: scheduler->threads_queue){
            if (thread->tid == tid){
                sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
                return 0;
            }
        }
        std::cerr << THREAD_ERROR << "There is no such thread" << std::endl;
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return -1;
    }
    Thread *cur_thread = scheduler->blocked_threads.at(tid);
    scheduler->blocked_threads.erase(tid);
    cur_thread->state = READY;
    scheduler->threads_queue.push_back(cur_thread);
    sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
    return 0;
}

/**
 * @brief Blocks the RUNNING thread for num_quantums quantums.
 *
 * Immediately after the RUNNING thread transitions to the BLOCKED state a scheduling decision should be made.
 * After the sleeping time is over, the thread should go back to the end of the READY queue.
 * If the thread which was just RUNNING should also be added to the READY queue, or if multiple threads wake up
 * at the same time, the order in which they're added to the end of the READY queue doesn't matter.
 * The number of quantums refers to the number of times a new quantum starts, regardless of the reason. Specifically,
 * the quantum of the thread which has made the call to uthread_sleep isn’t counted.
 * It is considered an error if the main thread (tid == 0) calls this function.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_sleep(int num_quantums){
    if(num_quantums <= 0){
        std::cerr << THREAD_ERROR << "The quantums number is illegal" << std::endl;
        return -1;
    }
    Thread *cur_thread = scheduler->threads_queue.front();
    if (cur_thread->tid == 0){
        std::cerr << THREAD_ERROR << "Main thread could not be blocked" << std::endl;
        return -1;
    }
    cur_thread->wakeup_quantum = scheduler->total_quantum + num_quantums;
    cur_thread->state = BLOCKED;
    scheduler->sleeping_threads.insert(cur_thread);
    scheduler->threads_queue.pop_front();
    Thread* next_thread = scheduler->threads_queue.front();
    next_thread->state = RUNNING;
    int ret_val = sigsetjmp(cur_thread->env, 1);
    bool did_just_save_bookmark = ret_val == 0;
    if(did_just_save_bookmark) {
        reset_timer(scheduler->quantum);
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        increase_quantum(next_thread);
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
    if (scheduler->blocked_threads.find(tid) != scheduler->blocked_threads.end()){
        sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
        return scheduler->blocked_threads.at(tid)->num_quantum;
    }
    for (auto &thread: scheduler->threads_queue){
        if (thread->tid == tid){
            sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
            return thread->num_quantum;
        }
    }
    std::cerr << THREAD_ERROR << "There is no such thread" << std::endl;
    sigprocmask(SIG_UNBLOCK, scheduler->signals, nullptr);
    return -1;
}
