/* Guest file system: PS4 mount points mapped onto host directories.
 *   /app0, /hostapp  -> game package root (read-only by convention)
 *   /temp0 (sceAppContentTemporaryDataMount2) and SaveData mounts -> user directory
 * Guest descriptors are small integers in our own table; stdio 0-2 pass through.
 * Parent components resolve within the selected guest mount; escaping its root
 * is rejected. Unity's local bundle URLs legitimately use bin/../pack paths. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/tempest_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
/* 64-bit sizes and times; directories are never opened as CRT descriptors. */
typedef struct _stat64 HostStat;
#define host_stat(path,s) _stat64(path,s)
#define host_fstat(fd,s) _fstat64(fd,s)
#define host_lseek _lseeki64
#define O_DIRECTORY 0x40000000 /* tempest marker, removed before _open */
#else
typedef struct stat HostStat;
#define host_stat(path,s) stat(path,s)
#define host_fstat(fd,s) fstat(fd,s)
#define host_lseek lseek
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define MAX_FILES 1024
#define MAX_MOUNTS 64 /* game, user, SaveData mounts, --mount, add-ons */

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    GuestTimespec atime, mtime, ctime;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    int32_t lspare;
    GuestTimespec birthtime;
} GuestStat;
_Static_assert(sizeof(GuestStat)==120,"FreeBSD stat layout");

typedef struct { char *names; size_t count, *offsets; unsigned char *types; } Listing;
/* Character devices served without a host file (shadPS4 file_system.cpp device table). */
enum { DEV_NONE, DEV_RANDOM, DEV_ZERO, DEV_NULL };
typedef struct { int used, host, device, flags, fdflags; Listing *dir; size_t position; char path[512]; } File;
#define MOUNT_GUEST 128 /* guest mount points, or whole file paths for --mount */
typedef struct { char guest[MOUNT_GUEST]; char host[512]; } Mount;
static File files[MAX_FILES];
static Mount mounts[MAX_MOUNTS];
static size_t mount_count, opens, reads, writes;
static _Atomic size_t missing;
static uint64_t bytes_read;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int game_path(const char *p);

/* Opt-in aggregate timings; no clock or counter work on ordinary file calls.
 * Durations overlap: resolver includes its metadata/lock work; lock hold time
 * includes any scans. Reads include short-read recovery, with page preparation
 * listed separately. Reporting never holds the file-table/mount mutex. */
enum FilePerfKind {
    FP_RESOLVE,FP_EXACT_STAT,FP_COMPONENT_STAT,FP_FINAL_STAT,FP_SCAN,FP_OPEN,FP_READ,
    FP_READ_PREP,FP_LOCK_WAIT,FP_LOCK_HOLD,FP_MISS_LOG,FP_COUNT
};
typedef struct { _Atomic uint64_t calls,ns; } FilePerfTime;
static FilePerfTime file_perf[FP_COUNT];
static _Atomic uint64_t file_perf_hits,file_perf_entries,file_perf_final_skips,
    file_perf_misses,file_perf_suppressed,file_perf_bytes,file_perf_next;
static _Atomic uint64_t path_perf_positive,path_perf_negative,path_perf_misses,
    path_perf_stores,path_perf_evictions;
static _Atomic uint64_t snapshot_perf_positive,snapshot_perf_negative,snapshot_perf_fallback,
    snapshot_perf_unknown,snapshot_perf_special,snapshot_perf_unusable,
    snapshot_perf_eligible_dirs,snapshot_perf_rejected_dirs;
static pthread_once_t file_perf_once=PTHREAD_ONCE_INIT;
static int file_perf_enabled;
static _Thread_local uint64_t file_lock_started;
static uint64_t file_perf_clock(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC,&ts)) return 0;
    return (uint64_t)ts.tv_sec*UINT64_C(1000000000)+(uint64_t)ts.tv_nsec;
}
static void file_perf_init(void) {
    const char *env=getenv("TEMPEST_FILE_PERF");
    file_perf_enabled=env && env[0]=='1';
    if (file_perf_enabled) atomic_store(&file_perf_next,file_perf_clock()+UINT64_C(5000000000));
}
static uint64_t file_perf_begin(void) {
    int saved=errno;
    pthread_once(&file_perf_once,file_perf_init);
    uint64_t start=file_perf_enabled ? file_perf_clock() : 0;
    errno=saved;
    return start;
}
static void file_perf_end(enum FilePerfKind kind,uint64_t start) {
    if (!start) return;
    int saved=errno;
    uint64_t end=file_perf_clock();
    atomic_fetch_add_explicit(&file_perf[kind].calls,1,memory_order_relaxed);
    if (end>=start) atomic_fetch_add_explicit(&file_perf[kind].ns,end-start,memory_order_relaxed);
    errno=saved;
}
static void file_perf_report(int force) {
    if (!file_perf_enabled) return;
    int saved=errno;
    uint64_t now=file_perf_clock(),next=atomic_load_explicit(&file_perf_next,memory_order_relaxed);
    if ((!force && now<next) || !atomic_compare_exchange_strong(&file_perf_next,&next,now+UINT64_C(5000000000))) {
        errno=saved; return;
    }
    uint64_t calls[FP_COUNT],ns[FP_COUNT],total=0;
    for (int i=0;i<FP_COUNT;++i) {
        calls[i]=atomic_exchange_explicit(&file_perf[i].calls,0,memory_order_relaxed);
        ns[i]=atomic_exchange_explicit(&file_perf[i].ns,0,memory_order_relaxed);
        total+=calls[i];
    }
    uint64_t hits=atomic_exchange(&file_perf_hits,0),entries=atomic_exchange(&file_perf_entries,0),
        skips=atomic_exchange(&file_perf_final_skips,0),misses=atomic_exchange(&file_perf_misses,0),
        suppressed=atomic_exchange(&file_perf_suppressed,0),bytes=atomic_exchange(&file_perf_bytes,0);
    uint64_t positive=atomic_exchange(&path_perf_positive,0),negative=atomic_exchange(&path_perf_negative,0),
        path_misses=atomic_exchange(&path_perf_misses,0),stores=atomic_exchange(&path_perf_stores,0),
        evictions=atomic_exchange(&path_perf_evictions,0);
    uint64_t snap_positive=atomic_exchange(&snapshot_perf_positive,0),snap_negative=atomic_exchange(&snapshot_perf_negative,0),
        fallback=atomic_exchange(&snapshot_perf_fallback,0),unknown=atomic_exchange(&snapshot_perf_unknown,0),
        special=atomic_exchange(&snapshot_perf_special,0),unusable=atomic_exchange(&snapshot_perf_unusable,0),
        eligible_dirs=atomic_exchange(&snapshot_perf_eligible_dirs,0),rejected_dirs=atomic_exchange(&snapshot_perf_rejected_dirs,0);
    if (total) {
        printf("File perf: resolve %llu/%.3fms; stat exact %llu/%.3fms component %llu/%.3fms final %llu/%.3fms (%llu skipped); scans %llu/%.3fms, cache hits %llu, names %llu\n",
            (unsigned long long)calls[FP_RESOLVE],ns[FP_RESOLVE]/1e6,
            (unsigned long long)calls[FP_EXACT_STAT],ns[FP_EXACT_STAT]/1e6,
            (unsigned long long)calls[FP_COMPONENT_STAT],ns[FP_COMPONENT_STAT]/1e6,
            (unsigned long long)calls[FP_FINAL_STAT],ns[FP_FINAL_STAT]/1e6,(unsigned long long)skips,
            (unsigned long long)calls[FP_SCAN],ns[FP_SCAN]/1e6,(unsigned long long)hits,(unsigned long long)entries);
        printf("File perf: open %llu/%.3fms; read %llu/%.3fms (%llu bytes), page prep %.3fms; file lock %llu waits %.3fms holds %.3fms; misses %llu, messages %llu/%.3fms, suppressed %llu\n",
            (unsigned long long)calls[FP_OPEN],ns[FP_OPEN]/1e6,
            (unsigned long long)calls[FP_READ],ns[FP_READ]/1e6,(unsigned long long)bytes,ns[FP_READ_PREP]/1e6,
            (unsigned long long)calls[FP_LOCK_WAIT],ns[FP_LOCK_WAIT]/1e6,ns[FP_LOCK_HOLD]/1e6,
            (unsigned long long)misses,(unsigned long long)calls[FP_MISS_LOG],ns[FP_MISS_LOG]/1e6,(unsigned long long)suppressed);
        printf("File perf: path cache positive %llu negative %llu misses %llu stores %llu evictions %llu\n",
            (unsigned long long)positive,(unsigned long long)negative,(unsigned long long)path_misses,
            (unsigned long long)stores,(unsigned long long)evictions);
        printf("File perf: cold snapshots positive %llu negative %llu fallback %llu (unknown type %llu special/non-dir %llu access/incomplete %llu); directories eligible %llu rejected %llu\n",
            (unsigned long long)snap_positive,(unsigned long long)snap_negative,(unsigned long long)fallback,
            (unsigned long long)unknown,(unsigned long long)special,(unsigned long long)unusable,
            (unsigned long long)eligible_dirs,(unsigned long long)rejected_dirs);
    }
    errno=saved;
}
static void file_lock(void) {
    uint64_t start=file_perf_begin();
    pthread_mutex_lock(&lock);
    file_perf_end(FP_LOCK_WAIT,start);
    file_lock_started=file_perf_begin();
}
static void file_unlock(void) {
    uint64_t start=file_lock_started;
    file_lock_started=0;
    pthread_mutex_unlock(&lock);
    file_perf_end(FP_LOCK_HOLD,start);
    file_perf_report(0);
}
static int file_case_stat(const char *path,HostStat *st,enum FilePerfKind kind) {
    uint64_t start=file_perf_begin();
    int result=host_stat(path,st);
    file_perf_end(kind,start);
    return result;
}
static int file_host_open(const char *path,int flags,int mode) {
    uint64_t start=file_perf_begin();
    int result=open(path,flags,mode);
    file_perf_end(FP_OPEN,start);
    return result;
}

