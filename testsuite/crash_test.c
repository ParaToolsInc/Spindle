/*
This file is part of Spindle.  For copyright information see the COPYRIGHT
file in the top level directory, or at
https://github.com/hpc/Spindle/blob/master/COPYRIGHT

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License (as published by the Free Software
Foundation) version 2.1 dated February 1999.  This program is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY; without even the IMPLIED
WARRANTY OF MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms
and conditions of the GNU Lesser General Public License for more details.  You should
have received a copy of the GNU Lesser General Public License along with this
program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

/*
 * Crash-handler test driver.
 * *
 * SIGSEGV modes:
 *   all-same       every rank crashes in crash_function_A
 *   all-different  rank N crashes in crash_function_<N>, up to 64 ranks
 *   partial        even ranks crash in crash_function_A,
 *                  odd ranks exit cleanly
 *   late-straggler rank 0 crashes immediately; others sleep --sleep
 *                  seconds before crashing
 *   two-groups     even ranks call crash_function_A, odd ranks call crash_function_B
 *   one-crashes    rank 0 crashes; others exit cleanly
 *   in-library     every rank dlopen()s libcrashfuncs.so and calls
 *                  crash_in_library()
 *   in-dlmopen-library
 *                  every rank dlmopen()s libcrashfuncs.so and calls crash_in_library()
 *   in-fixed-library
 *                  every rank dlopens() a library with a fixed load address
 *                  and calls a crashing function
 *   in-fixed-dlmopen-library
 *                  every rank dlmopen()s a fixed-load-address library into a new
 *                  namespace and calls a crashing function
 *   in-library-ctor rank 0 dlopen()s libcrashctor.so, whose constructor
 *                  crashes inside dlopen; others exit cleanly
 *   span-read      no app handler; a read spans two pages (PROT_READ then
 *                  PROT_NONE) and faults in the second page
 *   kill-segv      app sends SIGSEGV to itself via kill()
 *
 * SIGABRT modes:
 *   sigabrt        every rank calls abort()
 *   assert         every rank issues a failing assert()
 *   mixed-abort-segv
 *                  even ranks assert, odd ranks segfault
 *
 * Handler chaining modes
 *   safepoint      app handler fixes segfault on read
 *   safepoint-then-crash
 *                  app handler fixes segfault on read repeatedly,
 *                  then a real crash occurs on same thread.
 *   safepoint-bad  application handler returns but fails to fix read fault
 *   safepoint-bad-write
 *                  application handler returns but fails to fix write fault
 *   safepoint-fix-write
 *                  application handler fixes segfault on write
 *   safepoint-longjmp
                    application handler siglongjmp()s out of the handler
 *   safepoint-span-read
 *                  app handler fixes a read fault whose access spans two pages
 *                  (starts in an accessible page, faults in the next one)
 *   safepoint-span-write
 *                  app handler fixes a write fault whose access spans two pages
 *   safepoint-span-bad-read
 *                  app handler returns without fixing a page-spanning read fault
 *   safepoint-span-bad-write
 *                  app handler returns without fixing a page-spanning write fault
 *   mmap-sigbus-bad
 *                  app handler does not fix access past the end of an mmap'ed file
 *   mmap-sigbus-fixed
 *                  app handler fixes access past the end of an mmap'ed file by extending it
 *   chained-kill-segv
 *                  app handler handles a kill()-sent SIGSEGV and returns
 *   ignored-kill-segv
 *                  app sets SIGSEGV disposition to SIG_IGN via signal()
 *                  then kill()s itself with SIGSEGV
 *   ignored-siginfo-kill-segv
 *                  app sets SIGSEGV disposition to SIG_IGN via sigaction()
 *                  then kill()s itself with SIGSEGV
 *   default-siginfo-kill-segv
 *                  app explicitly sets SIG_DFL via sigaction() with SA_SIGINFO
 *                  then kill()s itself with SIGSEGV
 *
 * Fork modes.  
 *   fork-child-prereconnect
 *                  child sets core limit to zero and crashes at
 *                  crash_function_A before making any call Spindle intercepts; 
 *                  parent waits, then crashes at the same site
 *   fork-child-reconnect
 *                  child makes an intercepted call, which reconnects it
 *                  with OPT_FOLLOWFORK and crashes at crash_function_A;
 *                  parent exits cleanly 
 *   fork-child-reconnect-then-parent
 *                  as fork-child-reconnect, but the parent then crashes at
 *                  the same site
 *   fork-child-nofollow
 *                  child makes an intercepted call but does not reconnect when
 *                  run with --follow-fork=false
 *   fork-child-inherited-safepoint
 *                  parent installs safepoint handler before forking; parent 
 *                  and child recover from safepoint faults through the handler
 *   fork-exec-child-crash
 *                  child execs this program with --crash-mode
 *                  all-same-no-mpi; parent exits cleanly.
 *   all-same-no-mpi
 *                  crashes in crash_function_A without calling MPI_Init or
 *                  creating a per-rank directory
 *
 *   no-crash       every rank exits cleanly
 *
 */

#define _GNU_SOURCE
#include <mpi.h>
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>

#include "crash_functions.h"

#define SAFEPOINT_RC_INCOMPLETE      103
#define SAFEPOINT_RC_SETUP_FAILED    104
#define SAFEPOINT_RC_NOT_TERMINATED  105

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s --crash-mode {all-same|all-different|partial|"
            "late-straggler|two-groups|one-crashes|in-library|"
            "in-dlmopen-library|in-fixed-library|in-fixed-dlmopen-library|"
            "in-library-ctor|sigabrt|assert|"
            "long-assert|mixed-abort-segv|kill-segv|span-read|"
            "safepoint|safepoint-then-crash|"
            "safepoint-bad|safepoint-bad-write|safepoint-fix-write|"
            "safepoint-span-read|safepoint-span-write|"
            "safepoint-span-bad-read|safepoint-span-bad-write|"
            "mmap-sigbus-bad|mmap-sigbus-fixed|chained-kill-segv|"
            "ignored-kill-segv|ignored-siginfo-kill-segv|default-siginfo-kill-segv|"
            "safepoint-longjmp|safepoint-longjmp-mt|safepoint-concurrent-chain|"
            "fork-child-prereconnect|fork-child-reconnect|"
            "fork-child-reconnect-then-parent|fork-child-nofollow|"
            "fork-child-inherited-safepoint|fork-exec-child-crash|"
            "all-same-no-mpi|no-crash}"
            " [--sleep <seconds>] [--cycles <n>]\n",
            prog);
}

