/*******************************************************************************
 * Copyright (c) 2007, 2014 Wind River Systems, Inc. and others.
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
 * TCF Logger main module.
 *
 * TCF Logger is a simple TCF agent that does not provide any services itself,
 * instead it forward all TCF traffic to another agent.
 * Logger prints all messages it forwards.
 * It can be used as diagnostic and debugging tool.
 */

#include <tcf/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <tcf/framework/asyncreq.h>
#include <tcf/framework/events.h>
#include <tcf/framework/trace.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/channel.h>
#include <tcf/framework/protocol.h>
#include <tcf/framework/proxy.h>
#include <tcf/framework/errors.h>
#include <tcf/services/discovery.h>
#include <tcf/main/logfilter.h>
#include <tcf/main/main_hooks.h>

static const char * progname;
static const char * dest_url = "TCP::1534";

/* Hook to add help text. */
#ifndef HELP_TEXT_HOOK
#define HELP_TEXT_HOOK
#endif

/* Hook for illegal option case.  This hook allows for handling off
 * additional options. */
#ifndef ILLEGAL_OPTION_HOOK
#define ILLEGAL_OPTION_HOOK  do {} while(0)
#endif

/* Hook for adding properties */
#ifndef SERVER_ADDPROP_HOOK
#define SERVER_ADDPROP_HOOK do {} while(0)
#endif

typedef struct ConnectInfo {
    PeerServer * ps;
    Channel * c1;
} ConnectInfo;

static int auto_redirect = 1;
static TCFBroadcastGroup * bcg;

static void connect_done(void * args, int error, Channel * c2) {
    ConnectInfo * info = (ConnectInfo *)args;
    Channel * c1 = info->c1;

    if (!is_channel_closed(c1)) {
        assert(c1->state == ChannelStateRedirectReceived);
        if (error) {
            fprintf(stderr, "cannot connect to peer: %s\n", dest_url);
            channel_close(c1);
        }
        else {
            proxy_create(c1, c2);
        }
    }
    else if (!error) {
        channel_close(c2);
    }
    channel_unlock(c1);
    peer_server_free(info->ps);
    loc_free(info);
}

static void connect_dest(void * x) {
    Channel * c1 = (Channel *)x;
    PeerServer * ps;
    ConnectInfo * info;

    ps = channel_peer_from_url(dest_url);
    if (ps == NULL) {
        trace(LOG_ALWAYS, "cannot parse peer url: %s", dest_url);
        channel_close(c1);
        return;
    }
    channel_lock(c1);
    c1->state = ChannelStateRedirectReceived;
    info = (ConnectInfo *)loc_alloc_zero(sizeof(ConnectInfo));
    info->ps = ps;
    info->c1 = c1;
    channel_connect(ps, connect_done, info);
}

static void channel_server_connecting(Channel * c1) {
    trace(LOG_ALWAYS, "channel server connecting");

    assert(c1->state == ChannelStateStarted);
    if (auto_redirect) {
        /* Fake that we sent hello message. */
        c1->state = ChannelStateHelloSent;
    }
    else {
        /* Enable only the locator_service */
#if SERVICE_Locator
        ini_locator_service(c1->protocol, bcg);
#endif
        send_hello_message(c1);
    }
}

static void channel_server_connected(Channel * c1) {
    trace(LOG_ALWAYS, "channel server connected");

    assert(c1->state == ChannelStateConnected);

    if (auto_redirect) {
        /* Connect to destination on next dispatch since we are limited in
         * what we can do in a callback, e.g. cannot close channel. */
        post_event(connect_dest, c1);
    }
}

static void channel_server_disconnected(Channel * c1) {
    trace(LOG_ALWAYS, "channel server disconnected");
    protocol_release(c1->protocol);
}

static void channel_new_connection(ChannelServer * serv, Channel * c) {
    c->protocol = protocol_alloc();
    c->connecting = channel_server_connecting;
    c->connected = channel_server_connected;
    c->disconnected = channel_server_disconnected;
    channel_set_broadcast_group(c, bcg);
    channel_start(c);
}

#if !defined(_WRS_KERNEL)
static const char * help_text[] = {
    "Usage: tcflog [OPTION]...",
    "Start Target Communication Framework logger.",
    "The TCF logger can be used to capture traffic between two TCF peers and "
    "redirect it to either stderr or a file.",
    "For instance:",
    "    tcflog -s TCP::1437 TCP:128.224.218.33:4576",
    "This starts the TCF logger on port 1437 on the local machine and "
    "connects to target IP 128.224.218.33 on port 4576.",
    "  -L<file>         log file name, use -L- to send log to stderr",
#if ENABLE_Trace
    "  -l<level>        set log level, the level is comma separated list of:",
    "@",
#endif
    "  -s<url>          set agent listening port and protocol, default is TCP::1534",
    "  -f<t>,<m>,...    set proxy log filter, <t> is filter type, <m> is message type",
    "                   matching messages will be filtered out the when <t> is 'i'",
    "                   and will be filter in and <t> is 'o'. ",
    "                   Matching messages can be truncated when <t> is 't' ",
    "                   and be followed by an optional 'r' to indicate that reply",
    "                   will be truncated and then followed by the truncation size.",
    "                   <m> is message type, example",
    "                   'C' for command, 'E' for event.  Additional fields are",
    "                   message specific.  Multiple -f options can be specified.",
    "                   Examples:",
    "                      -fo,E                filter out all event messages",
    "                      -fi,C,Memory -fo     filter in Memory service command and",
    "                                           filter out all other messages",

    "                   Default filters (use -fi to disable):",
    "                      -fo,E,Locator,peerHeartBeat",
    "                      -fo,E,Locator,peerAdded",
    "                      -fo,E,Locator,peerRemoved",
    "                      -fo,E,Locator,peerChanged",
    "                      -fo,C,Locator,getPeers",
    "                      -ftr10,C,FileSystem,read",
    "                      -ft10,C,FileSystem,write",
    "  -S               print server properties in Json format to stdout",
    "  -n               no automatic redirect: client use Locator/redirect command",
    HELP_TEXT_HOOK
    NULL
};

