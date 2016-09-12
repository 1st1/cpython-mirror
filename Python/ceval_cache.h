#include <stdint.h>
#include <inttypes.h>


/* WARNING: This file is full of magic. */


#define OPCACHE_OPCODES(XX)                                     \
    XX(LOAD_GLOBAL)


#define OPCACHE_COLLECT_STATS       0
#define OPCACHE_CALLS_THRESHOLD     1000
#define OPCACHE_MISSES_BEFORE_DEOPT 20


#define OPCACHE_OPCODE_HEAD                                     \
    int8_t optimized;  /* < 0 - deoptimized;                    \
                          = 0 - not yet optimized;              \
                          > 0 - optimized */


typedef struct {
    OPCACHE_OPCODE_HEAD
    uint64_t globals_tag;
    PyObject *ptr;
} _PyCodeObjectCache_LOAD_GLOBAL;


/*
- How to implement cache for a new opcode?

Let's say we want to add cache to MY_OPCODE opcode:

1. Define a `_PyCodeObjectCache_MY_OPCODE` struct.
2. Add `XX(MY_OPCODE);` to OPCACHE_OPCODES macro.
3. Everything else will be handled automatically.
*/


#define _OPCACHE_OPCODE_FIELD(OPCODE)                           \
    uint8_t OPCODE##_size;                                      \
    _PyCodeObjectCache_##OPCODE *OPCODE##_cache;


typedef struct {
    uint8_t *index;
    uint64_t builtins_tag;

    OPCACHE_OPCODES(_OPCACHE_OPCODE_FIELD)
} _PyCodeObjectCache;


static int
init_opcode_cache(PyCodeObject *co)
{
    const uint16_t *instr;
    Py_ssize_t opcodes_num = PyBytes_Size(co->co_code) / 2;
    uint8_t *index = NULL;
    _PyCodeObjectCache *cache;

    cache = (_PyCodeObjectCache*)PyMem_Malloc(sizeof(_PyCodeObjectCache));
    if (cache == NULL) {
        goto error;
    }

#   define _OPCODE_PREPARE(OPCODE)                        \
        uint8_t OPCODE##_size = 0;                        \
        cache->OPCODE##_size = 0;                         \
        cache->OPCODE##_cache = NULL;

    OPCACHE_OPCODES(_OPCODE_PREPARE)
#   undef _OPCODE_PREPARE

    index = (uint8_t *)PyMem_Calloc(opcodes_num, sizeof(uint8_t));
    if (index == NULL) {
        goto error;
    }

    instr = (uint16_t*) PyBytes_AS_STRING(co->co_code);
    for (Py_ssize_t offset = 0; offset < opcodes_num; offset++) {
        uint16_t word = *instr;
        uint8_t opcode = OPCODE(word);
        instr++;

#       define _OPCODE_COUNT(OPCODE)                      \
        if (opcode == OPCODE && OPCODE##_size < 255) {    \
            index[offset] = OPCODE##_size++;              \
        }
        OPCACHE_OPCODES(_OPCODE_COUNT)
#       undef _OPCODE_COUNT
    }

#   define _OPCODE_INIT(OPCODE)                                               \
    if (OPCODE##_size) {                                                      \
        cache->OPCODE##_cache = (_PyCodeObjectCache_##OPCODE*) PyMem_Calloc(  \
            OPCODE##_size, sizeof(_PyCodeObjectCache_##OPCODE));              \
        if (cache->OPCODE##_cache == NULL) {                                  \
            goto error;                                                       \
        }                                                                     \
        cache->OPCODE##_size = OPCODE##_size;                                 \
    }
    OPCACHE_OPCODES(_OPCODE_INIT)
#   undef _OPCODE_INIT

    cache->index = index;
    if (_PyCode_SetExtra((PyObject *)co, 0, cache)) {
        goto error;
    }

    return 0;

error:
    PyMem_Free(index);

    /* Cleanup opcode structs */
#   define _OPCODE_CLEANUP(OPCODE) PyMem_Free(cache->OPCODE##_cache);               \
    OPCACHE_OPCODES(_OPCODE_CLEANUP)
#   undef _OPCODE_CLEANUP

    PyMem_Free(cache);

    return -1;
}


void
_PyEval_FreeOpcodeCache(void *co_extra)
{
    _PyCodeObjectCache *cache = (_PyCodeObjectCache *)co_extra;

#   define _OPCODE_CLEANUP(OPCODE) PyMem_Free(cache->OPCODE##_cache);
    OPCACHE_OPCODES(_OPCODE_CLEANUP)
#   undef _OPCODE_CLEANUP

    PyMem_Free(cache->index);
    PyMem_Free(cache);
}


