#include <ucontext.h>
#include <sys/time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include "thread.h"

#define MAX_THREADS 1000
#define MAX_MUTEXES 1000
#define MAX_BARRIERS 1000
#define STACK_SIZE 4096
#define DEADLOCK_BITSET_SIZE MAX_THREADS

thread_t threads[MAX_THREADS];

static int thread_count = 0;
static int current_index = 0;
static int current_id = 0;

struct itimerval timer = {
    .it_value = { 0, 10000 },
    .it_interval = { 0, 10000 }
};
struct itimerval no_timer = { {0, 0}, {0, 0} };
static inline void enable_timer() { setitimer(ITIMER_REAL, &timer, NULL); }
static inline void disable_timer() { setitimer(ITIMER_REAL, &no_timer, NULL); }

void thread_swap() {
    disable_timer();
    int old_index = current_index;
    int next_index = current_index;
    while(1) {
        next_index = (next_index + 1) % thread_count;
        if (threads[next_index].finished) continue;
        if (threads[next_index].waitsForRWLock != NULL) {   // it's a rwlock
            if (threads[next_index].waitsForRWLock->writer == NULL &&
                (threads[next_index].lockType == READ || threads[next_index].waitsForRWLock->readers == 0))
                break;
        }
        else if (threads[next_index].waitsForLock != NULL) {
            if (!threads[next_index].waitsForLock->locked) {
                threads[next_index].waitsForLock->locked = 1; 
                threads[next_index].waitsForLock->ownedBy = &threads[next_index];
                threads[next_index].hasLock = threads[next_index].waitsForLock;
                threads[next_index].waitsForLock = NULL;
                break;
            } 
        }
        else if (threads[next_index].waitsForBarrier != NULL) continue;
        else if (threads[next_index].waitsForThread != NULL) {
            if (threads[threads[next_index].waitsForThread->id].finished) {
                threads[next_index].waitsForThread = NULL;
                break;
            }
        } else {
            break;
        }    
    }
    current_index = next_index;
    enable_timer();
    swapcontext(&threads[old_index].context, &threads[next_index].context);
}

void thread_join(thread_t* target, void** retval) {
    if (target->finished) {
        if (retval) *retval = target->result;
        return;
    }
    disable_timer();
    threads[current_index].waitsForThread = target;
    detect_deadlock(&threads[current_index]);
    thread_swap();
    if (retval) *retval = target->result;
}

void handler(int sig) { if (sig == SIGALRM) thread_swap(); }

struct sigaction action = {
    .sa_handler = handler
};

void thread_init() {
    getcontext(&threads[thread_count].context);
    threads[thread_count].id = current_id;
    threads[thread_count].waitsForThread = NULL;
    threads[thread_count].waitsForLock = NULL;
    threads[thread_count].hasLock = NULL;
    threads[thread_count].waitsForBarrier = NULL;
    threads[thread_count].waitsForRWLock = NULL;
    threads[thread_count].finished = 0;
    threads[thread_count].lockType = NONE;
    thread_count++;
    sigaction(SIGALRM, &action, NULL);
    enable_timer();
}

void thread_exit(void* retval) {
    disable_timer();
    threads[current_index].result = retval;
    threads[current_index].finished = 1;
    thread_swap();
}

void thread_wrapper() {
    thread_exit(threads[current_index].function(threads[current_index].arg));
}

thread_t *thread_create(void* (*f)(void*), void *arg) {
    disable_timer();
    current_id++;
    getcontext(&threads[thread_count].context);
    void *stack = malloc(STACK_SIZE);
    threads[thread_count].context.uc_stack.ss_sp = stack;
    threads[thread_count].context.uc_stack.ss_size = STACK_SIZE;
    threads[thread_count].context.uc_stack.ss_flags = 0;
    makecontext(&threads[thread_count].context, thread_wrapper, 0);
    threads[thread_count].id = current_id;
    threads[thread_count].function = f;
    threads[thread_count].arg = arg;
    threads[thread_count].waitsForThread = NULL;
    threads[thread_count].waitsForLock = NULL;
    threads[thread_count].hasLock = NULL;
    threads[thread_count].waitsForBarrier = NULL;
    threads[thread_count].waitsForRWLock = NULL;
    threads[thread_count].lockType = NONE;
    threads[thread_count].finished = 0;
    thread_count++;
    enable_timer();
    return threads+thread_count-1;
}

void* self(void *arg) {
    int v = *(int*)arg;
    for (int i = 0; i < 10; i++) {
       thread_swap();
    }
    int *r = (int*)malloc(sizeof(int));
    *r = v + threads[current_index].id;
    return r;
}

mutex_t* create_mutex() {
    mutex_t *mutex = malloc(sizeof(mutex_t));
    mutex->id = 0;
    mutex->locked = 0;
    mutex->ownedBy = NULL;
    return mutex;
}

