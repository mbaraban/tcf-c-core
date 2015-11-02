/*******************************************************************************
 * Copyright (c) 2014 Wind River Systems, Inc. and others.
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
 * TCF Logger filter routines.
 *
 * Set of routines to filter queries and events.
 */

#include <tcf/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include <tcf/framework/myalloc.h>
#include <tcf/framework/channel.h>
#include <tcf/framework/proxy.h>
#include <tcf/main/logfilter.h>

#define FILTER_IN 1
#define FILTER_OUT 2
#define FILTER_LIMIT 4
#define FILTER_LIMIT_REPLY 8
#define FILTER_MODE (FILTER_IN | FILTER_OUT | FILTER_LIMIT |\
                      FILTER_LIMIT_REPLY)

typedef struct MessageFilter {
    LINK all;
    int flags;
    int argc;
    int limit;
    /* Dynamic array, must be last member in struct */
    char * argv[1];
} MessageFilter;

typedef struct TokenFilter {
    LINK all;
    Channel * chan;
    int  limit;
    char token[1];
} TokenFilter;

#define all2mf(A)   ((MessageFilter *)((char *)(A) - offsetof(MessageFilter, all)))
#define all2tf(A)   ((TokenFilter *)((char *)(A) - offsetof(TokenFilter, all)))

static LINK message_filters = TCF_LIST_INIT(message_filters);
static LINK token_filters = TCF_LIST_INIT(token_filters);

int filter_add_message_filter(const char * filter) {
    MessageFilter * mf;
    const char * s;
    int max = 1;
    int c;

    mf = (MessageFilter *)loc_alloc(sizeof *mf + (max - 1) * sizeof *mf->argv);
    mf->flags = 0;
    mf->argc = 0;

    s = filter;

    while ((c = *s) != '\0' && c != ',') {
        switch (c) {
        case 'i':
            mf->flags |= FILTER_IN;
            break;
        case 'o':
            mf->flags |= FILTER_OUT;
            break;
        case 't':
            mf->flags |= FILTER_LIMIT;
            break;
        default:
            return 1;
        }
        s++;
        if (mf->flags == FILTER_LIMIT) {
            c = *s;
            if (c == 'r') {
               mf->flags = FILTER_LIMIT_REPLY;
               s++;
               c = *s;
            }
            mf->limit = 0;
            do {
                mf->limit *= 10;
                if ((c >= '0') && (c <= '9'))
                    mf->limit += c - '0';
                else
                    return 1;
                s++;
            } while ((c = *s) != '\0' && c != ',');
        }
    }

    while (c == ',') {
        const char * start = ++s;
        while ((c = *s) != '\0' && c != ',')
            s++;
        if (mf->argc >= max) {
            max *= 2;
            mf = (MessageFilter *)loc_realloc(mf,
                  sizeof *mf + (max - 1) * sizeof *mf->argv);
        }
        if (mf->argc == 1 && !strcmp(mf->argv[0], "C")) {
            /* Match any entry for command token */
            mf->argv[mf->argc++] = NULL;
            s = start - 1;
            c = *s;
        } else {
            mf->argv[mf->argc++] = loc_strndup(start, s - start);
        }
    }
    assert(c == '\0');

    if ((mf->flags & FILTER_MODE) != FILTER_IN &&
        (mf->flags & FILTER_MODE) != FILTER_OUT &&
        (mf->flags & FILTER_MODE) != FILTER_LIMIT &&
        (mf->flags & FILTER_MODE) != FILTER_LIMIT_REPLY)
        return 1;

    list_add_last(&mf->all, &message_filters);
    return 0;
}

static MessageFilter * find_message_filter(int argc, char ** argv) {
    LINK * l = message_filters.next;
    while (l != &message_filters) {
        MessageFilter *mf = all2mf(l);
        if (mf != NULL && mf->argc <= argc) {
            int i;
            for (i = 0; i < mf->argc; i++) {
                if (mf->argv[i] != NULL && strcmp(mf->argv[i], argv[i]) != 0)
                    break;
            }
            if (i >= mf->argc)
                return mf;
        }
        l = l->next;
    }
    return NULL;
}

static TokenFilter * find_token_filter(Channel * c, char * token) {
    LINK * l = token_filters.next;
    while (l != &token_filters) {
        TokenFilter *tf = all2tf(l);

        if (tf->chan == c && !strcmp(tf->token, token))
            return tf;
        l = l->next;
    }
    return NULL;
}


int filter_is_log_filtered(Channel * src, Channel * dst, int argc,
                            char ** argv, int *limit) {
    MessageFilter * mf;
    *limit = 0;
    if (argc >= 2 && (argv[0][0] == 'P' || argv[0][0] == 'R')
        && argv[0][1] == '\0') {
        TokenFilter * tf = find_token_filter(dst, argv[1]);
        if (tf != NULL) {
            int res = PROXY_FILTER_FILTERED;
            if (tf->limit > 0) {
               *limit = tf->limit;
               res = PROXY_FILTER_LIMIT; /* limit  in size the reply */
            }
            if (argv[0][0] == 'R') {
                list_remove(&tf->all);
                loc_free(tf);
            }
            return res;
        }
    }

    mf = find_message_filter(argc, argv);
    if (mf && ((mf->flags & FILTER_OUT) || (mf->flags & FILTER_LIMIT) ||
               (mf->flags & FILTER_LIMIT_REPLY))) {
        /* Filter by default */
        int res = PROXY_FILTER_FILTERED;
        if (argc >= 2 && argv[0][0] == 'C' && argv[0][1] == '\0') {
            /* Need to store the token and propagate truncation size
             * for reply.
             */
            TokenFilter * tf = (TokenFilter *)loc_alloc_zero(
                                      sizeof(TokenFilter) + strlen(argv[1]));
            tf->chan = src;
            strcpy(tf->token, argv[1]);
            list_add_last(&tf->all, &token_filters);
            if (mf->flags & FILTER_LIMIT_REPLY) {
                tf->limit = mf->limit;
               /* We only want to limit the  reply in size :
                * don't filter the command */
               res = PROXY_FILTER_NOT_FILTERED;
            }
        } /* Command */
        if (mf->flags & FILTER_LIMIT) {
           /* Just limit the command or event */
           *limit = mf->limit;
           res = PROXY_FILTER_LIMIT;
        }
        return res;
    } /* something to filter */
    return PROXY_FILTER_NOT_FILTERED;
}