/* Read-only package names are sometimes lowercased by the guest SDK. Cache
 * only directories actually visited, never a recursive package inventory.
 * All accesses and mount invalidation use lock. Immutable game mounts make
 * negative name lookups stable until the next mount change. */
#define CASE_CACHE_DIRS 64
#define CASE_CACHE_NAMES (256u*1024u)
#define CASE_CACHE_PATH (512+1024)
typedef struct {
    char path[CASE_CACHE_PATH];
    char *names;
    size_t bytes;
    uint64_t age;
    int complete,searchable;
} CaseDirectory;
enum { CASE_UNKNOWN,CASE_REGULAR,CASE_DIRECTORY,CASE_SPECIAL };
static CaseDirectory case_directories[CASE_CACHE_DIRS];
static uint64_t case_cache_age;

/* Exact suffixes retain the existing per-component spelling precedence. A cached negative
 * carries the original host spelling too: public translate must still return that path.
 * Complete searchable directory snapshots can prove misses without a failed host open. */
#define PATH_CACHE_ENTRIES 512
typedef struct {
    uint64_t age,generation;
    size_t mount;
    int error;
    char suffix[1024],path[1024];
} CachedPath;
typedef struct {
    uint64_t generation;
    size_t mount;
    int eligible,stable_missing,error;
    char suffix[1024];
} PathResolution;
static CachedPath path_cache[PATH_CACHE_ENTRIES];
static uint64_t path_cache_age,mount_generation=1;
static pthread_once_t path_cache_once=PTHREAD_ONCE_INIT;
static int path_cache_enabled;
static void init_path_cache(void) {
    const char *env=getenv("TEMPEST_FILE_CACHE");
    path_cache_enabled=!env || strcmp(env,"0");
}
/* Called under the mount mutex for every successful mount mutation, including table moves. */
static void invalidate_paths(void) {
    ++mount_generation;
    memset(path_cache,0,sizeof(path_cache)); path_cache_age=0;
}
static CachedPath *find_path(size_t mount,const char *suffix) {
    for (size_t i=0;i<PATH_CACHE_ENTRIES;++i) {
        CachedPath *entry=&path_cache[i];
        if (entry->age && entry->generation==mount_generation && entry->mount==mount &&
            !strcmp(entry->suffix,suffix)) {
            entry->age=++path_cache_age;
            if (file_perf_enabled) atomic_fetch_add_explicit(entry->error ? &path_perf_negative :
                &path_perf_positive,1,memory_order_relaxed);
            return entry;
        }
    }
    if (file_perf_enabled) atomic_fetch_add_explicit(&path_perf_misses,1,memory_order_relaxed);
    return NULL;
}
static void store_path(size_t mount,const char *suffix,const char *path,int error) {
    if (strlen(suffix)>=sizeof(path_cache[0].suffix) || strlen(path)>=sizeof(path_cache[0].path)) return;
    CachedPath *slot=&path_cache[0];
    for (size_t i=0;i<PATH_CACHE_ENTRIES;++i) {
        CachedPath *entry=&path_cache[i];
        if (entry->age && entry->mount==mount && !strcmp(entry->suffix,suffix)) { slot=entry; break; }
        if (entry->age<slot->age) slot=entry;
    }
    if (file_perf_enabled) {
        atomic_fetch_add_explicit(&path_perf_stores,1,memory_order_relaxed);
        if (slot->age && (slot->mount!=mount || strcmp(slot->suffix,suffix)))
            atomic_fetch_add_explicit(&path_perf_evictions,1,memory_order_relaxed);
    }
    slot->age=++path_cache_age; slot->generation=mount_generation; slot->mount=mount; slot->error=error;
    strcpy(slot->suffix,suffix); strcpy(slot->path,path);
}
static void remember_missing(const PathResolution *resolution,const char *path,int error) {
    if (!resolution->eligible || !resolution->stable_missing || (error!=ENOENT && error!=ENOTDIR)) return;
    int saved=errno;
    file_lock();
    /* The host operation ran outside the mount lock; do not revive an older mount mapping. */
    if (resolution->generation==mount_generation)
        store_path(resolution->mount,resolution->suffix,path,error);
    file_unlock(); errno=saved;
}

