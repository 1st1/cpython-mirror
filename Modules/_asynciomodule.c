#include "Python.h"
#include "structmember.h"


/* identifiers used from some functions */
_Py_IDENTIFIER(call_soon);
_Py_IDENTIFIER(cancel);
_Py_IDENTIFIER(send);
_Py_IDENTIFIER(throw);
_Py_IDENTIFIER(add_done_callback);


/* State of the _asyncio module */
static PyObject *all_tasks;
static PyDictObject *current_tasks;
static PyObject *traceback_extract_stack;
static PyObject *asyncio_get_event_loop;
static PyObject *asyncio_future_repr_info_func;
static PyObject *asyncio_task_repr_info_func;
static PyObject *asyncio_task_get_stack_func;
static PyObject *asyncio_task_print_stack_func;
static PyObject *asyncio_InvalidStateError;
static PyObject *asyncio_CancelledError;
static PyObject *inspect_isgenerator;


/* Get FutureIter from Future */
static PyObject* new_future_iter(PyObject *fut);


typedef enum {
    STATE_PENDING,
    STATE_CANCELLED,
    STATE_FINISHED
} fut_state;

#define FutureObj_HEAD(prefix)                                              \
    PyObject_HEAD                                                           \
    PyObject *prefix##_loop;                                                \
    PyObject *prefix##_callbacks;                                           \
    PyObject *prefix##_exception;                                           \
    PyObject *prefix##_result;                                              \
    PyObject *prefix##_source_tb;                                           \
    fut_state prefix##_state;                                               \
    int prefix##_log_tb;                                                    \
    int prefix##_blocking;                                                  \
    PyObject *dict;                                                         \
    PyObject *prefix##_weakreflist;

typedef struct {
    FutureObj_HEAD(fut)
} FutureObj;

typedef struct {
    FutureObj_HEAD(task)
    PyObject *task_fut_waiter;
    PyObject *task_coro;
    int task_must_cancel;
    int task_log_destroy_pending;
} TaskObj;

typedef struct {
    PyObject_HEAD
    TaskObj *sw_task;
    PyObject *sw_arg;
} TaskSendMethWrapper;

typedef struct {
    PyObject_HEAD
    TaskObj *ww_task;
} TaskWakeupMethWrapper;



static int
_schedule_callbacks(FutureObj *fut)
{
    Py_ssize_t len;
    PyObject* iters;
    int i;

    if (fut->fut_callbacks == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "NULL callbacks");
        return -1;
    }

    len = PyList_GET_SIZE(fut->fut_callbacks);
    if (len == 0) {
        return 0;
    }

    iters = PyList_GetSlice(fut->fut_callbacks, 0, len);
    if (iters == NULL) {
        return -1;
    }
    if (PyList_SetSlice(fut->fut_callbacks, 0, len, NULL) < 0) {
        Py_DECREF(iters);
        return -1;
    }

    for (i = 0; i < len; i++) {
        PyObject *handle = NULL;
        PyObject *cb = PyList_GET_ITEM(iters, i);

        handle = _PyObject_CallMethodId(
            fut->fut_loop, &PyId_call_soon, "OO", cb, fut, NULL);

        if (handle == NULL) {
            Py_DECREF(iters);
            return -1;
        }
        else {
            Py_DECREF(handle);
        }
    }

    Py_DECREF(iters);
    return 0;
}

static int
_init_future(FutureObj *fut, PyObject *loop)
{
    PyObject *res = NULL;
    _Py_IDENTIFIER(get_debug);

    if (loop == NULL || loop == Py_None) {
        loop = PyObject_CallObject(asyncio_get_event_loop, NULL);
        if (loop == NULL) {
            return -1;
        }
    }
    else {
        Py_INCREF(loop);
    }
    Py_CLEAR(fut->fut_loop);
    fut->fut_loop = loop;

    res = _PyObject_CallMethodId(fut->fut_loop, &PyId_get_debug, "()", NULL);
    if (res == NULL) {
        return -1;
    }
    if (PyObject_IsTrue(res)) {
        Py_CLEAR(res);
        fut->fut_source_tb = PyObject_CallObject(traceback_extract_stack, NULL);
        if (fut->fut_source_tb == NULL) {
            return -1;
        }
    }
    else {
        Py_CLEAR(res);
    }

    fut->fut_callbacks = PyList_New(0);
    if (fut->fut_callbacks == NULL) {
        return -1;
    }

    return 0;

}

static int
FutureObj_init(FutureObj *fut, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"loop", NULL};
    PyObject *loop = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$O", kwlist, &loop)) {
        return -1;
    }

    return _init_future(fut, loop);
}

static int
FutureObj_clear(FutureObj *fut)
{
    Py_CLEAR(fut->fut_loop);
    Py_CLEAR(fut->fut_callbacks);
    Py_CLEAR(fut->fut_result);
    Py_CLEAR(fut->fut_exception);
    Py_CLEAR(fut->fut_source_tb);
    Py_CLEAR(fut->dict);
    return 0;
}

static int
FutureObj_traverse(FutureObj *fut, visitproc visit, void *arg)
{
    Py_VISIT(fut->fut_loop);
    Py_VISIT(fut->fut_callbacks);
    Py_VISIT(fut->fut_result);
    Py_VISIT(fut->fut_exception);
    Py_VISIT(fut->fut_source_tb);
    Py_VISIT(fut->dict);
    return 0;
}

static int
get_future_result(FutureObj *fut, PyObject **result)
{
    PyObject *exc;

    if (fut->fut_state == STATE_CANCELLED) {
        exc = _PyObject_CallNoArg(asyncio_CancelledError);
        if (exc == NULL) {
            return -1;
        }
        *result = exc;
        return 1;
    }

    if (fut->fut_state != STATE_FINISHED) {
        PyObject *msg = PyUnicode_FromString("Result is not ready.");
        if (msg == NULL) {
            return -1;
        }

        exc = _PyObject_CallArg1(asyncio_InvalidStateError, msg);
        Py_DECREF(msg);
        if (exc == NULL) {
            return -1;
        }

        *result = exc;
        return 1;
    }

    fut->fut_log_tb = 0;
    if (fut->fut_exception != NULL) {
        Py_INCREF(fut->fut_exception);
        *result = fut->fut_exception;
        return 1;
    }

    Py_INCREF(fut->fut_result);
    *result = fut->fut_result;
    return 0;
}

PyDoc_STRVAR(pydoc_result,
    "Return the result this future represents.\n"
    "\n"
    "If the future has been cancelled, raises CancelledError.  If the\n"
    "future's result isn't yet available, raises InvalidStateError.  If\n"
    "the future is done and has an exception set, this exception is raised."
);

static PyObject *
FutureObj_result(FutureObj *fut, PyObject *arg)
{
    PyObject *result;
    int res = get_future_result(fut, &result);

    if (res == -1) {
        return NULL;
    }

    if (res == 0) {
        return result;
    }

    assert(res == 1);

    PyErr_SetObject(PyExceptionInstance_Class(result), result);
    Py_DECREF(result);
    return NULL;
}

PyDoc_STRVAR(pydoc_exception,
    "Return the exception that was set on this future.\n"
    "\n"
    "The exception (or None if no exception was set) is returned only if\n"
    "the future is done.  If the future has been cancelled, raises\n"
    "CancelledError.  If the future isn't done yet, raises\n"
    "InvalidStateError."
);

static PyObject *
FutureObj_exception(FutureObj *fut, PyObject *arg)
{
    if (fut->fut_state == STATE_CANCELLED) {
        PyErr_SetString(asyncio_CancelledError, "");
        return NULL;
    }

    if (fut->fut_state != STATE_FINISHED) {
        PyErr_SetString(asyncio_InvalidStateError, "Result is not ready.");
        return NULL;
    }

    if (fut->fut_exception != NULL) {
        fut->fut_log_tb = 0;
        Py_INCREF(fut->fut_exception);
        return fut->fut_exception;
    }

    Py_RETURN_NONE;
}

