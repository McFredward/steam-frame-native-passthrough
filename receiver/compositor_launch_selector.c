/* Public launch shims inject diagnostics into the exact reviewed target only.
 * The explicit cold handshake then cleans real environ so libc-internal launch
 * paths inherit no full diagnostics. This is not a subprocess sandbox: callers
 * supplying their own saved dirty envp to unwrapped raw syscalls are outside its
 * contract. Startup health must verify actual helper environment and mappings.
 * No freeze/cleanup API is called implicitly. */
#define _GNU_SOURCE 1
#include "compositor_launch_selector.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#ifndef FRAME_COMPOSITOR_LAUNCH_CORE_ONLY
#include <dlfcn.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <spawn.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#if defined(__GNUC__)
#define HIDDEN __attribute__((visibility("hidden")))
#define PUBLIC __attribute__((visibility("default")))
#else
#define HIDDEN
#define PUBLIC
#endif

static size_t bounded_length(const char *s, size_t limit) {
    size_t n = 0;
    if (!s) return limit;
    while (n < limit && s[n]) ++n;
    return n;
}
static int safe_path(const char *p) {
    size_t i, segment = 1u, n = bounded_length(p, FRAME_LAUNCH_MAX_PATH);
    if (!n || n == FRAME_LAUNCH_MAX_PATH || p[0] != '/') return 0;
    for (i = 0; i < n; ++i)
        if ((unsigned char)p[i] < 33 || (unsigned char)p[i] > 126 || p[i] == ':' || p[i] == '=') return 0;
    for (i = 1u; i <= n; ++i) if (p[i] == '/' || !p[i]) {
        size_t count = i - segment;
        if (!count || (count == 1u && p[segment] == '.') ||
            (count == 2u && p[segment] == '.' && p[segment + 1u] == '.')) return 0;
        segment = i + 1u;
    }
    return 1;
}
HIDDEN int frame_launch_config_valid(const struct frame_launch_config *c) {
    return c && c->abi == FRAME_LAUNCH_CONFIG_ABI && safe_path(c->target) &&
        safe_path(c->selector) && safe_path(c->payload) && safe_path(c->audit) &&
        safe_path(c->envelope) && safe_path(c->output) && safe_path(c->journal) &&
        strcmp(c->selector, c->payload) && strcmp(c->selector, c->audit) && strcmp(c->payload, c->audit);
}
static int starts(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    return !strncmp(s, prefix, n);
}
static int diagnostic_key(const char *s) {
#ifdef FRAME_LAUNCH_RELEASE_BRIDGE
    (void)s; return 0;
#else
    return starts(s, "GHVR_FRAME_COMPOSITOR_RAW_") || starts(s, "FRAME_MASK_TRACE");
#endif
}
static int separator(char ch) { return ch == ':' || ch == ' ' || ch == '\t'; }
static int token_equals(const char *p, size_t n, const char *value) {
    return strlen(value) == n && !memcmp(p, value, n);
}
static int loader_owned(const char *p, size_t n, const struct frame_launch_config *c, int audit, int include_selector) {
#ifdef FRAME_LAUNCH_RELEASE_BRIDGE
    return audit ? 0 : token_equals(p, n, c->payload) || (include_selector && token_equals(p, n, c->selector));
#else
    return audit ? token_equals(p, n, c->audit) :
        token_equals(p, n, c->payload) || (include_selector && token_equals(p, n, c->selector));
#endif
}
/* Foreign tokens and their original separators remain byte-for-byte. Owned token
 * bytes alone are removed; repeated separators are legal loader delimiters. */
static int filter_loader(const char *value, char *out, size_t cap,
    const struct frame_launch_config *c, int audit, int *has_foreign) {
    size_t i = 0, used = 0;
    *has_foreign = 0;
    while (value[i]) {
        size_t begin = i, n;
        if (separator(value[i])) {
            if (used + 1 >= cap) return 0;
            out[used++] = value[i++];
            continue;
        }
        while (value[i] && !separator(value[i])) ++i;
        n = i - begin;
        if (loader_owned(value + begin, n, c, audit, 1)) continue;
        if (used + n >= cap) return 0;
        memcpy(out + used, value + begin, n); used += n; *has_foreign = 1;
    }
    out[used] = 0;
    if (!*has_foreign) out[0] = 0;
    return 1;
}
/* The secondary inspection reads at most 4,096 keys and 4 KiB per loader value.
 * Other oversized values are not traversed. Uninspectable input is unsafe, so
 * refusal replaces an unbounded search or accidental diagnostic propagation. */