static void clear_case_cache(void) {
    for (size_t i=0;i<CASE_CACHE_DIRS;++i) {
        free(case_directories[i].names);
        memset(&case_directories[i],0,sizeof(case_directories[i]));
    }
    case_cache_age=0;
}
static int searchable_directory(DIR *directory) {
#ifdef _WIN32
    (void)directory;
    return 0; /* Retain host case/permission semantics through the stat resolver. */
#else
    if (!faccessat(dirfd(directory),".",X_OK,AT_EACCESS)) return 1;
    if ((errno==EINVAL || errno==ENOSYS || errno==ENOTSUP) &&
        getuid()==geteuid() && getgid()==getegid())
        return !faccessat(dirfd(directory),".",X_OK,0);
    return 0;
#endif
}
static CaseDirectory *case_directory(const char *path) {
    CaseDirectory *slot=&case_directories[0];
    for (size_t i=0;i<CASE_CACHE_DIRS;++i) {
        CaseDirectory *entry=&case_directories[i];
        if (entry->age && !strcmp(entry->path,path)) {
            if (file_perf_enabled) atomic_fetch_add_explicit(&file_perf_hits,1,memory_order_relaxed);
            entry->age=++case_cache_age;
            return entry;
        }
        if (entry->age<slot->age) slot=entry;
    }
    uint64_t scan_start=file_perf_begin();
    DIR *dir=opendir(path);
    if (!dir) { file_perf_end(FP_SCAN,scan_start); return NULL; }
    free(slot->names);
    memset(slot,0,sizeof(*slot));
    snprintf(slot->path,sizeof(slot->path),"%s",path);
    slot->age=++case_cache_age;
    slot->complete=1;
    slot->searchable=path_cache_enabled && searchable_directory(dir);
    size_t capacity=0,name_bytes=0;
    struct dirent *entry;
    errno=0;
    while ((entry=readdir(dir))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        size_t n=strlen(entry->d_name)+2; /* dirent kind byte, then NUL-terminated name */
        /* Keep the original filename-byte limit, including in cache-off mode.
         * Type metadata uses at most one extra byte per two filename bytes. */
        if (n-1>CASE_CACHE_NAMES-name_bytes) { slot->complete=0; break; }
        if (slot->bytes+n>capacity) {
            size_t next=capacity ? capacity*2 : 4096;
            if (next<slot->bytes+n) next=slot->bytes+n;
            if (next>CASE_CACHE_NAMES*3u/2u) next=CASE_CACHE_NAMES*3u/2u;
            char *names=realloc(slot->names,next);
            if (!names) { slot->complete=0; break; }
            slot->names=names; capacity=next;
        }
        unsigned char kind=CASE_UNKNOWN;
#ifndef _WIN32
        kind=entry->d_type==DT_DIR ? CASE_DIRECTORY : entry->d_type==DT_REG ? CASE_REGULAR :
             entry->d_type==DT_UNKNOWN ? CASE_UNKNOWN : CASE_SPECIAL;
#endif
        slot->names[slot->bytes]=(char)kind;
        memcpy(slot->names+slot->bytes+1,entry->d_name,n-1);
        slot->bytes+=n; name_bytes+=n-1;
        if (file_perf_enabled) atomic_fetch_add_explicit(&file_perf_entries,1,memory_order_relaxed);
        errno=0;
    }
    if (!entry && errno) slot->complete=0;
    closedir(dir);
    file_perf_end(FP_SCAN,scan_start);
    /* A partial list cannot establish that a folded name is unambiguous. */
    if (!slot->complete) { free(slot->names); slot->names=NULL; slot->bytes=0; }
    if (file_perf_enabled && path_cache_enabled)
        atomic_fetch_add_explicit(slot->complete && slot->searchable ? &snapshot_perf_eligible_dirs :
            &snapshot_perf_rejected_dirs,1,memory_order_relaxed);
    return slot;
}
static unsigned char filename_fold(unsigned char c) {
    return c>='A' && c<='Z' ? (unsigned char)(c+('a'-'A')) : c;
}
static const char *case_name_kind(const char *directory,const char *name,size_t length,
                                   int *complete,int *searchable,unsigned char *kind) {
    CaseDirectory *entry=case_directory(directory);
    *complete=entry && entry->complete;
    if (searchable) *searchable=entry && entry->searchable;
    if (!entry || !entry->complete) return NULL;
    const char *match=NULL;
    unsigned char match_kind=CASE_UNKNOWN;
    int ambiguous=0;
    for (size_t offset=0;offset<entry->bytes;) {
        unsigned char candidate_kind=(unsigned char)entry->names[offset++];
        const char *candidate=entry->names+offset;
        size_t n=strlen(candidate); offset+=n+1;
        if (n!=length) continue;
        size_t i=0;
        while (i<n && filename_fold((unsigned char)candidate[i])==filename_fold((unsigned char)name[i])) ++i;
        if (i!=n) continue;
        if (!memcmp(candidate,name,n)) { if (kind) *kind=candidate_kind; return candidate; }
        if (match) ambiguous=1; /* Keep looking for an exact spelling, if present. */
        else { match=candidate; match_kind=candidate_kind; }
    }
    if (kind) *kind=match_kind;
    return ambiguous ? NULL : match;
}
static const char *case_name(const char *directory,const char *name,size_t length,int *complete) {
    return case_name_kind(directory,name,length,complete,NULL,NULL);
}
/* Cold lookup using only complete, searchable directory snapshots. Known directory entry
 * types let us avoid per-component stats; a positive leaf still gets one real stat. */
static int snapshot_game_path(const Mount *mount,const char *suffix,char *out,size_t size) {
    if (!*suffix) return 0;
    char resolved[CASE_CACHE_PATH]; size_t used=strlen(mount->host);
    memcpy(resolved,mount->host,used+1);
    while (*suffix) {
        ++suffix;
        const char *end=strchr(suffix,'/');
        size_t n=end ? (size_t)(end-suffix) : strlen(suffix);
        int complete=0,searchable=0; unsigned char kind=CASE_UNKNOWN;
        const char *name=case_name_kind(resolved,suffix,n,&complete,&searchable,&kind);
        if (!complete || !searchable) {
            if (file_perf_enabled) atomic_fetch_add_explicit(&snapshot_perf_unusable,1,memory_order_relaxed);
            return 0;
        }
        if (!name) return -1;
        if (kind==CASE_UNKNOWN || (end ? kind!=CASE_DIRECTORY :
            kind!=CASE_DIRECTORY && kind!=CASE_REGULAR)) {
            if (file_perf_enabled) atomic_fetch_add_explicit(kind==CASE_UNKNOWN ?
                &snapshot_perf_unknown : &snapshot_perf_special,1,memory_order_relaxed);
            return 0;
        }
        if (used+n+2>sizeof(resolved)) return 0;
        resolved[used++]='/'; memcpy(resolved+used,name,n); used+=n; resolved[used]=0;
        suffix+=n;
    }
    HostStat st;
    if (file_case_stat(resolved,&st,FP_FINAL_STAT)) return 0;
    if (used>=size) return 0;
    memcpy(out,resolved,used+1);
    return 1;
}
/* Leave the original normalized path untouched on a miss or ambiguity. Public
 * translate also serves callers that need the host spelling of missing paths. */
/* 1: validated existing path; -1: stable absent/ambiguous name; 0: unknown/error. */
static int case_game_path(const Mount *mount,const char *suffix,char *out,size_t size) {
    HostStat st;
    if (!file_case_stat(out,&st,FP_EXACT_STAT)) return 1;
    if (errno!=ENOENT && errno!=ENOTDIR) return 0;
    char resolved[CASE_CACHE_PATH];
    size_t used=strlen(mount->host);
    memcpy(resolved,mount->host,used+1);
    int final_valid=0;
    while (*suffix) {
        ++suffix; /* Normalized suffix has one slash before each component. */
        const char *end=strchr(suffix,'/');
        size_t n=end ? (size_t)(end-suffix) : strlen(suffix);
        if (used+n+2>sizeof(resolved)) return 0;
        resolved[used]='/'; memcpy(resolved+used+1,suffix,n); resolved[used+n+1]=0;
        final_valid=!file_case_stat(resolved,&st,FP_COMPONENT_STAT);
        if (!final_valid) {
            if (errno!=ENOENT && errno!=ENOTDIR) return 0;
            resolved[used]=0;
            int complete=0;
            const char *name=case_name(resolved,suffix,n,&complete);
            if (!name) return complete ? -1 : 0;
            resolved[used]='/'; memcpy(resolved+used+1,name,n); resolved[used+n+1]=0;
        }
        used+=n+1;
        suffix+=n;
    }
    if (final_valid) {
        if (file_perf_enabled) atomic_fetch_add_explicit(&file_perf_final_skips,1,memory_order_relaxed);
    } else final_valid=!file_case_stat(resolved,&st,FP_FINAL_STAT);
    if (final_valid && used<size) { memcpy(out,resolved,used+1); return 1; }
    return 0;
}

