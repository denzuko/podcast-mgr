/*
 * sandbox.h — portable privilege-drop for podcast-mgr (BCHS FastCGI worker)
 *
 * Two entry points:
 *
 *   sandbox_lockdown_ro()  — read-only worker: stdio + network I/O only.
 *                            Call this after feeds.xml is loaded, before
 *                            the FastCGI accept loop, when no disk writes
 *                            are needed (e.g. a read-only list/edit worker).
 *
 *   sandbox_lockdown_rw()  — read-write worker: as above but also permits
 *                            open(2) + write(2) for feeds.xml serialisation.
 *                            Call this at process start before the accept
 *                            loop; the write path (PAGE_SAVE / PAGE_DELETE)
 *                            needs it to remain active throughout.
 *
 * Platform coverage:
 *   Linux   — seccomp(2) BPF filter via syscall(2).  Requires kernel ≥ 3.5
 *              and CONFIG_SECCOMP_FILTER=y.  Falls back gracefully if the
 *              kernel rejects the filter (EINVAL / ENOSYS).
 *   FreeBSD — cap_rights_limit(2) on stdin/stdout/stderr + the XML fd, then
 *              cap_enter(2).  Must be called before fopen() in the rw case;
 *              see NOTE below.
 *   OpenBSD — pledge(2).  Clean and sufficient.
 *   NetBSD  — pledge(2) via sys/pledge.h (NetBSD 10+, signature differs from
 *              OpenBSD).
 *   other   — no-op, returns 0.
 *
 * NOTE (FreeBSD / rw):
 *   Capsicum's capability mode forbids open(2) entirely — once you call
 *   cap_enter() you cannot open new paths.  For the write path the correct
 *   approach is to open the xml file descriptor *before* cap_enter(), then
 *   pass it to write_feeds_xml_fd(fd, db).  sandbox_lockdown_rw() therefore
 *   accepts the pre-opened write fd and limits its rights to
 *   CAP_WRITE | CAP_FSTAT | CAP_FTRUNCATE before entering capability mode.
 *   Pass -1 if no write fd is needed (rw without an open fd is silently
 *   treated as ro on FreeBSD).
 */

#ifndef SANDBOX_H
#define SANDBOX_H

#include <unistd.h>
#include <errno.h>

/* -------------------------------------------------------------------------
 * LINUX — seccomp(2) BPF
 * ---------------------------------------------------------------------- */
#if defined(__linux__)

#include <stddef.h>       /* offsetof */
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>

/*
 * Minimal BPF macro set mirroring linux/filter.h style.
 * We avoid pulling in libseccomp to stay dependency-free.
 */
#define _SC_ALLOW(nr) \
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (nr), 0, 1), \
    BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW)

#define _SC_KILL \
    BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS)

/*
 * Validate architecture: reject the filter on any arch we didn't audit.
 * This prevents filter bypass via 32-bit compat on a 64-bit kernel.
 */
#if defined(__x86_64__)
# define AUDIT_ARCH_NATIVE AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
# define AUDIT_ARCH_NATIVE AUDIT_ARCH_AARCH64
#elif defined(__riscv) && __riscv_xlen == 64
# define AUDIT_ARCH_NATIVE AUDIT_ARCH_RISCV64
#else
# warning "sandbox.h: unknown arch; seccomp disabled on this platform"
# define SANDBOX_SECCOMP_DISABLED
#endif

#ifndef SANDBOX_SECCOMP_DISABLED

/*
 * Syscalls needed by the FastCGI worker (read-only profile):
 *
 *   read, write, recv, recvmsg   — socket I/O (FastCGI framing)
 *   send, sendmsg                — socket I/O
 *   close, fstat, lseek          — fd management
 *   mmap, mprotect, munmap       — arena / libc heap
 *   brk                          — libc malloc fallback
 *   rt_sigreturn, rt_sigaction   — signal handling (kcgi internals)
 *   nanosleep                    — sleep(3) used in some kcgi paths
 *   exit, exit_group             — clean shutdown
 *   futex                        — pthreads (kcgi may be threaded)
 *   clock_gettime, gettimeofday  — time(3)
 *   getpid, getuid, geteuid      — logging / sandboxing self-check
 *   recvfrom, sendto             — used by some libc resolver stubs
 *   accept4, accept              — FastCGI accept loop
 *   socket                       — allowed but useless post-cap; harmless
 *   ioctl                        — tty probing in some libc stdio paths
 */
