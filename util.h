#ifndef fooutilhfoo
#define fooutilhfoo

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define new(t, n) ((t*) malloc((n) * sizeof(t)))
#define new0(t, n) ((t*) calloc((n), sizeof(t)))

#define newa(t, n) ((t*) alloca((n) * sizeof(t)))

#define XCONCATENATE(x, y) x ## y
#define CONCATENATE(x, y) XCONCATENATE(x, y)

#define UNIQ_T(x, uniq) CONCATENATE(__unique_prefix_, CONCATENATE(x, uniq))
#define UNIQ __COUNTER__

#undef MAX
#define MAX(a, b) __MAX(UNIQ, (a), UNIQ, (b))
#define __MAX(aq, a, bq, b)                             \
        __extension__ ({                                \
                const typeof(a) UNIQ_T(A, aq) = (a);    \
                const typeof(b) UNIQ_T(B, bq) = (b);    \
                UNIQ_T(A,aq) > UNIQ_T(B,bq) ? UNIQ_T(A,aq) : UNIQ_T(B,bq); \
        })

#undef MIN
#define MIN(a, b) __MIN(UNIQ, (a), UNIQ, (b))
#define __MIN(aq, a, bq, b)                             \
        __extension__ ({                                \
                const typeof(a) UNIQ_T(A, aq) = (a);    \
                const typeof(b) UNIQ_T(B, bq) = (b);    \
                UNIQ_T(A,aq) < UNIQ_T(B,bq) ? UNIQ_T(A,aq) : UNIQ_T(B,bq); \
        })

static inline uint64_t timespec_to_nsec(const struct timespec t) {
        return (uint64_t) t.tv_sec * UINT64_C(1000000000) + (uint64_t) t.tv_nsec;
}

static inline struct timespec nsec_to_timespec(uint64_t u) {

        return (struct timespec) {
                        .tv_sec = u / UINT64_C(1000000000),
                        .tv_nsec = u % UINT64_C(1000000000)
                        };
}

static inline int log_oom(void) {
        fprintf(stderr, "Out of memory\n");
        return -ENOMEM;
}

int loop_write(int fd, const void *p, size_t l);
ssize_t loop_read(int fd, void *p, size_t l);

int skip_bytes(int fd, uint64_t bytes);

char *endswith(const char *p, const char *suffix);

static inline void* mfree(void* p) {
        free(p);
        return NULL;
}