int runtime_file_mount(const char *guest,const char *host) {
    file_lock();
    for (size_t i=0;i<mount_count;++i) if (!strcmp(mounts[i].guest,guest)) {
        if (game_path(guest)) clear_case_cache();
        invalidate_paths();
        snprintf(mounts[i].host,sizeof(mounts[i].host),"%s",host);
        file_unlock(); return 0;
    }
    if (mount_count==MAX_MOUNTS || strlen(guest)>=MOUNT_GUEST || strlen(host)>=512) { file_unlock(); return -1; }
    if (game_path(guest)) clear_case_cache();
    invalidate_paths();
    snprintf(mounts[mount_count].guest,MOUNT_GUEST,"%s",guest);
    snprintf(mounts[mount_count].host,512,"%s",host);
    ++mount_count;
    file_unlock();
    return 0;
}
void runtime_file_unmount(const char *guest) {
    file_lock();
    for (size_t i=0;i<mount_count;++i) if (!strcmp(mounts[i].guest,guest)) {
        if (game_path(guest)) clear_case_cache();
        invalidate_paths();
        mounts[i]=mounts[--mount_count]; break;
    }
    file_unlock();
}
static char user_root[512]="user";
const char *runtime_file_user_dir(void) { return user_root; }
void runtime_file_configure(const char *app0,const char *user) {
    snprintf(user_root,sizeof(user_root),"%s",user);
    runtime_file_mount("/app0",app0);
    runtime_file_mount("/hostapp",app0);
    mkdir(user,0755);
}
/* Resolve a guest path to a host path; returns 0 or a host errno. */
static int translate_resolved(const char *guest,char *out,size_t size,PathResolution *resolution) {
    uint64_t perf_start=file_perf_begin();
    if (resolution) memset(resolution,0,sizeof(*resolution));
    if (!guest || !*guest) { file_perf_end(FP_RESOLVE,perf_start); return ENOENT; }
    char buffer[1024];
    int length=guest[0]!='/' ? snprintf(buffer,sizeof(buffer),"/app0/%s",guest)
                              : snprintf(buffer,sizeof(buffer),"%s",guest);
    if (length<0 || (size_t)length>=sizeof(buffer)) { file_perf_end(FP_RESOLVE,perf_start); return ENAMETOOLONG; }
    file_lock();
    size_t best=0; const Mount *m=NULL;
    for (size_t i=0;i<mount_count;++i) {
        size_t n=strlen(mounts[i].guest);
        if (!strncmp(buffer,mounts[i].guest,n) && (buffer[n]=='/' || !buffer[n]) && n>best) { best=n; m=&mounts[i]; }
    }
    int result=0;
    if (!m) result=ENOENT;
    else {
        char suffix[1024]; size_t used=0,depth=0,starts[512];
        const char *component=buffer+best;
        while (*component && !result) {
            while (*component=='/') ++component;
            const char *end=component;
            while (*end && *end!='/') ++end;
            size_t n=(size_t)(end-component);
            if (!n) break;
            if (n==1 && component[0]=='.') { component=end; continue; }
            if (n==2 && component[0]=='.' && component[1]=='.') {
                if (!depth) result=EACCES;
                else used=starts[--depth];
            } else if (depth==512 || used+n+1>=sizeof(suffix)) result=ENAMETOOLONG;
            else {
                starts[depth++]=used;
                suffix[used++]='/'; memcpy(suffix+used,component,n); used+=n;
            }
            component=end;
        }
        suffix[used]=0;
        if (!result && (size_t)snprintf(out,size,"%s%s",m->host,suffix)>=size) result=ENAMETOOLONG;
        if (!result && game_path(m->guest)) {
            pthread_once(&path_cache_once,init_path_cache);
            size_t mount=(size_t)(m-mounts);
            if (path_cache_enabled && resolution) {
                resolution->eligible=1; resolution->generation=mount_generation;
                resolution->mount=mount; strcpy(resolution->suffix,suffix);
            }
            CachedPath *cached=path_cache_enabled ? find_path(mount,suffix) : NULL;
            if (cached) {
                size_t length=strlen(cached->path);
                if (length>=size) result=ENAMETOOLONG;
                else {
                    memcpy(out,cached->path,length+1);
                    if (resolution) resolution->error=cached->error;
                }
            } else {
                int snapshot=path_cache_enabled ? snapshot_game_path(m,suffix,out,size) : 0;
                if (file_perf_enabled && path_cache_enabled)
                    atomic_fetch_add_explicit(snapshot>0 ? &snapshot_perf_positive : snapshot<0 ?
                        &snapshot_perf_negative : &snapshot_perf_fallback,1,memory_order_relaxed);
                int status=snapshot ? snapshot : case_game_path(m,suffix,out,size);
                if (path_cache_enabled && status>0) store_path(mount,suffix,out,0);
                if (resolution) resolution->stable_missing=status<0;
                /* Only the snapshot walk proves every parent is a searchable directory.
                 * Fallbacks (including symlinks) retain the original host-error check. */
                if (snapshot<0) {
                    store_path(mount,suffix,out,ENOENT);
                    if (resolution) resolution->error=ENOENT;
                }
            }
        }
    }
    file_unlock();
    file_perf_end(FP_RESOLVE,perf_start);
    if (result==ENOENT) fprintf(stderr,"Runtime: no mount for guest path %s\n",guest);
    return result;
}
static int translate(const char *guest,char *out,size_t size) {
    return translate_resolved(guest,out,size,NULL);
}
static int host_flags(int flags) {
    int r;
    switch (flags&3) { case 0: r=O_RDONLY; break; case 1: r=O_WRONLY; break; default: r=O_RDWR; }
    if (flags&0x4) r|=O_NONBLOCK;
    if (flags&0x8) r|=O_APPEND;
    if (flags&0x80) r|=O_SYNC;
    if (flags&0x200) r|=O_CREAT;
    if (flags&0x400) r|=O_TRUNC;
    if (flags&0x800) r|=O_EXCL;
    if (flags&0x20000) r|=O_DIRECTORY;
#ifdef _WIN32
    r|=O_BINARY;
#endif
    return r|O_CLOEXEC;
}
static void free_listing(Listing *l) { if (l) { free(l->names); free(l->offsets); free(l->types); free(l); } }
static Listing *list_directory(const char *path) {
    DIR *d=opendir(path);
    if (!d) return NULL;
    Listing *l=calloc(1,sizeof(*l));
    size_t capacity=0,bytes=0,cap_names=0;
    struct dirent *e;
    while (l && (e=readdir(d))) {
        size_t n=strlen(e->d_name)+1;
        if (l->count==capacity) {
            capacity=capacity ? capacity*2 : 64;
            l->offsets=realloc(l->offsets,capacity*sizeof(size_t));
            l->types=realloc(l->types,capacity);
        }
        if (bytes+n>cap_names) { cap_names=(bytes+n)*2; l->names=realloc(l->names,cap_names); }
        if (!l->offsets || !l->types || !l->names) { fputs("Out of memory listing directory\n",stderr); exit(1); }
        memcpy(l->names+bytes,e->d_name,n);
        l->offsets[l->count]=bytes;
#ifdef _WIN32
        /* MinGW's dirent has no d_type. */
        char child[1100]; HostStat entry;
        snprintf(child,sizeof(child),"%s/%s",path,e->d_name);
        l->types[l->count]=host_stat(child,&entry) ? 0 : S_ISDIR(entry.st_mode) ? 4 : S_ISREG(entry.st_mode) ? 8 : 0;
#else
        unsigned char type=e->d_type;
        if (type==DT_LNK || type==DT_UNKNOWN) {
            struct stat entry;
            if (!fstatat(dirfd(d),e->d_name,&entry,0))
                type=S_ISDIR(entry.st_mode) ? DT_DIR : S_ISREG(entry.st_mode) ? DT_REG : type;
        }
        l->types[l->count]=type==DT_DIR ? 4 : type==DT_REG ? 8 : type==DT_LNK ? 10 : 0;
#endif
        ++l->count; bytes+=n;
    }
    closedir(d);
    return l;
}
static void convert_stat(const HostStat *s,GuestStat *g) {
    memset(g,0,sizeof(*g));
    g->dev=(uint32_t)s->st_dev; g->ino=(uint32_t)s->st_ino;
    g->nlink=(uint16_t)s->st_nlink;
    g->size=s->st_size;
#ifdef _WIN32
    /* FreeBSD mode bits: directories rwxr-xr-x, files rw-r--r--. */
    g->mode=(uint16_t)(S_ISDIR(s->st_mode) ? 0040755 : 0100644);
    g->blocks=(s->st_size+511)/512; g->blksize=4096;
    g->atime=(GuestTimespec){s->st_atime,0};
    g->mtime=(GuestTimespec){s->st_mtime,0};
    g->ctime=(GuestTimespec){s->st_ctime,0};
#else
    g->mode=(uint16_t)s->st_mode;
    g->blocks=s->st_blocks; g->blksize=(uint32_t)s->st_blksize;
    g->atime=(GuestTimespec){s->st_atim.tv_sec,s->st_atim.tv_nsec};
    g->mtime=(GuestTimespec){s->st_mtim.tv_sec,s->st_mtim.tv_nsec};
    g->ctime=(GuestTimespec){s->st_ctim.tv_sec,s->st_ctim.tv_nsec};
#endif
    g->birthtime=g->ctime;
}
#ifdef _WIN32
/* The CRT reports st_ino 0 for every file. Mono's FileStream takes files with the same device and
 * inode for one file, so with a save file open for writing its share check refused the next one
 * ("Sharing violation") and the save never finished. The volume serial and the 64-bit file index
 * identify a file; ino folds the index into 32 bits. */