static void show_help(void) {
    const char ** p = help_text;
    while (*p != NULL) {
        if (**p == '@') {
#if ENABLE_Trace
            struct trace_mode * tm = trace_mode_table;
            while (tm->mode != 0) {
                fprintf(stderr,
                    "      %-12s %s (%#x)\n", tm->name,
                    tm->description, tm->mode);
                tm++;
            }
#endif
            p++;
        }
        else {
            fprintf(stderr, "%s\n", *p++);
        }
    }
}
#endif

#if defined(_WRS_KERNEL)
int tcf_log(void);
int tcf_log(void) {
#else
int main(int argc, char ** argv) {
    int c;
    int ind;
    const char * log_name = "-";
#endif
    const char * url = "TCP:";
    PeerServer * ps;
    ChannelServer * serv;
    int print_server_properties = 0;

    ini_mdep();
    ini_trace();
    ini_events_queue();
    ini_asyncreq();

    log_mode = LOG_TCFLOG;

#if defined(_WRS_KERNEL)

    progname = "tcf";
    open_log_file("-");

#else

    progname = argv[0];

    /* Parse arguments */
    for (ind = 1; ind < argc; ind++) {
        char * s = argv[ind];
        if (*s++ != '-') break;
        while (s && (c = *s++) != '\0') {
            switch (c) {
            case 'h':
                show_help();
                exit(0);

            case 'n':
                auto_redirect = 0;
                break;

            case 'S':
                print_server_properties = 1;
                break;

#if ENABLE_Trace
            case 'l':
#endif
            case 'L':
            case 's':
            case 'f':
                if (*s == '\0') {
                    if (++ind >= argc) {
                        fprintf(stderr, "%s: error: no argument given to option '%c'\n", progname, c);
                        exit(1);
                    }
                    s = argv[ind];
                }
                switch (c) {
#if ENABLE_Trace
                case 'l':
                    if (parse_trace_mode(s, &log_mode) != 0) {
                        fprintf(stderr, "Cannot parse log level: %s\n", s);
                        exit(1);
                    }
                    break;
#endif

                case 'L':
                    log_name = s;
                    break;

                case 's':
                    url = s;
                    break;

                case 'f':
                    if (filter_add_message_filter(s) != 0) {
                        fprintf(stderr, "Cannot parse filter level: %s\n", s);
                        exit(1);
                    }
                    break;

                default:
                    fprintf(stderr, "%s: error: illegal option '%c'\n", progname, c);
                    show_help();
                    exit(1);
                }
                s = NULL;
                break;

            default:
                ILLEGAL_OPTION_HOOK;
                fprintf(stderr, "%s: error: illegal option '%c'\n", progname, c);
                show_help();
                exit(1);
            }
        }
    }
    open_log_file(log_name);

    if (ind < argc) {
        dest_url = argv[ind++];
        if (!auto_redirect) {
            fprintf(stderr, "Automatic redirect disabled: argument '%s' ignored\n", dest_url);
            dest_url = NULL;
        }
    }
#endif

    bcg = broadcast_group_alloc();

    /* Default filters (use "-fi" to disable). */
    filter_add_message_filter("o,E,Locator,peerHeartBeat");
    filter_add_message_filter("o,E,Locator,peerAdded");
    filter_add_message_filter("o,E,Locator,peerRemoved");
    filter_add_message_filter("o,E,Locator,peerChanged");
    filter_add_message_filter("o,C,Locator,getPeers");
    filter_add_message_filter("tr10,C,FileSystem,read");
    filter_add_message_filter("t10,C,FileSystem,write");

    set_proxy_log_filter_listener2(filter_is_log_filtered);

    ps = channel_peer_from_url(url);
    if (ps == NULL) {
        fprintf(stderr, "%s: invalid server URL (-s option value): %s\n", progname, url);
        exit(1);
    }
    peer_server_addprop(ps, loc_strdup("Name"), loc_strdup("TCF Protocol Logger"));
    peer_server_addprop(ps, loc_strdup("Proxy"), loc_strdup(""));
    SERVER_ADDPROP_HOOK;
    serv = channel_server(ps);
    if (serv == NULL) {
        fprintf(stderr, "%s: cannot create TCF server: %s\n", progname, errno_to_str(errno));
        exit(1);
    }
    serv->new_conn = channel_new_connection;

    discovery_start();

    if (print_server_properties) {
        ChannelServer * s;
        char * server_properties;
        assert(!list_is_empty(&channel_server_root));
        s = servlink2channelserverp(channel_server_root.next);
        server_properties = channel_peer_to_json(s->ps);
        printf("Server-Properties: %s\n", server_properties);
        fflush(stdout);
        trace(LOG_ALWAYS, "Server-Properties: %s", server_properties);
        loc_free(server_properties);
    }

    /* Process events - must run on the initial thread since ptrace()
     * returns ECHILD otherwise, thinking we are not the owner. */
    run_event_loop();
    return 0;
}