static inline int addr_in_range(uintptr_t a, void *base, size_t len) {
    uintptr_t lo = (uintptr_t) base;
    return a >= lo && a < lo + len;
}

static void *map_trap_page(const char *mode, int rank, long pagesize) {
    void *p = mmap(NULL, pagesize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "%s rank=%d: mmap failed\n", mode, rank);
        return NULL;
    }
    return p;
}

static int install_sigsegv_handler(void (*handler)(int, siginfo_t *, void *),
                                   const char *mode, int rank) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL) != 0) {
        fprintf(stderr, "%s rank=%d: sigaction failed\n", mode, rank);
        return -1;
    }
    return 0;
}

static void handler_die(const char *msg) {
    ssize_t w = write(2, msg, strlen(msg));
    (void) w;
    _exit(SAFEPOINT_RC_SETUP_FAILED);
}

static void call_crash_in_library_common(int rank, int use_dlmopen) {
    const char *dlopen_name = use_dlmopen ? "dlmopen" : "dlopen";
    void *h = use_dlmopen ? dlmopen(LM_ID_NEWLM, "./libcrashfuncs.so", RTLD_NOW)
                          : dlopen("./libcrashfuncs.so", RTLD_NOW);
    if (!h)
        h = use_dlmopen ? dlmopen(LM_ID_NEWLM, "libcrashfuncs.so", RTLD_NOW)
                        : dlopen("libcrashfuncs.so", RTLD_NOW);
    if (!h) {
        fprintf(stderr, "rank=%d %s of libcrashfuncs.so failed: %s\n",
                rank, dlopen_name, dlerror());
        exit(3);
    }
    void (*fn)(int) = (void (*)(int)) dlsym(h, "crash_in_library");
    if (!fn) {
        fprintf(stderr, "rank=%d dlsym of crash_in_library failed: %s\n",
                rank, dlerror());
        exit(3);
    }
    fn(rank);
}

static void call_crash_in_fixed_library_common(int rank, int use_dlmopen) {
    const char *dlopen_name = use_dlmopen ? "dlmopen" : "dlopen";
    void *h = use_dlmopen ? dlmopen(LM_ID_NEWLM, "./libcrashfixed.so", RTLD_NOW)
                          : dlopen("./libcrashfixed.so", RTLD_NOW);
    if (!h)
        h = use_dlmopen ? dlmopen(LM_ID_NEWLM, "libcrashfixed.so", RTLD_NOW)
                        : dlopen("libcrashfixed.so", RTLD_NOW);
    if (!h) {
        fprintf(stderr, "rank=%d %s of libcrashfixed.so failed: %s\n",
                rank, dlopen_name, dlerror());
        exit(3);
    }
    void (*fn)(int) = (void (*)(int)) dlsym(h, "crash_in_fixed_library");
    if (!fn) {
        fprintf(stderr, "rank=%d dlsym of crash_in_fixed_library failed: %s\n",
                rank, dlerror());
        exit(3);
    }
    fn(rank);
}

static void crash_in_library_ctor(int rank) {
    if (rank != 0) {
        fprintf(stderr, "rank=%d exiting cleanly\n", rank);
        fflush(stderr);
        return;
    }

    void *handle = dlopen("./libcrashctor.so", RTLD_NOW);
    if (!handle)
        handle = dlopen("libcrashctor.so", RTLD_NOW);

    (void) handle;
    exit(EXIT_FAILURE);
}

static void do_assert(int rank) {
    assert(rank < 0 && "Assertion message");
}

static void do_mixed_abort_segv(int rank) {
    if ((rank % 2) == 0) {
        assert(0 && "Assertion message");
    } else {
        *(volatile int *) 0 = 0;
    }
}

static void do_kill_segv(int rank) {
    kill(getpid(), SIGSEGV);
    fprintf(stderr, "kill-segv rank=%d: unexpectedly continued after kill(SIGSEGV)\n", rank);
    _exit(SAFEPOINT_RC_NOT_TERMINATED);
}

static void do_span_read(int rank) {
    long pagesize = sysconf(_SC_PAGESIZE);
    unsigned char *region = mmap(NULL, 2 * pagesize, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) {
        fprintf(stderr, "span-read rank=%d: mmap failed\n", rank);
        _exit(SAFEPOINT_RC_SETUP_FAILED);
    }
    memset(region, 0xAB, 2 * pagesize);
    if (mprotect(region, pagesize, PROT_READ) != 0 ||
        mprotect(region + pagesize, pagesize, PROT_NONE) != 0) {
        fprintf(stderr, "span-read rank=%d: mprotect failed\n", rank);
        _exit(SAFEPOINT_RC_SETUP_FAILED);
    }

    volatile uint64_t v = *(volatile uint64_t *) (region + pagesize - 4);
    (void) v;

    fprintf(stderr, "span-read rank=%d: unexpectedly continued after SIGSEGV\n", rank);
    _exit(SAFEPOINT_RC_NOT_TERMINATED);
}

/* Safepoint tests. */

#define SAFEPOINT_CYCLES_DEFAULT 100

static void          *safepoint_page;
static long           safepoint_pagesize;
static volatile int   safepoint_handler_invocations;

