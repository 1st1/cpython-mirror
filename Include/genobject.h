
/* Generator object interface */

#ifndef Py_LIMITED_API
#ifndef Py_GENOBJECT_H
#define Py_GENOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

struct _frame; /* Avoid including frameobject.h */

/* _PyGenObject_HEAD defines the initial segment of generator
   and coroutine objects. */
#define _PyGenObject_HEAD                                                   \
    PyObject_HEAD                                                           \
    /* The gi_ prefix is intended to remind of generator-iterator. */       \
    /* Note: gi_frame can be NULL if the generator is "finished" */         \
    struct _frame *gi_frame;                                                \
    /* True if generator is being executed. */                              \
    char gi_running;                                                        \
    /* The code object backing the generator */                             \
    PyObject *gi_code;                                                      \
    /* List of weak reference. */                                           \
    PyObject *gi_weakreflist;                                               \
    /* Name of the generator. */                                            \
    PyObject *gi_name;                                                      \
    /* Qualified name of the generator. */                                  \
    PyObject *gi_qualname;

typedef struct {
    _PyGenObject_HEAD
} PyGenObject;

PyAPI_DATA(PyTypeObject) PyGen_Type;

#define PyGen_Check(op) PyObject_TypeCheck(op, &PyGen_Type)
#define PyGen_CheckExact(op) (Py_TYPE(op) == &PyGen_Type)

PyAPI_FUNC(PyObject *) PyGen_New(struct _frame *);
PyAPI_FUNC(PyObject *) PyGen_NewWithQualName(struct _frame *,
    PyObject *name, PyObject *qualname);
PyAPI_FUNC(int) PyGen_NeedsFinalizing(PyGenObject *);
PyAPI_FUNC(int) _PyGen_FetchStopIterationValue(PyObject **);
PyObject *_PyGen_Send(PyGenObject *, PyObject *);
PyAPI_FUNC(void) _PyGen_Finalize(PyObject *self);

#ifndef Py_LIMITED_API
typedef struct {
    _PyGenObject_HEAD
} PyCoroObject;

PyAPI_DATA(PyTypeObject) PyCoro_Type;
PyAPI_DATA(PyTypeObject) _PyCoroWrapper_Type;

#define PyCoro_CheckExact(op) (Py_TYPE(op) == &PyCoro_Type)
PyObject *_PyCoro_GetAwaitableIter(PyObject *o);
PyAPI_FUNC(PyObject *) PyCoro_New(struct _frame *,
    PyObject *name, PyObject *qualname);
#endif

#undef _PyGenObject_HEAD

#ifdef __cplusplus
}
#endif
#endif /* !Py_GENOBJECT_H */
#endif /* Py_LIMITED_API */
