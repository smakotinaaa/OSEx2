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

    Thread(int tid, char *stack, thread_entry_point entry_point)
    {
        // initializes env[tid] to use the right stack, and to run from the function 'entry_point', when we'll use
        // siglongjmp to jump into the thread.
        address_t sp = (address_t) stack + STACK_SIZE - sizeof(address_t);
        address_t pc = (address_t) entry_point;
        sigsetjmp(env, 1);
        (env->__jmpbuf)[JB_SP] = translate_address(sp);
        (env->__jmpbuf)[JB_PC] = translate_address(pc);
        sigemptyset(&env->__saved_mask);
    }
    Thread(int tid): tid(tid), state(RUNNING){};
};

class ThreadScheduler{
public:
    std::deque<Thread> threads_queue;
    std::unordered_map<int, Thread> blocked_threads;
    int total_quantum;
    int cur_index;
    int threads_num;
    ThreadScheduler(){
        total_quantum = 0;
        cur_index = 0;
        threads_num = 0;
        threads_queue = *new std::deque<Thread>();
        blocked_threads = *new std::unordered_map<int, Thread>();
    }
    ~ThreadScheduler(){
        for (int i = 1; i < threads_queue.size(); ++i) {

            Thread cur_thread = threads_queue[i];
            if(cur_thread.tid == 0){

                delete &cur_thread;
            }
            else{
                delete[] cur_thread.stack;
                delete &cur_thread;
            }
        }
        delete &threads_queue;
        for (auto & blocked_thread : blocked_threads) {
            delete[] blocked_thread.second.stack;
            delete &blocked_thread.second;
        }
        delete &blocked_threads;
    }
};

ThreadScheduler scheduler = *new ThreadScheduler();
struct sigaction sa = {0};
struct itimerval timer;
void timer_handler(int sig){
    while(!scheduler.threads_queue.empty()){
        scheduler.total_quantum ++;
        Thread cur_thread = scheduler.threads_queue.front();
        scheduler.threads_queue.pop_front();
        cur_thread.state = READY;
        scheduler.threads_queue.push_back(cur_thread);
        Thread next_thread = scheduler.threads_queue.front();
        next_thread.state = RUNNING;

        int ret_val = sigsetjmp(cur_thread.env, 1);
        siglongjmp(next_thread.env, 1);
    }
}
/**
 * @brief initializes the thread library.
 *
 * Once this function returns, the main thread (tid == 0) will be set as RUNNING. There is no need to
 * provide an entry_point or to create a stack for the main thread - it will be using the "regular" stack and PC.
 * You may assume that this function is called before any other thread library function, and that it is called
 * exactly once.
 * The input to the function is the length of a quantum in micro-seconds.
 * It is an error to call this function with non-positive quantum_usecs.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_init(int quantum_usecs){
    if(quantum_usecs <= 0){
        std::cerr << THREAD_ERROR <<"Quantum_usecs is not positive" << std::endl;
        return -1;
    }
    scheduler.threads_queue.push_back(*new Thread(0));
    // Install timer_handler as the signal handler for SIGVTALRM.
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
    return 0;
}

/**
 * @brief Creates a new thread, whose entry point is the function entry_point with the signature
 * void entry_point(void).
 *
 * The thread is added to the end of the READY threads list.
 * The uthread_spawn function should fail if it would cause the number of concurrent threads to exceed the
 * limit (MAX_THREAD_NUM).
 * Each thread should be allocated with a stack of size STACK_SIZE bytes.
 * It is an error to call this function with a null entry_point.
 *
 * @return On success, return the ID of the created thread. On failure, return -1.
*/
int uthread_spawn(thread_entry_point entry_point){
    if(scheduler.threads_num >= MAX_THREAD_NUM){
        std::cerr << THREAD_ERROR << "Exceeds maximum number of threads" << std::endl;
        return -1;
    }
    char* stack_pointer = new char[STACK_SIZE];
    scheduler.cur_index ++;
    Thread new_thread = *new Thread(scheduler.cur_index, stack_pointer, entry_point);
    new_thread.state = READY;
    scheduler.threads_queue.push_back(new_thread);
    return new_thread.tid;
}

/**
 * @brief Terminates the thread with ID tid and deletes it from all relevant control structures.
 *
 * All the resources allocated by the library for this thread should be released. If no thread with ID tid exists it
 * is considered an error. Terminating the main thread (tid == 0) will result in the termination of the entire
 * process using exit(0) (after releasing the assigned library memory).
 *
 * @return The function returns 0 if the thread was successfully terminated and -1 otherwise. If a thread terminates
 * itself or the main thread is terminated, the function does not return.
*/
int uthread_terminate(int tid){
    if (tid == 0){
        delete &scheduler;
        exit(0);
    }
    if(tid == scheduler.threads_queue.front().tid){
        Thread cur_thread = scheduler.threads_queue.front();
        scheduler.threads_queue.pop_front();
        delete[] cur_thread.stack;
        delete &cur_thread;

        scheduler.threads_queue.front().state = RUNNING;
        siglongjmp(scheduler.threads_queue.front().env, 1);
    }
    else{
        auto iterator = scheduler.threads_queue.begin();
        for (int i = 0; i <scheduler.threads_queue.size(); ++i) {
            if(tid == scheduler.threads_queue[i].tid){
                delete[] scheduler.threads_queue[i].stack;
                delete &scheduler.threads_queue[i];
                scheduler.threads_queue.erase(iterator + i);
                return 0;
            }
        }
        std::cerr << THREAD_ERROR << "The thread was not terminated, no such thread" << std::endl;
        return -1;
    }
}
/**
 * @brief Blocks the thread with ID tid. The thread may be resumed later using uthread_resume.
 *
 * If no thread with ID tid exists it is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision should be made. Blocking a thread in
 * BLOCKED state has no effect and is not considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_block(int tid){
    if(tid == 0){
        std::cerr << THREAD_ERROR << "It isn't possible to block the main thread" << std::endl;
        return -1;
    }
    Thread cur_thread = scheduler.threads_queue.front();
    if(tid == cur_thread.tid){
        cur_thread.state = BLOCKED;
        scheduler.threads_queue.pop_front();
        scheduler.blocked_threads[cur_thread.tid] = cur_thread;

    }

}