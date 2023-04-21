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
    ThreadScheduler(){
        total_quantum = 0;
        largest_index = 0;
        threads_num = 0;
        //threads_queue = new std::deque<Thread*>();
        //blocked_threads = new std::unordered_map<int, Thread*>();
    }
    ~ThreadScheduler(){
        for (int i = 1; i < threads_queue.size(); ++i) {

            Thread* cur_thread = threads_queue[i];
            if(cur_thread->tid == 0){

                delete &cur_thread;
            }
            else{
                delete[] cur_thread->stack;
                delete &cur_thread;
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

void timer_handler(int sig){
    while(!scheduler->threads_queue.empty()){
        scheduler->total_quantum ++;
        Thread* cur_thread = scheduler->threads_queue.front();
        scheduler->threads_queue.pop_front();
        cur_thread->state = READY;
        scheduler->threads_queue.push_back(cur_thread);
        Thread* next_thread = scheduler->threads_queue.front();
        next_thread->state = RUNNING;
        next_thread->num_quantum++;
        std::cout << "In handler, thread num: " << next_thread->tid;

        int ret_val = sigsetjmp(cur_thread->env, 1);
        siglongjmp(next_thread->env, 1);
    }
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
    scheduler->threads_queue.push_back(new Thread(0));
    scheduler->quantum = quantum_usecs;
    reset_timer(quantum_usecs);

    return 0;
}

int uthread_spawn(thread_entry_point entry_point){
    if(scheduler->threads_num >= MAX_THREAD_NUM){
        std::cerr << THREAD_ERROR << "Exceeds maximum number of threads" << std::endl;
        return -1;
    }
    char* stack_pointer = new char[STACK_SIZE];
    int next_tid;
    if (scheduler->avaliable_tids.empty()){
        scheduler->largest_index++;
        next_tid = scheduler->largest_index;
    }
    else {
        next_tid = *scheduler->avaliable_tids.begin();
        scheduler->avaliable_tids.erase(next_tid);
    }

    Thread *new_thread = new Thread(next_tid, stack_pointer, entry_point);
    new_thread->state = READY;
    scheduler->threads_queue.push_back(new_thread);


    return new_thread->tid;
}

int uthread_terminate(int tid){
    if (tid == 0){
        delete &scheduler;
        exit(0);
    }
    if(tid == scheduler->threads_queue.front()->tid){
        Thread cur_thread = *scheduler->threads_queue.front();
        scheduler->threads_queue.pop_front();
        delete[] cur_thread.stack;
        delete &cur_thread;
        scheduler->avaliable_tids.insert(tid);
        reset_timer(scheduler->quantum);
        scheduler->threads_queue.front()->state = RUNNING;
        siglongjmp(scheduler->threads_queue.front()->env, 1);

    }
    else{
        auto iterator = scheduler->threads_queue.begin();
        for (int i = 0; i <scheduler->threads_queue.size(); ++i) {
            if(tid == scheduler->threads_queue[i]->tid){
                Thread* cur_thread = scheduler->threads_queue[i];
                delete[] cur_thread->stack;
                delete scheduler->threads_queue[i];
                scheduler->avaliable_tids.insert(tid);
                scheduler->threads_queue.erase(iterator + i);
                return 0;
            }

        }
        //TODO: Check id in blocked threads
        std::cerr << THREAD_ERROR << "The thread was not terminated, no such thread" << std::endl;
        return -1;
    }
}

int uthread_block(int tid){
    if(tid == 0){
        std::cerr << THREAD_ERROR << "It isn't possible to block the main thread" << std::endl;
        return -1;
    }
    Thread* cur_thread = scheduler->threads_queue.front();
    if(tid == cur_thread->tid){
        cur_thread->state = BLOCKED;
        scheduler->threads_queue.pop_front();
        scheduler->blocked_threads.insert(std::make_pair(cur_thread->tid, cur_thread));
        reset_timer(scheduler->quantum);
        scheduler->threads_queue.front()->state = RUNNING;
        siglongjmp(scheduler->threads_queue.front()->env, 1);
    }
    auto iterator = scheduler->threads_queue.begin();
    for(auto it = scheduler->threads_queue.begin(); it != scheduler->threads_queue.end(); it++){
        if(tid == (*it)->tid){
            scheduler->blocked_threads.insert(std::make_pair((*it)->tid, *it));
            scheduler->threads_queue.erase(it);
            return 0;
        }
    }
    std::cerr << THREAD_ERROR << "There is no such thread" << std::endl;
    return -1;
}

int uthread_resume(int tid){

    if (scheduler->blocked_threads.find(tid) == scheduler->blocked_threads.end()){
        for (auto &thread: scheduler->threads_queue){
            if (thread->tid == tid){
                return 0;
            }
        }
        std::cerr << THREAD_ERROR << "There is no such thread" << std::endl;
        return -1;
    }
    Thread *cur_thread = scheduler->blocked_threads.at(tid);
    scheduler->blocked_threads.erase(tid);
    cur_thread->state = READY;
    scheduler->threads_queue.push_back(cur_thread);
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
    if (scheduler->threads_queue.front()->tid == 0){
        std::cerr << THREAD_ERROR << "Main thread could not be blocked" << std::endl;
        return -1;
    }
}

int uthread_get_tid(){
    return scheduler->threads_queue.front()->tid;
}

int uthread_get_total_quantums(){
    return scheduler->total_quantum;
}

int uthread_get_quantums(int tid){
    if (scheduler->blocked_threads.find(tid) != scheduler->blocked_threads.end()){
        return scheduler->blocked_threads.at(tid)->num_quantum;
    }
    for (auto &thread: scheduler->threads_queue){
        if (thread->tid == tid){
            return thread->num_quantum;
        }
    }
    std::cerr << THREAD_ERROR << "There is no such thread" << std::endl;
    return -1;
}
