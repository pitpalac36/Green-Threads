#include <ucontext.h>
#include <stdint.h>

typedef enum {
    READ, WRITE, NONE
}rwlock_type_t;

typedef struct thread {
    int id;
    ucontext_t context;
    void *(*function)(void *);
    void *arg;
    struct thread* waitsForThread;
    int finished;
    void* result;
    struct mutex* waitsForLock;
    struct mutex* hasLock;
    struct barrier* waitsForBarrier;
    struct rwlock* waitsForRWLock;
    rwlock_type_t lockType;
} thread_t;

void thread_init();

thread_t *thread_create(void* (*f)(void*), void *arg);

void thread_join(thread_t* target, void** retval);

typedef struct mutex {
    int id;
    int locked;
    thread_t* ownedBy;
} mutex_t;

mutex_t* create_mutex();

void destroy_mutex();

int lock_mutex(mutex_t* mutex);

int unlock_mutex(mutex_t* mutex);

typedef struct barrier {
    int threads_waiting;
    int threads_required;
    mutex_t* lock;
    int* threads;
} barrier_t;

int barrier_init(barrier_t* barrier, int count);

int barrier_wait(barrier_t* barrier);

int barrier_destroy(barrier_t* barrier);

typedef struct rwlock {
    int readers;
    thread_t* writer;
} rwlock_t;

void rwlock_init(rwlock_t* rwlock);

void rwlock_rdlock(rwlock_t* rwlock);

void rwlock_wrlock(rwlock_t* rwlock);

void rwlock_unlock(rwlock_t* rwlock);

void rwlock_destroy(rwlock_t* rwlock);

void detect_deadlock(thread_t *thread);