PyDoc_STRVAR(pydoc_set_result,
    "Mark the future done and set its result.\n"
    "\n"
    "If the future is already done when this method is called, raises\n"
    "InvalidStateError."
);

static PyObject *
FutureObj_set_result(FutureObj *fut, PyObject *res)
{
    if (fut->fut_state != STATE_PENDING) {
        PyErr_SetString(asyncio_InvalidStateError, "invalid state");
        return NULL;
    }

    Py_INCREF(res);
    fut->fut_result = res;
    fut->fut_state = STATE_FINISHED;

    if (_schedule_callbacks(fut) == -1) {
        return NULL;
    }
    Py_RETURN_NONE;
}

PyDoc_STRVAR(pydoc_set_exception,
    "Mark the future done and set an exception.\n"
    "\n"
    "If the future is already done when this method is called, raises\n"
    "InvalidStateError."
);

static PyObject *
FutureObj_set_exception(FutureObj *fut, PyObject *exc)
{
    PyObject *exc_val = NULL;

    if (fut->fut_state != STATE_PENDING) {
        PyErr_SetString(asyncio_InvalidStateError, "invalid state");
        return NULL;
    }

    if (PyExceptionClass_Check(exc)) {
        exc_val = PyObject_CallObject(exc, NULL);
        if (exc_val == NULL) {
            return NULL;
        }
    }
    else {
        exc_val = exc;
        Py_INCREF(exc_val);
    }
    if (!PyExceptionInstance_Check(exc_val)) {
        Py_DECREF(exc_val);
        PyErr_SetString(PyExc_TypeError, "invalid exception object");
        return NULL;
    }
    if ((PyObject*)Py_TYPE(exc_val) == PyExc_StopIteration) {
        Py_DECREF(exc_val);
        PyErr_SetString(PyExc_TypeError,
                        "StopIteration interacts badly with generators "
                        "and cannot be raised into a Future");
        return NULL;
    }

    fut->fut_exception = exc_val;
    fut->fut_state = STATE_FINISHED;

    if (_schedule_callbacks(fut) == -1) {
        return NULL;
    }

    fut->fut_log_tb = 1;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(pydoc_add_done_callback,
    "Add a callback to be run when the future becomes done.\n"
    "\n"
    "The callback is called with a single argument - the future object. If\n"
    "the future is already done when this is called, the callback is\n"
    "scheduled with call_soon.";
);

static PyObject *
FutureObj_add_done_callback(FutureObj *fut, PyObject *arg)
{
    if (fut->fut_state != STATE_PENDING) {
        PyObject *handle = _PyObject_CallMethodId(
            fut->fut_loop, &PyId_call_soon, "OO", arg, fut, NULL);

        if (handle == NULL) {
            return NULL;
        }
        else {
            Py_DECREF(handle);
        }
    }
    else {
        int err = PyList_Append(fut->fut_callbacks, arg);
        if (err != 0) {
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

PyDoc_STRVAR(pydoc_remove_done_callback,
    "Remove all instances of a callback from the \"call when done\" list.\n"
    "\n"
    "Returns the number of callbacks removed."
);

static PyObject *
FutureObj_remove_done_callback(FutureObj *fut, PyObject *arg)
{
    PyObject *newlist;
    Py_ssize_t len, i, j=0;

    len = PyList_GET_SIZE(fut->fut_callbacks);
    if (len == 0) {
        return PyLong_FromSsize_t(0);
    }

    newlist = PyList_New(len);
    if (newlist == NULL) {
        return NULL;
    }

    for (i = 0; i < len; i++) {
        int ret;
        PyObject *item = PyList_GET_ITEM(fut->fut_callbacks, i);

        if ((ret = PyObject_RichCompareBool(arg, item, Py_EQ)) < 0) {
            goto fail;
        }
        if (ret == 0) {
            Py_INCREF(item);
            PyList_SET_ITEM(newlist, j, item);
            j++;
        }
    }

    if (PyList_SetSlice(newlist, j, len, NULL) < 0) {
        goto fail;
    }
    if (PyList_SetSlice(fut->fut_callbacks, 0, len, newlist) < 0) {
        goto fail;
    }
    Py_DECREF(newlist);
    return PyLong_FromSsize_t(len - j);

fail:
    Py_DECREF(newlist);
    return NULL;
}

PyDoc_STRVAR(pydoc_cancel,
    "Cancel the future and schedule callbacks.\n"
    "\n"
    "If the future is already done or cancelled, return False.  Otherwise,\n"
    "change the future's state to cancelled, schedule the callbacks and\n"
    "return True."
);

static PyObject *
FutureObj_cancel(FutureObj *fut, PyObject *arg)
{
    if (fut->fut_state != STATE_PENDING) {
        Py_RETURN_FALSE;
    }
    fut->fut_state = STATE_CANCELLED;

    if (_schedule_callbacks(fut) == -1) {
        return NULL;
    }

    Py_RETURN_TRUE;
}

PyDoc_STRVAR(pydoc_cancelled, "Return True if the future was cancelled.");

static PyObject *
FutureObj_cancelled(FutureObj *fut, PyObject *arg)
{
    if (fut->fut_state == STATE_CANCELLED) {
        Py_RETURN_TRUE;
    }
    else {
        Py_RETURN_FALSE;
    }
}

PyDoc_STRVAR(pydoc_done,
    "Return True if the future is done.\n"
    "\n"
    "Done means either that a result / exception are available, or that the\n"
    "future was cancelled."
);

static PyObject *
FutureObj_done(FutureObj *fut, PyObject *arg)
{
    if (fut->fut_state == STATE_PENDING) {
        Py_RETURN_FALSE;
    }
    else {
        Py_RETURN_TRUE;
    }
}

static PyObject *
FutureObj_get_blocking(FutureObj *fut)
{
    if (fut->fut_blocking) {
        Py_RETURN_TRUE;
    }
    else {
        Py_RETURN_FALSE;
    }
}

static int
FutureObj_set_blocking(FutureObj *fut, PyObject *val)
{
    int is_true = PyObject_IsTrue(val);
    if (is_true < 0) {
        return -1;
    }
    fut->fut_blocking = is_true;
    return 0;
}

static PyObject *
FutureObj_get_log_traceback(FutureObj *fut)
{
    if (fut->fut_log_tb) {
        Py_RETURN_TRUE;
    }
    else {
        Py_RETURN_FALSE;
    }
}

static PyObject *
FutureObj_get_loop(FutureObj *fut)
{
    if (fut->fut_loop == NULL) {
        Py_RETURN_NONE;
    }
    Py_INCREF(fut->fut_loop);
    return fut->fut_loop;
}

static PyObject *
FutureObj_get_callbacks(FutureObj *fut)
{
    if (fut->fut_callbacks == NULL) {
        Py_RETURN_NONE;
    }
    Py_INCREF(fut->fut_callbacks);
    return fut->fut_callbacks;
}

static PyObject *
FutureObj_get_result(FutureObj *fut)
{
    if (fut->fut_result == NULL) {
        Py_RETURN_NONE;
    }
    Py_INCREF(fut->fut_result);
    return fut->fut_result;
}

static PyObject *
FutureObj_get_exception(FutureObj *fut)
{
    if (fut->fut_exception == NULL) {
        Py_RETURN_NONE;
    }
    Py_INCREF(fut->fut_exception);
    return fut->fut_exception;
}

static PyObject *
FutureObj_get_source_traceback(FutureObj *fut)
{
    if (fut->fut_source_tb == NULL) {
        Py_RETURN_NONE;
    }
    Py_INCREF(fut->fut_source_tb);
    return fut->fut_source_tb;
}

static PyObject *
FutureObj_get_state(FutureObj *fut)
{
    _Py_IDENTIFIER(PENDING);
    _Py_IDENTIFIER(CANCELLED);
    _Py_IDENTIFIER(FINISHED);
    PyObject *ret = NULL;

    switch (fut->fut_state) {
    case STATE_PENDING:
        ret = _PyUnicode_FromId(&PyId_PENDING);
        break;
    case STATE_CANCELLED:
        ret = _PyUnicode_FromId(&PyId_CANCELLED);
        break;
    case STATE_FINISHED:
        ret = _PyUnicode_FromId(&PyId_FINISHED);
        break;
    default:
        assert (0);
    }
    Py_INCREF(ret);
    return ret;
}

static PyObject*
FutureObj__repr_info(FutureObj *fut)
{
    return PyObject_CallFunctionObjArgs(
        asyncio_future_repr_info_func, fut, NULL);
}

static PyObject *
FutureObj_repr(FutureObj *fut)
{
    _Py_IDENTIFIER(_repr_info);

    PyObject *_repr_info = _PyUnicode_FromId(&PyId__repr_info);  // borrowed
    if (_repr_info == NULL) {
        return NULL;
    }

    PyObject *rinfo = PyObject_CallMethodObjArgs((PyObject*)fut, _repr_info,
                                                 NULL);
    if (rinfo == NULL) {
        return NULL;
    }

    PyObject *sp = PyUnicode_FromString(" ");
    if (sp == NULL) {
        Py_DECREF(rinfo);
        return NULL;
    }

    PyObject *rinfo_s = PyUnicode_Join(sp, rinfo);
    Py_DECREF(sp);
    Py_DECREF(rinfo);
    if (rinfo_s == NULL) {
        return NULL;
    }

    PyObject *rstr = NULL;
    PyObject *type_name = PyObject_GetAttrString((PyObject*)Py_TYPE(fut),
                                                 "__name__");
    if (type_name != NULL) {
        rstr = PyUnicode_FromFormat("<%S %S>", type_name, rinfo_s);
        Py_DECREF(type_name);
    }
    Py_DECREF(rinfo_s);
    return rstr;
}

static void
FutureObj_finalize(FutureObj *fut)
{
    _Py_IDENTIFIER(call_exception_handler);
    _Py_IDENTIFIER(message);
    _Py_IDENTIFIER(exception);
    _Py_IDENTIFIER(future);
    _Py_IDENTIFIER(source_traceback);

    if (!fut->fut_log_tb) {
        return;
    }
    assert(fut->fut_exception != NULL);
    fut->fut_log_tb = 0;;

    PyObject *error_type, *error_value, *error_traceback;
    /* Save the current exception, if any. */
    PyErr_Fetch(&error_type, &error_value, &error_traceback);

    PyObject *context = NULL;
    PyObject *type_name = NULL;
    PyObject *message = NULL;
    PyObject *func = NULL;
    PyObject *res = NULL;

    context = PyDict_New();
    if (context == NULL) {
        goto finally;
    }

    type_name = PyObject_GetAttrString((PyObject*)Py_TYPE(fut), "__name__");
    if (type_name == NULL) {
        goto finally;
    }

    message = PyUnicode_FromFormat(
        "%S exception was never retrieved", type_name);
    if (message == NULL) {
        goto finally;
    }

    if (_PyDict_SetItemId(context, &PyId_message, message) < 0 ||
        _PyDict_SetItemId(context, &PyId_exception, fut->fut_exception) < 0 ||
        _PyDict_SetItemId(context, &PyId_future, (PyObject*)fut) < 0) {
        goto finally;
    }
    if (fut->fut_source_tb != NULL) {
        if (_PyDict_SetItemId(context, &PyId_source_traceback,
                              fut->fut_source_tb) < 0) {
            goto finally;
        }
    }

    func = _PyObject_GetAttrId(fut->fut_loop, &PyId_call_exception_handler);
    if (func != NULL) {
        res = _PyObject_CallArg1(func, context);
        if (res == NULL) {
            PyErr_WriteUnraisable(func);
        }
    }

finally:
    Py_CLEAR(context);
    Py_CLEAR(type_name);
    Py_CLEAR(message);
    Py_CLEAR(func);
    Py_CLEAR(res);

    /* Restore the saved exception. */
    PyErr_Restore(error_type, error_value, error_traceback);
}


static PyAsyncMethods FutureType_as_async = {
    (unaryfunc)new_future_iter,         /* am_await */
    0,                                  /* am_aiter */
    0                                   /* am_anext */
};

static PyMethodDef FutureType_methods[] = {
    {"_repr_info", (PyCFunction)FutureObj__repr_info, METH_NOARGS, NULL},
    {"add_done_callback",
        (PyCFunction)FutureObj_add_done_callback,
        METH_O, pydoc_add_done_callback},
    {"remove_done_callback",
        (PyCFunction)FutureObj_remove_done_callback,
        METH_O, pydoc_remove_done_callback},
    {"set_result",
        (PyCFunction)FutureObj_set_result, METH_O, pydoc_set_result},
    {"set_exception",
        (PyCFunction)FutureObj_set_exception, METH_O, pydoc_set_exception},
    {"cancel", (PyCFunction)FutureObj_cancel, METH_NOARGS, pydoc_cancel},
    {"cancelled",
        (PyCFunction)FutureObj_cancelled, METH_NOARGS, pydoc_cancelled},
    {"done", (PyCFunction)FutureObj_done, METH_NOARGS, pydoc_done},
    {"result", (PyCFunction)FutureObj_result, METH_NOARGS, pydoc_result},
    {"exception",
        (PyCFunction)FutureObj_exception, METH_NOARGS, pydoc_exception},
    {NULL, NULL}        /* Sentinel */
};

static PyGetSetDef FutureType_getsetlist[] = {
    {"_state", (getter)FutureObj_get_state, NULL, NULL},
    {"_asyncio_future_blocking", (getter)FutureObj_get_blocking,
                                 (setter)FutureObj_set_blocking, NULL},
    {"_loop", (getter)FutureObj_get_loop, NULL, NULL},
    {"_callbacks", (getter)FutureObj_get_callbacks, NULL, NULL},
    {"_result", (getter)FutureObj_get_result, NULL, NULL},
    {"_exception", (getter)FutureObj_get_exception, NULL, NULL},
    {"_log_traceback", (getter)FutureObj_get_log_traceback, NULL, NULL},
    {"_source_traceback", (getter)FutureObj_get_source_traceback, NULL, NULL},
    {NULL} /* Sentinel */
};

static void FutureObj_dealloc(PyObject *self);

static PyTypeObject FutureType = {
    PyVarObject_HEAD_INIT(0, 0)
    "_asyncio.Future",
    sizeof(FutureObj),                       /* tp_basicsize */
    .tp_dealloc = FutureObj_dealloc,
    .tp_as_async = &FutureType_as_async,
    .tp_repr = (reprfunc)FutureObj_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE
        | Py_TPFLAGS_HAVE_FINALIZE,
    .tp_doc = "Fast asyncio.Future implementation.",
    .tp_traverse = (traverseproc)FutureObj_traverse,
    .tp_clear = (inquiry)FutureObj_clear,
    .tp_weaklistoffset = offsetof(FutureObj, fut_weakreflist),
    .tp_iter = (getiterfunc)new_future_iter,
    .tp_methods = FutureType_methods,
    .tp_getset = FutureType_getsetlist,
    .tp_dictoffset = offsetof(FutureObj, dict),
    .tp_init = (initproc)FutureObj_init,
    .tp_new = PyType_GenericNew,
    .tp_finalize = (destructor)FutureObj_finalize,
};

#define Future_CheckExact(obj) (Py_TYPE(obj) == &FutureType)

static void
FutureObj_dealloc(PyObject *self)
{
    FutureObj *fut = (FutureObj *)self;

    if (Future_CheckExact(fut)) {
        /* When fut is subclass of Future, finalizer is called from
         * subtype_dealloc.
         */
        if (PyObject_CallFinalizerFromDealloc(self) < 0) {
            // resurrected.
            return;
        }
    }

    if (fut->fut_weakreflist != NULL) {
        PyObject_ClearWeakRefs(self);
    }

    (void)FutureObj_clear(fut);
    Py_TYPE(fut)->tp_free(fut);
}


/*********************** Future Iterator **************************/

typedef struct {
    PyObject_HEAD
    FutureObj *future;
} futureiterobject;

static void
FutureIter_dealloc(futureiterobject *it)
{
    PyObject_GC_UnTrack(it);
    Py_XDECREF(it->future);
    PyObject_GC_Del(it);
}

static PyObject *
FutureIter_iternext(futureiterobject *it)
{
    PyObject *res;
    FutureObj *fut = it->future;

    if (fut == NULL) {
        return NULL;
    }

    if (fut->fut_state == STATE_PENDING) {
        if (!fut->fut_blocking) {
            fut->fut_blocking = 1;
            Py_INCREF(fut);
            return (PyObject *)fut;
        }
        PyErr_Format(PyExc_AssertionError,
                     "yield from wasn't used with future");
        return NULL;
    }

    res = FutureObj_result(fut, NULL);
    if (res != NULL) {
        /* The result of the Future is not an exception.

           We construct an exception instance manually with
           PyObject_CallFunctionObjArgs and pass it to PyErr_SetObject
           (similarly to what genobject.c does).

           We do this to handle a situation when "res" is a tuple, in which
           case PyErr_SetObject would set the value of StopIteration to
           the first element of the tuple.

           (See PyErr_SetObject/_PyErr_CreateException code for details.)
        */
        PyObject *e = PyObject_CallFunctionObjArgs(
            PyExc_StopIteration, res, NULL);
        Py_DECREF(res);
        if (e == NULL) {
            return NULL;
        }
        PyErr_SetObject(PyExc_StopIteration, e);
        Py_DECREF(e);
    }

    it->future = NULL;
    Py_DECREF(fut);
    return NULL;
}

static PyObject *
FutureIter_send(futureiterobject *self, PyObject *unused)
{
    /* Future.__iter__ doesn't care about values that are pushed to the
     * generator, it just returns "self.result().
     */
    return FutureIter_iternext(self);
}

static PyObject *
FutureIter_throw(futureiterobject *self, PyObject *args)
{
    PyObject *type=NULL, *val=NULL, *tb=NULL;
    if (!PyArg_ParseTuple(args, "O|OO", &type, &val, &tb))
        return NULL;

    if (val == Py_None) {
        val = NULL;
    }
    if (tb == Py_None) {
        tb = NULL;
    }

    Py_CLEAR(self->future);

    if (tb != NULL) {
        PyErr_Restore(type, val, tb);
    }
    else if (val != NULL) {
        PyErr_SetObject(type, val);
    }
    else {
        if (PyExceptionClass_Check(type)) {
            val = PyObject_CallObject(type, NULL);
        }
        else {
            val = type;
            assert (PyExceptionInstance_Check(val));
            type = (PyObject*)Py_TYPE(val);
            assert (PyExceptionClass_Check(type));
        }
        PyErr_SetObject(type, val);
    }
    return FutureIter_iternext(self);
}

static PyObject *
FutureIter_close(futureiterobject *self, PyObject *arg)
{
    Py_CLEAR(self->future);
    Py_RETURN_NONE;
}

static int
FutureIter_traverse(futureiterobject *it, visitproc visit, void *arg)
{
    Py_VISIT(it->future);
    return 0;
}

static PyMethodDef FutureIter_methods[] = {
    {"send",  (PyCFunction)FutureIter_send, METH_O, NULL},
    {"throw", (PyCFunction)FutureIter_throw, METH_VARARGS, NULL},
    {"close", (PyCFunction)FutureIter_close, METH_NOARGS, NULL},
    {NULL, NULL}        /* Sentinel */
};

static PyTypeObject FutureIterType = {
    PyVarObject_HEAD_INIT(0, 0)
    "_asyncio.FutureIter",
    sizeof(futureiterobject),                /* tp_basicsize */
    0,                                       /* tp_itemsize */
    (destructor)FutureIter_dealloc,          /* tp_dealloc */
    0,                                       /* tp_print */
    0,                                       /* tp_getattr */
    0,                                       /* tp_setattr */
    0,                                       /* tp_as_async */
    0,                                       /* tp_repr */
    0,                                       /* tp_as_number */
    0,                                       /* tp_as_sequence */
    0,                                       /* tp_as_mapping */
    0,                                       /* tp_hash */
    0,                                       /* tp_call */
    0,                                       /* tp_str */
    PyObject_GenericGetAttr,                 /* tp_getattro */
    0,                                       /* tp_setattro */
    0,                                       /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /* tp_flags */
    0,                                       /* tp_doc */
    (traverseproc)FutureIter_traverse,       /* tp_traverse */
    0,                                       /* tp_clear */
    0,                                       /* tp_richcompare */
    0,                                       /* tp_weaklistoffset */
    PyObject_SelfIter,                       /* tp_iter */
    (iternextfunc)FutureIter_iternext,       /* tp_iternext */
    FutureIter_methods,                      /* tp_methods */
    0,                                       /* tp_members */
};

static PyObject *
new_future_iter(PyObject *fut)
{
    futureiterobject *it;

    if (!PyObject_TypeCheck(fut, &FutureType)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    it = PyObject_GC_New(futureiterobject, &FutureIterType);
    if (it == NULL) {
        return NULL;
    }
    Py_INCREF(fut);
    it->future = (FutureObj*)fut;
    PyObject_GC_Track(it);
    return (PyObject*)it;
}


/*********************** Task **************************/

static PyObject * task_wakeup(TaskObj *, PyObject *);
static PyObject * task_step(TaskObj *, PyObject *);

/* ----- Task._step wrapper */

static int
TaskSendMethWrapper_clear(TaskSendMethWrapper *o)
{
    Py_CLEAR(o->sw_task);
    Py_CLEAR(o->sw_arg);
    return 0;
}

static void
TaskSendMethWrapper_dealloc(TaskSendMethWrapper *o)
{
    _PyObject_GC_UNTRACK(o);
    (void)TaskSendMethWrapper_clear(o);
    Py_TYPE(o)->tp_free(o);
}

static PyObject *
TaskSendMethWrapper_call(TaskSendMethWrapper *o,
                         PyObject *args, PyObject *kwds)
{
    return task_step(o->sw_task, o->sw_arg);
}

static int
TaskSendMethWrapper_traverse(TaskSendMethWrapper *o,
                             visitproc visit, void *arg)
{
    Py_VISIT(o->sw_task);
    Py_VISIT(o->sw_arg);
    return 0;
}

static PyObject *
TaskSendMethWrapper_get___self__(TaskSendMethWrapper *o)
{
    if (o->sw_task) {
        Py_INCREF(o->sw_task);
        return (PyObject*)o->sw_task;
    }
    Py_RETURN_NONE;
}

static PyGetSetDef TaskSendMethWrapper_getsetlist[] = {
    {"__self__", (getter)TaskSendMethWrapper_get___self__, NULL, NULL},
    {NULL} /* Sentinel */
};

PyTypeObject TaskSendMethWrapper_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "TaskSendMethWrapper",
    .tp_basicsize = sizeof(TaskSendMethWrapper),
    .tp_itemsize = 0,
    .tp_getset = TaskSendMethWrapper_getsetlist,
    .tp_dealloc = (destructor)TaskSendMethWrapper_dealloc,
    .tp_call = (ternaryfunc)TaskSendMethWrapper_call,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)TaskSendMethWrapper_traverse,
    .tp_clear = (inquiry)TaskSendMethWrapper_clear,
};

static PyObject *
TaskSendMethWrapper_new(TaskObj *task, PyObject *arg)
{
    TaskSendMethWrapper *o;
    o = PyObject_GC_New(TaskSendMethWrapper, &TaskSendMethWrapper_Type);
    if (o == NULL) {
        return NULL;
    }

    Py_INCREF(task);
    o->sw_task = task;

    Py_XINCREF(arg);
    o->sw_arg = arg;

    _PyObject_GC_TRACK(o);
    return (PyObject*) o;
}

/* ----- Task._wakeup wrapper */

static PyObject *
TaskWakeupMethWrapper_call(TaskWakeupMethWrapper *o,
                           PyObject *args, PyObject *kwds)
{
    PyObject *fut;

    if (!PyArg_ParseTuple(args, "O|", &fut)) {
        return NULL;
    }

    return task_wakeup(o->ww_task, fut);
}

static int
TaskWakeupMethWrapper_clear(TaskWakeupMethWrapper *o)
{
    Py_CLEAR(o->ww_task);
    return 0;
}

static int
TaskWakeupMethWrapper_traverse(TaskWakeupMethWrapper *o,
                               visitproc visit, void *arg)
{
    Py_VISIT(o->ww_task);
    return 0;
}

static void
TaskWakeupMethWrapper_dealloc(TaskWakeupMethWrapper *o)
{
    PyObject_GC_UnTrack(o);
    (void)TaskWakeupMethWrapper_clear(o);
    Py_TYPE(o)->tp_free(o);
}

PyTypeObject TaskWakeupMethWrapper_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "TaskWakeupMethWrapper",
    .tp_basicsize = sizeof(TaskWakeupMethWrapper),
    .tp_itemsize = 0,
    .tp_dealloc = (destructor)TaskWakeupMethWrapper_dealloc,
    .tp_call = (ternaryfunc)TaskWakeupMethWrapper_call,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)TaskWakeupMethWrapper_traverse,
    .tp_clear = (inquiry)TaskWakeupMethWrapper_clear,
};

static PyObject *
TaskWakeupMethWrapper_new(TaskObj *task)
{
    TaskWakeupMethWrapper *o;
    o = PyObject_GC_New(TaskWakeupMethWrapper, &TaskWakeupMethWrapper_Type);
    if (o == NULL) {
        return NULL;
    }

    Py_INCREF(task);
    o->ww_task = task;

    PyObject_GC_Track(o);
    return (PyObject*) o;
}

/* ----- Task */

static int
TaskObj_call_step_soon(TaskObj *task, PyObject *arg)
{
    PyObject *handle;
    PyObject *cb = TaskSendMethWrapper_new(task, arg);

    if (cb == NULL) {
        return -1;
    }

    handle = _PyObject_CallMethodId(
        task->task_loop, &PyId_call_soon, "O", cb, NULL);

    Py_DECREF(cb);

    if (handle == NULL) {
        return -1;
    }

    Py_DECREF(handle);
    return 0;
}

static int
TaskObj_init(TaskObj *task, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"coro", "loop", NULL};
    PyObject *loop = NULL;
    PyObject *coro = NULL;
    PyObject *res;
    _Py_IDENTIFIER(add);

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "O|$O", kwlist, &coro, &loop))
    {
        return -1;
    }

    if (_init_future((FutureObj*)task, loop)) {
        return -1;
    }

    task->task_fut_waiter = NULL;
    task->task_must_cancel = 0;
    task->task_log_destroy_pending = 1;

    Py_INCREF(coro);
    task->task_coro = coro;

    if (TaskObj_call_step_soon(task, NULL)) {
        return -1;
    }

    res = _PyObject_CallMethodId(all_tasks, &PyId_add, "O", task, NULL);
    if (res == NULL) {
        return -1;
    }
    Py_DECREF(res);

    return 0;
}

