/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */
#ifndef UTILPVT_H
#define UTILPVT_H

#include "osiSockExt.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <synchapi.h>
#  include <errhandlingapi.h>
#  include <ws2ipdef.h>
#else
#  include <pthread.h>
#endif

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <sstream>
#include <type_traits>
#include <limits>

#include <stdio.h>

#include <event2/util.h>

#include <compilerDependencies.h>
#include <epicsThread.h>

#include <pvxs/version.h>
#include <pvxs/util.h>

#ifndef EVUTIL_INVALID_SOCKET
#  define EVUTIL_INVALID_SOCKET INVALID_SOCKET
#endif

#ifndef EPICS_ALWAYS_INLINE
#  if __GNUC__
#    define EPICS_ALWAYS_INLINE __inline__ __attribute__((always_inline))
#  else
#    define EPICS_ALWAYS_INLINE inline
#  endif
#endif

#include <epicsThread.h>

// hooks for std::unique_ptr
namespace std {
template<>
struct default_delete<FILE> {
    inline void operator()(FILE* fp) { if(fp) fclose(fp); }
};
}

namespace pvxs {namespace impl {

template<typename T>
struct promote_print { static T op(const T& v) { return v; }};
template<>
struct promote_print<int8_t> { static int op(const char& v) { return v; }};
template<>
struct promote_print<uint8_t> { static unsigned op(const char& v) { return v; }};

PVXS_API
bool inUnitTest();

/* specialization of bad_alloc which notes the location from which
 * the exception originates.
 */
struct PVXS_API loc_bad_alloc final : public std::bad_alloc
{
    loc_bad_alloc(const char *file, int line);
    virtual ~loc_bad_alloc();

    virtual const char* what() const noexcept override final;

private:
    char msg[64];
};
#define BAD_ALLOC() ::pvxs::impl::loc_bad_alloc(__FILE__, __LINE__)

//! in-line string builder (eg. for exception messages)
//! eg. @code throw std::runtime_error(SB()<<"Some message"<<42); @endcode
struct SB {
    std::ostringstream strm;
    SB() {}
    operator std::string() const { return strm.str(); }
    std::string str() const { return strm.str(); }
    template<typename T>
    SB& operator<<(const T& i) { strm<<i; return *this; }
};

PVXS_API
void strDiff(std::ostream& out,
             const char *lhs,
             const char *rhs);

struct threadOnceInfo {
    epicsThreadOnceId id = EPICS_THREAD_ONCE_INIT;
    void (* const fn)();
    bool ok = false;
    explicit constexpr threadOnceInfo(void (*fn)()) :fn(fn) {}
};

PVXS_API
void threadOnce_(threadOnceInfo *info) ;

template<void (*onceFn)()>
void threadOnce() noexcept {
    // global name qualified by onceFn address.
    // effectively replicated for each onceFn
    static threadOnceInfo info{onceFn};
    threadOnce_(&info);
}

namespace idetail {
template <typename I>
struct Range {
    I a, b;

    struct iterator {
        typedef std::forward_iterator_tag iterator_category;
        typedef I         value_type;
        typedef ptrdiff_t difference_type;
        typedef I*        pointer;
        typedef I&        reference;
        I val;
        explicit constexpr iterator(I val) :val(val) {}
        EPICS_ALWAYS_INLINE I operator*() const { return val; }
        EPICS_ALWAYS_INLINE iterator& operator++() { val++; return *this; }
        EPICS_ALWAYS_INLINE iterator operator++(int) { return iterator{val++}; }
        EPICS_ALWAYS_INLINE bool operator==(const iterator& o) const { return val==o.val; }
        EPICS_ALWAYS_INLINE bool operator!=(const iterator& o) const { return val!=o.val; }
    };

    EPICS_ALWAYS_INLINE iterator begin() const { return iterator{a}; }
    EPICS_ALWAYS_INLINE iterator cbegin() const { return begin(); }
    EPICS_ALWAYS_INLINE iterator end() const { return iterator{b}; }
    EPICS_ALWAYS_INLINE iterator cend() const { return end(); }
};
} // namespace idetail

template<typename I>
constexpr idetail::Range<I> range(I end) { return idetail::Range<I>{I(0), end}; }

template<typename I>
constexpr idetail::Range<I> range(I begin, I end) { return idetail::Range<I>{begin, end}; }

template<typename T>
T parseTo(const std::string& s); // not implemented

template<>
PVXS_API
double parseTo<double>(const std::string& s);
template<>
PVXS_API
uint64_t parseTo<uint64_t>(const std::string& s);
template<>
PVXS_API
int64_t parseTo<int64_t>(const std::string& s);

#ifdef _WIN32
#  define RWLOCK_TYPE SRWLOCK
#  define RWLOCK_INIT(PLOCK)    InitializeSRWLock(PLOCK)
#  define RWLOCK_DTOR(PLOCK)    do{(void)(PLOCK);}while(0)
#  define RWLOCK_WLOCK(PLOCK)   AcquireSRWLockExclusive(PLOCK)
#  define RWLOCK_WUNLOCK(PLOCK) ReleaseSRWLockExclusive(PLOCK)
#  define RWLOCK_RLOCK(PLOCK)   AcquireSRWLockShared(PLOCK)
#  define RWLOCK_RUNLOCK(PLOCK) ReleaseSRWLockShared(PLOCK)
#else
#  define RWLOCK_TYPE pthread_rwlock_t
#  define RWLOCK_INIT(PLOCK)    pthread_rwlock_init(PLOCK, nullptr)
#  define RWLOCK_DTOR(PLOCK)    pthread_rwlock_destroy(PLOCK)
#  define RWLOCK_WLOCK(PLOCK)   pthread_rwlock_wrlock(PLOCK)
#  define RWLOCK_WUNLOCK(PLOCK) pthread_rwlock_unlock(PLOCK)
#  define RWLOCK_RLOCK(PLOCK)   pthread_rwlock_rdlock(PLOCK)
#  define RWLOCK_RUNLOCK(PLOCK) pthread_rwlock_unlock(PLOCK)
#endif

class RWLock
{
    RWLOCK_TYPE lock;
public:
    inline RWLock() { RWLOCK_INIT(&lock); }
    inline ~RWLock() { RWLOCK_DTOR(&lock); }

