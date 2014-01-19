#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <signal.h>
#include "compat.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#if PY_MAJOR_VERSION >= 3
  #define MOD_ERROR_VAL NULL
  #define MOD_SUCCESS_VAL(val) val
  #define MOD_INIT(name) PyMODINIT_FUNC PyInit_##name(void)
  #define MOD_DEF(ob, name, doc, methods) \
          static struct PyModuleDef moduledef = { \
            PyModuleDef_HEAD_INIT, name, doc, -1, methods, }; \
          ob = PyModule_Create(&moduledef);
#else
  #define MOD_ERROR_VAL
  #define MOD_SUCCESS_VAL(val)
  #define MOD_INIT(name) void init##name(void)
  #define MOD_DEF(ob, name, doc, methods) \
          ob = Py_InitModule3(name, methods, doc);
#endif


#define STREAM_NONE        0
#define STREAM_SHUTDOWN    1 << 0
#define STREAM_RESCHEDULE  1 << 1


const char callback_error_msg[] = "Unable to call callback function.";

static PyTypeObject PyFSEventStreamType;

typedef struct {
    PyObject_HEAD
    PyObject *callback;
    PyObject *paths;
    PyThreadState *state;
    FSEventStreamRef stream;
    FSEventStreamCreateFlags flags;
    CFAbsoluteTime latency;
    CFRunLoopRef loop;
    CFRunLoopSourceRef signal_source;
    unsigned int action;
} streamobject;


static void
_stream_handler(ConstFSEventStreamRef stream,
                void *info,
                size_t numEvents,
                void *eventPaths,
                const FSEventStreamEventFlags eventFlags[],
                const FSEventStreamEventId eventIds[])
{
    const char **paths = eventPaths;
    streamobject *object = info;
    PyObject *result = NULL;

    assert(numEvents <= PY_SSIZE_T_MAX);

    PyGILState_STATE gil_state = PyGILState_Ensure();
    PyThreadState *thread_state = PyThreadState_Swap(object->state);

    /* Convert event data to Python objects */
    PyObject *event_paths = PyList_New(numEvents);
    if (!event_paths) {
    	goto final;
    }

    PyObject *event_flags = PyList_New(numEvents);
    if (!event_flags) {
    	goto final;
    }

    for (size_t i = 0; i < numEvents; i++) {
        PyObject *str = PyBytes_FromString(paths[i]);

        #if PY_MAJOR_VERSION >= 3
            PyObject *num = PyLong_FromLong(eventFlags[i]);
        #else
            PyObject *num = PyInt_FromLong(eventFlags[i]);
        #endif

        if ((!num) || (!str)) {
            goto final;
        }
        PyList_SET_ITEM(event_paths, i, str);
        PyList_SET_ITEM(event_flags, i, num);
    }

    if ( (result = PyObject_CallFunctionObjArgs(
              object->callback, event_paths, event_flags, NULL)) == NULL) {
        /* May can return NULL if an exception is raised */
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_ValueError, callback_error_msg);

        /* Stop listening */
        CFRunLoopStop(object->loop);
    }

final:
    if (!result) {
        Py_XDECREF(event_paths);
        Py_XDECREF(event_flags);
    }
    Py_XDECREF(result);
    PyThreadState_Swap(thread_state);
    PyGILState_Release(gil_state);
}


static void
_pyfsevents_destroy_stream(streamobject *self)
{
    if (self->stream == NULL)
        return;

    FSEventStreamFlushSync(self->stream);
    FSEventStreamStop(self->stream);
    FSEventStreamInvalidate(self->stream);
    FSEventStreamRelease(self->stream);

    self->stream = NULL;
}


static int
_pyfsevents_create_stream(streamobject *self, CFArrayRef paths)
{
    FSEventStreamContext ctx;
    FSEventStreamRef ref;

    /* Initialize context */
    ctx.version = 0;
    ctx.info = self;
    ctx.retain = NULL;
    ctx.release = NULL;
    ctx.copyDescription = NULL;

    ref = FSEventStreamCreate(NULL,
                              &_stream_handler,
                              &ctx,
                              paths,
                              kFSEventStreamEventIdSinceNow,
                              self->latency,
                              self->flags);

    assert(ref != NULL);

    FSEventStreamScheduleWithRunLoop(ref,
                                     self->loop,
                                     kCFRunLoopDefaultMode);

    if (!FSEventStreamStart(ref)) {
        FSEventStreamInvalidate(ref);
        FSEventStreamRelease(ref);
        return -1;
  }

  self->stream = ref;
  return 0;
}


