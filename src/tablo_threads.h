#ifndef TABLO_THREADS_H
#define TABLO_THREADS_H

#if !defined(__APPLE__) && !defined(__STDC_NO_THREADS__)
#if defined(__has_include)
#if __has_include(<threads.h>)
#include <threads.h>
#define TABLO_HAS_C11_THREADS 1
#endif
#else
#include <threads.h>
#define TABLO_HAS_C11_THREADS 1
#endif
#endif

#ifndef TABLO_HAS_C11_THREADS

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

typedef pthread_t thrd_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t cnd_t;
typedef pthread_once_t once_flag;
typedef int (*thrd_start_t)(void*);

enum {
    thrd_success = 0,
    thrd_nomem = 1,
    thrd_timedout = 2,
    thrd_busy = 3,
    thrd_error = 4
};

enum {
    mtx_plain = 0,
    mtx_recursive = 1,
    mtx_timed = 2
};

#define ONCE_FLAG_INIT PTHREAD_ONCE_INIT

typedef struct {
    thrd_start_t function;
    void* arg;
} tablo_thrd_start_ctx;

static void* tablo_thrd_start_adapter(void* opaque) {
    tablo_thrd_start_ctx* ctx = (tablo_thrd_start_ctx*)opaque;
    thrd_start_t function = NULL;
    void* arg = NULL;
    int result = 0;

    if (!ctx) {
        return (void*)(intptr_t)thrd_error;
    }

    function = ctx->function;
    arg = ctx->arg;
    free(ctx);

    if (!function) {
        return (void*)(intptr_t)thrd_error;
    }

    result = function(arg);
    return (void*)(intptr_t)result;
}

static inline int thrd_create(thrd_t* thr, thrd_start_t function, void* arg) {
    tablo_thrd_start_ctx* ctx = NULL;
    int rc = 0;

    if (!thr || !function) {
        return thrd_error;
    }

    ctx = (tablo_thrd_start_ctx*)malloc(sizeof(*ctx));
    if (!ctx) {
        return thrd_nomem;
    }
    ctx->function = function;
    ctx->arg = arg;

    rc = pthread_create(thr, NULL, tablo_thrd_start_adapter, ctx);
    if (rc == 0) {
        return thrd_success;
    }

    free(ctx);
    return rc == ENOMEM ? thrd_nomem : thrd_error;
}

static inline int thrd_join(thrd_t thr, int* res) {
    void* retval = NULL;
    if (pthread_join(thr, &retval) != 0) {
        return thrd_error;
    }
    if (res) {
        *res = (int)(intptr_t)retval;
    }
    return thrd_success;
}

static inline int thrd_sleep(const struct timespec* duration, struct timespec* remaining) {
    if (!duration) {
        return -2;
    }
    if (nanosleep(duration, remaining) == 0) {
        return 0;
    }
    return errno == EINTR ? -1 : -2;
}

static inline int thrd_yield(void) {
    return sched_yield();
}

static inline int mtx_init(mtx_t* mutex, int type) {
    pthread_mutexattr_t attr;
    int rc = 0;

    if (!mutex) {
        return thrd_error;
    }
    if (pthread_mutexattr_init(&attr) != 0) {
        return thrd_error;
    }
    if ((type & mtx_recursive) != 0) {
        (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    }
    rc = pthread_mutex_init(mutex, &attr);
    (void)pthread_mutexattr_destroy(&attr);
    return rc == 0 ? thrd_success : thrd_error;
}

static inline int mtx_lock(mtx_t* mutex) {
    return pthread_mutex_lock(mutex) == 0 ? thrd_success : thrd_error;
}

static inline int mtx_unlock(mtx_t* mutex) {
    return pthread_mutex_unlock(mutex) == 0 ? thrd_success : thrd_error;
}

static inline void mtx_destroy(mtx_t* mutex) {
    if (mutex) {
        (void)pthread_mutex_destroy(mutex);
    }
}

static inline int cnd_init(cnd_t* cond) {
    return pthread_cond_init(cond, NULL) == 0 ? thrd_success : thrd_error;
}

static inline int cnd_signal(cnd_t* cond) {
    return pthread_cond_signal(cond) == 0 ? thrd_success : thrd_error;
}

static inline int cnd_broadcast(cnd_t* cond) {
    return pthread_cond_broadcast(cond) == 0 ? thrd_success : thrd_error;
}

static inline int cnd_wait(cnd_t* cond, mtx_t* mutex) {
    return pthread_cond_wait(cond, mutex) == 0 ? thrd_success : thrd_error;
}

static inline int cnd_timedwait(cnd_t* cond, mtx_t* mutex, const struct timespec* time_point) {
    int rc = pthread_cond_timedwait(cond, mutex, time_point);
    if (rc == 0) {
        return thrd_success;
    }
    return rc == ETIMEDOUT ? thrd_timedout : thrd_error;
}

static inline void cnd_destroy(cnd_t* cond) {
    if (cond) {
        (void)pthread_cond_destroy(cond);
    }
}

static inline void call_once(once_flag* flag, void (*func)(void)) {
    if (flag && func) {
        (void)pthread_once(flag, func);
    }
}

#endif

#endif