static void file_identity(HANDLE h,GuestStat *g) {
    BY_HANDLE_FILE_INFORMATION i;
    if (h==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h,&i)) return;
    g->dev=(uint32_t)i.dwVolumeSerialNumber;
    g->ino=(uint32_t)(i.nFileIndexLow^i.nFileIndexHigh*UINT32_C(0x9e3779b1));
}
static void path_identity(const char *path,GuestStat *g) {
    HANDLE h=CreateFileA(path,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    if (h==INVALID_HANDLE_VALUE) return;
    file_identity(h,g);
    CloseHandle(h);
}
#endif
static File *get(int fd) {
    if (fd<3 || fd>=MAX_FILES || !files[fd].used) return NULL;
    return &files[fd];
}
/* TEMPEST_FILE_TRACE=1: opens, writes, renames and removals outside the game package (/app0,
 * /hostapp) and failed reads anywhere. */
static int file_trace(void) { static int v=-1; if (v<0) { const char *e=getenv("TEMPEST_FILE_TRACE"); v=e && e[0]=='1'; } return v; }
static const char *traced_path(int fd) {
    static const char none[]="?";
    return fd>=3 && fd<MAX_FILES && files[fd].used ? files[fd].path : none;
}
/* Game mounts (including linked mod overlays) are read-only. Saves use other mounts. */
static int game_path(const char *p) {
    if (!p || !*p) return 0;
    if (*p!='/') return 1;
    return (!strncmp(p,"/app0",5) && (!p[5] || p[5]=='/')) ||
           (!strncmp(p,"/hostapp",8) && (!p[8] || p[8]=='/'));
}
/* Archive probing repeats the same missing paths thousands of times. Keep the
 * first32 distinct game misses and one suppression notice; save/device misses
 * remain visible. FILE_TRACE restores every line. This only changes logging. */
static void report_missing(const char *guest) {
    static pthread_mutex_t miss_lock=PTHREAD_MUTEX_INITIALIZER;
    static uint64_t names[32];
    static unsigned count;
    static int notice;
    ++missing;
    if (file_perf_enabled) atomic_fetch_add_explicit(&file_perf_misses,1,memory_order_relaxed);
    int print=1;
    if (game_path(guest) && !file_trace()) {
        uint64_t hash=UINT64_C(14695981039346656037);
        for (const unsigned char *p=(const unsigned char *)guest;*p;++p)
            hash=(hash^*p)*UINT64_C(1099511628211);
        pthread_mutex_lock(&miss_lock);
        for (unsigned i=0;i<count;++i) if (names[i]==hash) { print=0; break; }
        if (print) {
            if (count<32) names[count++]=hash;
            else { print=notice ? 0 : 2; notice=1; }
        }
        pthread_mutex_unlock(&miss_lock);
    }
    if (print!=1 && file_perf_enabled)
        atomic_fetch_add_explicit(&file_perf_suppressed,1,memory_order_relaxed);
    if (print) {
        uint64_t start=file_perf_begin();
        if (print==1) printf("Runtime: open(%s) -> not found\n",guest);
        else puts("Runtime: further game-file misses suppressed; TEMPEST_FILE_TRACE=1 prints every path (missing counter retained)");
        file_perf_end(FP_MISS_LOG,start);
    }
    file_perf_report(0);
}
static int device_kind(const char *p) {
    static const struct { const char *path; int kind; } devices[]={
        {"/dev/urandom",DEV_RANDOM}, {"/dev/random",DEV_RANDOM}, {"/dev/srandom",DEV_RANDOM},
        {"/dev/rng",DEV_RANDOM}, {"/dev/zero",DEV_ZERO}, {"/dev/null",DEV_NULL},
    };
    if (!p) return DEV_NONE;
    for (size_t i=0;i<sizeof(devices)/sizeof(*devices);++i) if (!strcmp(p,devices[i].path)) return devices[i].kind;
    return DEV_NONE;
}
/* Reads of a random device return that many bytes from the host CSPRNG. */
static int64_t device_read(int kind,void *buffer,uint64_t size) {
    if (kind==DEV_NULL) return 0;
    if (kind==DEV_ZERO) { memset(buffer,0,size); return (int64_t)size; }
#ifdef _WIN32
    for (uint64_t done=0; done<size;) {
        ULONG n=(ULONG)(size-done>0x10000000 ? 0x10000000 : size-done);
        if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL,(unsigned char *)buffer+done,n,BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return -EIO;
        done+=n;
    }
    return (int64_t)size;
#else
    int h=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    if (h<0) return -errno;
    uint64_t done=0;
    while (done<size) { ssize_t n=read(h,(char *)buffer+done,size-done); if (n<=0) break; done+=(uint64_t)n; }
    close(h);
    return done==size ? (int64_t)size : -EIO;
#endif
}
/* All operations return >=0 or -(host errno); wrappers adapt the convention. */
static int64_t do_open(const char *guest,int flags,int mode) {
    int device=device_kind(guest);
    if (device) {
        file_lock();
        int fd=-1;
        for (int i=3;i<MAX_FILES;++i) if (!files[i].used) { fd=i; break; }
        if (fd>=0) {
            files[fd]=(File){.used=1,.host=-1,.device=device,.flags=flags,.fdflags=!!(flags&0x100000)};
            snprintf(files[fd].path,sizeof(files[fd].path),"%s",guest);
            ++opens;
        }
        file_unlock();
        return fd<0 ? -EMFILE : fd;
    }
    if (game_path(guest) && (flags & (3|0x8|0x200|0x400|0x800))) return -EROFS;
    /* skip_intro: the engine waits on its intro movie and does not always come back from it
     * (AvPlayer). Reporting the movie missing is what the engine handles cleanly, and it then
     * goes straight to the title screen. Only the game's own .mp4 files are refused. */
    if (tempest_skip_intro() && game_path(guest)) {
        size_t n = strlen(guest);
        if (n > 4 && !strcasecmp(guest + n - 4, ".mp4")) {
            printf("Runtime: open(%s) -> skipped (skip_intro=1)\n", guest);
            return -ENOENT;
        }
    }
    char path[1024];
    PathResolution resolution;
    int e=translate_resolved(guest,path,sizeof(path),&resolution);
    if (e) return -e;
    if (resolution.error) {
        if (resolution.error==ENOENT) report_missing(guest);
        return -resolution.error;
    }
#ifdef _WIN32
    /* Windows cannot open a directory as a CRT descriptor: directories are listings only. */
    (void)mode;
    HostStat s;
    Listing *dir=NULL;
    int host=-1;
    if (!host_stat(path,&s) && S_ISDIR(s.st_mode)) {
        if (flags&3) return -EISDIR;
        if (!(dir=list_directory(path))) return -EACCES;
    } else if (flags&0x20000) {
        e=errno==ENOENT ? ENOENT : ENOTDIR;
        remember_missing(&resolution,path,e);
        if (e==ENOENT) report_missing(guest);
        return -e;
    } else {
        host=file_host_open(path,host_flags(flags)&~O_DIRECTORY,_S_IREAD|_S_IWRITE);
        if (host<0) {
            e=errno;
            remember_missing(&resolution,path,e);
            if (e==ENOENT) report_missing(guest);
            return -e;
        }
        if (host_fstat(host,&s)) memset(&s,0,sizeof(s));
    }
#else
    int host=file_host_open(path,host_flags(flags),mode ? mode : 0644);
    if (host<0) {
        e=errno;
        remember_missing(&resolution,path,e);
        if (e==ENOENT) report_missing(guest);
        return -e;
    }
    struct stat s;
    Listing *dir=NULL;
    if (!fstat(host,&s) && S_ISDIR(s.st_mode)) dir=list_directory(path);
#endif
    file_lock();
    int fd=-1;
    for (int i=3;i<MAX_FILES;++i) if (!files[i].used) { fd=i; break; }
    if (fd<0) { file_unlock(); if (host>=0) close(host); free_listing(dir); return -EMFILE; }
    files[fd]=(File){.used=1,.host=host,.dir=dir,.flags=flags,.fdflags=!!(flags&0x100000)};
    snprintf(files[fd].path,sizeof(files[fd].path),"%s",guest);
    ++opens;
    file_unlock();
    if (file_trace() && !game_path(guest)) {
        char frames[2048];
        runtime_guest_backtrace(frames,sizeof(frames),32);
        printf("File trace: open(%s, 0x%x) -> fd %d, %lld bytes\n    from %s\n",guest,flags,fd,(long long)s.st_size,frames);
    }
    return fd;
}
static int64_t do_close(int fd) {
    if (fd>=0 && fd<3) return 0;
    file_lock();
    File *f=get(fd);
    if (!f) { file_unlock(); return -EBADF; }
    if (file_trace() && !game_path(f->path) && !f->device) printf("File trace: close(fd %d %s)\n",fd,f->path);
    if (f->host>=0) close(f->host);
    free_listing(f->dir);
    *f=(File){0};
    file_unlock();
    return 0;
}
static int host_fd(int fd) {
    if (fd>=0 && fd<3) return fd;
    File *f=get(fd);
    return f ? f->host : -1;
}
#ifdef _WIN32
/* The CRT reads and writes at most INT_MAX bytes per call. */
static ssize_t host_read(int h,void *buffer,uint64_t size) { return read(h,buffer,(unsigned)(size>0x7ffff000 ? 0x7ffff000 : size)); }
static ssize_t host_write(int h,const void *buffer,uint64_t size) { return write(h,buffer,(unsigned)(size>0x7ffff000 ? 0x7ffff000 : size)); }
#else
#define host_read read
#define host_write write
#endif
/* Pages of the destination may be write-protected for GPU tracking: the kernel's copy then
 * fails with EFAULT instead of faulting to our handler. A user-mode write to each page first
 * goes through the handler, which unprotects it (and records the upcoming write). */
static void touch_for_write(void *buffer,uint64_t size) {
    if (!size) return;
    uintptr_t p=(uintptr_t)buffer & ~(uintptr_t)4095, end=(uintptr_t)buffer+size;
    for (; p<end; p+=4096) {
        volatile unsigned char *b=(volatile unsigned char *)(p<(uintptr_t)buffer ? (uintptr_t)buffer : p);
        *b=*b;
    }
}
static int device_of(int fd) { File *f=get(fd); return f ? f->device : DEV_NONE; }
/* The GPU tracker can protect a page again between touch_for_write and the kernel's copy (a page
 * shared with a buffer the GPU thread uploads meanwhile): the copy then fails or stops short, and
 * the game drops the asset for the session. A read that ends before the end of the file is
 * finished through a host buffer, copied in user mode, where such a write faults to our handler. */
static ssize_t finish_short_read(int fd,int h,unsigned char *buffer,uint64_t size,int64_t offset,ssize_t n) {
    const int error=n<0 ? errno : 0;
    HostStat st;
    if ((uint64_t)(n<0 ? 0 : n)>=size || host_fstat(h,&st) || offset+(n<0 ? 0 : n)>=(int64_t)st.st_size) return n;
    const uint64_t want=size<(uint64_t)(st.st_size-offset) ? size : (uint64_t)(st.st_size-offset);
    uint64_t done=n<0 ? 0 : (uint64_t)n;
    unsigned char *bounce=malloc(1<<20);
    if (!bounce) return n;
    while (done<want) {
        const uint64_t chunk=want-done<(1u<<20) ? want-done : (1u<<20);
        ssize_t got=pread(h,bounce,chunk,offset+(int64_t)done);
        if (got<=0) break;
        memcpy(buffer+done,bounce,(size_t)got);
        done+=(uint64_t)got;
    }
    free(bounce);
    printf("Runtime: file read into guest memory came back short (%lld of %llu bytes, errno %d, fd %d %s); "
           "finished through a host buffer: %llu bytes\n",(long long)n,(unsigned long long)size,error,fd,
           traced_path(fd),(unsigned long long)done);
    if (!done) errno=error;
    return done ? (ssize_t)done : n;
}
static int64_t do_read(int fd,void *buffer,uint64_t size) {
    int device=device_of(fd);
    if (device) { touch_for_write(buffer,size); return device_read(device,buffer,size); }
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    uint64_t prep_start=file_perf_begin();
    touch_for_write(buffer,size);
    file_perf_end(FP_READ_PREP,prep_start);
    const int64_t start=host_lseek(h,0,SEEK_CUR);
    const uint64_t wait_start=runtime_wait_begin();
    uint64_t perf_start=file_perf_begin();
    ssize_t n=host_read(h,buffer,size);
    if (start>=0 && n<(ssize_t)size) {
        n=finish_short_read(fd,h,buffer,size,start,n);
        if (n>0) host_lseek(h,start+n,SEEK_SET);
    }
    file_perf_end(FP_READ,perf_start);
    if (file_perf_enabled && n>0) atomic_fetch_add_explicit(&file_perf_bytes,(uint64_t)n,memory_order_relaxed);
    file_perf_report(0);
    runtime_wait_end("file read",wait_start,"bytes",(long long)size);
    if (n<0) { if (file_trace()) printf("File trace: read(fd %d %s, %llu) failed, errno %d\n",fd,traced_path(fd),(unsigned long long)size,errno); return -errno; }
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pread(int fd,void *buffer,uint64_t size,int64_t offset) {
    int device=device_of(fd);
    if (device) { touch_for_write(buffer,size); return device_read(device,buffer,size); }
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    uint64_t prep_start=file_perf_begin();
    touch_for_write(buffer,size);
    file_perf_end(FP_READ_PREP,prep_start);
    const uint64_t wait_start=runtime_wait_begin();
    uint64_t perf_start=file_perf_begin();
    ssize_t n=pread(h,buffer,size,offset);
    if (n<(ssize_t)size) n=finish_short_read(fd,h,buffer,size,offset,n);
    file_perf_end(FP_READ,perf_start);
    if (file_perf_enabled && n>0) atomic_fetch_add_explicit(&file_perf_bytes,(uint64_t)n,memory_order_relaxed);
    file_perf_report(0);
    runtime_wait_end("file pread",wait_start,"bytes",(long long)size);
    if (n<0) { if (file_trace()) printf("File trace: pread(fd %d %s, %llu @%lld) failed, errno %d\n",fd,traced_path(fd),(unsigned long long)size,(long long)offset,errno); return -errno; }
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_write(int fd,const void *buffer,uint64_t size) {
    if (device_of(fd)) return (int64_t)size; /* devices discard writes */
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    ssize_t n=host_write(h,buffer,size);
    if (file_trace()) printf("File trace: write(fd %d %s, %llu) -> %lld\n",fd,traced_path(fd),(unsigned long long)size,(long long)(n<0 ? -errno : n));
    if (n<0) return -errno;
    __atomic_add_fetch(&writes,1,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pwrite(int fd,const void *buffer,uint64_t size,int64_t offset) {
    if (device_of(fd)) return (int64_t)size;
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    ssize_t n=pwrite(h,buffer,size,offset);
    if (file_trace()) printf("File trace: pwrite(fd %d %s, %llu @%lld) -> %lld\n",fd,traced_path(fd),(unsigned long long)size,(long long)offset,(long long)(n<0 ? -errno : n));
    return n<0 ? -errno : n;
}
static int64_t do_lseek(int fd,int64_t offset,int whence) {
    File *f=get(fd);
    if (!f) return -EBADF;
    if (whence<0 || whence>2) return -EINVAL;
    if (f->device) return 0;
    if (f->dir) {
        /* Directory offsets are entry indices for getdirentries. */
        if (whence==0 && offset>=0) { f->position=(size_t)offset; return offset; }
        return -EINVAL;
    }
    int64_t r=host_lseek(f->host,offset,whence);
    return r<0 ? -errno : r;
}
static int64_t do_fstat(int fd,GuestStat *out) {
    if (device_of(fd)) {
        if (!out) return -EFAULT;
        memset(out,0,sizeof(*out)); out->mode=020777; /* S_IFCHR | 0777 */
        return 0;
    }
    int h=host_fd(fd);
    HostStat s;
#ifdef _WIN32
    if (h<0) { /* directory: a listing only, stat the path it was opened with */
        File *f=get(fd); char path[1024];
        if (!f || !f->dir) return -EBADF;
        if (!out) return -EFAULT;
        if (translate(f->path,path,sizeof(path)) || host_stat(path,&s)) return -EBADF;
        convert_stat(&s,out); path_identity(path,out); return 0;
    }
#else
    if (h<0) return -EBADF;
#endif
    if (!out) return -EFAULT;
    if (host_fstat(h,&s)) return -errno;
    convert_stat(&s,out);
#ifdef _WIN32
    file_identity((HANDLE)_get_osfhandle(h),out);
#endif
    return 0;
}
static int64_t do_stat(const char *guest,GuestStat *out) {
    char path[1024]; HostStat s;
    if (device_kind(guest)) {
        if (!out) return -EFAULT;
        memset(out,0,sizeof(*out)); out->mode=020777;
        return 0;
    }
    PathResolution resolution;
    int e=translate_resolved(guest,path,sizeof(path),&resolution);
    if (e) return -e;
    if (!out) return -EFAULT;
    if (resolution.error) return -resolution.error;
    if (host_stat(path,&s)) { e=errno; remember_missing(&resolution,path,e); if (file_trace() && !game_path(guest)) printf("File trace: stat(%s) -> errno %d\n",guest,e); return -e; }
    if (file_trace() && !game_path(guest)) printf("File trace: stat(%s) -> %lld bytes\n",guest,(long long)s.st_size);
    convert_stat(&s,out);
#ifdef _WIN32
    path_identity(path,out);
#endif
    return 0;
}
static int64_t do_getdents(int fd,char *buffer,uint64_t size,int64_t *basep) {
    file_lock();
    File *f=get(fd);
    int64_t result=0;
    if (!f) result=-EBADF;
    else if (!f->dir) result=-EINVAL;
    else if (!buffer) result=-EFAULT;
    else if (size<512) result=-EINVAL;
    else {
        if (basep) *basep=(int64_t)f->position;
        uint64_t written=0;
        while (f->position<f->dir->count) {
            const char *name=f->dir->names+f->dir->offsets[f->position];
            size_t n=strlen(name); if (n>255) n=255;
            uint16_t reclen=(uint16_t)((8+n+1+7)&~(size_t)7);
            if (written+reclen>size) break;
            char *p=buffer+written;
            memset(p,0,reclen);
            uint32_t ino=(uint32_t)f->position+1;
            memcpy(p,&ino,4); memcpy(p+4,&reclen,2);
            p[6]=(char)f->dir->types[f->position]; p[7]=(char)n;
            memcpy(p+8,name,n);
            written+=reclen; ++f->position;
        }
        result=(int64_t)written;
    }
    file_unlock();
    return result;
}
static int64_t path_op(const char *guest,int op,int mode) {
    if (game_path(guest)) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    int r= op==0 ? mkdir(path,mode ? mode : 0755) : op==1 ? rmdir(path) : unlink(path);
    if (r) r=-errno;
    if (file_trace()) printf("File trace: %s(%s) -> %d\n",op==0 ? "mkdir" : op==1 ? "rmdir" : "unlink",guest,r);
    return r;
}
static int64_t do_rename(const char *from,const char *to) {
    if (game_path(from) || game_path(to)) return -EROFS;
    char a[1024],b[1024];
    int e=translate(from,a,sizeof(a));
    if (!e) e=translate(to,b,sizeof(b));
    if (e) return -e;
#ifdef _WIN32
    /* POSIX rename replaces an existing target; the CRT's fails. */
    int r=MoveFileExA(a,b,MOVEFILE_REPLACE_EXISTING) ? 0 : -compat_errno_from_win32(GetLastError());
#else
    int r=rename(a,b) ? -errno : 0;
#endif
    if (file_trace()) printf("File trace: rename(%s, %s) -> %d\n",from,to,r);
    return r;
}
static int64_t do_ftruncate(int fd,int64_t length) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    if (file_trace()) printf("File trace: ftruncate(fd %d %s, %lld)\n",fd,traced_path(fd),(long long)length);
#ifdef _WIN32
    return _chsize_s(h,length) ? -errno : 0;
#else
    return ftruncate(h,length) ? -errno : 0;
#endif
}
static int64_t do_truncate(const char *guest,int64_t length) {
    if (game_path(guest)) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    if (file_trace()) printf("File trace: truncate(%s, %lld)\n",guest,(long long)length);
#ifdef _WIN32
    int h=open(path,O_RDWR|O_BINARY);
    if (h<0) return -errno;
    int r=_chsize_s(h,length) ? -errno : 0;
    close(h);
    return r;
#else
    return truncate(path,length) ? -errno : 0;
#endif
}
static int64_t do_fsync(int fd) { int h=host_fd(fd); if (h<0) return -EBADF; return fsync(h) ? -errno : 0; }
static int64_t do_chmod(const char *guest,uint16_t mode) {
    if (game_path(guest)) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    return chmod(path,mode&07777) ? -errno : 0;
}
/* FreeBSD descriptor/status commands have the same numbers 1..4 as POSIX,
 * but O_NONBLOCK and O_APPEND use different bits from Linux. Host descriptors
 * always stay CLOEXEC; the guest flag is independent of that host safeguard. */
static int64_t do_fcntl(int fd,int command,int64_t argument) {
    file_lock();
    File *f=get(fd);
    if (!f) { file_unlock(); return -EBADF; }
    int64_t result=0;
    if (command==1) result=f->fdflags; /* F_GETFD */
    else if (command==2) f->fdflags=(int)argument&1; /* F_SETFD */
    else if (command==3) result=f->flags&(3|0x4|0x8|0x80); /* F_GETFL */
    else if (command==4) { /* F_SETFL: access mode cannot change. */
#ifndef _WIN32
        if (f->host>=0) {
            int flags=fcntl(f->host,F_GETFL);
            if (flags<0) result=-errno;
            else {
                flags&=~(O_NONBLOCK|O_APPEND);
                if (argument&0x4) flags|=O_NONBLOCK;
                if (argument&0x8) flags|=O_APPEND;
                if (fcntl(f->host,F_SETFL,flags)<0) result=-errno;
            }
        }
#else
        if (!f->device) result=-ENOSYS;
#endif
        if (!result) f->flags=(f->flags&~(0x4|0x8))|((int)argument&(0x4|0x8));
    } else result=-EINVAL;
    file_unlock();
    return result;
}

/* Convention adapters: sceKernel* -> Orbis error codes, POSIX -> -1 + errno. */
static int64_t sce(int64_t r) { return r<0 ? ERR(runtime_guest_errno((int)-r)) : r; }
static int64_t posix(int64_t r) { if (r<0) { *runtime_errno()=runtime_guest_errno((int)-r); return -1; } return r; }
#define SCE(name,params,args) static ABI int64_t sce_##name params { return sce(do_##name args); }
SCE(open,(const char *p,int f,int m),(p,f,m))
SCE(close,(int fd),(fd))
SCE(read,(int fd,void *b,uint64_t n),(fd,b,n))
SCE(pread,(int fd,void *b,uint64_t n,int64_t o),(fd,b,n,o))
SCE(write,(int fd,const void *b,uint64_t n),(fd,b,n))
SCE(pwrite,(int fd,const void *b,uint64_t n,int64_t o),(fd,b,n,o))
SCE(lseek,(int fd,int64_t o,int w),(fd,o,w))
SCE(fstat,(int fd,GuestStat *s),(fd,s))
SCE(stat,(const char *p,GuestStat *s),(p,s))
SCE(getdents,(int fd,char *b,uint64_t n,int64_t *base),(fd,b,n,base))
SCE(rename,(const char *a,const char *b),(a,b))
SCE(ftruncate,(int fd,int64_t l),(fd,l))
SCE(truncate,(const char *p,int64_t l),(p,l))
SCE(fsync,(int fd),(fd))
/* libScePosix file_system.cpp: open/close/stat return s32; byte counts and
 * offsets return s64. These share the kernel path/descriptor handling, with
 * POSIX failures (-1 and guest errno) instead of encoded SCE errors. */
static ABI int32_t posix_open(const char *p,int32_t flags,uint16_t mode) { return (int32_t)posix(do_open(p,flags,mode)); }
static ABI int32_t posix_close(int32_t fd) { return (int32_t)posix(do_close(fd)); }
static ABI int64_t posix_read(int fd,void *b,uint64_t n) { return posix(do_read(fd,b,n)); }
static ABI int64_t posix_write(int fd,const void *b,uint64_t n) { return posix(do_write(fd,b,n)); }
static ABI int64_t posix_lseek(int32_t fd,int64_t offset,int32_t whence) { return posix(do_lseek(fd,offset,whence)); }
static ABI int32_t posix_stat(const char *p,GuestStat *s) { return (int32_t)posix(do_stat(p,s)); }
static ABI int32_t posix_fstat(int32_t fd,GuestStat *s) { return (int32_t)posix(do_fstat(fd,s)); }
static ABI int32_t posix_mkdir(const char *p,uint16_t mode) { return (int32_t)posix(path_op(p,0,mode)); }
static ABI int32_t posix_rmdir(const char *p) { return (int32_t)posix(path_op(p,1,0)); }
static ABI int32_t posix_unlink(const char *p) { return (int32_t)posix(path_op(p,2,0)); }
static ABI int32_t posix_rename(const char *from,const char *to) { return (int32_t)posix(do_rename(from,to)); }
static ABI int32_t posix_fsync(int32_t fd) { return (int32_t)posix(do_fsync(fd)); }
static ABI int32_t posix_ftruncate(int32_t fd,int64_t length) { return (int32_t)posix(do_ftruncate(fd,length)); }
static ABI int32_t posix_chmod(const char *p,uint16_t mode) { return (int32_t)posix(do_chmod(p,mode)); }
static ABI int32_t posix_fcntl(int32_t fd,int32_t command,int64_t argument) { return (int32_t)posix(do_fcntl(fd,command,argument)); }
static ABI int64_t sce_mkdir(const char *p,int m) { return sce(path_op(p,0,m)); }
static ABI int64_t sce_rmdir(const char *p) { return sce(path_op(p,1,0)); }
static ABI int64_t sce_unlink(const char *p) { return sce(path_op(p,2,0)); }

static const RuntimeExport exports[]={
    {"sceKernelOpen",sce_open}, {"open",posix_open},
    {"sceKernelClose",sce_close}, {"close",posix_close},
    {"sceKernelRead",sce_read}, {"_read",posix_read}, {"read",posix_read},
    {"sceKernelPread",sce_pread},
    {"sceKernelWrite",sce_write}, {"_write",posix_write}, {"write",posix_write},
    {"sceKernelPwrite",sce_pwrite},
    {"sceKernelLseek",sce_lseek}, {"lseek",posix_lseek},
    {"sceKernelFstat",sce_fstat}, {"fstat",posix_fstat},
    {"sceKernelStat",sce_stat}, {"stat",posix_stat},
    {"sceKernelGetdirentries",sce_getdents},
    {"sceKernelRename",sce_rename}, {"rename",posix_rename},
    {"sceKernelFtruncate",sce_ftruncate}, {"ftruncate",posix_ftruncate},
    {"sceKernelTruncate",sce_truncate},
    {"sceKernelFsync",sce_fsync}, {"fsync",posix_fsync},
    {"chmod",posix_chmod}, {"fcntl",posix_fcntl},

    {"sceKernelMkdir",sce_mkdir}, {"mkdir",posix_mkdir},
    {"sceKernelRmdir",sce_rmdir}, {"rmdir",posix_rmdir},
    {"sceKernelUnlink",sce_unlink}, {"unlink",posix_unlink},
};
uintptr_t runtime_file_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
/* Host-side helpers for other modules (e.g. SaveData, Fios). */
int64_t runtime_file_open(const char *p,int f,int m) { return do_open(p,f,m); }
int64_t runtime_file_close(int fd) { return do_close(fd); }
int64_t runtime_file_read(int fd,void *b,uint64_t n) { return do_read(fd,b,n); }
int64_t runtime_file_pread(int fd,void *b,uint64_t n,int64_t o) { return do_pread(fd,b,n,o); }
int runtime_file_translate(const char *guest,char *out,size_t size) { return translate(guest,out,size); }
void runtime_file_report(void) {
    printf("Runtime: files opened=%zu, reads=%zu (%llu bytes), writes=%zu, not found=%zu\n",
           opens,reads,(unsigned long long)bytes_read,writes,missing);
    (void)file_perf_begin();
    file_perf_report(1);
}