static int
_pyfsevents_reschedule_stream(streamobject *self)
{
    CFMutableArrayRef cf_paths = NULL;
    CFStringRef cf_path = NULL;
    PyObject *path = NULL;
    PyObject *_ = NULL;
    Py_ssize_t i = 0, pos = 0, path_count = 0;
    int err = -1;

    /* Destroy previous event stream */
    _pyfsevents_destroy_stream(self);

    path_count = PyDict_Size(self->paths);
    if (path_count == 0) {
        return 0;
    }

    cf_paths = CFArrayCreateMutable(kCFAllocatorDefault, 1,
    		&kCFTypeArrayCallBacks);
    if (cf_paths == NULL) {
    	goto final;
    }

    while (PyDict_Next(self->paths, &pos, &path, &_)) {
    	cf_path = CFStringCreateWithCString(kCFAllocatorDefault,
                                            PyBytes_AsString(path),
                                            kCFStringEncodingUTF8);
    	if (cf_path == NULL) {
            goto final;
        }
        CFArraySetValueAtIndex(cf_paths, i++, cf_path);
        CFRelease(cf_path);
    }

    err = _pyfsevents_create_stream(self, cf_paths);

final:
    if (cf_paths != NULL) {
        CFRelease(cf_paths);
    }

    return err;
}


static void
_signal_handler(void *info)
{
    int err = -1;
    streamobject *object = info;

    PyGILState_STATE gil_state = PyGILState_Ensure();
    PyThreadState *thread_state = PyThreadState_Swap(object->state);

    if (object->action & STREAM_SHUTDOWN) {
        CFRunLoopStop(object->loop);
        goto final;
    }

    if (object->action & STREAM_RESCHEDULE) {
        /* Refresh event stream */ 
        err = _pyfsevents_reschedule_stream(object);
    }

final:
    object->action = STREAM_NONE;
    PyThreadState_Swap(thread_state);
    PyGILState_Release(gil_state);
}


static void
streamobject_dealloc(streamobject *self)
{
    // Free state?
    if (self->paths) {
        Py_DECREF(self->paths);
        self->paths = NULL;
    }
    if (self->callback) {
        Py_DECREF(self->callback);
        self->callback = NULL;
    }

    _pyfsevents_destroy_stream(self);
    if (self->signal_source) {
        CFRelease(self->signal_source);
        self->signal_source = NULL;
    }
}


static PyObject *
pyfsevents_streamobject(PyObject *selfptr, PyObject *args, PyObject *kwargs)
{
    streamobject *self;
    PyObject *callback;
    CFRunLoopSourceContext ctx;
    CFAbsoluteTime latency = 0.01;
    int file_events = 0;

    static char *kwlist[] = {"callback", "file_events", "latency", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|id:streamobject", kwlist,
                                     &callback, &file_events, &latency)) {
        return NULL;
    }

    self = PyObject_New(streamobject, &PyFSEventStreamType);
    if (self == NULL) {
        PyErr_SetString(PyExc_MemoryError,
            "Could not allocate a new stream object.");
        return NULL;
    }
    self->callback = callback;
    Py_INCREF(self->callback);

    self->latency = latency;

    self->flags = kFSEventStreamCreateFlagNoDefer;
    if (file_events) {
        self->flags |= kFSEventStreamCreateFlagFileEvents;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.info = self;
    ctx.perform = _signal_handler;
    self->signal_source = CFRunLoopSourceCreate(NULL, 0, &ctx);
    if (self->signal_source == NULL) {
        // XXX: cleanups
        return NULL;
    }

    self->loop = NULL;
    self->stream = NULL;
    self->action = STREAM_NONE;
    self->paths = PyDict_New();

    return (PyObject *) self;
}


static PyObject *
streamobject_loop(streamobject *self, PyObject *args)
{
    self->loop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(self->loop,
                       self->signal_source,
                       kCFRunLoopDefaultMode);

    self->state = PyThreadState_Get();
    /* No timeout, block until events */
    Py_BEGIN_ALLOW_THREADS
    CFRunLoopRun();
    Py_END_ALLOW_THREADS

    CFRunLoopRemoveSource(self->loop,
                          self->signal_source,
                          kCFRunLoopDefaultMode);
    _pyfsevents_destroy_stream(self);

    self->loop = NULL;

    Py_RETURN_NONE;
}


static PyObject *
streamobject_stop(streamobject *self, PyObject *args)
{
    if (self->loop == NULL) {
        Py_RETURN_NONE;
    }

    self->action = STREAM_SHUTDOWN;
    CFRunLoopSourceSignal(self->signal_source);
    CFRunLoopWakeUp(self->loop);

    Py_RETURN_NONE;
}


static void
_pyfsevents_signal_reschedule(streamobject *self)
{
    if (self->loop == NULL) {
        return;
    }

    if (self->action != STREAM_NONE) {
    	return;
    }

    self->action = STREAM_RESCHEDULE;
    CFRunLoopSourceSignal(self->signal_source);
    CFRunLoopWakeUp(self->loop);
}