static int
TaskObj_clear(TaskObj *task)
{
    (void)FutureObj_clear((FutureObj*) task);
    Py_CLEAR(task->task_coro);
    Py_CLEAR(task->task_fut_waiter);
    return 0;
}

static int
TaskObj_traverse(TaskObj *task, visitproc visit, void *arg)
{
    Py_VISIT(task->task_coro);
    Py_VISIT(task->task_fut_waiter);
    (void)FutureObj_traverse((FutureObj*) task, visit, arg);
    return 0;
}

static PyObject *
TaskObj_get_log_destroy_pending(TaskObj *task)
{
    if (task->task_log_destroy_pending) {
        Py_RETURN_TRUE;
    }
    else {
        Py_RETURN_FALSE;
    }
}

static int
TaskObj_set_log_destroy_pending(TaskObj *task, PyObject *val)
{
    int is_true = PyObject_IsTrue(val);
    if (is_true < 0) {
        return -1;
    }
    task->task_log_destroy_pending = is_true;
    return 0;
}

static PyObject *
TaskObj_get_must_cancel(TaskObj *task)
{
    if (task->task_must_cancel) {
        Py_RETURN_TRUE;
    }
    else {
        Py_RETURN_FALSE;
    }
}

static PyObject *
TaskObj_get_coro(TaskObj *task)
{
    if (task->task_coro) {
        Py_INCREF(task->task_coro);
        return task->task_coro;
    }

    Py_RETURN_NONE;
}