static void safepoint_nofix_handler(int sig, siginfo_t *info, void *uctx) {
    (void) sig; (void) info; (void) uctx;
    /* App handler that doesn't fix the problem. */
    safepoint_handler_invocations++;
}

static int safepoint_result(const char *mode, int rank, int cycles) {
    if (safepoint_handler_invocations == 0) {
        fprintf(stderr, "%s rank=%d: handler never invoked\n", mode, rank);
        return SAFEPOINT_RC_INCOMPLETE;
    }
    fprintf(stderr,
            "%s rank=%d: completed %d safepoint cycles, recorded %d invocations\n",
            mode, rank, cycles, safepoint_handler_invocations);
    return 0;
}

static int setup_safepoint(const char *mode, int rank,
                           void (*handler)(int, siginfo_t *, void *)) {
    safepoint_pagesize = sysconf(_SC_PAGESIZE);
    safepoint_page = map_trap_page(mode, rank, safepoint_pagesize);
    if (safepoint_page == NULL)
        return SAFEPOINT_RC_SETUP_FAILED;
    if (install_sigsegv_handler(handler, mode, rank) != 0)
        return SAFEPOINT_RC_SETUP_FAILED;
    return 0;
}

static void safepoint_sigsegv_handler(int sig, siginfo_t *info, void *uctx) {
    (void) sig; (void) info; (void) uctx;
    safepoint_handler_invocations++;

    /* Unprotect the page so the read succeeds on retry. */
    if (mprotect(safepoint_page, safepoint_pagesize, PROT_READ) != 0)
        handler_die("mprotect failed in safepoint_sigsegv_handler\n");
}

static int do_safepoint(int rank, int cycles) {
    int rc = setup_safepoint("safepoint", rank, safepoint_sigsegv_handler);
    if (rc)
        return rc;

    for (int i = 0; i < cycles; i++) {
        if (mprotect(safepoint_page, safepoint_pagesize, PROT_NONE) != 0) {
            fprintf(stderr, "safepoint rank=%d cycle=%d: mprotect PROT_NONE failed\n",
                    rank, i);
            return SAFEPOINT_RC_SETUP_FAILED;
        }
        volatile int v = *(volatile int *) safepoint_page;
        (void) v;
    }

    return safepoint_result("safepoint", rank, cycles);
}

#define SAFEPOINT_THEN_CRASH_PRECYCLES 5

static volatile int   stc_safepoint_fixes;
static volatile int   stc_realcrash_observed;

static void safepoint_then_crash_handler(int sig, siginfo_t *info, void *uctx) {
    (void) sig; (void) uctx;

    int is_safepoint = 0;
    if (info != NULL && info->si_addr != NULL &&
        addr_in_range((uintptr_t) info->si_addr, safepoint_page, safepoint_pagesize))
        is_safepoint = 1;

    if (is_safepoint) {
        stc_safepoint_fixes++;
        if (mprotect(safepoint_page, safepoint_pagesize, PROT_READ) != 0) {
            _exit(SAFEPOINT_RC_SETUP_FAILED);
        }
        return;
    }

    stc_realcrash_observed = 1;
    /* This wasn't in the safepoint page, so restore default disposition and return. */
    signal(SIGSEGV, SIG_DFL);
}

static int do_safepoint_then_crash(int rank) {
    int rc = setup_safepoint("safepoint-then-crash", rank,
                             safepoint_then_crash_handler);
    if (rc)
        return rc;

    for (int i = 0; i < SAFEPOINT_THEN_CRASH_PRECYCLES; i++) {
        if (mprotect(safepoint_page, safepoint_pagesize, PROT_NONE) != 0) {
            fprintf(stderr,
                    "safepoint-then-crash rank=%d cycle=%d: mprotect PROT_NONE failed\n",
                    rank, i);
            return SAFEPOINT_RC_SETUP_FAILED;
        }
        volatile int v = *(volatile int *) safepoint_page;
        (void) v;
    }
    fprintf(stderr,
            "safepoint-then-crash rank=%d: completed %d safepoint cycles, now triggering real crash\n",
            rank, stc_safepoint_fixes);
    fflush(stderr);

    crash_function_A(rank);

    fprintf(stderr, "safepoint-then-crash rank=%d: crash_function_A unexpectedly returned\n", rank);
    return SAFEPOINT_RC_NOT_TERMINATED;
}

static int do_safepoint_bad(int rank) {
    int rc = setup_safepoint("safepoint-bad", rank, safepoint_nofix_handler);
    if (rc)
        return rc;

    if (mprotect(safepoint_page, safepoint_pagesize, PROT_NONE) != 0) {
        fprintf(stderr,
                "safepoint-bad rank=%d: mprotect PROT_NONE failed\n", rank);
        return SAFEPOINT_RC_SETUP_FAILED;
    }

    volatile int v = *(volatile int *) safepoint_page;
    (void) v;

    fprintf(stderr,
            "safepoint-bad rank=%d: unexpectedly continued after unrecovered SIGSEGV\n",
            rank);
    return SAFEPOINT_RC_NOT_TERMINATED;
}

static int do_safepoint_bad_write(int rank) {
    int rc = setup_safepoint("safepoint-bad-write", rank, safepoint_nofix_handler);
    if (rc)
        return rc;

    if (mprotect(safepoint_page, safepoint_pagesize, PROT_READ) != 0) {
        fprintf(stderr,
                "safepoint-bad-write rank=%d: mprotect PROT_READ failed\n",
                rank);
        return SAFEPOINT_RC_SETUP_FAILED;
    }

    *(volatile int *) safepoint_page = 0;

    fprintf(stderr,
            "safepoint-bad-write rank=%d: unexpectedly continued after unrecovered SIGSEGV\n",
            rank);
    return SAFEPOINT_RC_NOT_TERMINATED;
}