static PyObject *
streamobject_schedule(streamobject *self, PyObject *path)
{
    /* PyDict_SetItem creates new references */
    if (PyDict_SetItem(self->paths, path, Py_None)) {
        return NULL;
    }
    _pyfsevents_signal_reschedule(self);
    Py_RETURN_NONE;
}


static PyObject *
streamobject_unschedule(streamobject *self, PyObject *path)
{
    int result;
	
    result = PyDict_Contains(self->paths, path);
    if (result == -1) {
        return NULL;
    } else if (result == 1) {
        if (PyDict_DelItem(self->paths, path) == -1) {
            return NULL;
        }
        _pyfsevents_signal_reschedule(self);
    }
    Py_RETURN_NONE;
}


static PyMethodDef streamobject_methods[] = {
    {"loop", (PyCFunction) streamobject_loop, METH_NOARGS, NULL},
    {"schedule", (PyCFunction) streamobject_schedule, METH_O, NULL},
    {"unschedule", (PyCFunction) streamobject_unschedule, METH_O, NULL},
    {"stop", (PyCFunction) streamobject_stop, METH_NOARGS, NULL},

    {NULL, NULL, 0, NULL}

};

static PyMethodDef methods[] = {
    {"streamobject", (PyCFunction) pyfsevents_streamobject,
     METH_VARARGS | METH_KEYWORDS, NULL},

    {NULL, NULL, 0, NULL}

};

static PyObject *
streamobject_getattr(streamobject *self, char *name)
{
    return Py_FindMethod(streamobject_methods, (PyObject *)self, name);
}

static PyTypeObject PyFSEventStreamType = {
    PyVarObject_HEAD_INIT(0, 0)
        "_fsevents.FSEventStream",
    sizeof(streamobject),
    0,
    (destructor) streamobject_dealloc,   /*tp_dealloc */
    0,      /*tp_print */
    (getattrfunc) streamobject_getattr,  /*tp_getattr */
    0,      /*tp_setattr */
    0,      /*tp_compare */
    0,      /*tp_repr */
    0,      /*tp_as_number */
    0,      /*tp_as_sequence */
    0,      /*tp_as_mapping */
};


static char doc[] = "Low-level FSEvent interface.";

MOD_INIT(_fsevents) {
    PyObject *mod;

    MOD_DEF(mod, "_fsevents", doc, methods)

    if (mod == NULL)
        return MOD_ERROR_VAL;

    PyModule_AddIntConstant(mod, "CF_POLLIN", kCFFileDescriptorReadCallBack);
    PyModule_AddIntConstant(mod, "CF_POLLOUT", kCFFileDescriptorWriteCallBack);
    PyModule_AddIntConstant(mod, "FS_IGNORESELF", kFSEventStreamCreateFlagIgnoreSelf);
    PyModule_AddIntConstant(mod, "FS_FILEEVENTS", kFSEventStreamCreateFlagFileEvents);
    PyModule_AddIntConstant(mod, "FS_ITEMCREATED", kFSEventStreamEventFlagItemCreated);
    PyModule_AddIntConstant(mod, "FS_ITEMREMOVED", kFSEventStreamEventFlagItemRemoved);
    PyModule_AddIntConstant(mod, "FS_ITEMINODEMETAMOD", kFSEventStreamEventFlagItemInodeMetaMod);
    PyModule_AddIntConstant(mod, "FS_ITEMRENAMED", kFSEventStreamEventFlagItemRenamed);
    PyModule_AddIntConstant(mod, "FS_ITEMMODIFIED", kFSEventStreamEventFlagItemModified);
    PyModule_AddIntConstant(mod, "FS_ITEMFINDERINFOMOD", kFSEventStreamEventFlagItemFinderInfoMod);
    PyModule_AddIntConstant(mod, "FS_ITEMCHANGEOWNER", kFSEventStreamEventFlagItemChangeOwner);
    PyModule_AddIntConstant(mod, "FS_ITEMXATTRMOD", kFSEventStreamEventFlagItemXattrMod);
    PyModule_AddIntConstant(mod, "FS_ITEMISFILE", kFSEventStreamEventFlagItemIsFile);
    PyModule_AddIntConstant(mod, "FS_ITEMISDIR", kFSEventStreamEventFlagItemIsDir);
    PyModule_AddIntConstant(mod, "FS_ITEMISSYMLINK", kFSEventStreamEventFlagItemIsSymlink);

	if (!PyEval_ThreadsInitialized()) {
        PyEval_InitThreads();
    }

    return MOD_SUCCESS_VAL(mod);
}

