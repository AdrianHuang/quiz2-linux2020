#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_STR_LEN_BITS (54)
#define MAX_STR_LEN ((1UL << MAX_STR_LEN_BITS) - 1)

#define LARGE_STRING_LEN 256

typedef union {
    /* allow strings up to 15 bytes to stay on the stack
     * use the last byte as a null terminator and to store flags
     * much like fbstring:
     * https://github.com/facebook/folly/blob/master/folly/docs/FBString.md
     */
    char data[16];

    struct {
        uint8_t filler[15],
            /* how many free bytes in this stack allocated string
             * same idea as fbstring
             */
            space_left : 4,
            /* if it is on heap, set to 1 */
            is_ptr : 1, is_large_string : 1, flag2 : 1, flag3 : 1;
    };

    /* heap allocated */
    struct {
        char *ptr;
        /* supports strings up to 2^MAX_STR_LEN_BITS - 1 bytes */
        size_t size : MAX_STR_LEN_BITS,
                      /* capacity is always a power of 2 (unsigned)-1 */
                      capacity : 6;
        /* the last 4 bits are important flags */
    };
} xs;

static inline bool xs_is_ptr(const xs *x)
{
    return x->is_ptr;
}
static inline bool xs_is_large_string(const xs *x)
{
    return x->is_large_string;
}
static inline size_t xs_size(const xs *x)
{
    return xs_is_ptr(x) ? x->size : 15 - x->space_left;
}
static inline char *xs_data(const xs *x)
{
    if (!xs_is_ptr(x))
        return (char *) x->data;

    if (xs_is_large_string(x)) {
        return (char *) (x->ptr + 4);
    }
    return (char *) x->ptr;
}
static inline size_t xs_capacity(const xs *x)
{
    return xs_is_ptr(x) ? ((size_t) 1 << x->capacity) - 1 : 15;
}
static inline void xs_set_ref_count(const xs *x, int val)
{
    *((int *) ((size_t) x->ptr)) = val;
}
static inline void xs_inc_ref_count(const xs *x)
{
    if (xs_is_large_string(x))
        ++(*(int *) ((size_t) x->ptr));
}
static inline int xs_dec_ref_count(const xs *x)
{
    if (!xs_is_large_string(x))
        return 0;
    return --(*(int *) ((size_t) x->ptr));
}

static inline int xs_get_ref_count(const xs *x)
{
    if (!xs_is_large_string(x))
        return 0;
    return *(int *) ((size_t) x->ptr);
}

#define xs_literal_empty() \
    (xs) { .space_left = 15 }

static inline int ilog2(uint32_t n)
{
    return 32 - __builtin_clz(n) - 1;
}

static void xs_allocate_data(xs *x, size_t len, bool reallocate)
{
    /* Medium string */
    if (len < LARGE_STRING_LEN) {
        x->ptr = reallocate ? realloc(x->ptr, (size_t) 1 << x->capacity)
                            : malloc((size_t) 1 << x->capacity);
        return;
    }

    /*
     * Large string
     */
    x->is_large_string = 1;

    /* The extra 4 bytes are used to store the reference count */
    x->ptr = reallocate ? realloc(x->ptr, (size_t)(1 << x->capacity) + 4)
                        : malloc((size_t)(1 << x->capacity) + 4);

    xs_set_ref_count(x, 1);
}

xs *xs_new(xs *x, const void *p)
{
    *x = xs_literal_empty();
    size_t len = strlen(p) + 1;
    if (len > 16) {
        x->capacity = ilog2(len) + 1;
        x->size = len - 1;
        x->is_ptr = true;
        xs_allocate_data(x, x->size, 0);
        memcpy(xs_data(x), p, len);
    } else {
        memcpy(x->data, p, len);
        x->space_left = 15 - (len - 1);
    }
    return x;
}

/* Memory leaks happen if the string is too long but it is still useful for
 * short strings.
 */
#define xs_tmp(x)                                                   \
    ((void) ((struct {                                              \
         _Static_assert(sizeof(x) <= MAX_STR_LEN, "it is too big"); \
         int dummy;                                                 \
     }){1}),                                                        \
     xs_new(&xs_literal_empty(), x))