static PyObject *
TaskObj_get_fut_waiter(TaskObj *task)
{
    if (task->task_fut_waiter) {
        Py_INCREF(task->task_fut_waiter);
        return task->task_fut_waiter;
    }

    Py_RETURN_NONE;
}

static PyObject *
TaskClass_current_task(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"loop", NULL};
    PyObject *loop = NULL;
    PyObject *res;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &loop)) {
        return NULL;
    }

    if (loop == NULL) {
        loop = PyObject_CallObject(asyncio_get_event_loop, NULL);
        if (loop == NULL) {
            return NULL;
        }

        res = PyDict_GetItem((PyObject*)current_tasks, loop);
        Py_DECREF(loop);
    }
    else {
        res = PyDict_GetItem((PyObject*)current_tasks, loop);
    }

    if (res == NULL) {
        Py_RETURN_NONE;
    }
    else {
        Py_INCREF(res);
        return res;
    }
}

static PyObject *
TaskClass_all_tasks_impl(PyObject *loop)
{
    PyObject *task;
    PyObject *task_loop;
    PyObject *set;
    PyObject *iter;

    set = PySet_New(NULL);
    if (set == NULL) {
        return NULL;
    }

    iter = PyObject_GetIter(all_tasks);
    if (iter == NULL) {
        goto fail;
    }

    while ((task = PyIter_Next(iter))) {
        task_loop = PyObject_GetAttrString(task, "_loop");
        if (task_loop == NULL) {
            Py_DECREF(task);
            goto fail;
        }
        if (task_loop == loop) {
            if (PySet_Add(set, task) == -1) {
                Py_DECREF(task);
                goto fail;
            }
        }
        Py_DECREF(task_loop);
        Py_DECREF(task);
    }

    Py_DECREF(iter);
    return set;

fail:
    Py_XDECREF(set);
    Py_XDECREF(iter);
    return NULL;
}