static const struct sock_filter _sc_filter_ro[] = {
    /* 1. Validate architecture */
    BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
             offsetof(struct seccomp_data, arch)),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_NATIVE, 1, 0),
    BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS),

    /* 2. Load syscall number */
    BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
             offsetof(struct seccomp_data, nr)),

    /* 3. Allowlist */
    _SC_ALLOW(__NR_read),
    _SC_ALLOW(__NR_write),
    _SC_ALLOW(__NR_close),
    _SC_ALLOW(__NR_fstat),
#ifdef __NR_fstat64
    _SC_ALLOW(__NR_fstat64),
#endif
    _SC_ALLOW(__NR_lseek),
#ifdef __NR__llseek
    _SC_ALLOW(__NR__llseek),
#endif
    _SC_ALLOW(__NR_mmap),
#ifdef __NR_mmap2
    _SC_ALLOW(__NR_mmap2),
#endif
    _SC_ALLOW(__NR_mprotect),
    _SC_ALLOW(__NR_munmap),
    _SC_ALLOW(__NR_brk),
    _SC_ALLOW(__NR_rt_sigreturn),
    _SC_ALLOW(__NR_rt_sigaction),
    _SC_ALLOW(__NR_rt_sigprocmask),
    _SC_ALLOW(__NR_nanosleep),
    _SC_ALLOW(__NR_exit),
    _SC_ALLOW(__NR_exit_group),
    _SC_ALLOW(__NR_futex),
#ifdef __NR_futex_time64
    _SC_ALLOW(__NR_futex_time64),
#endif
    _SC_ALLOW(__NR_clock_gettime),
#ifdef __NR_clock_gettime64
    _SC_ALLOW(__NR_clock_gettime64),
#endif
    _SC_ALLOW(__NR_gettimeofday),
    _SC_ALLOW(__NR_getpid),
    _SC_ALLOW(__NR_getuid),
    _SC_ALLOW(__NR_geteuid),
    _SC_ALLOW(__NR_recvmsg),
    _SC_ALLOW(__NR_sendmsg),
    _SC_ALLOW(__NR_recvfrom),
    _SC_ALLOW(__NR_sendto),
    _SC_ALLOW(__NR_accept4),
#ifdef __NR_accept
    _SC_ALLOW(__NR_accept),
#endif
    _SC_ALLOW(__NR_socket),
    _SC_ALLOW(__NR_ioctl),
    _SC_ALLOW(__NR_poll),
#ifdef __NR_ppoll
    _SC_ALLOW(__NR_ppoll),
#endif
    _SC_ALLOW(__NR_select),
#ifdef __NR_pselect6
    _SC_ALLOW(__NR_pselect6),
#endif
    _SC_ALLOW(__NR_fcntl),
#ifdef __NR_fcntl64
    _SC_ALLOW(__NR_fcntl64),
#endif
    _SC_ALLOW(__NR_getsockopt),
    _SC_ALLOW(__NR_setsockopt),
    _SC_ALLOW(__NR_getsockname),
    _SC_ALLOW(__NR_getpeername),
    /* kcgi FastCGI spawns worker threads — allow thread-related syscalls */
    _SC_ALLOW(__NR_clone),
#ifdef __NR_clone3
    _SC_ALLOW(__NR_clone3),
#endif
    _SC_ALLOW(__NR_set_robust_list),
    _SC_ALLOW(__NR_sigaltstack),
#ifdef __NR_eventfd2
    _SC_ALLOW(__NR_eventfd2),
#endif
#ifdef __NR_eventfd
    _SC_ALLOW(__NR_eventfd),
#endif
    _SC_ALLOW(__NR_tgkill),
    _SC_ALLOW(__NR_tkill),
    _SC_ALLOW(__NR_waitid),