void destroy_mutex(mutex_t* mutex) {
    mutex->id = -1;
    mutex->locked = -1;
    mutex->ownedBy = NULL;
}

int lock_mutex(mutex_t* mutex) {
    disable_timer();
    if (!mutex->locked) {
        mutex->locked = 1; 
        mutex->ownedBy = &threads[current_index];
        threads[current_index].waitsForLock = NULL;
        threads[current_index].hasLock = mutex;
        return 0;
    }
    threads[current_index].waitsForLock = mutex;
    detect_deadlock(&threads[current_index]);
    thread_swap();
}

int unlock_mutex(mutex_t* mutex) {
    disable_timer();
    if(!mutex->locked || (mutex->locked && mutex->ownedBy->finished)) {
        return -1;
    }
    mutex->ownedBy = NULL;
    mutex->locked = 0;
    threads[current_index].hasLock = NULL;
    enable_timer();
}

int barrier_init(barrier_t* barrier, int count) {
    barrier->threads_required = count;
    barrier->threads_waiting = 0;
    barrier->lock = create_mutex();
    barrier->threads = malloc(count * sizeof(int));
    if (barrier->threads == NULL) {
        printf("Malloc failed\n");
        return -1;
    }
    return 0;
}

int barrier_wait(barrier_t* barrier) {
    lock_mutex(barrier->lock);
    disable_timer();

    barrier->threads[barrier->threads_waiting] = current_index;
    barrier->threads_waiting++;
    threads[current_index].waitsForBarrier = barrier;

    if (barrier->threads_required == barrier->threads_waiting) {
        printf("Condition met\n");
        for (int i = 0; i < barrier->threads_waiting; i++) {
            printf("%d ", barrier->threads[i]);
        }
        printf("\n");
        for (int i = 0; i < barrier->threads_waiting; i++) {
            printf("Thread %d does not wait for barrier anymore\n", barrier->threads[i]);
            threads[barrier->threads[i]].waitsForBarrier = NULL;
            barrier->threads[i] = -1;
        }
        barrier->threads_waiting = 0;
        enable_timer();
        unlock_mutex(barrier->lock);
    } else {
        unlock_mutex(barrier->lock);
        thread_swap();
    }
}

int barrier_destroy(barrier_t* barrier) {
    barrier->threads_required = -1;
    barrier->threads_waiting = -1;
    destroy_mutex(barrier->lock);
    return 0;
}

void rwlock_init(rwlock_t* rwlock) {
    rwlock->readers = 0;
    rwlock->writer = NULL;
}

void rwlock_rdlock(rwlock_t* rwlock) {
    disable_timer();
    if (rwlock->writer != NULL) {
        threads[current_index].lockType = READ;
        threads[current_index].waitsForRWLock = rwlock;
        thread_swap();
        disable_timer();
        threads[current_index].lockType = NONE;
        threads[current_index].waitsForRWLock = NULL;
    }
    rwlock->readers++;
    enable_timer();
}

void rwlock_wrlock(rwlock_t* rwlock) {
    disable_timer();
    if (rwlock->writer != NULL || rwlock->readers > 0) {
        threads[current_index].lockType = WRITE;
        threads[current_index].waitsForRWLock = rwlock;
        thread_swap();
        disable_timer();
        threads[current_index].lockType = NONE;
        threads[current_index].waitsForRWLock = NULL;
    }
    rwlock->writer = &threads[current_index];
    enable_timer();
}

void rwlock_unlock(rwlock_t* rwlock) {
    disable_timer();
    if (rwlock->writer != NULL) {
        rwlock->writer = NULL;
    } else {
        rwlock->readers--;
    }
    enable_timer();
}

void rwlock_destroy(rwlock_t* rwlock) {
    rwlock->readers = -1;
    rwlock->writer = NULL;
}

void detect_deadlock(thread_t *thread) {
    thread_t *current = thread;
    do {
        if (current->waitsForThread != NULL) {
            current = current->waitsForThread;
        }
        else if (current->waitsForLock != NULL) {
            current = current->waitsForLock->ownedBy;
        }
        else break;
        if (current == thread) {
            printf("Deadlock");
        }
    } while(1);
}


// ------------------------------------------------ TEST THREADS CREATION AND JOIN

// mutex_t* lock;
// int counter = 0;

// void* doSomeThing(void *arg)
// {
//     lock_mutex(lock);
//     unsigned long i = 0;
//     counter += 1;
//     printf("\n Thread %d started\n", threads[current_index].id);
//     for(i=0; i<(0xFFFFFFFF);i++);
//     printf("\n Thread %d finished\n", threads[current_index].id);
//     unlock_mutex(lock);
//     return NULL;
// }