static int environment_dirty(char *const envp[], const struct frame_launch_config *c) {
    size_t j;
    if (!envp) return 0;
    for (j = 0; j < FRAME_LAUNCH_MAX_INSPECT_ENTRIES && envp[j]; ++j) {
        const char *s = envp[j], *value;
        int audit;
        size_t i;
        if (diagnostic_key(s)) return 1;
        if (starts(s, "LD_PRELOAD=")) { value = s + 11; audit = 0; }
        else if (starts(s, "LD_AUDIT=")) { value = s + 9; audit = 1; }
        else continue;
        if (bounded_length(value, FRAME_LAUNCH_MAX_LOADER_VALUE + 1u) > FRAME_LAUNCH_MAX_LOADER_VALUE) return 1;
        for (i = 0; value[i];) {
            size_t begin, n;
            if (separator(value[i])) { ++i; continue; }
            begin = i;
            while (value[i] && !separator(value[i])) ++i;
            n = i - begin;
            if (loader_owned(value + begin, n, c, audit, 0)) return 1;
        }
    }
    return envp[j] != NULL;
}
static enum frame_launch_result oversized(char *const envp[], const struct frame_launch_config *c,
    struct frame_launch_environment *out) {
    out->forward = (char **)envp;
    return environment_dirty(envp, c) ? FRAME_LAUNCH_DIRTY_OVERFLOW : FRAME_LAUNCH_CLEAN_UNCHANGED;
}
static int append_path(char *out, size_t cap, const char *path) {
    size_t n = strlen(out), p = strlen(path);
    if (n + (n ? 1u : 0u) + p + 1 > cap) return 0;
    if (n) out[n++] = ':';
    memcpy(out + n, path, p + 1); return 1;
}
HIDDEN enum frame_launch_result frame_launch_build_environment(const struct frame_launch_config *c,
    char *const envp[], int target, struct frame_launch_environment *out) {
    size_t i, count = 0, bytes = 0, nout = 0;
    int preload_seen = 0, audit_seen = 0;
    if (!out) return FRAME_LAUNCH_INVALID_CONFIG;
    out->forward = (char **)envp;
    if (!frame_launch_config_valid(c)) return FRAME_LAUNCH_INVALID_CONFIG;
    if (envp) for (i = 0; envp[i]; ++i) {
        size_t n;
        if (i == FRAME_LAUNCH_MAX_ENTRIES) return oversized(envp, c, out);
        n = bounded_length(envp[i], FRAME_LAUNCH_MAX_ENV_BYTES + 1u);
        if (n >= FRAME_LAUNCH_MAX_ENV_BYTES || bytes > FRAME_LAUNCH_MAX_ENV_BYTES - n - 1u)
            return oversized(envp, c, out);
        bytes += n + 1; ++count;
        /* Duplicate loader keys need independent buffers; decline a malformed
         * duplicated loader input rather than silently changing its semantics. */
        if (starts(envp[i], "LD_PRELOAD=") && preload_seen++) return oversized(envp, c, out);
        if (starts(envp[i], "LD_AUDIT=") && audit_seen++) return oversized(envp, c, out);
    }
    for (i = 0; i < count; ++i) {
        char *s = envp[i];
        if (diagnostic_key(s)) continue;
        if (starts(s, "LD_PRELOAD=") || starts(s, "LD_AUDIT=")) {
            int is_audit = starts(s, "LD_AUDIT="), foreign;
            const char *value = s + (is_audit ? 9u : 11u);
            char *buffer = is_audit ? out->audit : out->preload;
            const char *key = is_audit ? "LD_AUDIT=" : "LD_PRELOAD=";
            size_t keylen = strlen(key), cap = is_audit ? sizeof out->audit : sizeof out->preload;
            if (bounded_length(value, FRAME_LAUNCH_MAX_LOADER_VALUE + 1u) > FRAME_LAUNCH_MAX_LOADER_VALUE)
                return oversized(envp, c, out);
            memcpy(buffer, key, keylen);
            if (!filter_loader(value, buffer + keylen, cap - keylen, c, is_audit, &foreign))
                return oversized(envp, c, out);
            if (is_audit) {
#ifdef FRAME_LAUNCH_RELEASE_BRIDGE
                out->entries[nout++] = s;
#else
                if (target && !append_path(buffer + keylen, cap - keylen, c->audit)) return oversized(envp, c, out);
                if (foreign || target) out->entries[nout++] = buffer;
#endif
            } else {
                if (!append_path(buffer + keylen, cap - keylen, c->selector) ||
                    (target && !append_path(buffer + keylen, cap - keylen, c->payload))) return oversized(envp, c, out);
                out->entries[nout++] = buffer;
            }
        } else out->entries[nout++] = s;
    }
    if (!preload_seen) {
        strcpy(out->preload, "LD_PRELOAD=");
        if (!append_path(out->preload + 11, sizeof out->preload - 11, c->selector) ||
            (target && !append_path(out->preload + 11, sizeof out->preload - 11, c->payload))) return oversized(envp, c, out);
        out->entries[nout++] = out->preload;
    }
#ifndef FRAME_LAUNCH_RELEASE_BRIDGE
    if (target) {
        if (!audit_seen) {
            strcpy(out->audit, "LD_AUDIT=");
            if (!append_path(out->audit + 9, sizeof out->audit - 9, c->audit)) return oversized(envp, c, out);
            out->entries[nout++] = out->audit;
        }
        strcpy(out->values[0], "GHVR_FRAME_COMPOSITOR_RAW_ENVELOPE="); strcat(out->values[0], c->envelope);
        strcpy(out->values[1], "GHVR_FRAME_COMPOSITOR_RAW_OUTPUT="); strcat(out->values[1], c->output);
        strcpy(out->values[2], "FRAME_MASK_TRACE_OUTPUT="); strcat(out->values[2], c->journal);
        out->entries[nout++] = out->values[0]; out->entries[nout++] = out->values[1]; out->entries[nout++] = out->values[2];
        out->entries[nout++] = "FRAME_MASK_TRACE=1";
        out->entries[nout++] = "FRAME_MASK_TRACE_BINDINGS=1";
    }
#endif
    out->entries[nout] = NULL; out->forward = out->entries;
    return FRAME_LAUNCH_REWRITTEN;
}
HIDDEN int frame_launch_is_target(const struct frame_launch_config *c, const char *file,
    const char *cwd, const char *search_path, int path_search, int has_actions) {
    const char *base, *rel = file;
    size_t dirlen, n;
    if (!frame_launch_config_valid(c) || !file || !*file) return 0;
    n = bounded_length(file, FRAME_LAUNCH_MAX_PATH);
    if (n == FRAME_LAUNCH_MAX_PATH) return 0;
    if (file[0] == '/') return !strcmp(file, c->target);
    if (has_actions) return 0;
    base = strrchr(c->target, '/'); dirlen = (size_t)(base - c->target);
    if (!strncmp(rel, "./", 2)) rel += 2;
    if (strchr(rel, '/') || strcmp(rel, base + 1)) return 0;
    if (path_search && !strchr(file, '/')) {
        if (!search_path || bounded_length(search_path, FRAME_LAUNCH_MAX_PATH) == FRAME_LAUNCH_MAX_PATH || strchr(search_path, ':')) return 0;
        if (*search_path && strcmp(search_path, "."))
            return strlen(search_path) == dirlen && !memcmp(search_path, c->target, dirlen);
    }
    return cwd && bounded_length(cwd, FRAME_LAUNCH_MAX_PATH) == dirlen && !memcmp(cwd, c->target, dirlen);
}