static PyObject *
TaskClass_all_tasks(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"loop", NULL};
    PyObject *loop = NULL;
    PyObject *res;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &loop)) {
        return NULL;
    }

    if (loop == NULL) {
        loop = PyObject_CallObject(asyncio_get_event_loop, NULL);
        if (loop == NULL) {
            return NULL;
        }

        res = TaskClass_all_tasks_impl(loop);
        Py_DECREF(loop);
    }
    else {
        res = TaskClass_all_tasks_impl(loop);
    }

    return res;
}

static PyObject *
TaskObj__repr_info(FutureObj *fut)
{
    return PyObject_CallFunctionObjArgs(
        asyncio_task_repr_info_func, fut, NULL);
}

static PyObject *
TaskObj_cancel(TaskObj *task)
{
    if (task->task_state != STATE_PENDING) {
        Py_RETURN_FALSE;
    }

    if (task->task_fut_waiter) {
        PyObject *res;
        int is_true;

        res = _PyObject_CallMethodId(
            task->task_fut_waiter, &PyId_cancel, NULL);
        if (res == NULL) {
            return NULL;
        }

        is_true = PyObject_IsTrue(res);
        Py_DECREF(res);
        if (is_true < 0) {
            return NULL;
        }

        if (is_true) {
            Py_RETURN_TRUE;
        }
    }

    task->task_must_cancel = 1;
    Py_RETURN_TRUE;
}

static PyObject *
TaskObj_step(TaskObj *task, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"exc", NULL};
    PyObject *exc = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &exc)) {
        return NULL;
    }

    return task_step(task, exc);
}