static void safepoint_fix_write_handler(int sig, siginfo_t *info, void *uctx) {
    (void) sig; (void) info; (void) uctx;

    safepoint_handler_invocations++;

    if (mprotect(safepoint_page, safepoint_pagesize,
                 PROT_READ | PROT_WRITE) != 0)
        handler_die("safepoint-fix-write: mprotect failed in handler\n");
}

static int do_safepoint_fix_write(int rank, int cycles) {
    int rc = setup_safepoint("safepoint-fix-write", rank,
                             safepoint_fix_write_handler);
    if (rc) return rc;

    for (int i = 0; i < cycles; i++) {
        if (mprotect(safepoint_page, safepoint_pagesize, PROT_READ) != 0) {
            fprintf(stderr,
                    "safepoint-fix-write rank=%d: mprotect PROT_READ failed at cycle %d\n",
                    rank, i);
            return SAFEPOINT_RC_SETUP_FAILED;
        }
        *(volatile int *) safepoint_page = i;
    }

    return safepoint_result("safepoint-fix-write", rank, cycles);
}

static sigjmp_buf longjmp_env;
static volatile int longjmp_cycles_completed;

static void safepoint_longjmp_handler(int sig, siginfo_t *info, void *uctx) {
    (void) sig; (void) info; (void) uctx;
    if (mprotect(safepoint_page, safepoint_pagesize, PROT_READ) != 0)
        handler_die("safepoint-longjmp: mprotect failed in handler\n");
    longjmp_cycles_completed++;
    siglongjmp(longjmp_env, 1);
}

static int do_safepoint_longjmp(int rank, int cycles) {
    int rc = setup_safepoint("safepoint-longjmp", rank,
                             safepoint_longjmp_handler);
    if (rc)
        return rc;

    for (int i = 0; i < cycles; i++) {
        if (sigsetjmp(longjmp_env, 1) == 0) {
            if (mprotect(safepoint_page, safepoint_pagesize,
                         PROT_NONE) != 0) {
                fprintf(stderr,
                        "safepoint-longjmp rank=%d cycle=%d: mprotect PROT_NONE failed\n", rank, i);
                return SAFEPOINT_RC_SETUP_FAILED;
            }
            volatile int v = *(volatile int *) safepoint_page;
            (void) v;
            fprintf(stderr,
                    "safepoint-longjmp rank=%d: unexpectedly continued past siglongjmp\n", rank);
            return SAFEPOINT_RC_NOT_TERMINATED;
        }
    }

    if (longjmp_cycles_completed != cycles) {
        fprintf(stderr,
                "safepoint-longjmp rank=%d: completed %d of %d cycles\n",
                rank, longjmp_cycles_completed, cycles);
        return SAFEPOINT_RC_INCOMPLETE;
    }
    fprintf(stderr,
            "safepoint-longjmp rank=%d: completed %d siglongjmp cycles\n",
            rank, cycles);
    return 0;
}

static unsigned char *span_region;
static long           span_pagesize;

static unsigned char *span_page2(void) { return span_region + span_pagesize; }

static volatile uint64_t *span_access(void) {
    return (volatile uint64_t *) (span_page2() - 4);
}

