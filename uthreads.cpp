#include <stdio.h>
#include <setjmp.h>
#include "uthreads.h"

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

enum State {
    RUNNING, READY, BLOCKED
};

struct Thread {
    int tid;
    State state;
    sigjmp_buf env;
    int savemask;

//    void setup_thread(int tid, char *stack, thread_entry_point entry_point)
//    {
//        // initializes env[tid] to use the right stack, and to run from the function 'entry_point', when we'll use
//        // siglongjmp to jump into the thread.
//        address_t sp = (address_t) stack + STACK_SIZE - sizeof(address_t);
//        address_t pc = (address_t) entry_point;
//        sigsetjmp(env[tid], 1);
//        (env[tid]->__jmpbuf)[JB_SP] = translate_address(sp);
//        (env[tid]->__jmpbuf)[JB_PC] = translate_address(pc);
//        sigemptyset(&env[tid]->__saved_mask);
//    }
};