static PyObject *
TaskObj_wakeup(TaskObj *task, PyObject *arg)
{
    return task_wakeup(task, arg);
}

static PyObject *
TaskObj_get_stack(TaskObj *task, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"limit", NULL};
    PyObject *limit = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$O", kwlist, &limit)) {
        return NULL;
    }

    if (limit == NULL) {
        limit = Py_None;
    }

    return PyObject_CallFunctionObjArgs(
        asyncio_task_get_stack_func, task, limit, NULL);
}

static PyObject *
TaskObj_print_stack(TaskObj *task, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"limit", "file", NULL};
    PyObject *limit = NULL;
    PyObject *file = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|$OO", kwlist, &limit, &file))
    {
        return NULL;
    }

    if (limit == NULL) {
        limit = Py_None;
    }

    if (file == NULL) {
        file = Py_None;
    }

    return PyObject_CallFunctionObjArgs(
        asyncio_task_print_stack_func, task, limit, file, NULL);
}

static void
TaskObj_finalize(TaskObj *task)
{
    _Py_IDENTIFIER(call_exception_handler);
    _Py_IDENTIFIER(task);
    _Py_IDENTIFIER(message);
    _Py_IDENTIFIER(source_traceback);

    PyObject *message = NULL;
    PyObject *context = NULL;
    PyObject *func = NULL;
    PyObject *res = NULL;

    PyObject *error_type, *error_value, *error_traceback;

    if (task->task_state != STATE_PENDING || !task->task_log_destroy_pending) {
        goto done;
    }

    /* Save the current exception, if any. */
    PyErr_Fetch(&error_type, &error_value, &error_traceback);

    context = PyDict_New();
    if (context == NULL) {
        goto finally;
    }

    message = PyUnicode_FromString("Task was destroyed but it is pending!");
    if (message == NULL) {
        goto finally;
    }

    if (_PyDict_SetItemId(context, &PyId_message, message) < 0 ||
        _PyDict_SetItemId(context, &PyId_task, (PyObject*)task) < 0)
    {
        goto finally;
    }

    if (task->task_source_tb != NULL) {
        if (_PyDict_SetItemId(context, &PyId_source_traceback,
                              task->task_source_tb) < 0)
        {
            goto finally;
        }
    }

    func = _PyObject_GetAttrId(task->task_loop, &PyId_call_exception_handler);
    if (func != NULL) {
        res = _PyObject_CallArg1(func, context);
        if (res == NULL) {
            PyErr_WriteUnraisable(func);
        }
    }

finally:
    Py_CLEAR(context);
    Py_CLEAR(message);
    Py_CLEAR(func);
    Py_CLEAR(res);

    /* Restore the saved exception. */
    PyErr_Restore(error_type, error_value, error_traceback);

done:
    FutureObj_finalize((FutureObj*)task);
}

static void TaskObj_dealloc(PyObject *);  /* Needs Task_CheckExact */

static PyMethodDef TaskType_methods[] = {
    {"_repr_info", (PyCFunction)TaskObj__repr_info, METH_NOARGS, NULL},
    {"add_done_callback",
        (PyCFunction)FutureObj_add_done_callback,
        METH_O, pydoc_add_done_callback},
    {"remove_done_callback",
        (PyCFunction)FutureObj_remove_done_callback,
        METH_O, pydoc_remove_done_callback},
    {"set_result",
        (PyCFunction)FutureObj_set_result, METH_O, pydoc_set_result},
    {"set_exception",
        (PyCFunction)FutureObj_set_exception, METH_O, pydoc_set_exception},
    {"cancel", (PyCFunction)TaskObj_cancel, METH_NOARGS, pydoc_cancel},
    {"cancelled",
        (PyCFunction)FutureObj_cancelled, METH_NOARGS, pydoc_cancelled},
    {"done", (PyCFunction)FutureObj_done, METH_NOARGS, pydoc_done},
    {"result", (PyCFunction)FutureObj_result, METH_NOARGS, pydoc_result},
    {"exception",
        (PyCFunction)FutureObj_exception, METH_NOARGS, pydoc_exception},
    {"current_task",
        (PyCFunction)TaskClass_current_task,
        METH_VARARGS | METH_KEYWORDS | METH_CLASS,
        NULL},
    {"all_tasks",
        (PyCFunction)TaskClass_all_tasks,
        METH_VARARGS | METH_KEYWORDS | METH_CLASS,
        NULL},
    {"get_stack",
        (PyCFunction)TaskObj_get_stack,
        METH_VARARGS | METH_KEYWORDS,
        NULL},
    {"print_stack",
        (PyCFunction)TaskObj_print_stack,
        METH_VARARGS | METH_KEYWORDS,
        NULL},
    {"_step",
        (PyCFunction)TaskObj_step,
        METH_VARARGS | METH_KEYWORDS,
        NULL},
    {"_wakeup",
        (PyCFunction)TaskObj_wakeup,
        METH_O,
        NULL},
    {NULL, NULL}        /* Sentinel */
};

static PyGetSetDef TaskType_getsetlist[] = {
    {"_state", (getter)FutureObj_get_state, NULL, NULL},
    {"_asyncio_future_blocking", (getter)FutureObj_get_blocking,
                                 (setter)FutureObj_set_blocking, NULL},
    {"_loop", (getter)FutureObj_get_loop, NULL, NULL},
    {"_callbacks", (getter)FutureObj_get_callbacks, NULL, NULL},
    {"_result", (getter)FutureObj_get_result, NULL, NULL},
    {"_exception", (getter)FutureObj_get_exception, NULL, NULL},
    {"_log_traceback", (getter)FutureObj_get_log_traceback, NULL, NULL},
    {"_source_traceback", (getter)FutureObj_get_source_traceback, NULL, NULL},
    {"_log_destroy_pending", (getter)TaskObj_get_log_destroy_pending,
                             (setter)TaskObj_set_log_destroy_pending, NULL},
    {"_must_cancel", (getter)TaskObj_get_must_cancel, NULL, NULL},
    {"_coro", (getter)TaskObj_get_coro, NULL, NULL},
    {"_fut_waiter", (getter)TaskObj_get_fut_waiter, NULL, NULL},
    {NULL} /* Sentinel */
};

static PyTypeObject TaskType = {
    PyVarObject_HEAD_INIT(0, 0)
    "_asyncio.Task",
    sizeof(TaskObj),                       /* tp_basicsize */
    .tp_base = &FutureType,
    .tp_dealloc = TaskObj_dealloc,
    .tp_as_async = &FutureType_as_async,
    .tp_repr = (reprfunc)FutureObj_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE
        | Py_TPFLAGS_HAVE_FINALIZE,
    .tp_doc = "A coroutine wrapped in a Future.",
    .tp_traverse = (traverseproc)TaskObj_traverse,
    .tp_clear = (inquiry)TaskObj_clear,
    .tp_weaklistoffset = offsetof(TaskObj, task_weakreflist),
    .tp_iter = (getiterfunc)new_future_iter,
    .tp_methods = TaskType_methods,
    .tp_getset = TaskType_getsetlist,
    .tp_dictoffset = offsetof(TaskObj, dict),
    .tp_init = (initproc)TaskObj_init,
    .tp_new = PyType_GenericNew,
    .tp_finalize = (destructor)TaskObj_finalize,
};

#define Task_CheckExact(obj) (Py_TYPE(obj) == &TaskType)

static void
TaskObj_dealloc(PyObject *self)
{
    TaskObj *task = (TaskObj *)self;

    if (Task_CheckExact(self)) {
        /* When fut is subclass of Task, finalizer is called from
         * subtype_dealloc.
         */
        if (PyObject_CallFinalizerFromDealloc(self) < 0) {
            // resurrected.
            return;
        }
    }

    if (task->task_weakreflist != NULL) {
        PyObject_ClearWeakRefs(self);
    }

    (void)TaskObj_clear(task);
    Py_TYPE(task)->tp_free(task);
}