// int main(int argc, char **argv) {
//     thread_init();
//     int i = 0;
//     lock = create_mutex();
//     thread_t *t1 = thread_create(&doSomeThing, NULL);
//     thread_t *t2 = thread_create(&doSomeThing, NULL);
//     thread_join(t1, NULL);
//     thread_join(t2, NULL);
//     destroy_mutex(lock);
//     return 0;
// }

// ------------------------------------------------ TEST MUTEXES

// void* g(void *arg) {
//     mutex_t *mutex = (mutex_t*)arg;
//     lock_mutex(mutex);
//     for (int i=0; i<10; ++i) {
//         printf("Thread id: %d => %d\n", threads[current_index].id, i);
//         thread_swap();
//     }
//     unlock_mutex(mutex);
//     return NULL;
// }
// void main() {
//     thread_init();
//     mutex_t* mutex = create_mutex();
//     thread_t *t1 = thread_create(g, mutex);
//     thread_t *t2 = thread_create(g, mutex);
//     printf("before join\n");
//     thread_join(t1, NULL);
//     thread_join(t2, NULL);
//     printf("after join returned\n");
// }

// ------------------------------------------------ TEST BARRIERS

// barrier_t barrier;

// void *thread_func(void *threadid)
// {
//     int tid = (int)threadid;
//     printf("Thread %d before the barrier\n", tid);
//     barrier_wait(&barrier);
//     printf("Thread %d after the barrier\n", tid);
//     return NULL;
// }

// int main(int argc, char *argv[])
// {
//     thread_t* threads_test = malloc(10 * sizeof(thread_t));
//     int rc;
//     long t;
//     thread_init();
//     barrier_init(&barrier, 5);
//     for(t = 0; t < 10; t++) {
//         printf("In main: creating thread %ld\n", t+1);
//         threads_test[t] = *thread_create(thread_func, (void *)(t+1));
//     }
//     for(t = 0; t < 10; t++) {
//         thread_join(threads_test + t, NULL);
//     }
//     barrier_destroy(&barrier);
//     return 0;
// }


// ------------------------------------------------ TEST READ-WRITE LOCKS

// int shared_data = 0;
// rwlock_t lock;

// void *writer(void *arg) {
//     int thread_id = *(int *) arg;
//     rwlock_wrlock(&lock);
//     printf("Writer %d is writing shared data.\n", thread_id);
//     shared_data++; thread_swap();
//     printf("Writer %d has finished writing. Shared data is now %d.\n", thread_id, shared_data);
//     rwlock_unlock(&lock);
//     return NULL;
// }

// void *reader(void *arg) {
//     int thread_id = *(int *) arg;
//     rwlock_rdlock(&lock);
//     printf("Reader %d is reading shared data. Shared data is currently %d.\n", thread_id, shared_data);
//     thread_swap();
//     rwlock_unlock(&lock);
//     return NULL;
// }

// int main() {
//     thread_t *threads_test[4];
//     int thread_ids[4];

//     thread_init();
//     rwlock_init(&lock);

//     printf("creating writers\n");
//     // create writer threads
//     for (int i = 0; i < 2; i++) {
//         thread_ids[i] = i;
//         threads_test[i] = thread_create(writer, &thread_ids[i]);
//     }

// printf("creating readers\n");
//     // create reader threads
//     for (int i = 2; i < 4; i++) {
//         thread_ids[i] = i;
//         threads_test[i] = thread_create(reader, &thread_ids[i]);
//     }
// printf("created readers & writers\n");
//     // wait for all threads to finish
//     for (int i = 0; i < 4; i++) {
//         thread_join(threads_test[i], NULL);
//     }

//     rwlock_destroy(&lock);

//     return 0;
// }

// ------------------------------------------------ DEADLOCK

mutex_t *first_mutex, *second_mutex;

void *function1() {
    lock_mutex(first_mutex);
    printf("Thread ONE acquired first_mutex\n");
    thread_swap();
    lock_mutex(second_mutex);
    printf("Thread ONE acquired second_mutex\n");
    unlock_mutex(second_mutex);
    printf("Thread ONE released second_mutex\n");
    unlock_mutex(first_mutex);
    printf("Thread ONE released first_mutex\n");
}

void *function2() {
    lock_mutex(second_mutex);
    printf("Thread TWO acquired second_mutex\n");
    thread_swap();
    lock_mutex(first_mutex);
    printf("Thread TWO acquired first_mutex\n");
    unlock_mutex(first_mutex);
    printf("Thread TWO released first_mutex\n");
    unlock_mutex(second_mutex);
    printf("Thread TWO released second_mutex\n");
}

int main() {
    thread_init();
    first_mutex = create_mutex();
    second_mutex = create_mutex();
    thread_t *one, *two;  
    one = thread_create(function1, NULL);
    two = thread_create(function2, NULL);
    thread_join(one, NULL);
    thread_join(two, NULL);
    printf("Thread joined\n");
    destroy_mutex(first_mutex);
    destroy_mutex(second_mutex);
    return 0;
}