/* grow up to specified size */
xs *xs_grow(xs *x, size_t len)
{
    char buf[16];

    if (len <= xs_capacity(x))
        return x;

    /* Backup first */
    if (!xs_is_ptr(x))
        memcpy(buf, x->data, 16);

    x->is_ptr = true;
    x->capacity = ilog2(len) + 1;

    if (xs_is_ptr(x)) {
        xs_allocate_data(x, len, 1);
    } else {
        xs_allocate_data(x, len, 0);
        memcpy(xs_data(x), buf, 16);
    }
    return x;
}

static inline xs *xs_newempty(xs *x)
{
    *x = xs_literal_empty();
    return x;
}

static inline xs *xs_free(xs *x)
{
    if (xs_is_ptr(x) && xs_dec_ref_count(x) <= 0)
        free(x->ptr);
    return xs_newempty(x);
}

static bool xs_cow_lazy_copy(xs *x, char **data)
{
    if (xs_get_ref_count(x) <= 1)
        return false;

    /*
     * Lazy copy
     */
    xs_dec_ref_count(x);
    xs_allocate_data(x, x->size, 0);

    if (data) {
        memcpy(xs_data(x), *data, x->size);

        /* Update the newly allocated pointer */
        *data = xs_data(x);
    }
}

xs *xs_concat(xs *string, const xs *prefix, const xs *suffix)
{
    size_t pres = xs_size(prefix), sufs = xs_size(suffix),
           size = xs_size(string), capacity = xs_capacity(string);

    char *pre = xs_data(prefix), *suf = xs_data(suffix),
         *data = xs_data(string);

    xs_cow_lazy_copy(string, &data);

    if (size + pres + sufs <= capacity) {
        memmove(data + pres, data, size);
        memcpy(data, pre, pres);
        memcpy(data + pres + size, suf, sufs + 1);

        if (xs_is_ptr(string))
            string->size = size + pres + sufs;
        else
            string->space_left = 15 - (size + pres + sufs);
    } else {
        xs tmps = xs_literal_empty();
        xs_grow(&tmps, size + pres + sufs);
        char *tmpdata = xs_data(&tmps);
        memcpy(tmpdata + pres, data, size);
        memcpy(tmpdata, pre, pres);
        memcpy(tmpdata + pres + size, suf, sufs + 1);
        xs_free(string);
        *string = tmps;
        string->size = size + pres + sufs;
    }
    return string;
}

xs *xs_trim(xs *x, const char *trimset)
{
    if (!trimset[0])
        return x;

    char *dataptr = xs_data(x), *orig = dataptr;

    if (xs_cow_lazy_copy(x, &dataptr))
        orig = dataptr;

    /* similar to strspn/strpbrk but it operates on binary data */
    uint8_t mask[32] = {0};

#define check_bit(byte) (mask[(uint8_t) byte / 8] & 1 << (uint8_t) byte % 8)
#define set_bit(byte) (mask[(uint8_t) byte / 8] |= 1 << (uint8_t) byte % 8)

    size_t i, slen = xs_size(x), trimlen = strlen(trimset);

    for (i = 0; i < trimlen; i++)
        set_bit(trimset[i]);
    for (i = 0; i < slen; i++)
        if (!check_bit(dataptr[i]))
            break;
    for (; slen > 0; slen--)
        if (!check_bit(dataptr[slen - 1]))
            break;
    dataptr += i;
    slen -= i;

    /* reserved space as a buffer on the heap.
     * Do not reallocate immediately. Instead, reuse it as possible.
     * Do not shrink to in place if < 16 bytes.
     */
    memmove(orig, dataptr, slen);
    /* do not dirty memory unless it is needed */
    if (orig[slen])
        orig[slen] = 0;

    if (xs_is_ptr(x))
        x->size = slen;
    else
        x->space_left = 15 - slen;
    return x;
#undef check_bit
#undef set_bit
}

void xs_copy(xs *dest, xs *src)
{
    *dest = *src;

    /*
     * src string from stack: No need to invoke memcpy() since the data
     * has been copied from the statement '*dest = *src'
     */
    if (!xs_is_ptr(src))
        return;

    if (xs_is_large_string(src)) {
        /* CoW: simply increase the reference count */
        xs_inc_ref_count(src);
    } else {
        /* Medium string */
        dest->ptr = malloc((size_t) 1 << src->capacity);
        memcpy(dest->ptr, src->ptr, src->size);
    }
}

