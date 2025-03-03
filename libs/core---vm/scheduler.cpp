#include "pxt.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <errno.h>

#ifndef PXT_ESP32
// __MINGW32__ is defined on both mingw32 and mingw64
#ifdef __MINGW32__
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#endif

// should this be something like CXX11 or whatever?
#define THROW throw()
#define THREAD_DBG(...)

void *xmalloc(size_t sz) {
    auto r = malloc(sz);
    if (r == NULL)
        oops(50); // shouldn't happen
    return r;
}

void *operator new(size_t size) {
    return xmalloc(size);
}
void *operator new[](size_t size) {
    return xmalloc(size);
}

void operator delete(void *p)THROW {
    xfree(p);
}
void operator delete[](void *p) THROW {
    xfree(p);
}

uint8_t *gcBase;

namespace pxt {

static uint64_t startTime;

FiberContext *allFibers;
FiberContext *currentFiber;
static pthread_mutex_t eventMutex;
static pthread_cond_t newEventBroadcast;

static struct Event *eventHead, *eventTail;

struct Event {
    struct Event *next;
    int source;
    int value;
};

Event lastEvent;

Event *mkEvent(int source, int value) {
    auto res = new Event();
    memset(res, 0, sizeof(Event));
    res->source = source;
    res->value = value;
    return res;
}

volatile int panicCode;
extern "C" void drawPanic(int code);

void schedule() {
    auto f = currentFiber;
    if (!f->wakeTime && !f->waitSource)
        oops(55);
    f->resumePC = f->pc;
    f->pc = NULL; // this will break the exec_loop()
}

void dmesg_flush();

static void panic_core(int error_code) {
    int prevErr = errno;

    panicCode = error_code;

    drawPanic(error_code);

    DMESG("PANIC %d", error_code % 1000);
    DMESG("errno=%d %s", prevErr, strerror(prevErr));

    dmesg_flush();
}

extern "C" void target_panic(int error_code) {
    panic_core(error_code);

#if defined(PXT_ESP32)
    // sleep_core_us(5 * 1000 * 1000);
    abort();
#elif defined(PXT_VM)
    systemReset();
#else
    while (1)
        sleep_core_us(10000);
#endif
}

DLLEXPORT int pxt_get_panic_code() {
    return panicCode;
}

void soft_panic(int errorCode) {
    if (errorCode >= 999)
        errorCode = 999;
    if (errorCode <= 0)
        errorCode = 1;
    panic_core(1000 + errorCode);
    systemReset();
}

void sleep_core_us(uint64_t us) {
#ifdef PXT_ESP32
    uint64_t endp = esp_timer_get_time() + us;
    while (esp_timer_get_time() < endp)
        ;
#else
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    while (nanosleep(&ts, &ts))
        ;
#endif
}

void target_yield() {
#ifdef PXT_ESP32
    vTaskDelay(1);
#else
    sleep_core_us(1000);
#endif
}

void sleep_ms(uint32_t ms) {
    currentFiber->wakeTime = current_time_ms() + ms;
    schedule();
}

void sleep_us(uint64_t us) {
    if (us > 20000) {
        sleep_ms((uint32_t)(us / 1000));
    } else {
        sleep_core_us(us);
    }
}

#ifndef PXT_ESP32
static uint64_t currTime() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

uint64_t current_time_us() {
    if (!startTime)
        startTime = currTime();
    return currTime() - startTime;
}
#endif

int current_time_ms() {
    return (int)(current_time_us() / 1000);
}

void disposeFiber(FiberContext *t) {
    if (allFibers == t) {
        allFibers = t->next;
    } else {
        for (auto tt = allFibers; tt; tt = tt->next) {
            if (tt->next == t) {
                tt->next = t->next;
                break;
            }
        }
    }

    xfree(t->stackBase);
    xfree(t);
}

FiberContext *setupThread(Action a, TValue arg = 0) {
    // DMESG("setup thread: %p", a);
    auto t = (FiberContext *)xmalloc(sizeof(FiberContext));
    memset(t, 0, sizeof(*t));
    t->stackBase = (TValue *)xmalloc(VM_STACK_SIZE * sizeof(TValue));
    t->stackLimit = t->stackBase + VM_MAX_FUNCTION_STACK + 5;
    t->sp = t->stackBase + VM_STACK_SIZE;
    *--t->sp = (TValue)0xf00df00df00df00d;
    *--t->sp = 0;
    *--t->sp = 0;
    *--t->sp = 0;
    *--t->sp = arg;
    *--t->sp = 0;
    *--t->sp = TAG_STACK_BOTTOM;
    auto ra = (RefAction *)a;
    // we only pass 1 argument, but can in fact handle up to 4
    if (ra->numArgs > 2)
        target_panic(PANIC_INVALID_IMAGE);
    t->currAction = ra;
    t->resumePC = actionPC(ra);

    t->img = vmImg;
    t->imgbase = (uint16_t *)vmImg->dataStart;

    // add at the end
    if (allFibers)
        for (auto p = allFibers; p; p = p->next) {
            if (!p->next) {
                p->next = t;
                break;
            }
        }
    else
        allFibers = t;

    return t;
}

void runInParallel(Action a) {
    setupThread(a);
}

void runForever(Action a) {
    auto f = setupThread(a);
    f->foreverPC = f->resumePC;
}

void waitForEvent(int source, int value) {
    currentFiber->waitSource = source;
    currentFiber->waitValue = value;
    schedule();
}

FiberContext *suspendFiber() {
    currentFiber->waitSource = PXT_WAIT_SOURCE_PROMISE;
    schedule();
    return currentFiber;
}

void resumeFiberWithFn(FiberContext *ctx, fiber_resume_t fn, void *arg) {
    if (ctx->waitSource != PXT_WAIT_SOURCE_PROMISE)
        oops(52);
    ctx->waitSource = 0;
    ctx->wakeFn = fn;
    ctx->wakeFnArg = arg;
}

void resumeFiber(FiberContext *ctx, TValue v) {
    if (ctx->waitSource != PXT_WAIT_SOURCE_PROMISE)
        oops(52);
    ctx->waitSource = 0;
    ctx->r0 = v;
}

static void dispatchEvent(Event &e) {
    lastEvent = e;

    auto curr = findBinding(e.source, e.value);
    if (curr)
        setupThread(curr->action, fromInt(e.value));

    curr = findBinding(e.source, DEVICE_EVT_ANY);
    if (curr)
        setupThread(curr->action, fromInt(e.value));
}

static void wakeFibers() {
    for (;;) {
        pthread_mutex_lock(&eventMutex);
        if (eventHead == NULL) {
            pthread_mutex_unlock(&eventMutex);
            return;
        }
        Event *ev = eventHead;
        eventHead = ev->next;
        if (eventHead == NULL)
            eventTail = NULL;
        pthread_mutex_unlock(&eventMutex);

        for (auto thr = allFibers; thr; thr = thr->next) {
            if (thr->waitSource == 0)
                continue;
            if (thr->waitValue != ev->value && thr->waitValue != DEVICE_EVT_ANY)
                continue;
            if (thr->waitSource == ev->source) {
                thr->waitSource = 0;
            } else if (thr->waitSource == DEVICE_ID_NOTIFY && ev->source == DEVICE_ID_NOTIFY_ONE) {
                thr->waitSource = 0;
                break; // do not wake up any other threads
            }
        }

        dispatchEvent(*ev);
        delete ev;
    }
}

static void mainRunLoop() {
    FiberContext *f = NULL;
    for (;;) {
        if (panicCode)
            return;
        wakeFibers();
        auto now = current_time_ms();
        auto fromBeg = false;
        if (!f) {
            f = allFibers;
            fromBeg = true;
        }
        while (f) {
            if (f->wakeTime && now >= (int)f->wakeTime)
                f->wakeTime = 0;
            if (!f->wakeTime && !f->waitSource)
                break;
            f = f->next;
        }
        if (f) {
            currentFiber = f;
            f->pc = f->resumePC;
            f->resumePC = NULL;
            if (f->wakeFn) {
                auto fn = f->wakeFn;
                f->wakeFn = NULL;
                f->r0 = fn(f->wakeFnArg);
                if (f->wakeTime || f->waitSource)
                    continue; // we got suspended again
            }
            exec_loop(f);
            if (panicCode)
                return;
            auto n = f->next;
            if (f->resumePC == NULL) {
                if (f->foreverPC) {
                    f->resumePC = f->foreverPC;
                    f->wakeTime = current_time_ms() + 20;
                    // restore stack, as setupThread() does it
                    for (int i = 0; i < 5; ++i) {
                        if (*--f->sp == TAG_STACK_BOTTOM)
                            break;
                    }
                    if (*f->sp != TAG_STACK_BOTTOM)
                        target_panic(PANIC_INVALID_IMAGE);
                } else {
                    disposeFiber(f);
                }
            }
            f = n;
        } else if (fromBeg) {
            target_yield();
        }
    }
}

int allocateNotifyEvent() {
    static volatile int notifyId;
    return ++notifyId;
}

void raiseEvent(int id, int event) {
    auto e = mkEvent(id, event);
    pthread_mutex_lock(&eventMutex);
    if (eventTail == NULL) {
        if (eventHead != NULL)
            oops(51);
        eventHead = eventTail = e;
    } else {
        eventTail->next = e;
        eventTail = e;
    }
    pthread_cond_broadcast(&newEventBroadcast);
    pthread_mutex_unlock(&eventMutex);
}

DLLEXPORT void pxt_raise_event(int id, int event) {
    raiseEvent(id, event);
}

void registerWithDal(int id, int event, Action a, int flags) {
    // TODO support flags
    setBinding(id, event, a);
}

uint32_t afterProgramPage() {
    return 0;
}

char **initialArgv;

void screen_init();
void initKeys();
void target_startup();

void initRuntime() {
    current_time_ms();
    target_startup();

    setupThread((TValue)vmImg->entryPoint);

    target_init();
    screen_init();
    initKeys();

    DMESG("start main loop");

    mainRunLoop();
    systemReset();
}

void *gcAllocBlock(size_t sz) {
#ifdef PXT_ESP32
    void *r = xmalloc(sz);
#else
    static uint8_t *currPtr = (uint8_t *)GC_BASE;
    sz = (sz + GC_PAGE_SIZE - 1) & ~(GC_PAGE_SIZE - 1);
#if defined(PXT64) || defined(__MINGW32__)
    if (!gcBase) {
        gcBase = (uint8_t *)xmalloc(1 << PXT_VM_HEAP_ALLOC_BITS);
        currPtr = gcBase;
    }
    void *r = currPtr;
    if ((uint8_t *)currPtr - gcBase > (1 << PXT_VM_HEAP_ALLOC_BITS) - (int)sz)
        target_panic(20);
#else
    void *r = mmap(currPtr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (r == MAP_FAILED) {
        DMESG("mmap %p failed; err=%d", currPtr, errno);
        target_panic(PANIC_INTERNAL_ERROR);
    }
#endif
    currPtr = (uint8_t *)r + sz;
#endif

    if (isReadOnly((TValue)r)) {
        DMESG("mmap returned read-only address: %p", r);
        target_panic(PANIC_INTERNAL_ERROR);
    }
    return r;
}

void gcProcessStacks(int flags) {
    int cnt = 0;
    for (auto f = allFibers; f; f = f->next) {
        auto end = f->stackBase + VM_STACK_SIZE - 1;
        auto ptr = f->sp;
        gcProcess((TValue)f->currAction);
        gcProcess((TValue)f->r0);
        if (flags & 2)
            DMESG("RS%d:%p/%d", cnt++, ptr, end - ptr);
        // VLOG("mark: %p - %p", ptr, end);
        while (ptr <= end) {
            gcProcess(*ptr++);
        }
    }
}

#define MAX_RESET_FN 32
static reset_fn_t resetFunctions[MAX_RESET_FN];

void registerResetFunction(reset_fn_t fn) {
    for (int i = 0; i < MAX_RESET_FN; ++i) {
        if (!resetFunctions[i]) {
            resetFunctions[i] = fn;
            return;
        }
    }

    target_panic(PANIC_INTERNAL_ERROR);
}

void systemReset() {
#ifdef PXT_ESP32
    esp_restart();
#else
    if (!panicCode)
        panicCode = -1;

    dmesg("TARGET RESET");

    gcFreeze();

    for (int i = 0; i < MAX_RESET_FN; ++i) {
        auto fn = resetFunctions[i];
        if (fn)
            fn();
    }

    coreReset(); // clears handler bindings

    currentFiber = NULL;
    while (allFibers) {
        disposeFiber(allFibers);
    }

    // this will consume all events, but won't dispatch anything, since all listener maps are empty
    wakeFibers();

    // mark all GC memory as free
    gcReset();

    pthread_exit(NULL);
#endif
}

} // namespace pxt