    RWLock(const RWLock&) = delete;
    RWLock(RWLock&&) = delete;
    RWLock& operator=(const RWLock&) = delete;
    RWLock& operator=(RWLock&&) = delete;

    struct UnlockReader {
        inline void operator()(RWLock *plock) { RWLOCK_RUNLOCK(&plock->lock); }
    };
    inline std::unique_ptr<RWLock, UnlockReader> lockReader() {
        RWLOCK_RLOCK(&lock);
        return std::unique_ptr<RWLock, UnlockReader>{this};
    }

    struct UnlockWriter {
        inline void operator()(RWLock *plock) { RWLOCK_WUNLOCK(&plock->lock); }
    };
    inline std::unique_ptr<RWLock, UnlockWriter> lockWriter() {
        RWLOCK_WLOCK(&lock);
        return std::unique_ptr<RWLock, UnlockWriter>{this};
    }
};

#undef RWLOCK_TYPE
#undef RWLOCK_INIT
#undef RWLOCK_DTOR
#undef RWLOCK_WLOCK
#undef RWLOCK_WUNLOCK
#undef RWLOCK_RLOCK
#undef RWLOCK_RUNLOCK

PVXS_API
void osdGetRoles(const std::string& account, std::set<std::string>& roles);

void logger_shutdown();

// std::max() isn't constexpr until c++14 :(
constexpr size_t cmax(size_t A, size_t B) {
    return A>B ? A : B;
}

// gcc 4.9 has aligned_storage but not aligned_union
#if GCC_VERSION && GCC_VERSION<VERSION_INT(4,10,0,0)

template<typename... Types>
struct max_sizeof {
    static const size_t align = 0;
    static const size_t size = 0;
};

template<typename Head, typename... Types>
struct max_sizeof<Head, Types...> {
    static const size_t align = cmax(alignof(Head), max_sizeof<Types...>::align);
    static const size_t size = cmax(sizeof(Head), max_sizeof<Types...>::size);
};

template <size_t Len, typename... Types>
struct aligned_union
{
    using _info = max_sizeof<Types...>;

    typedef typename std::aligned_storage<cmax(Len, _info::size), _info::align>::type type;
};

#else

template <size_t Len, typename... Types>
using aligned_union = std::aligned_union<Len, Types...>;
#endif

} // namespace impl
using namespace impl;

inline
timeval totv(double t)
{
    timeval ret;
    ret.tv_sec = t;
    ret.tv_usec = (t - ret.tv_sec)*1e6;
    return ret;
}

namespace ioc {
void IOCGroupConfigCleanup();
}

//! Scoped restore of std::ostream state (format flags, fill char, and field width)
struct Restore {
    std::ostream& strm;
    std::ios_base::fmtflags pflags;
    std::ostream::char_type pfill;
    std::streamsize pwidth;
    Restore(std::ostream& strm)
        :strm(strm)
        ,pflags(strm.flags())
        ,pfill(strm.fill())
        ,pwidth(strm.width())
    {}
    Restore(const Restore&) = delete;
    Restore& operator=(const Restore&) = delete;
    ~Restore() {
        strm.flags(pflags);
        strm.fill(pfill);
        strm.width(pwidth);
    }
};

PVXS_API
void registerICount(const char* name, std::atomic<size_t>& Cnt);

// Name and Cnt must have global lifetime
template<std::atomic<size_t>& Cnt>
struct InstCounter {
    explicit InstCounter(const char* Name) {
        if(0u==Cnt.fetch_add(1u, std::memory_order_relaxed)) { // first
            registerICount(Name, Cnt);
        }
    }
    InstCounter(const InstCounter&) = delete;
    InstCounter& operator=(const InstCounter&) = delete;
    ~InstCounter() {
        Cnt.fetch_sub(1u, std::memory_order_relaxed);
    }
};

#define INST_COUNTER(KLASS) \
                    static std::atomic<size_t> cnt_ ## KLASS; \
                    InstCounter<cnt_ ## KLASS> instances{#KLASS}
#define DEFINE_INST_COUNTER2(KLASS, NAME) std::atomic<size_t> KLASS::cnt_ ## NAME {0u}
#define DEFINE_INST_COUNTER(KLASS) DEFINE_INST_COUNTER2(KLASS, KLASS)

} // namespace pvxs

#endif // UTILPVT_H