static PyObject *
TaskObj_SetErrorSoon(TaskObj *task, PyObject *et, const char *format, ...)
{
    PyObject* msg;

    va_list vargs;
#ifdef HAVE_STDARG_PROTOTYPES
    va_start(vargs, format);
#else
    va_start(vargs);
#endif
    msg = PyUnicode_FromFormatV(format, vargs);
    va_end(vargs);

    if (msg == NULL) {
        return NULL;
    }

    PyObject *e = PyObject_CallFunctionObjArgs(et, msg, NULL);
    Py_DECREF(msg);
    if (e == NULL) {
        return NULL;
    }

    if (TaskObj_call_step_soon(task, e) == -1) {
        Py_DECREF(e);
        return NULL;
    }

    Py_DECREF(e);
    Py_RETURN_NONE;
}

static PyObject *
task_step_impl(TaskObj *task, PyObject *exc)
{
    int res;
    int clear_exc = 0;
    PyObject *result = NULL;
    PyObject *coro = task->task_coro;
    PyObject *o;

    if (task->task_state != STATE_PENDING) {
        PyErr_Format(PyExc_AssertionError,
                     "_step(): already done: %R %R",
                     task,
                     exc ? exc : Py_None);
        goto fail;
    }

    if (task->task_must_cancel) {
        assert(exc != Py_None);

        if (exc) {
            /* Check if exc is a CancelledError */
            res = PyObject_IsInstance(exc, asyncio_CancelledError);
            if (res == -1) {
                /* An error occurred, abort */
                goto fail;
            }
            if (res == 0) {
                /* exc is not CancelledError; reset it to NULL */
                exc = NULL;
            }
        }

        if (!exc) {
            /* exc was not a CancelledError */
            exc = PyObject_CallFunctionObjArgs(asyncio_CancelledError, NULL);
            if (!exc) {
                goto fail;
            }
            clear_exc = 1;
        }

        task->task_must_cancel = 0;
    }

    Py_CLEAR(task->task_fut_waiter);

    if (exc == NULL) {
        if (PyGen_CheckExact(coro) || PyCoro_CheckExact(coro)) {
            result = _PyGen_Send((PyGenObject*)coro, Py_None);
        }
        else {
            result = _PyObject_CallMethodIdObjArgs(
                coro, &PyId_send, Py_None, NULL);
        }
    } else {
        result = _PyObject_CallMethodIdObjArgs(
            coro, &PyId_throw, exc, NULL);
        if (clear_exc) {
            /* We created 'exc' during this call */
            Py_CLEAR(exc);
        }
    }

    if (result == NULL) {
        PyObject *et, *ev, *tb;

        if (_PyGen_FetchStopIterationValue(&o) == 0) {
            /* The error is StopIteration and that means that
               the underlying coroutine has resolved */
            PyObject *res = FutureObj_set_result((FutureObj*)task, o);
            Py_DECREF(o);
            if (res == NULL) {
                return NULL;
            }
            Py_DECREF(res);
            Py_RETURN_NONE;
        }

        if (PyErr_ExceptionMatches(asyncio_CancelledError)) {
            /* CancelledError */
            PyErr_Clear();
            return FutureObj_cancel((FutureObj*)task, NULL);
        }

        /* Some other exception; pop it and call Task.set_exception() */
        PyErr_Fetch(&et, &ev, &tb);
        assert(et);
        if (!ev || !PyObject_TypeCheck(ev, (PyTypeObject *) et)) {
            PyErr_NormalizeException(&et, &ev, &tb);
        }
        o = FutureObj_set_exception((FutureObj*)task, ev);
        if (!o) {
            /* An exception in Task.set_exception() */
            Py_XDECREF(et);
            Py_XDECREF(tb);
            Py_XDECREF(ev);
            goto fail;
        }
        assert(o == Py_None);
        Py_CLEAR(o);

        if (!PyErr_GivenExceptionMatches(et, PyExc_Exception)) {
            /* We've got a BaseException; re-raise it */
            PyErr_Restore(et, ev, tb);
            goto fail;
        }

        Py_XDECREF(et);
        Py_XDECREF(tb);
        Py_XDECREF(ev);

        Py_RETURN_NONE;
    }

    /* Check if `result` is FutureObj or TaskObj (and not a subclass) */
    if (Future_CheckExact(result) || Task_CheckExact(result)) {
        PyObject *wrapper;
        PyObject *res;

        /* Check if `result` future is attached to a different loop */
        if (((FutureObj*)result)->fut_loop != task->task_loop) {
            goto different_loop;
        }

        if (result == (PyObject*)task) {
            /* We have a task that wants to await on itself */
            PyObject *res;
            res = TaskObj_SetErrorSoon(
                task, PyExc_RuntimeError,
                "Task cannot await on itself: %R", task);
            Py_DECREF(result);
            return res;
        }

        FutureObj *fut = (FutureObj*)result;

        if (fut->fut_blocking) {
            fut->fut_blocking = 0;

            /* result.add_done_callback(task._wakeup) */
            wrapper = TaskWakeupMethWrapper_new(task);
            if (wrapper == NULL) {
                goto fail;
            }
            res = FutureObj_add_done_callback((FutureObj*)result, wrapper);
            Py_DECREF(wrapper);
            if (res == NULL) {
                goto fail;
            }
            Py_DECREF(res);

            /* task._fut_waiter = result */
            task->task_fut_waiter = result;  /* no incref is necessary */

            if (task->task_must_cancel) {
                PyObject *r;
                r = FutureObj_cancel(fut, NULL);
                if (r == NULL) {
                    return NULL;
                }
                if (r == Py_True) {
                    task->task_must_cancel = 0;
                }
                Py_DECREF(r);
            }

            Py_RETURN_NONE;
        }
        else {
            goto yield_insteadof_yf;
        }
    }

    /* Check if `result` is a Future-compatible object */
    o = PyObject_GetAttrString(result, "_asyncio_future_blocking");
    if (o == NULL) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Clear();
        }
        else {
            goto fail;
        }
    }
    else {
        if (o == Py_None) {
            Py_CLEAR(o);
        }
        else {
            /* `result` is a Future-compatible object */
            PyObject *wrapper;
            PyObject *res;

            int blocking = PyObject_IsTrue(o);
            Py_CLEAR(o);
            if (blocking < 0) {
                goto fail;
            }

            /* Check if `result` future is attached to a different loop */
            PyObject *oloop = PyObject_GetAttrString(result, "_loop");
            if (oloop == NULL) {
                goto fail;
            }
            if (oloop != task->task_loop) {
                Py_DECREF(oloop);
                goto different_loop;
            } else {
                Py_DECREF(oloop);
            }

            if (blocking) {
                /* result._asyncio_future_blocking = False */
                if (PyObject_SetAttrString(
                        result, "_asyncio_future_blocking", Py_False) == -1) {
                    goto fail;
                }

                /* result.add_done_callback(task._wakeup) */
                wrapper = TaskWakeupMethWrapper_new(task);
                if (wrapper == NULL) {
                    goto fail;
                }
                res = _PyObject_CallMethodId(
                    result, &PyId_add_done_callback, "O", wrapper, NULL);
                Py_DECREF(wrapper);
                if (res == NULL) {
                    goto fail;
                }
                Py_DECREF(res);

                /* task._fut_waiter = result */
                task->task_fut_waiter = result;  /* no incref is necessary */

                if (task->task_must_cancel) {
                    PyObject *r;
                    int is_true;
                    r = _PyObject_CallMethodId(result, &PyId_cancel, NULL);
                    if (r == NULL) {
                        return NULL;
                    }
                    is_true = PyObject_IsTrue(r);
                    Py_DECREF(r);
                    if (is_true < 0) {
                        return NULL;
                    }
                    else if (is_true) {
                        task->task_must_cancel = 0;
                    }
                }

                Py_RETURN_NONE;
            }
            else {
                goto yield_insteadof_yf;
            }
        }
    }

    /* Check if `result` is None */
    if (result == Py_None) {
        /* Bare yield relinquishes control for one event loop iteration. */
        if (TaskObj_call_step_soon(task, NULL)) {
            goto fail;
        }
        Py_DECREF(result);
        Py_RETURN_NONE;
    }

    /* Check if `result` is a generator */
    o = PyObject_CallFunctionObjArgs(inspect_isgenerator, result, NULL);
    if (o == NULL) {
        /* An exception in inspect.isgenerator */
        goto fail;
    }
    res = PyObject_IsTrue(o);
    Py_CLEAR(o);
    if (res == -1) {
        /* An exception while checking if 'val' is True */
        goto fail;
    }
    if (res == 1) {
        /* `result` is a generator */
        PyObject *ret;
        ret = TaskObj_SetErrorSoon(
            task, PyExc_RuntimeError,
            "yield was used instead of yield from for "
            "generator in task %R with %S", task, result);
        Py_DECREF(result);
        return ret;
    }

    /* The `result` is none of the above */
    Py_DECREF(result);
    return TaskObj_SetErrorSoon(
        task, PyExc_RuntimeError, "Task got bad yield: %R", result);