#define NR_TESTS 10000
#define TEST_MAX_STRING (4 * 1024 * 1024 - 1)

#define CONCAT_STRING_IDX_START 0
#define CONCAT_STRING_IDX_END 99
#define CONCAT_STRING_TIMES \
    (CONCAT_STRING_IDX_END - CONCAT_STRING_IDX_START + 1)

#define TRIM_STRING_IDX_START 100
#define TRIM_STRING_IDX_END 199
#define TRIM_STRING_TIMES (TRIM_STRING_IDX_END - TRIM_STRING_IDX_START + 1)

enum {
    SMALL_STRING,
    MEDIUM_STRING,
    LARGE_STRING,

    NR_STRING_TYPE
};

static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
static const char str_type_desc[NR_STRING_TYPE][8] = {"Small", "Medium",
                                                      "Large"};
static char random_string[NR_STRING_TYPE][TEST_MAX_STRING];
static xs backup_string[NR_TESTS];

static void init_random_string(uint8_t *buf, uint32_t type)
{
    size_t length_array[] = {15, 255, TEST_MAX_STRING};
    size_t len = length_array[type];
    size_t n;

    /* Bookmark for trimming string. */
    buf[0] = '@';
    for (n = 1; n < len - 1; n++)
        buf[n] = charset[rand() % (sizeof charset - 1)];

    /* Bookmark for trimming string. */
    buf[n] = '#';
    buf[n + 1] = 0;
}

static void run_concat_test(xs *orig_string, xs *backup_string)
{
    int j;

    xs prefix = *xs_tmp("((("), suffix = *xs_tmp(")))");

    printf("concatenate copied string for %d sets, ", CONCAT_STRING_TIMES);
    for (j = CONCAT_STRING_IDX_START; j <= CONCAT_STRING_IDX_END; j++)
        xs_concat(backup_string + j, &prefix, &suffix);

    for (j = CONCAT_STRING_IDX_START; j <= CONCAT_STRING_IDX_END; j++) {
        if (xs_is_large_string(backup_string + j) &&
            xs_get_ref_count(backup_string + j) != 1) {
            printf("[Error]: backup_string[%d] ref. count != 1\n", j);
        }
    }
    printf("ref. count: %d\n", xs_get_ref_count(orig_string));
}

static void run_trim_test(xs *orig_string, xs *backup_string)
{
    int j;

    printf("trim copied string for another %d sets, ", TRIM_STRING_TIMES);
    for (j = TRIM_STRING_IDX_START; j <= TRIM_STRING_IDX_END; j++)
        xs_trim(backup_string + j, "@#");

    for (j = TRIM_STRING_IDX_START; j <= TRIM_STRING_IDX_END; j++) {
        if (xs_is_large_string(backup_string + j) &&
            xs_get_ref_count(backup_string + j) != 1) {
            printf("[Error]: backup_string[%d] ref. count != 1\n", j);
        }
    }
    printf("ref. count: %d\n", xs_get_ref_count(orig_string));
}

static void run_string_strategy_test(void)
{
    xs string;
    int i, j;

    srand((unsigned int) (time(NULL)));

    for (i = SMALL_STRING; i < NR_STRING_TYPE; i++) {
        init_random_string(random_string[i], i);

        string = *xs_tmp(random_string[i]);

        printf("-------------- %s string --------------\n", str_type_desc[i]);
        printf("copy string %d times, ", NR_TESTS);

        for (j = 0; j < NR_TESTS; j++)
            xs_copy(&backup_string[j], &string);
        printf("ref. count: %d\n", xs_get_ref_count(&string));

        run_concat_test(&string, backup_string);
        run_trim_test(&string, backup_string);
        printf("------------------------------------------\n\n");

        xs_free(&string);
        for (j = 0; j < NR_TESTS; j++)
            xs_free(&backup_string[j]);
    }
}

static void func_test(void)
{
    xs string = *xs_tmp("\n foobarbar \n\n\n");
    xs_trim(&string, "\n ");
    printf("[%s] : %2zu\n", xs_data(&string), xs_size(&string));

    xs prefix = *xs_tmp("((("), suffix = *xs_tmp(")))");
    xs_concat(&string, &prefix, &suffix);
    printf("[%s] : %2zu\n", xs_data(&string), xs_size(&string));
}

int main()
{
    func_test();
    run_string_strategy_test();
    return 0;
}
