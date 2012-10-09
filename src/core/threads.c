#include "moarvm.h"

/* Temporary structure for passing data to thread start. */
struct _MVMThreadStart {
    MVMThreadContext *tc;
    MVMFrame         *caller;
    MVMObject        *invokee;
};

/* This callback is passed to the interpreter code. It takes care of making
 * the initial invocation of the thread code. */
static void thread_initial_invoke(MVMThreadContext *tc, void *data) {
    /* The passed data is simply the code object to invoke. */
    MVMObject *code = (MVMObject *)data;
    
    /* Dummy, 0-arg callsite. */
    MVMCallsite no_arg_callsite;
    no_arg_callsite.arg_flags = NULL;
    no_arg_callsite.arg_count = 0;
    no_arg_callsite.num_pos   = 0;
    
    /* Create initial frame, which sets up all of the interpreter state also. */
    STABLE(code)->invoke(tc, code, &no_arg_callsite, NULL);
    
    /* This frame should be marked as the thread entry frame, so that any
     * return from it will cause us to drop out of the interpreter and end
     * the thread. */
    tc->thread_entry_frame = tc->cur_frame;
}

/* This callback handles starting execution of a thread. */
static void * APR_THREAD_FUNC start_thread(apr_thread_t *thread, void *data) {
    struct _MVMThreadStart *ts = (struct _MVMThreadStart *)data;
    
    /* Set the current frame in the thread to be the initial caller;
     * the ref count for this was incremented in the original thread. */
     ts->tc->cur_frame = ts->caller;
    
    /* Enter the interpreter, to run code. */
    MVM_interp_run(ts->tc, &thread_initial_invoke, ts->invokee);
    
    /* Now we're done, decrement the reference count of the caller. */
    MVM_frame_dec_ref(ts->tc, ts->caller);
    
    return NULL;
}

MVMObject * MVM_thread_start(MVMThreadContext *tc, MVMObject *invokee, MVMObject *result_type) {
    int apr_return_status;
    apr_threadattr_t *thread_attr;
    struct _MVMThreadStart *ts;
    MVMObject *child_obj;

    /* Create a thread object to wrap it up in. */
    child_obj = REPR(result_type)->allocate(tc, STABLE(result_type));
    if (REPR(child_obj)->ID == MVM_REPR_ID_MVMThread) {
        MVMThread *child = (MVMThread *)child_obj;
        
        /* Create a new thread context. */
        MVMThreadContext *child_tc = MVM_tc_create(tc->instance);
        child->body.tc = child_tc;

        /* Allocate APR pool. */
        if ((apr_return_status = apr_pool_create(&child->body.apr_pool, NULL)) != APR_SUCCESS) {
            MVM_panic(MVM_exitcode_compunit, "Could not allocate APR memory pool: errorcode %d", apr_return_status);
        }
        
        /* Create the thread. Note that we take a reference to the current frame,
         * since it must survive to be the dynamic scope of where the thread was
         * started, and there's no promises that the thread won't start before
         * the code creating the thread returns. The count is decremented when
         * the thread is done. */
        ts = malloc(sizeof(struct _MVMThreadStart));
        ts->tc = child_tc;
        ts->caller = MVM_frame_inc_ref(tc, tc->cur_frame);
        ts->invokee = invokee;
        apr_return_status = apr_threadattr_create(&thread_attr, child->body.apr_pool);
        if (apr_return_status != APR_SUCCESS) {
            MVM_panic(MVM_exitcode_compunit, "Could not create threadattr: errorcode %d", apr_return_status);
        }
        apr_return_status = apr_thread_create(&child->body.apr_thread,
            thread_attr, &start_thread, ts, child->body.apr_pool);
        if (apr_return_status != APR_SUCCESS) {
            MVM_panic(MVM_exitcode_compunit, "Could not spawn thread: errorcode %d", apr_return_status);
        }
    }
    else {
        MVM_exception_throw_adhoc(tc,
            "Thread result type must have representation MVMThread");
    }
    
    return child_obj;
}