static int setup_span(const char *mode, int rank,
                      void (*handler)(int, siginfo_t *, void *)) {
    span_pagesize = sysconf(_SC_PAGESIZE);
    span_region = mmap(NULL, 2 * span_pagesize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (span_region == MAP_FAILED) {
        fprintf(stderr, "%s rank=%d: mmap failed\n", mode, rank);
        return SAFEPOINT_RC_SETUP_FAILED;
    }
    memset(span_region, 0xAB, 2 * span_pagesize);
    if (install_sigsegv_handler(handler, mode, rank) != 0)
        return SAFEPOINT_RC_SETUP_FAILED;
    return 0;
}

static void span_read_fix_handler(int sig, siginfo_t *info, void *uctx) {
    (void) sig; (void) info; (void) uctx;
    safepoint_handler_invocations++;
    if (mprotect(span_page2(), span_pagesize, PROT_READ) != 0)
        handler_die("safepoint-span-read: mprotect failed in handler\n");
}

static void span_write_fix_handler(int sig, siginfo_t *info, void *uctx) {
    (void) sig; (void) info; (void) uctx;
    safepoint_handler_invocations++;
    if (mprotect(span_page2(), span_pagesize, PROT_READ | PROT_WRITE) != 0)
        handler_die("safepoint-span-write: mprotect failed in handler\n");
}

static int do_safepoint_span_read(int rank, int cycles) {
    int rc = setup_span("safepoint-span-read", rank, span_read_fix_handler);
    if (rc)
        return rc;

    for (int i = 0; i < cycles; i++) {
        if (mprotect(span_page2(), span_pagesize, PROT_NONE) != 0) {
            fprintf(stderr,
                    "safepoint-span-read rank=%d cycle=%d: mprotect PROT_NONE failed\n",
                    rank, i);
            return SAFEPOINT_RC_SETUP_FAILED;
        }
        volatile uint64_t v = *span_access();
        (void) v;
    }

    return safepoint_result("safepoint-span-read", rank, cycles);
}

static int do_safepoint_span_write(int rank, int cycles) {
    int rc = setup_span("safepoint-span-write", rank, span_write_fix_handler);
    if (rc)
        return rc;

    for (int i = 0; i < cycles; i++) {
        if (mprotect(span_page2(), span_pagesize, PROT_NONE) != 0) {
            fprintf(stderr,
                    "safepoint-span-write rank=%d cycle=%d: mprotect PROT_NONE failed\n",
                    rank, i);
            return SAFEPOINT_RC_SETUP_FAILED;
        }
        *span_access() = (uint64_t) i;
    }

    return safepoint_result("safepoint-span-write", rank, cycles);
}

static int do_safepoint_span_bad_read(int rank) {
    int rc = setup_span("safepoint-span-bad-read", rank, safepoint_nofix_handler);
    if (rc)
        return rc;

    if (mprotect(span_page2(), span_pagesize, PROT_NONE) != 0) {
        fprintf(stderr,
                "safepoint-span-bad-read rank=%d: mprotect PROT_NONE failed\n", rank);
        return SAFEPOINT_RC_SETUP_FAILED;
    }

    volatile uint64_t v = *span_access();
    (void) v;

    fprintf(stderr,
            "safepoint-span-bad-read rank=%d: unexpectedly continued after unrecovered SIGSEGV\n",
            rank);
    return SAFEPOINT_RC_NOT_TERMINATED;
}

static int do_safepoint_span_bad_write(int rank) {
    int rc = setup_span("safepoint-span-bad-write", rank, safepoint_nofix_handler);
    if (rc)
        return rc;

    if (mprotect(span_page2(), span_pagesize, PROT_NONE) != 0) {
        fprintf(stderr,
                "safepoint-span-bad-write rank=%d: mprotect PROT_NONE failed\n", rank);
        return SAFEPOINT_RC_SETUP_FAILED;
    }

    *span_access() = 0;

    fprintf(stderr,
            "safepoint-span-bad-write rank=%d: unexpectedly continued after unrecovered SIGSEGV\n",
            rank);
    return SAFEPOINT_RC_NOT_TERMINATED;
}

static int             mmap_sigbus_fd = -1;
static long            mmap_sigbus_pagesize;
static unsigned char  *mmap_sigbus_map;

static volatile char *mmap_sigbus_target(void) {
    return (volatile char *) (mmap_sigbus_map + mmap_sigbus_pagesize);
}

static int setup_mmap_sigbus(const char *mode, int rank,
                             void (*handler)(int, siginfo_t *, void *)) {
    mmap_sigbus_pagesize = sysconf(_SC_PAGESIZE);

    char path[] = "/tmp/spindle_mmap_sigbus_XXXXXX";
    mmap_sigbus_fd = mkstemp(path);
    if (mmap_sigbus_fd < 0) {
        fprintf(stderr, "%s rank=%d: mkstemp failed\n", mode, rank);
        return SAFEPOINT_RC_SETUP_FAILED;
    }
    (void) unlink(path);
    if (ftruncate(mmap_sigbus_fd, (off_t) mmap_sigbus_pagesize) != 0) {
        fprintf(stderr, "%s rank=%d: ftruncate failed\n", mode, rank);
        return SAFEPOINT_RC_SETUP_FAILED;
    }
    mmap_sigbus_map = mmap(NULL, 2 * mmap_sigbus_pagesize,
                           PROT_READ | PROT_WRITE, MAP_SHARED,
                           mmap_sigbus_fd, 0);
    if (mmap_sigbus_map == MAP_FAILED) {
        fprintf(stderr, "%s rank=%d: mmap failed\n", mode, rank);
        return SAFEPOINT_RC_SETUP_FAILED;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGBUS, &sa, NULL) != 0) {
        fprintf(stderr, "%s rank=%d: sigaction(SIGBUS) failed\n", mode, rank);
        return SAFEPOINT_RC_SETUP_FAILED;
    }
    return 0;
}

static void mmap_sigbus_fix_handler(int sig, siginfo_t *info, void *uctx) {
    (void) sig; (void) info; (void) uctx;
    safepoint_handler_invocations++;
    /* Increase the size of the file so the retried read succeeds */
    if (ftruncate(mmap_sigbus_fd, (off_t) (2 * mmap_sigbus_pagesize)) != 0)
        handler_die("ftruncate failed in handler\n");
}

static int do_mmap_sigbus_fixed(int rank) {
    int rc = setup_mmap_sigbus("mmap-sigbus-fixed", rank,
                               mmap_sigbus_fix_handler);
    if (rc)
        return rc;

    /* Read one page past EOF. The app handler extends the file so the retried
       read succeeds. */
    volatile char v = *mmap_sigbus_target();
    (void) v;

    if (safepoint_handler_invocations == 0) {
        fprintf(stderr, "mmap-sigbus-fixed rank=%d: handler never invoked\n", rank);
        return SAFEPOINT_RC_INCOMPLETE;
    }
    return 0;
}

static int do_mmap_sigbus_bad(int rank) {
    int rc = setup_mmap_sigbus("mmap-sigbus-bad", rank, safepoint_nofix_handler);
    if (rc)
        return rc;

    volatile char v = *mmap_sigbus_target();
    (void) v;

    fprintf(stderr,
            "mmap-sigbus-bad rank=%d: unexpectedly continued after unrecovered SIGBUS\n",
            rank);
    return SAFEPOINT_RC_NOT_TERMINATED;
}

static void chained_kill_handler(int sig, siginfo_t *info, void *uctx) {
    (void) sig; (void) uctx;
    if (info == NULL || info->si_code > 0)
        handler_die("chained-kill-segv: handler unexpectedly got a kernel fault\n");
    safepoint_handler_invocations++;
}

static int do_chained_kill_segv(int rank) {
    if (install_sigsegv_handler(chained_kill_handler, "chained-kill-segv", rank) != 0)
        return SAFEPOINT_RC_SETUP_FAILED;

    kill(getpid(), SIGSEGV);

    if (safepoint_handler_invocations != 1) {
        fprintf(stderr, "chained-kill-segv rank=%d: handler invoked %d times\n",
                rank, safepoint_handler_invocations);
        return SAFEPOINT_RC_INCOMPLETE;
    }
    fprintf(stderr, "chained-kill-segv rank=%d: correctly survived handled kill(SIGSEGV)\n",
            rank);
    return 0;
}

/* Install a special disposition (SIG_IGN or SIG_DFL) for SIGSEGV through
   sigaction() with the given flags, then read it back and check that the
   same value is returned. This tests Spindle's sigaction wrapper. */