#ifdef __NR_wait4
    _SC_ALLOW(__NR_wait4),
#endif
    _SC_ALLOW(__NR_sched_yield),
    _SC_ALLOW(__NR_getdents64),
    _SC_ALLOW(__NR_set_tid_address),
    _SC_ALLOW(__NR_rseq),
#ifdef __NR_membarrier
    _SC_ALLOW(__NR_membarrier),
#endif

    /* 4. Default: kill the process on any unlisted syscall */
    _SC_KILL,
};

/*
 * rw profile adds: open/openat, creat, write (already in ro), ftruncate,
 * rename/renameat2 (atomic replace), fsync, unlink.
 * Everything in ro is inherited — we just append the extra allows before
 * the terminal kill rule, which means we rebuild the filter rather than
 * patching, for clarity.
 */
static const struct sock_filter _sc_filter_rw[] = {
    BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
             offsetof(struct seccomp_data, arch)),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_NATIVE, 1, 0),
    BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS),
    BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
             offsetof(struct seccomp_data, nr)),

    /* ro allowlist (duplicated) */
    _SC_ALLOW(__NR_read),
    _SC_ALLOW(__NR_write),
    _SC_ALLOW(__NR_close),
    _SC_ALLOW(__NR_fstat),
#ifdef __NR_fstat64
    _SC_ALLOW(__NR_fstat64),
#endif
    _SC_ALLOW(__NR_lseek),
#ifdef __NR__llseek
    _SC_ALLOW(__NR__llseek),
#endif
    _SC_ALLOW(__NR_mmap),
#ifdef __NR_mmap2
    _SC_ALLOW(__NR_mmap2),
#endif
    _SC_ALLOW(__NR_mprotect),
    _SC_ALLOW(__NR_munmap),
    _SC_ALLOW(__NR_brk),
    _SC_ALLOW(__NR_rt_sigreturn),
    _SC_ALLOW(__NR_rt_sigaction),
    _SC_ALLOW(__NR_rt_sigprocmask),
    _SC_ALLOW(__NR_nanosleep),
    _SC_ALLOW(__NR_exit),
    _SC_ALLOW(__NR_exit_group),
    _SC_ALLOW(__NR_futex),
#ifdef __NR_futex_time64
    _SC_ALLOW(__NR_futex_time64),
#endif
    _SC_ALLOW(__NR_clock_gettime),
#ifdef __NR_clock_gettime64
    _SC_ALLOW(__NR_clock_gettime64),
#endif
    _SC_ALLOW(__NR_gettimeofday),
    _SC_ALLOW(__NR_getpid),
    _SC_ALLOW(__NR_getuid),
    _SC_ALLOW(__NR_geteuid),
    _SC_ALLOW(__NR_recvmsg),
    _SC_ALLOW(__NR_sendmsg),
    _SC_ALLOW(__NR_recvfrom),
    _SC_ALLOW(__NR_sendto),
    _SC_ALLOW(__NR_accept4),
#ifdef __NR_accept
    _SC_ALLOW(__NR_accept),
#endif
    _SC_ALLOW(__NR_socket),
    _SC_ALLOW(__NR_ioctl),
    _SC_ALLOW(__NR_poll),
#ifdef __NR_ppoll
    _SC_ALLOW(__NR_ppoll),
#endif
    _SC_ALLOW(__NR_select),
#ifdef __NR_pselect6
    _SC_ALLOW(__NR_pselect6),
#endif
    _SC_ALLOW(__NR_fcntl),
#ifdef __NR_fcntl64
    _SC_ALLOW(__NR_fcntl64),
#endif
    _SC_ALLOW(__NR_getsockopt),
    _SC_ALLOW(__NR_setsockopt),
    _SC_ALLOW(__NR_getsockname),
    _SC_ALLOW(__NR_getpeername),

    /* kcgi FastCGI spawns worker threads — allow thread-related syscalls */
    _SC_ALLOW(__NR_clone),
#ifdef __NR_clone3
    _SC_ALLOW(__NR_clone3),
#endif
    _SC_ALLOW(__NR_set_robust_list),
    _SC_ALLOW(__NR_sigaltstack),
