#include <stdint.h>


/* WARNING: This file is full of magic. */


#define OPCACHE_OPCODES(XX)                                     \
    XX(LOAD_GLOBAL)


#define OPCACHE_CALLS_THRESHOLD 1000


#define OPCACHE_OPCODE_HEAD                                     \
    int8_t optimized;  /* < 0 - deoptimized;                    \
                          = 0 - not yet optimized;              \
                          > 0 - optimized */


typedef struct {
    OPCACHE_OPCODE_HEAD
    uint64_t globals_tag;
    uint64_t builtins_tag;
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

    OPCACHE_OPCODES(_OPCACHE_OPCODE_FIELD)
} _PyCodeObjectCache;


#define _OPCACHE_DEFINE_GETTER(OPCODE)                              \
    static inline _PyCodeObjectCache_##OPCODE *                     \
    OPCACHE_GET_##OPCODE(_PyCodeObjectCache *cache, int offset)     \
    {                                                               \
        Py_ssize_t position;                                        \
        if (cache == NULL) {                                        \
            return NULL;                                            \
        }                                                           \
        position = cache->index[offset];                            \
        assert(cache->OPCODE##_size > position);                    \
        return &cache->OPCODE##_cache[position];                    \
    }
OPCACHE_OPCODES(_OPCACHE_DEFINE_GETTER)
#undef _OPCACHE_DEFINE_GETTER


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
