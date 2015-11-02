/*******************************************************************************
 * Copyright (c) 2010, 2011 Wind River Systems, Inc. and others.
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
 * Event queue manager for delayed/periodic events.
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
#include <time.h>
#include <assert.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/errors.h>
#include <tcf/framework/trace.h>
#include <tcf/framework/timed_event_queue.h>
#include <tcf/framework/mdep-threads.h>  /* for pthread_* definitions */


static pthread_t event_thread;
static pthread_mutex_t event_lock;
static pthread_cond_t event_cond;
static int process_events = 1;
static LINK queue;

static int time_cmp(const struct timespec * tv1, const struct timespec * tv2) {
    if (tv1->tv_sec < tv2->tv_sec) return -1;
    if (tv1->tv_sec > tv2->tv_sec) return 1;
    if (tv1->tv_nsec < tv2->tv_nsec) return -1;
    if (tv1->tv_nsec > tv2->tv_nsec) return 1;
    return 0;
}

/*
 * Add microsecond value to timespec.
 */
static void time_add_usec(struct timespec * tv, unsigned long usec) {
    tv->tv_sec += usec / 1000000;
    tv->tv_nsec += (usec % 1000000) * 1000;
    if (tv->tv_nsec >= 1000000000) {
        tv->tv_sec++;
        tv->tv_nsec -= 1000000000;
    }
}

static void dequeue_event(struct timed_event_node * ev) {
    list_remove(&ev->list_node);
    ev->list_node.next = NULL;
    ev->list_node.prev = NULL;
}

void timed_event_queue_init(void)
{
    list_init(&queue);
    event_thread = pthread_self();
    check_error(pthread_mutex_init(&event_lock, NULL));
    check_error(pthread_cond_init(&event_cond, NULL));
}

/* release resources associated with this event queue */
void timed_event_queue_destroy(void)
{
    pthread_mutex_destroy(&event_lock);
    pthread_cond_destroy(&event_cond);
}

int is_timed_event_queue_thread(void) {
    return pthread_equal(event_thread, pthread_self());
}

void timed_event_queue_post_event(struct timed_event_node * tev, struct event_queue * destination, unsigned long delay) {
    struct LINK * trav;

    assert(tev);
    check_error(pthread_mutex_lock(&event_lock));

    if (clock_gettime(CLOCK_REALTIME, &tev->runtime)) {
        check_error(errno);
    }
    time_add_usec(&tev->runtime, delay);

    list_foreach(trav, &queue) {
        struct timed_event_node * tmp = (struct timed_event_node *) trav;
        if (time_cmp(&tev->runtime, &tmp->runtime) < 0)
            break;
    }
    tev->list_node.prev = trav->prev;
    tev->list_node.next = trav->next;
    tev->list_node.next->next = &tev->list_node;
    trav->next = &tev->list_node;

    if (queue.next == &tev->list_node)
        check_error(pthread_cond_signal(&event_cond));

    trace(LOG_EVENTCORE, "post_event: event %#lx, handler %#lx", tev, tev->event.handler);
    check_error(pthread_mutex_unlock(&event_lock));
}

int timed_event_queue_cancel_event(struct timed_event_node * event_to_cancel) {
    assert(event_to_cancel);
    /* must be called from the non-timed dispatch thread */
    /* assert(event_to_cancel->destination_queue.thread == pthread_self()); */

    trace(LOG_EVENTCORE, "cancel_event: event %p", event_to_cancel);
    check_error(pthread_mutex_lock(&event_lock));
    if (event_to_cancel->list_node.prev != NULL) {
        assert(event_to_cancel->list_node.next != NULL);
        dequeue_event(event_to_cancel);
        check_error(pthread_mutex_unlock(&event_lock));
        return 1;
    }

    check_error(pthread_mutex_unlock(&event_lock));

    return event_queue_cancel_event(event_to_cancel->destination_queue, &event_to_cancel->event, 0);
}

void timed_event_loop_run(void) {
    assert(is_timed_event_queue_thread());
    check_error(pthread_mutex_lock(&event_lock));

    while (process_events) {
        if (!list_is_empty(&queue)) {
            struct timed_event_node * ev = (struct timed_event_node *) queue.next;
            struct timespec timenow;
            if (clock_gettime(CLOCK_REALTIME, &timenow)) {
                check_error(errno);
            }
            if (time_cmp(&ev->runtime, &timenow) <= 0) {
                dequeue_event(ev);
                trace(LOG_EVENTCORE, "timed_event_loop_run: event %p", ev->event);
                event_queue_post_event(ev->destination_queue, &ev->event);
            } else {
                int error = pthread_cond_timedwait(&event_cond, &event_lock, &ev->runtime);
                if (error && error != ETIMEDOUT) check_error(error);
            }
        } else {
            check_error(pthread_cond_wait(&event_cond, &event_lock));
        }
    }
}

void timed_event_loop_cancel(void) {
    process_events = 0;
    check_error(pthread_cond_signal(&event_cond));
}