#ifdef __NR_eventfd2
    _SC_ALLOW(__NR_eventfd2),
#endif
#ifdef __NR_eventfd
    _SC_ALLOW(__NR_eventfd),
#endif
    _SC_ALLOW(__NR_tgkill),
    _SC_ALLOW(__NR_tkill),
    _SC_ALLOW(__NR_waitid),
#ifdef __NR_wait4
    _SC_ALLOW(__NR_wait4),
#endif
    _SC_ALLOW(__NR_sched_yield),
    _SC_ALLOW(__NR_getdents64),
    _SC_ALLOW(__NR_set_tid_address),
    _SC_ALLOW(__NR_rseq),
#ifdef __NR_membarrier
    _SC_ALLOW(__NR_membarrier),
#endif

    /* rw extras */
    _SC_ALLOW(__NR_openat),
#ifdef __NR_open
    _SC_ALLOW(__NR_open),
#endif
    _SC_ALLOW(__NR_ftruncate),
#ifdef __NR_ftruncate64
    _SC_ALLOW(__NR_ftruncate64),
#endif
#ifdef __NR_renameat2
    _SC_ALLOW(__NR_renameat2),
#endif
    _SC_ALLOW(__NR_renameat),
    _SC_ALLOW(__NR_rename),
    _SC_ALLOW(__NR_fsync),
    _SC_ALLOW(__NR_fdatasync),
    _SC_ALLOW(__NR_unlinkat),
#ifdef __NR_unlink
    _SC_ALLOW(__NR_unlink),
#endif
    _SC_ALLOW(__NR_stat),
#ifdef __NR_stat64
    _SC_ALLOW(__NR_stat64),
#endif
    _SC_ALLOW(__NR_newfstatat),

    _SC_KILL,
};

static inline int _seccomp_install(const struct sock_filter *f, unsigned short n) {
    struct sock_fprog prog = { .len = n, .filter = (struct sock_filter *)f };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
    /*
     * Use syscall() directly — glibc's seccomp(2) wrapper may not exist on
     * older distributions; the raw syscall is always available if the kernel
     * supports it.
     */
    long rc = syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog);
    if (rc != 0) {
        /*
         * Graceful degradation: if the kernel doesn't support seccomp filters
         * (ENOSYS, EINVAL) we continue without sandboxing rather than killing
         * the worker.  If you want hard failure, return -1 unconditionally.
         */
        if (errno == ENOSYS || errno == EINVAL) return 0;
        return -1;
    }
    return 0;
}

static inline int sandbox_lockdown_ro(void) {
    return _seccomp_install(_sc_filter_ro,
               (unsigned short)(sizeof(_sc_filter_ro) / sizeof(_sc_filter_ro[0])));
}

static inline int sandbox_lockdown_rw(int xml_write_fd) {
    (void)xml_write_fd;  /* unused on Linux — open() is in the rw allowlist */
    return _seccomp_install(_sc_filter_rw,
               (unsigned short)(sizeof(_sc_filter_rw) / sizeof(_sc_filter_rw[0])));
}

#else  /* SANDBOX_SECCOMP_DISABLED (unknown arch) */
static inline int sandbox_lockdown_ro(void)        { return 0; }
static inline int sandbox_lockdown_rw(int fd)      { (void)fd; return 0; }
#endif /* SANDBOX_SECCOMP_DISABLED */

/* -------------------------------------------------------------------------
 * FREEBSD — Capsicum
 *
 * cap_enter() forbids open(2) of new paths.  For the rw case the caller
 * must open the xml file for writing BEFORE calling sandbox_lockdown_rw(),
 * then pass the fd here so we can constrain its rights before entering
 * capability mode.  write_feeds_xml() in main.c must accept an fd, not a
 * path, when running under Capsicum.
 * ---------------------------------------------------------------------- */
#elif defined(__FreeBSD__)

#include <sys/capsicum.h>
#include <sys/types.h>