#define assert_se(x)                                                 \
        do {                                                         \
                if (!(x)) {                                          \
                        fputs("Assertion failed: " #x "\n", stderr); \
                        abort();                                     \
                }                                                    \
        } while(false)

static inline int safe_close(int fd) {
        if (fd >= 0)
                assert_se(close(fd) >= 0 || errno != EBADF);
        return -1;
}

typedef uint16_t le16_t;
typedef uint32_t le32_t;
typedef uint64_t le64_t;

static inline uint64_t read_le64(const void *p) {
        uint64_t u;
        assert(p);
        memcpy(&u, p, sizeof(uint64_t));
        return le64toh(u);
}

static inline uint32_t read_le32(const void *p) {
        uint32_t u;
        assert(p);
        memcpy(&u, p, sizeof(uint32_t));
        return le32toh(u);
}

static inline uint16_t read_le16(const void *p) {
        uint16_t u;
        assert(p);
        memcpy(&u, p, sizeof(uint16_t));
        return le16toh(u);
}

static inline void write_le64(void *p, uint64_t u) {
        assert(p);
        u = htole64(u);
        memcpy(p, &u, sizeof(uint64_t));
}

static inline void write_le32(void *p, uint32_t u) {
        assert(p);
        u = htole32(u);
        memcpy(p, &u, sizeof(uint32_t));
}

static inline void write_le16(void *p, uint16_t u) {
        assert(p);
        u = htole16(u);
        memcpy(p, &u, sizeof(uint16_t));
}

static inline void* memdup(const void *p, size_t size) {
        void *q;

        q = malloc(size);
        if (!q)
                return NULL;

        memcpy(q, p, size);
        return q;
}

int dev_urandom(void *p, size_t n);

static inline uint64_t random_u64(void) {
        uint64_t u;
        dev_urandom(&u, sizeof(u));
        return u;
}

#define _sentinel_ __attribute__ ((sentinel))
#define _unused_ __attribute__ ((unused))

#define CASE_F(X) case X:
#define CASE_F_1(CASE, X) CASE_F(X)
#define CASE_F_2(CASE, X, ...)  CASE(X) CASE_F_1(CASE, __VA_ARGS__)
#define CASE_F_3(CASE, X, ...)  CASE(X) CASE_F_2(CASE, __VA_ARGS__)
#define CASE_F_4(CASE, X, ...)  CASE(X) CASE_F_3(CASE, __VA_ARGS__)
#define CASE_F_5(CASE, X, ...)  CASE(X) CASE_F_4(CASE, __VA_ARGS__)
#define CASE_F_6(CASE, X, ...)  CASE(X) CASE_F_5(CASE, __VA_ARGS__)
#define CASE_F_7(CASE, X, ...)  CASE(X) CASE_F_6(CASE, __VA_ARGS__)
#define CASE_F_8(CASE, X, ...)  CASE(X) CASE_F_7(CASE, __VA_ARGS__)
#define CASE_F_9(CASE, X, ...)  CASE(X) CASE_F_8(CASE, __VA_ARGS__)
#define CASE_F_10(CASE, X, ...) CASE(X) CASE_F_9(CASE, __VA_ARGS__)
#define CASE_F_11(CASE, X, ...) CASE(X) CASE_F_10(CASE, __VA_ARGS__)
#define CASE_F_12(CASE, X, ...) CASE(X) CASE_F_11(CASE, __VA_ARGS__)
#define CASE_F_13(CASE, X, ...) CASE(X) CASE_F_12(CASE, __VA_ARGS__)
#define CASE_F_14(CASE, X, ...) CASE(X) CASE_F_13(CASE, __VA_ARGS__)
#define CASE_F_15(CASE, X, ...) CASE(X) CASE_F_14(CASE, __VA_ARGS__)
#define CASE_F_16(CASE, X, ...) CASE(X) CASE_F_15(CASE, __VA_ARGS__)
#define CASE_F_17(CASE, X, ...) CASE(X) CASE_F_16(CASE, __VA_ARGS__)
#define CASE_F_18(CASE, X, ...) CASE(X) CASE_F_17(CASE, __VA_ARGS__)
#define CASE_F_19(CASE, X, ...) CASE(X) CASE_F_18(CASE, __VA_ARGS__)
#define CASE_F_20(CASE, X, ...) CASE(X) CASE_F_19(CASE, __VA_ARGS__)

#define GET_CASE_F(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,_17,_18,_19,_20,NAME,...) NAME
#define FOR_EACH_MAKE_CASE(...) \
        GET_CASE_F(__VA_ARGS__,CASE_F_20,CASE_F_19,CASE_F_18,CASE_F_17,CASE_F_16,CASE_F_15,CASE_F_14,CASE_F_13,CASE_F_12,CASE_F_11, \
                               CASE_F_10,CASE_F_9,CASE_F_8,CASE_F_7,CASE_F_6,CASE_F_5,CASE_F_4,CASE_F_3,CASE_F_2,CASE_F_1) \
                   (CASE_F,__VA_ARGS__)

#define IN_SET(x, ...)                          \
        ({                                      \
                bool _found = false;            \
                /* If the build breaks in the line below, you need to extend the case macros */ \
                static _unused_ char _static_assert__macros_need_to_be_extended[20 - sizeof((int[]){__VA_ARGS__})/sizeof(int)]; \
                switch(x) {                     \
                FOR_EACH_MAKE_CASE(__VA_ARGS__) \
                        _found = true;          \
                        break;                  \
                default:                        \
                        break;                  \
                }                               \
                _found;                         \
        })


char *hexmem(const void *p, size_t l);

bool filename_is_valid(const char *p);
int tempfn_random(const char *p, char **ret);

static inline bool isempty(const char *p) {
        return !p || !p[0];
}

#define streq(a,b) (strcmp((a),(b)) == 0)

static inline bool streq_ptr(const char *a, const char *b) {
        if (!a && !b)
                return true;
        if (!a || !b)
                return false;

        return streq(a, b);
}

static inline const char *strna(const char *p) {
        return p ?: "n/a";
}

void hexdump(FILE *f, const void *p, size_t s);
char* dirname_malloc(const char *path);

char *strjoin_real(const char *x, ...) _sentinel_;
#define strjoin(a, ...) strjoin_real((a), __VA_ARGS__, NULL)

#define LS_FORMAT_MODE_MAX (1+3+3+3+1)
char* ls_format_mode(mode_t m, char ret[LS_FORMAT_MODE_MAX]);

#endif