static int install_special_sigsegv(void *disposition, int flags,
                                   const char *mode, int rank) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = (void (*)(int)) disposition;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL) != 0) {
        fprintf(stderr, "%s rank=%d: sigaction failed\n", mode, rank);
        return -1;
    }
    memset(&old, 0, sizeof old);
    if (sigaction(SIGSEGV, NULL, &old) != 0) {
        fprintf(stderr, "%s rank=%d: sigaction readback failed\n", mode, rank);
        return -1;
    }
    if ((void *) old.sa_handler != disposition) {
        fprintf(stderr, "%s rank=%d: disposition read back was different from expected: installed %p, read back %p\n",
                mode, rank, disposition, (void *) old.sa_handler);
        return -1;
    }
    return 0;
}

/* A SIGSEGV sent via kill() with SIG_IGN set should be discarded. */
static int do_ignored_kill_segv_common(const char *mode, int rank, int use_siginfo) {
    if (use_siginfo) {
        if (install_special_sigsegv((void *) SIG_IGN, SA_SIGINFO, mode, rank) != 0)
            return SAFEPOINT_RC_SETUP_FAILED;
    } else {
        if (signal(SIGSEGV, SIG_IGN) == SIG_ERR) {
            fprintf(stderr, "%s rank=%d: signal(SIGSEGV, SIG_IGN) failed\n", mode, rank);
            return SAFEPOINT_RC_SETUP_FAILED;
        }
    }

    /* Send SIGSEGV to ourself; SIG_IGN is set, so this is expected to return. */
    kill(getpid(), SIGSEGV);

    struct sigaction old;
    memset(&old, 0, sizeof old);
    if (sigaction(SIGSEGV, NULL, &old) != 0) {
        fprintf(stderr, "%s rank=%d: sigaction readback failed\n", mode, rank);
        return SAFEPOINT_RC_INCOMPLETE;
    }
    if (old.sa_handler != SIG_IGN) {
        fprintf(stderr, "%s rank=%d: unexpectedly got value other than SIG_IGN: got %p\n",
                mode, rank, (void *) old.sa_handler);
        return SAFEPOINT_RC_INCOMPLETE;
    }
    fprintf(stderr, "%s rank=%d: correctly survived ignored kill(SIGSEGV)\n", mode, rank);
    return 0;
}

static int do_ignored_kill_segv(int rank) {
    return do_ignored_kill_segv_common("ignored-kill-segv", rank, 0);
}

static int do_ignored_siginfo_kill_segv(int rank) {
    return do_ignored_kill_segv_common("ignored-siginfo-kill-segv", rank, 1);
}

/* Verify that explicitly setting SIG_DFL with SA_SIGINFO still
   produces the default disposition. */
static void do_default_siginfo_kill_segv(int rank) {
    if (install_special_sigsegv((void *) SIG_DFL, SA_SIGINFO,
                                "default-siginfo-kill-segv", rank) != 0)
        _exit(SAFEPOINT_RC_SETUP_FAILED);
    kill(getpid(), SIGSEGV);
    fprintf(stderr, "default-siginfo-kill-segv rank=%d: unexpectedly continued after kill(SIGSEGV)\n", rank);
    _exit(SAFEPOINT_RC_NOT_TERMINATED);
}

/* Fork modes. */

static const char *crash_test_argv0;

static void child_banner(const char *mode, int rank, int size) {
    fprintf(stderr, "crash_test rank=%d size=%d mode=%s pid=%d (fork child)\n",
            rank, size, mode, (int) getpid());
    fflush(stderr);
}

/* An intercepted call to cause the child to reconnect. */
static void child_intercepted_call(const char *mode, int rank) {
    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "%s rank=%d: child open(/dev/null) failed\n", mode, rank);
        _exit(SAFEPOINT_RC_SETUP_FAILED);
    }
    close(fd);
}

static void child_disable_core(const char *mode, int rank) {
    struct rlimit no_core = { 0, 0 };
    if (setrlimit(RLIMIT_CORE, &no_core) != 0) {
        fprintf(stderr, "%s rank=%d: child setrlimit failed\n", mode, rank);
        _exit(SAFEPOINT_RC_SETUP_FAILED);
    }
}

static int wait_for_segv(const char *mode, int rank, pid_t child) {
    int status = 0;
    if (waitpid(child, &status, 0) != child) {
        fprintf(stderr, "%s rank=%d: waitpid failed\n", mode, rank);
        return -1;
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        fprintf(stderr, "%s rank=%d: child did not die by SIGSEGV (status 0x%x)\n",
                mode, rank, status);
        return -1;
    }
    fprintf(stderr, "%s rank=%d: child died by SIGSEGV\n", mode, rank);
    return 0;
}

static pid_t fork_or_die(const char *mode, int rank) {
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "%s rank=%d: fork failed\n", mode, rank);
        _exit(SAFEPOINT_RC_SETUP_FAILED);
    }
    return child;
}

/* Child crashes without a reconnect opportunity.
   The crash in the child will be ignored by Spindle. */
static void do_fork_child_prereconnect(int rank) {
    const char *mode = "fork-child-prereconnect";
    pid_t child = fork_or_die(mode, rank);
    if (child == 0) {
        child_disable_core(mode, rank);
        crash_function_A(rank);
        _exit(SAFEPOINT_RC_NOT_TERMINATED);
    }
    if (wait_for_segv(mode, rank, child) != 0)
        _exit(SAFEPOINT_RC_INCOMPLETE);
    crash_function_A(rank);
    _exit(SAFEPOINT_RC_NOT_TERMINATED);
}

/* Fork a child that performs a Spindle-intercepted call to allow
   reconnection, then crashes at crash_function_A. */
static int fork_reconnecting_crasher(const char *mode, int rank, int size) {
    pid_t child = fork_or_die(mode, rank);
    if (child == 0) {
        child_intercepted_call(mode, rank);
        child_banner(mode, rank, size);
        crash_function_A(rank);
        _exit(SAFEPOINT_RC_NOT_TERMINATED);
    }
    if (wait_for_segv(mode, rank, child) != 0)
        return SAFEPOINT_RC_INCOMPLETE;
    return 0;
}