#ifndef FRAME_COMPOSITOR_LAUNCH_CORE_ONLY
extern char **environ;
__attribute__((weak, visibility("hidden"))) const struct frame_launch_config *frame_compositor_launch_config_v1(void) { return NULL; }
typedef int (*execve_fn)(const char *, char *const[], char *const[]);
typedef int (*spawn_fn)(pid_t *, const char *, const posix_spawn_file_actions_t *,
    const posix_spawnattr_t *, char *const[], char *const[]);
typedef char *(*getenv_fn)(const char *);
static execve_fn native_execve, native_execvpe;
static spawn_fn native_spawn, native_spawnp;
static getenv_fn native_getenv, native_secure_getenv;
static struct frame_launch_environment clean_baseline;
static _Atomic unsigned freeze_attempted;
static _Atomic pid_t frozen_pid;
static char frozen_values[3u][FRAME_LAUNCH_MAX_PATH];
static const char *const frozen_names[5u] = {
    "GHVR_FRAME_COMPOSITOR_RAW_ENVELOPE", "GHVR_FRAME_COMPOSITOR_RAW_OUTPUT",
    "FRAME_MASK_TRACE_OUTPUT", "FRAME_MASK_TRACE", "FRAME_MASK_TRACE_BINDINGS"
};
#if defined(__aarch64__)
#define EXECVE_VERSION "GLIBC_2.17"
#define EXECVPE_VERSION "GLIBC_2.17"
#define SPAWN_VERSION "GLIBC_2.17"
#define GETENV_VERSION "GLIBC_2.17"
#elif defined(__x86_64__)
#define EXECVE_VERSION "GLIBC_2.2.5"
#define EXECVPE_VERSION "GLIBC_2.11"
#define SPAWN_VERSION "GLIBC_2.15"
#define GETENV_VERSION "GLIBC_2.2.5"
#else
#error "Launch selector provider versions must be reviewed for this architecture"
#endif
__attribute__((constructor)) static void resolve_providers(void) {
    int saved = errno;
    /* An earlier payload constructor may already have completed the cold
     * handshake. Do not rewrite provider pointers after it starts its writer. */
    if (native_execve && native_execvpe && native_spawn && native_spawnp && native_getenv && native_secure_getenv) return;
    void *p = dlvsym(RTLD_NEXT, "execve", EXECVE_VERSION); memcpy(&native_execve, &p, sizeof p);
    p = dlvsym(RTLD_NEXT, "execvpe", EXECVPE_VERSION); memcpy(&native_execvpe, &p, sizeof p);
    p = dlvsym(RTLD_NEXT, "posix_spawn", SPAWN_VERSION); memcpy(&native_spawn, &p, sizeof p);
    p = dlvsym(RTLD_NEXT, "posix_spawnp", SPAWN_VERSION); memcpy(&native_spawnp, &p, sizeof p);
    p = dlvsym(RTLD_NEXT, "getenv", GETENV_VERSION); memcpy(&native_getenv, &p, sizeof p);
    p = dlvsym(RTLD_NEXT, "secure_getenv", "GLIBC_2.17"); memcpy(&native_secure_getenv, &p, sizeof p);
    errno = saved;
}
PUBLIC unsigned frame_compositor_launch_provider_mask(void) {
    return (native_execve ? 1u : 0u) | (native_execvpe ? 2u : 0u) |
        (native_spawn ? 4u : 0u) | (native_spawnp ? 8u : 0u) | (native_getenv ? 16u : 0u) |
        (native_secure_getenv ? 32u : 0u);
}
/* ELF constructors may call getenv before this preload's own constructor.
 * This bootstrap path performs libc's ordinary exact-key scan without changing
 * environ or admitting virtual diagnostics. Genuine forwarding is established
 * in the constructor/cold handshake; no dlsym or I/O occurs in this wrapper. */