/* --- Stats --- */


#if OPCACHE_COLLECT_STATS

static uint64_t opcode_stats_opts[255];
static uint64_t opcode_stats_deopts[255];
static uint64_t opcode_stats_hits[255];
static uint64_t opcode_stats_misses[255];


#define _OPCACHE_STATS_OPT(opcode) do {                  \
        opcode_stats_opts[opcode]++;                     \
    } while (0);

#define _OPCACHE_STATS_DEOPT(opcode) do {                \
        opcode_stats_deopts[opcode]++;                   \
    } while (0);

#define OPCACHE_STATS_HIT(opcode) do {                   \
        opcode_stats_hits[opcode]++;                     \
    } while (0);

#define OPCACHE_STATS_MISS(opcode) do {                  \
        opcode_stats_misses[opcode]++;                   \
    } while (0);


static void
opcode_cache_print_stats(void)
{
#   define _OPCODE_PRINT_STAT(OPCODE)                              \
    printf("--- " #OPCODE " ---\n");                               \
    printf("opts:   %" PRIu64 "\n", opcode_stats_opts[OPCODE]);    \
    printf("deopts: %" PRIu64 "\n", opcode_stats_deopts[OPCODE]);  \
    printf("hits:   %" PRIu64 "\n", opcode_stats_hits[OPCODE]);    \
    printf("misses: %" PRIu64 "\n\n", opcode_stats_misses[OPCODE]);

    OPCACHE_OPCODES(_OPCODE_PRINT_STAT)
#   undef _OPCODE_PRINT_STAT
}


#else

#define _OPCACHE_STATS_OPT(opcode)
#define _OPCACHE_STATS_DEOPT(opcode)

#define OPCACHE_STATS_HIT(opcode)
#define OPCACHE_STATS_MISS(opcode)

#endif


#define _OPCACHE_DEFINE_GETTER(OPCODE)                              \
    static inline _PyCodeObjectCache_##OPCODE *                     \
    OPCACHE_GET_##OPCODE(_PyCodeObjectCache *cache, int offset)     \
    {                                                               \
        Py_ssize_t position;                                        \
        _PyCodeObjectCache_##OPCODE *opcache;                       \
        if (cache == NULL) {                                        \
            return NULL;                                            \
        }                                                           \
        position = cache->index[offset];                            \
        assert(cache->OPCODE##_size > position);                    \
        opcache = &cache->OPCODE##_cache[position];                 \
        return opcache->optimized >= 0 ? opcache : NULL;            \
    }
OPCACHE_OPCODES(_OPCACHE_DEFINE_GETTER)
#undef _OPCACHE_DEFINE_GETTER


#define _OPCACHE_DEFINE_MAYBE_DEOPT(OPCODE)                         \
    static inline void                                              \
    OPCACHE_MAYBE_DEOPT_##OPCODE(                                   \
        _PyCodeObjectCache_##OPCODE *opcache)                       \
    {                                                               \
        if (opcache->optimized >= 0) {                              \
            opcache->optimized--;                                   \
            if (opcache->optimized == 0) {                          \
                opcache->optimized = -1;                            \
                _OPCACHE_STATS_DEOPT(OPCODE);                       \
            }                                                       \
        }                                                           \
    }
OPCACHE_OPCODES(_OPCACHE_DEFINE_MAYBE_DEOPT)
#undef _OPCACHE_DEFINE_MAYBE_DEOPT


#define _OPCACHE_DEFINE_UPDATER(OPCODE)                             \
    static inline int                                               \
    OPCACHE_UPDATE_##OPCODE(_PyCodeObjectCache_##OPCODE *opcache)   \
    {                                                               \
        if (opcache == NULL) return -1;                             \
        if (opcache->optimized == 0) { /* first time */             \
            opcache->optimized = OPCACHE_MISSES_BEFORE_DEOPT;       \
            _OPCACHE_STATS_OPT(OPCODE);                             \
        } else {                                                    \
            OPCACHE_MAYBE_DEOPT_##OPCODE(opcache);                  \
        }                                                           \
        return 0;                                                   \
    }
OPCACHE_OPCODES(_OPCACHE_DEFINE_UPDATER)
#undef _OPCACHE_DEFINE_UPDATER