/* Child reconnects, then crashes while the parent exits cleanly. */
static int do_fork_child_reconnect(int rank, int size) {
    return fork_reconnecting_crasher("fork-child-reconnect", rank, size);
}

/* Child reconnects and crashes, then the parent crashes at the same site. */
static void do_fork_child_reconnect_then_parent(int rank, int size) {
    const char *mode = "fork-child-reconnect-then-parent";
    if (fork_reconnecting_crasher(mode, rank, size) != 0)
        _exit(SAFEPOINT_RC_INCOMPLETE);
    crash_function_A(rank);
    _exit(SAFEPOINT_RC_NOT_TERMINATED);
}

/* Meant to run with --follow-fork=false */
static void do_fork_child_nofollow(int rank) {
    const char *mode = "fork-child-nofollow";
    pid_t child = fork_or_die(mode, rank);
    if (child == 0) {
        child_intercepted_call(mode, rank);
        child_disable_core(mode, rank);
        crash_function_A(rank);
        _exit(SAFEPOINT_RC_NOT_TERMINATED);
    }
    if (wait_for_segv(mode, rank, child) != 0)
        _exit(SAFEPOINT_RC_INCOMPLETE);
    crash_function_A(rank);
    _exit(SAFEPOINT_RC_NOT_TERMINATED);
}

/* Fault on the safepoint page `cycles` times. */
static int safepoint_cycles_inherited(const char *mode, int rank, int cycles) {
    for (int i = 0; i < cycles; i++) {
        if (mprotect(safepoint_page, safepoint_pagesize, PROT_NONE) != 0) {
            fprintf(stderr, "%s rank=%d cycle=%d: mprotect PROT_NONE failed\n",
                    mode, rank, i);
            return SAFEPOINT_RC_SETUP_FAILED;
        }
        volatile int v = *(volatile int *) safepoint_page;
        (void) v;
    }
    return 0;
}

static int do_fork_child_inherited_safepoint(int rank, int cycles) {
    const char *mode = "fork-child-inherited-safepoint";
    int rc = setup_safepoint(mode, rank, safepoint_sigsegv_handler);
    if (rc)
        return rc;
    pid_t child = fork_or_die(mode, rank);
    if (child == 0) {
        rc = safepoint_cycles_inherited(mode, rank, cycles);
        if (rc == 0 && safepoint_handler_invocations < cycles)
            rc = SAFEPOINT_RC_INCOMPLETE;
        fprintf(stderr, "%s rank=%d: child completed %d cycles, %d invocations, rc=%d\n",
                mode, rank, cycles, safepoint_handler_invocations, rc);
        _exit(rc);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s rank=%d: child failed (status 0x%x)\n", mode, rank, status);
        return SAFEPOINT_RC_INCOMPLETE;
    }
    rc = safepoint_cycles_inherited(mode, rank, cycles);
    if (rc)
        return rc;
    return safepoint_result(mode, rank, cycles);
}

/* Child execs this program in the all-same-no-mpi mode and crashes there. */
static int do_fork_exec_child_crash(int rank, int size) {
    const char *mode = "fork-exec-child-crash";
    (void) size;
    pid_t child = fork_or_die(mode, rank);
    if (child == 0) {
        execl(crash_test_argv0, crash_test_argv0,
              "--crash-mode", "all-same-no-mpi", (char *) NULL);
        fprintf(stderr, "%s rank=%d: execl(%s) failed\n", mode, rank, crash_test_argv0);
        _exit(SAFEPOINT_RC_SETUP_FAILED);
    }
    if (wait_for_segv(mode, rank, child) != 0)
        return SAFEPOINT_RC_INCOMPLETE;
    return 0;
}

static int sleep_seconds    = 10;
static int safepoint_cycles = SAFEPOINT_CYCLES_DEFAULT;