static inline int sandbox_lockdown_ro(void) {
    /*
     * Restrict stdin/stdout/stderr to just what kcgi's FastCGI layer needs.
     * kcgi opens its own socket; if it's inherited as fd 0 that's fine.
     */
    cap_rights_t rights;
    cap_rights_init(&rights, CAP_READ, CAP_WRITE, CAP_EVENT,
                    CAP_RECV, CAP_SEND, CAP_GETSOCKOPT, CAP_SETSOCKOPT,
                    CAP_GETPEERNAME, CAP_GETSOCKNAME, CAP_ACCEPT);
    for (int fd = 0; fd <= 2; ++fd) {
        /* Ignore errors on fds that may not be sockets */
        (void)cap_rights_limit(fd, &rights);
    }
    return cap_enter();
}

static inline int sandbox_lockdown_rw(int xml_write_fd) {
    cap_rights_t sock_rights, file_rights;

    cap_rights_init(&sock_rights,
                    CAP_READ, CAP_WRITE, CAP_EVENT,
                    CAP_RECV, CAP_SEND, CAP_GETSOCKOPT, CAP_SETSOCKOPT,
                    CAP_GETPEERNAME, CAP_GETSOCKNAME, CAP_ACCEPT);
    for (int fd = 0; fd <= 2; ++fd)
        (void)cap_rights_limit(fd, &sock_rights);

    if (xml_write_fd >= 0) {
        cap_rights_init(&file_rights,
                        CAP_READ, CAP_WRITE, CAP_SEEK,
                        CAP_FSTAT, CAP_FTRUNCATE, CAP_FSYNC);
        if (cap_rights_limit(xml_write_fd, &file_rights) != 0)
            return -1;
    }

    return cap_enter();
}

/* -------------------------------------------------------------------------
 * OPENBSD — pledge(2)
 * ---------------------------------------------------------------------- */
#elif defined(__OpenBSD__)

static inline int sandbox_lockdown_ro(void) {
    /*
     * "stdio"  — read, write, close, fstat, and similar basic I/O.
     * "inet"   — socket I/O for the FastCGI Unix-domain or TCP socket.
     *            If kcgi uses only Unix sockets, "unix" suffices instead.
     * "unix"   — Unix-domain sockets (FastCGI typically uses these).
     */
    return pledge("stdio unix inet", NULL);
}

static inline int sandbox_lockdown_rw(int xml_write_fd) {
    (void)xml_write_fd;
    /*
     * "wpath"  — write to existing paths (feeds.xml already exists).
     * "cpath"  — needed only if write_feeds_xml uses a temp file + rename;
     *            remove if you write in-place.
     */
    return pledge("stdio unix inet wpath cpath", NULL);
}

/* -------------------------------------------------------------------------
 * NETBSD — pledge(2) (NetBSD 10+)
 *
 * NetBSD's pledge signature:
 *   int pledge(const char *promises, int flags);
 * flags: 0 (no exec), PLEDGE_EXECPROMISES (not needed here).
 * Promise strings are the same as OpenBSD.
 * ---------------------------------------------------------------------- */
#elif defined(__NetBSD__)

#include <sys/pledge.h>

static inline int sandbox_lockdown_ro(void) {
    return pledge("stdio unix inet", 0);
}

static inline int sandbox_lockdown_rw(int xml_write_fd) {
    (void)xml_write_fd;
    return pledge("stdio unix inet wpath cpath", 0);
}

/* -------------------------------------------------------------------------
 * FALLBACK — unsupported platform, no-op
 * ---------------------------------------------------------------------- */
#else

static inline int sandbox_lockdown_ro(void)        { return 0; }
static inline int sandbox_lockdown_rw(int fd)      { (void)fd; return 0; }

#endif /* platform dispatch */

/*
 * Backwards-compat shim: original main.c called sandbox_lockdown() with no
 * args and no write-fd concept.  Map it to the rw profile (permissive) so
 * existing call sites compile without change.  Callers should migrate to
 * sandbox_lockdown_ro() or sandbox_lockdown_rw(fd) explicitly.
 */
static inline int sandbox_lockdown(void) {
    return sandbox_lockdown_rw(-1);
}

#endif /* SANDBOX_H */