yield_insteadof_yf:
    o = TaskObj_SetErrorSoon(
        task, PyExc_RuntimeError,
        "yield was used instead of yield from "
        "in task %R with %R",
        task, result);
    Py_DECREF(result);
    return o;

different_loop:
    o = TaskObj_SetErrorSoon(
        task, PyExc_RuntimeError,
        "Task %R got Future %R attached to a different loop",
        task, result);
    Py_DECREF(result);
    return o;

fail:
    Py_XDECREF(result);
    return NULL;
}

static PyObject *
task_step(TaskObj *task, PyObject *exc)
{
    PyObject *res;
    PyObject *ot;

    if (PyDict_SetItem((PyObject *)current_tasks,
                       task->task_loop, (PyObject*)task) == -1)
    {
        return NULL;
    }

    res = task_step_impl(task, exc);

    if (res == NULL) {
        PyObject *et, *ev, *tb;
        PyErr_Fetch(&et, &ev, &tb);
        ot = _PyDict_Pop(current_tasks, task->task_loop, NULL);
        if (ot == NULL) {
            Py_XDECREF(et);
            Py_XDECREF(tb);
            Py_XDECREF(ev);
            return NULL;
        }
        Py_DECREF(ot);
        PyErr_Restore(et, ev, tb);
        return NULL;
    }
    else {
        ot = _PyDict_Pop(current_tasks, task->task_loop, NULL);
        if (ot == NULL) {
            Py_DECREF(res);
            return NULL;
        }
        else {
            Py_DECREF(ot);
            return res;
        }
    }
}

static PyObject *
task_wakeup(TaskObj *task, PyObject *o)
{
    assert(o);

    if (Future_CheckExact(o) || Task_CheckExact(o)) {
        PyObject *fut_result = NULL;
        int res = get_future_result((FutureObj*)o, &fut_result);
        PyObject *result;

        switch(res) {
        case -1:
            assert(fut_result == NULL);
            return NULL;
        case 0:
            Py_DECREF(fut_result);
            return task_step(task, NULL);
        default:
            assert(res == 1);
            result = task_step(task, fut_result);
            Py_DECREF(fut_result);
            return result;
        }
    }

    PyObject *fut_result = PyObject_CallMethod(o, "result", NULL);
    if (fut_result == NULL) {
        PyObject *et, *ev, *tb;
        PyObject *res;

        PyErr_Fetch(&et, &ev, &tb);
        if (!ev || !PyObject_TypeCheck(ev, (PyTypeObject *) et)) {
            PyErr_NormalizeException(&et, &ev, &tb);
        }

        res = task_step(task, ev);

        Py_XDECREF(et);
        Py_XDECREF(tb);
        Py_XDECREF(ev);

        return res;
    }
    else {
        Py_DECREF(fut_result);
        return task_step(task, NULL);
    }
}


/*********************** Module **************************/


static int
init_module(void)
{
    PyObject *module = NULL;
    PyObject *cls;

#define WITH_MOD(NAME) \
    Py_CLEAR(module); \
    module = PyImport_ImportModule(NAME); \
    if (module == NULL) { \
        return -1; \
    }

#define GET_MOD_ATTR(VAR, NAME) \
    VAR = PyObject_GetAttrString(module, NAME); \
    if (VAR == NULL) { \
        goto fail; \
    }

    WITH_MOD("asyncio.events")
    GET_MOD_ATTR(asyncio_get_event_loop, "get_event_loop")

    WITH_MOD("asyncio.base_futures")
    GET_MOD_ATTR(asyncio_future_repr_info_func, "_future_repr_info")
    GET_MOD_ATTR(asyncio_InvalidStateError, "InvalidStateError")
    GET_MOD_ATTR(asyncio_CancelledError, "CancelledError")

    WITH_MOD("asyncio.base_tasks")
    GET_MOD_ATTR(asyncio_task_repr_info_func, "_task_repr_info")
    GET_MOD_ATTR(asyncio_task_get_stack_func, "_task_get_stack")
    GET_MOD_ATTR(asyncio_task_print_stack_func, "_task_print_stack")

    WITH_MOD("inspect")
    GET_MOD_ATTR(inspect_isgenerator, "isgenerator")

    WITH_MOD("traceback")
    GET_MOD_ATTR(traceback_extract_stack, "extract_stack")

    WITH_MOD("weakref")
    GET_MOD_ATTR(cls, "WeakSet")
    all_tasks = PyObject_CallObject(cls, NULL);
    Py_CLEAR(cls);
    if (all_tasks == NULL) {
        goto fail;
    }

    current_tasks = (PyDictObject *)PyDict_New();
    if (current_tasks == NULL) {
        goto fail;
    }

    Py_CLEAR(module);
    return 0;

fail:
    Py_CLEAR(module);
    Py_CLEAR(current_tasks);
    Py_CLEAR(all_tasks);
    Py_CLEAR(traceback_extract_stack);
    Py_CLEAR(asyncio_get_event_loop);
    Py_CLEAR(asyncio_future_repr_info_func);
    Py_CLEAR(asyncio_task_repr_info_func);
    Py_CLEAR(asyncio_task_get_stack_func);
    Py_CLEAR(asyncio_task_print_stack_func);
    Py_CLEAR(asyncio_InvalidStateError);
    Py_CLEAR(asyncio_CancelledError);
    Py_CLEAR(inspect_isgenerator);
    return -1;

#undef WITH_MOD
#undef GET_MOD_ATTR
}

PyDoc_STRVAR(module_doc, "Accelerator module for asyncio");

static struct PyModuleDef _asynciomodule = {
    PyModuleDef_HEAD_INIT,      /* m_base */
    "_asyncio",                 /* m_name */
    module_doc,                 /* m_doc */
    -1,                         /* m_size */
    NULL,                       /* m_methods */
    NULL,                       /* m_slots */
    NULL,                       /* m_traverse */
    NULL,                       /* m_clear */
    NULL                        /* m_free */
};


PyMODINIT_FUNC
PyInit__asyncio(void)
{
    if (init_module() < 0) {
        return NULL;
    }
    if (PyType_Ready(&FutureType) < 0) {
        return NULL;
    }
    if (PyType_Ready(&FutureIterType) < 0) {
        return NULL;
    }
    if (PyType_Ready(&TaskSendMethWrapper_Type) < 0) {
        return NULL;
    }
    if(PyType_Ready(&TaskWakeupMethWrapper_Type) < 0) {
        return NULL;
    }
    if (PyType_Ready(&TaskType) < 0) {
        return NULL;
    }

    PyObject *m = PyModule_Create(&_asynciomodule);
    if (m == NULL) {
        return NULL;
    }

    Py_INCREF(&FutureType);
    if (PyModule_AddObject(m, "Future", (PyObject *)&FutureType) < 0) {
        Py_DECREF(&FutureType);
        return NULL;
    }

    Py_INCREF(&TaskType);
    if (PyModule_AddObject(m, "Task", (PyObject *)&TaskType) < 0) {
        Py_DECREF(&TaskType);
        return NULL;
    }

    return m;
}