int main(int argc, char **argv) {
    const char *mode = NULL;
    int rank = 0, size = 1;
    crash_test_argv0 = argv[0];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--crash-mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (strncmp(argv[i], "--crash-mode=", 13) == 0) {
            mode = argv[i] + 13;
        } else if (strcmp(argv[i], "--sleep") == 0 && i + 1 < argc) {
            int v = atoi(argv[++i]);
            if (v >= 1) sleep_seconds = v;
        } else if (strncmp(argv[i], "--sleep=", 8) == 0) {
            int v = atoi(argv[i] + 8);
            if (v >= 1) sleep_seconds = v;
        } else if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) {
            int v = atoi(argv[++i]);
            if (v > 0) safepoint_cycles = v;
        } else if (strncmp(argv[i], "--cycles=", 9) == 0) {
            int v = atoi(argv[i] + 9);
            if (v > 0) safepoint_cycles = v;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }
    if (!mode) {
        usage(argv[0]);
        return 2;
    }

    if (strcmp(mode, "all-same-no-mpi") == 0) {
        fprintf(stderr, "crash_test mode=%s pid=%d (exec child)\n",
                mode, (int) getpid());
        fflush(stderr);
        crash_function_A(0);
        fprintf(stderr, "all-same-no-mpi: unexpectedly continued\n");
        return SAFEPOINT_RC_NOT_TERMINATED;
    }

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    fprintf(stderr, "crash_test rank=%d size=%d mode=%s pid=%d\n",
            rank, size, mode, (int) getpid());
    fflush(stderr);


    /* create and chdir into per-rank directory so that a given rank's
       coredumps go into that directory and we can count the coredumps
       per rank. */
    {
        char rankdir[64];
        snprintf(rankdir, sizeof rankdir, "rank_%d", rank);
        (void) mkdir(rankdir, 0755);
        if (chdir(rankdir) != 0) {
            fprintf(stderr, "rank=%d chdir(%s) failed\n", rank, rankdir);
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
    /* Finalize MPI before crashing; otherwise, MPICH sometimes hangs
     * waiting for an acknowledgement that never comes from a dead 
     * process. */
    MPI_Finalize();

    if (strcmp(mode, "all-same") == 0) {
        crash_function_A(rank);
    } else if (strcmp(mode, "all-different") == 0) {
        int idx = rank % CRASH_TABLE_SIZE;
        crash_table[idx](rank);
    } else if (strcmp(mode, "partial") == 0) {
        if ((rank % 2) == 0) {
            crash_function_A(rank);
        } else {
            fprintf(stderr, "rank=%d exiting cleanly\n", rank);
            fflush(stderr);
            return 0;
        }
    } else if (strcmp(mode, "late-straggler") == 0) {
        int sleep_s = sleep_seconds;
        if (rank == 0) {
            crash_function_A(rank);
        } else {
            fprintf(stderr, "rank=%d sleeping %ds before crash\n", rank, sleep_s);
            fflush(stderr);
            sleep(sleep_s);
            crash_function_A(rank);
        }
    } else if (strcmp(mode, "two-groups") == 0) {
        if ((rank % 2) == 0)
            crash_function_A(rank);
        else
            crash_function_B(rank);
    } else if (strcmp(mode, "one-crashes") == 0) {
        if (rank == 0) {
            crash_function_A(rank);
        } else {
            fprintf(stderr, "rank=%d exiting cleanly\n", rank);
            fflush(stderr);
            return 0;
        }
    } else if (strcmp(mode, "in-library") == 0) {
        call_crash_in_library_common(rank, 0);
    } else if (strcmp(mode, "in-dlmopen-library") == 0) {
        call_crash_in_library_common(rank, 1);
    } else if (strcmp(mode, "in-fixed-library") == 0) {
        call_crash_in_fixed_library_common(rank, 0);
    } else if (strcmp(mode, "in-fixed-dlmopen-library") == 0) {
        call_crash_in_fixed_library_common(rank, 1);
    } else if (strcmp(mode, "in-library-ctor") == 0) {
        crash_in_library_ctor(rank);
    } else if (strcmp(mode, "sigabrt") == 0) {
        abort();
    } else if (strcmp(mode, "assert") == 0) {
        do_assert(rank);
    } else if (strcmp(mode, "mixed-abort-segv") == 0) {
        do_mixed_abort_segv(rank);
    } else if (strcmp(mode, "kill-segv") == 0) {
        do_kill_segv(rank);
    } else if (strcmp(mode, "span-read") == 0) {
        do_span_read(rank);
    } else if (strcmp(mode, "safepoint") == 0) {
        int rc = do_safepoint(rank, safepoint_cycles);
        return rc;
    } else if (strcmp(mode, "safepoint-then-crash") == 0) {
        return do_safepoint_then_crash(rank);
    } else if (strcmp(mode, "safepoint-bad") == 0) {
        return do_safepoint_bad(rank);
    } else if (strcmp(mode, "safepoint-bad-write") == 0) {
        return do_safepoint_bad_write(rank);
    } else if (strcmp(mode, "safepoint-fix-write") == 0) {
        int rc = do_safepoint_fix_write(rank, safepoint_cycles);
        return rc;
    } else if (strcmp(mode, "safepoint-longjmp") == 0) {
        int rc = do_safepoint_longjmp(rank, safepoint_cycles);
        return rc;
    } else if (strcmp(mode, "safepoint-span-read") == 0) {
        int rc = do_safepoint_span_read(rank, safepoint_cycles);
        return rc;
    } else if (strcmp(mode, "safepoint-span-write") == 0) {
        int rc = do_safepoint_span_write(rank, safepoint_cycles);
        return rc;
    } else if (strcmp(mode, "safepoint-span-bad-read") == 0) {
        return do_safepoint_span_bad_read(rank);
    } else if (strcmp(mode, "safepoint-span-bad-write") == 0) {
        return do_safepoint_span_bad_write(rank);
    } else if (strcmp(mode, "mmap-sigbus-bad") == 0) {
        return do_mmap_sigbus_bad(rank);
    } else if (strcmp(mode, "mmap-sigbus-fixed") == 0) {
        int rc = do_mmap_sigbus_fixed(rank);
        return rc;
    } else if (strcmp(mode, "chained-kill-segv") == 0) {
        int rc = do_chained_kill_segv(rank);
        return rc;
    } else if (strcmp(mode, "ignored-kill-segv") == 0) {
        int rc = do_ignored_kill_segv(rank);
        return rc;
    } else if (strcmp(mode, "ignored-siginfo-kill-segv") == 0) {
        int rc = do_ignored_siginfo_kill_segv(rank);
        return rc;
    } else if (strcmp(mode, "default-siginfo-kill-segv") == 0) {
        do_default_siginfo_kill_segv(rank);
    } else if (strcmp(mode, "fork-child-prereconnect") == 0) {
        do_fork_child_prereconnect(rank);
    } else if (strcmp(mode, "fork-child-reconnect") == 0) {
        int rc = do_fork_child_reconnect(rank, size);
        return rc;
    } else if (strcmp(mode, "fork-child-reconnect-then-parent") == 0) {
        do_fork_child_reconnect_then_parent(rank, size);
    } else if (strcmp(mode, "fork-child-nofollow") == 0) {
        do_fork_child_nofollow(rank);
    } else if (strcmp(mode, "fork-child-inherited-safepoint") == 0) {
        int rc = do_fork_child_inherited_safepoint(rank, safepoint_cycles);
        return rc;
    } else if (strcmp(mode, "fork-exec-child-crash") == 0) {
        int rc = do_fork_exec_child_crash(rank, size);
        return rc;
    } else if (strcmp(mode, "no-crash") == 0) {
        fprintf(stderr, "rank=%d no-crash, exiting cleanly\n", rank);
        fflush(stderr);
        return 0;
    } else {
        if (rank == 0) usage(argv[0]);
        return 2;
    }

    return 0;
}