static char *bootstrap_getenv(const char *name) {
    size_t n;
    char **entry;
    if (!name || !*name) return NULL;
    n = strlen(name);
    for (entry = environ; entry && *entry; ++entry)
        if (!strncmp(*entry, name, n) && (*entry)[n] == '=') return *entry + n + 1u;
    return NULL;
}
static char *frozen_value(const char *name) {
    size_t i;
    if (!name) return NULL;
    for (i = 0; i < 5u; ++i) if (!strcmp(name, frozen_names[i]))
        return i < 3u ? frozen_values[i] : (char *)"1";
    return NULL;
}
PUBLIC char *getenv(const char *name) {
    int saved = errno;
    char *result = NULL;
    pid_t owner = atomic_load_explicit(&frozen_pid, memory_order_acquire);
    if (owner && owner == (pid_t)syscall(SYS_getpid) && (result = frozen_value(name))) {
        errno = saved; return result;
    }
    result = native_getenv ? native_getenv(name) : bootstrap_getenv(name);
    errno = saved; return result;
}
static int nonsecure_aux(void) {
    unsigned long secure;
    errno = 0; secure = getauxval(AT_SECURE);
    return !errno && !secure;
}
static int nonsecure_process(void) {
    return nonsecure_aux() && syscall(SYS_getuid) == syscall(SYS_geteuid) &&
        syscall(SYS_getgid) == syscall(SYS_getegid);
}
PUBLIC char *secure_getenv(const char *name) {
    int saved = errno;
    char *result = NULL;
    pid_t owner = atomic_load_explicit(&frozen_pid, memory_order_acquire);
    /* Freeze already proved nonsecure admission; check live credentials too.
     * Later credential changes must never resurrect a virtual secure key. */
    if (owner && owner == (pid_t)syscall(SYS_getpid) && nonsecure_process() && (result = frozen_value(name))) {
        errno = saved; return result;
    }
    if (native_secure_getenv) result = native_secure_getenv(name);
    else result = nonsecure_aux() ? bootstrap_getenv(name) : NULL;
    errno = saved; return result;
}
static int target_path(const struct frame_launch_config *c, const char *path, int search, int actions) {
    char cwd[FRAME_LAUNCH_MAX_PATH];
    const char *current = NULL, *path_value = NULL;
    size_t i;
    if (!path || path[0] == '/') return frame_launch_is_target(c, path, NULL, NULL, search, actions);
    /* Linux public cwd metadata only. Avoid libc getcwd fallback directory
     * traversal/allocation after fork; failure declines relative admission. */
    if (!actions && syscall(SYS_getcwd, cwd, sizeof cwd) > 0) current = cwd;
    /* GNU execvpe's search PATH is the process environ, not its envp argument. */
    if (search && environ) for (i = 0; i < FRAME_LAUNCH_MAX_ENTRIES && environ[i]; ++i)
        if (starts(environ[i], "PATH=")) { path_value = environ[i] + 5; break; }
    return frame_launch_is_target(c, path, current, path_value, search, actions);
}
static enum frame_launch_result prepare(const char *path, int search, int actions,
    char *const envp[], struct frame_launch_environment *buffer) {
    const struct frame_launch_config *c = frame_compositor_launch_config_v1();
    return frame_launch_build_environment(c, envp, target_path(c, path, search, actions), buffer);
}
static int forward_exec(execve_fn provider, const char *path, char *const argv[], char *const envp[], int search) {
    struct frame_launch_environment buffer;
    int saved = errno;
    enum frame_launch_result r = prepare(path, search, 0, envp, &buffer);
    if (!provider) { errno = ENOSYS; return -1; }
    if (r == FRAME_LAUNCH_DIRTY_OVERFLOW) { errno = E2BIG; return -1; }
    errno = saved; return provider(path, argv, buffer.forward);
}
PUBLIC int execve(const char *path, char *const argv[], char *const envp[]) { return forward_exec(native_execve, path, argv, envp, 0); }
PUBLIC int execvpe(const char *path, char *const argv[], char *const envp[]) { return forward_exec(native_execvpe, path, argv, envp, 1); }
/* GLIBC execv/execvp are exact shims to execve/execvpe with process environ. */
PUBLIC int execv(const char *path, char *const argv[]) { return forward_exec(native_execve, path, argv, environ, 0); }
PUBLIC int execvp(const char *path, char *const argv[]) { return forward_exec(native_execvpe, path, argv, environ, 1); }
static int forward_spawn(spawn_fn provider, pid_t *pid, const char *path,
    const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr,
    char *const argv[], char *const envp[], int search) {
    struct frame_launch_environment buffer;
    int saved = errno;
    enum frame_launch_result r = prepare(path, search, actions != NULL, envp, &buffer);
    if (!provider) { errno = saved; return ENOSYS; }
    if (r == FRAME_LAUNCH_DIRTY_OVERFLOW) { errno = saved; return E2BIG; }
    errno = saved; return provider(pid, path, actions, attr, argv, buffer.forward);
}
PUBLIC int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
    const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
    return forward_spawn(native_spawn, pid, path, actions, attr, argv, envp, 0);
}
PUBLIC int posix_spawnp(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
    const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
    return forward_spawn(native_spawnp, pid, path, actions, attr, argv, envp, 1);
}
struct task_entry { uint64_t ino; int64_t offset; unsigned short size; unsigned char type; char name[]; };
static int cold_single_thread(void) {
    union { uint64_t alignment; char bytes[4096]; } storage;
    char *bytes = storage.bytes;
    long fd = syscall(SYS_openat, AT_FDCWD, "/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    long got, second;
    size_t offset = 0, count = 0;
    int valid = 1;
    if (fd < 0) return 0;
    got = syscall(SYS_getdents64, fd, bytes, sizeof storage.bytes);
    if (got <= 0) valid = 0;
    while (valid && offset < (size_t)got) {
        struct task_entry *entry = (struct task_entry *)(void *)(bytes + offset);
        size_t available = (size_t)got - offset, n, i;
        if (available < offsetof(struct task_entry, name) + 1u || entry->size > available ||
            entry->size < offsetof(struct task_entry, name) + 1u) { valid = 0; break; }
        n = bounded_length(entry->name, entry->size - offsetof(struct task_entry, name));
        if (n == entry->size - offsetof(struct task_entry, name)) { valid = 0; break; }
        if (strcmp(entry->name, ".") && strcmp(entry->name, "..")) {
            for (i = 0; i < n; ++i) if (entry->name[i] < '0' || entry->name[i] > '9') valid = 0;
            if (!n || ++count != 1u) valid = 0;
        }
        offset += entry->size;
    }
    second = syscall(SYS_getdents64, fd, bytes, sizeof storage.bytes);
    syscall(SYS_close, fd);
    return valid && count == 1u && !second;
}
static int known_environment(const struct frame_launch_config *c) {
    const char *expected[5u] = {c->envelope, c->output, c->journal, "1", "1"};
    unsigned seen = 0;
    size_t entry, i;
    int preload = 0, audit = 0;
    for (entry = 0; environ && environ[entry] && entry < FRAME_LAUNCH_MAX_ENTRIES; ++entry) {
        const char *p = environ[entry];
        if (diagnostic_key(p)) {
            for (i = 0; i < 5u; ++i) {
                size_t n = strlen(frozen_names[i]);
                if (!strncmp(p, frozen_names[i], n) && p[n] == '=') {
                    if ((seen & (1u << i)) || strcmp(p + n + 1u, expected[i])) return 0;
                    seen |= 1u << i; break;
                }
            }
            if (i == 5u) return 0;
        }
        if (starts(p, "LD_PRELOAD=")) {
            size_t j = 11u;
            while (p[j]) {
                size_t begin;
                if (separator(p[j])) { ++j; continue; }
                begin = j; while (p[j] && !separator(p[j])) ++j;
                if (token_equals(p + begin, j - begin, c->payload)) preload = 1;
            }
        }
        if (starts(p, "LD_AUDIT=")) {
            size_t j = 9u;
            while (p[j]) {
                size_t begin;
                if (separator(p[j])) { ++j; continue; }
                begin = j; while (p[j] && !separator(p[j])) ++j;
                if (token_equals(p + begin, j - begin, c->audit)) audit = 1;
            }
        }
    }
    return (!environ || !environ[entry]) && seen == 31u && preload && audit;
}
PUBLIC int frame_compositor_launch_freeze_environment_v1(unsigned abi) {
    int saved = errno, result = FRAME_LAUNCH_FREEZE_NOT_COLD;
    const struct frame_launch_config *c = frame_compositor_launch_config_v1();
    unsigned expected = 0;
    uint64_t signals = UINT64_MAX, old_signals;
    char executable[FRAME_LAUNCH_MAX_PATH];
    long n;
    enum frame_launch_result rewrite;
    size_t i;
    if (abi != FRAME_LAUNCH_FREEZE_ABI) { result = FRAME_LAUNCH_FREEZE_INVALID_ABI; goto done; }
    if (!frame_launch_config_valid(c)) { result = FRAME_LAUNCH_FREEZE_INVALID_CONFIG; goto done; }
    n = syscall(SYS_readlinkat, AT_FDCWD, "/proc/self/exe", executable, sizeof executable);
    if (n <= 0 || n >= (long)sizeof executable) { result = FRAME_LAUNCH_FREEZE_WRONG_PROCESS; goto done; }
    executable[n] = 0;
    if (strcmp(executable, c->target)) { result = FRAME_LAUNCH_FREEZE_WRONG_PROCESS; goto done; }
    if (syscall(SYS_gettid) != syscall(SYS_getpid)) goto done;
    if (!atomic_compare_exchange_strong_explicit(&freeze_attempted, &expected, 1u,
            memory_order_acq_rel, memory_order_acquire)) { result = FRAME_LAUNCH_FREEZE_ALREADY_ATTEMPTED; goto done; }
    /* Linux uses one 64-bit kernel sigset on both reviewed architectures. No
     * handler may race environment publication after the one-task proof. */
    if (syscall(SYS_rt_sigprocmask, 0 /* SIG_BLOCK */, &signals, &old_signals, sizeof signals)) goto done;
    if (!cold_single_thread()) goto restore_signals;
    rewrite = frame_launch_build_environment(c, environ, 0, &clean_baseline);
    if (rewrite != FRAME_LAUNCH_REWRITTEN) { result = FRAME_LAUNCH_FREEZE_ENVIRONMENT; goto restore_signals; }
    /* Resolve before publication, including reversed preload constructor order.
     * No allocation or loader resolution is ever performed by getters. */
    resolve_providers();
    if (!cold_single_thread()) goto restore_signals;
    if (!nonsecure_process() || frame_compositor_launch_provider_mask() != 63u)
        result = FRAME_LAUNCH_FREEZE_PROVIDER;
    else if (!known_environment(c)) result = FRAME_LAUNCH_FREEZE_ENVIRONMENT;
    else {
        for (i = 0; i < 3u; ++i) {
            const char *value = i == 0u ? c->envelope : i == 1u ? c->output : c->journal;
            memcpy(frozen_values[i], value, strlen(value) + 1u);
        }
        result = FRAME_LAUNCH_FROZEN;
    }
    /* Denied admitted attempts are scrubbed too; only success publishes the
     * virtual view. No thread existed and all handlers remain blocked here. */
    environ = clean_baseline.forward;
    if (result == FRAME_LAUNCH_FROZEN)
        atomic_store_explicit(&frozen_pid, (pid_t)syscall(SYS_getpid), memory_order_release);
restore_signals:
    if (syscall(SYS_rt_sigprocmask, 2 /* SIG_SETMASK */, &old_signals, NULL, sizeof signals)) {
        atomic_store_explicit(&frozen_pid, 0, memory_order_release);
        result = FRAME_LAUNCH_FREEZE_NOT_COLD;
    }
done:
    errno = saved; return result;
}
PUBLIC unsigned frame_compositor_launch_frozen_status_v1(void) {
    int saved = errno;
    pid_t owner = atomic_load_explicit(&frozen_pid, memory_order_acquire);
    unsigned admitted = owner && owner == (pid_t)syscall(SYS_getpid);
    errno = saved; return admitted;
}
#endif
