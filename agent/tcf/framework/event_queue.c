/*******************************************************************************
 * Copyright (c) 2010, 2012 Wind River Systems, Inc. and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * The Eclipse Public License is available at
 * http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 * You may elect to redistribute this code under either of these licenses.
 *
 * Contributors:
 *     Wind River Systems - initial API and implementation
 *******************************************************************************/

/*
 * Event queue manager.
 * An Event is a data pointer plus a function pointer (a.k.a. event handler).
 *
 * Posting an event means placing it into an event queue.
 * Dispatching an event means removing it from the queue and calling the
 * event's function pointer with its event data as an argument.
 *
 * All events are dispatched by a single thread - the dispatch thread. This
 * makes it safe to access global data structures from event handlers without
 * further synchronization, while allowing for high levels of concurrency.
 */

#include <tcf/config.h>
#include <assert.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/errors.h>
#include <tcf/framework/trace.h>
#include <tcf/framework/event_queue.h>
#include <tcf/framework/mdep-threads.h>

#if defined(WIN32)
#  define current_thread() GetCurrentThreadId()
#  define is_event_thread(eq) ((eq)->event_thread == current_thread())
#else
#  define current_thread() pthread_self()
#  define is_event_thread(eq) pthread_equal((eq)->event_thread, current_thread())
#endif

struct event_queue {
#if defined(WIN32)
    DWORD event_thread;
#else
    pthread_t event_thread;
#endif

    pthread_mutex_t event_lock;
    pthread_mutex_t loop_lock;
    pthread_cond_t event_cond;
    pthread_cond_t cancel_cond;
    int process_events;

    LINK queue;

    struct event_node * event_to_cancel;
};

struct event_queue * event_queue_create(void)
{
    struct event_queue * eq = loc_alloc(sizeof(struct event_queue));
    if (pthread_mutex_init(&eq->event_lock, NULL))
        goto fail;
    if (pthread_mutex_init(&eq->loop_lock, NULL))
        goto fail;
    if (pthread_cond_init(&eq->event_cond, NULL))
        goto fail;
    if (pthread_cond_init(&eq->cancel_cond, NULL))
        goto fail;

    list_init(&eq->queue);
    eq->event_thread = current_thread();
    eq->process_events = 1;
    eq->event_to_cancel = NULL;
    return eq;
 fail:
    loc_free(eq);
    return NULL;
}

/* release resources associated with this event queue */
void event_queue_destroy(struct event_queue * eq)
{
    assert(eq->process_events == 0);
    pthread_mutex_lock(&eq->loop_lock);
    pthread_mutex_unlock(&eq->loop_lock);

    pthread_mutex_destroy(&eq->event_lock);
    pthread_mutex_destroy(&eq->loop_lock);
    pthread_cond_destroy(&eq->event_cond);
    pthread_cond_destroy(&eq->cancel_cond);
    loc_free(eq);
}

int is_event_queue_thread(struct event_queue * eq) {
    return is_event_thread(eq);
}

static void dequeue_event(struct event_node * ev)
{
    list_remove(&ev->list_node);
    ev->list_node.next = NULL;
    ev->list_node.prev = NULL;
}

void event_queue_post_event(struct event_queue * eq, struct event_node * event) {
    assert(eq);
    assert(event);

    check_error(pthread_mutex_lock(&eq->event_lock));
    if (eq->event_to_cancel == event) {
        eq->event_to_cancel = NULL;
        check_error(pthread_cond_signal(&eq->cancel_cond));
        check_error(pthread_mutex_unlock(&eq->event_lock));
        return;
    }

    if (list_is_empty(&eq->queue))
        check_error(pthread_cond_signal(&eq->event_cond));
    list_add_last(&event->list_node, &eq->queue);

    trace(LOG_EVENTCORE, "post_event: event %#lx, handler %#lx", event, event->handler);
    check_error(pthread_mutex_unlock(&eq->event_lock));
}

int event_queue_cancel_event(struct event_queue * eq, struct event_node * event_to_cancel, int wait) {
    assert(eq);
    assert(event_to_cancel);
    assert(is_event_queue_thread(eq));
    assert(eq->event_to_cancel == NULL);

    trace(LOG_EVENTCORE, "cancel_event: event %p, wait %d", event_to_cancel, wait);
    check_error(pthread_mutex_lock(&eq->event_lock));
    if (event_to_cancel->list_node.prev != NULL) {
        assert(event_to_cancel->list_node.next != NULL);
        dequeue_event(event_to_cancel);
        check_error(pthread_mutex_unlock(&eq->event_lock));
        return 1;
    }

    if (!wait) {
        check_error(pthread_mutex_unlock(&eq->event_lock));
        return 0;
    }

    eq->event_to_cancel = event_to_cancel;
    do check_error(pthread_cond_wait(&eq->cancel_cond, &eq->event_lock));
    while (eq->event_to_cancel != NULL);
    check_error(pthread_mutex_unlock(&eq->event_lock));
    return 1;
}

void event_loop_run(struct event_queue * eq) {
    assert(eq);
    assert(is_event_queue_thread(eq));
    check_error(pthread_mutex_lock(&eq->loop_lock));
    check_error(pthread_mutex_lock(&eq->event_lock));

    while (eq->process_events) {
        if (!list_is_empty(&eq->queue)) {
            struct event_node * ev = (struct event_node *) eq->queue.next;

            dequeue_event(ev);
            check_error(pthread_mutex_unlock(&eq->event_lock));
            trace(LOG_EVENTCORE, "run_event_loop: event %#lx, handler %#lx", ev, ev->handler);
            ev->handler(ev);
            check_error(pthread_mutex_lock(&eq->event_lock));
        } else {
            check_error(pthread_cond_wait(&eq->event_cond, &eq->event_lock));
        }
    }

    check_error(pthread_mutex_unlock(&eq->loop_lock));
}

void event_loop_cancel(struct event_queue * eq) {
    assert(eq);
    eq->process_events = 0;
    check_error(pthread_cond_signal(&eq->event_cond));
}
