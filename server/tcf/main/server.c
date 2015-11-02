/*******************************************************************************
 * Copyright (c) 2007, 2015 Wind River Systems, Inc. and others.
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
 * Server initialization code.
 */

#include <tcf/config.h>

#include <tcf/framework/exceptions.h>
#include <tcf/framework/json.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/proxy.h>
#if SERVICE_Expressions
#include <tcf/services/expressions.h>
#endif
#include <tcf/services/linenumbers.h>
#include <tcf/services/symbols.h>
#include <tcf/services/pathmap.h>
#include <tcf/services/disassembly.h>
#include <tcf/services/context-proxy.h>
#include <tcf/main/server.h>
#include <tcf/main/server_hooks.h>

#include <assert.h>

/* Hook when checking target service. */
#ifndef TARGET_SERVICE_CHECK_HOOK
#define TARGET_SERVICE_CHECK_HOOK do {} while(0)
#endif

/* Hook for adding properties */
#ifndef SERVER_ADDPROP_HOOK
#define SERVER_ADDPROP_HOOK do {} while(0)
#endif

#ifndef PROXY_NAME
#define PROXY_NAME "TCF Proxy"
#endif

static Protocol * proto;
static TCFBroadcastGroup * bcg;

static void channel_new_connection(ChannelServer * serv, Channel * c) {
    protocol_reference(proto);
    c->protocol = proto;
    channel_set_broadcast_group(c, bcg);
    channel_start(c);
}

static void channel_redirection_listener(Channel * host, Channel * target) {
    if (target->state == ChannelStateStarted) {
#if SERVICE_LineNumbers
        ini_line_numbers_service(target->protocol);
#endif
#if SERVICE_Symbols
        ini_symbols_service(target->protocol);
#endif
    }
    if (target->state == ChannelStateConnected) {
        int i;
#if SERVICE_LineNumbers
        int service_ln = 0;
#endif
#if SERVICE_PathMap
#  if ENABLE_DebugContext && ENABLE_ContextProxy
        int service_pm = 0;
#  endif
#endif
#if SERVICE_Symbols
        int service_sm = 0;
#endif
#if SERVICE_Disassembly
        int service_da = 0;
#endif
#if ENABLE_DebugContext && ENABLE_ContextProxy
        int forward_pm = 0;
#endif
        for (i = 0; i < target->peer_service_cnt; i++) {
            char * nm = target->peer_service_list[i];
            /* Added this line to avoid build warnings if none of the
             * services below are defined (note that nm may be used
             * in TARGET_SERVICE_CHECK_HOOK() macro). */
            if (nm);
#if SERVICE_LineNumbers
            if (strcmp(nm, "LineNumbers") == 0) service_ln = 1;
#endif
#if SERVICE_Symbols
            if (strcmp(nm, "Symbols") == 0) service_sm = 1;
#endif
#if SERVICE_PathMap
#  if ENABLE_DebugContext && ENABLE_ContextProxy
            if (strcmp(nm, "PathMap") == 0) service_pm = 1;
#  endif
#endif
#if SERVICE_Disassembly
            if (strcmp(nm, "Disassembly") == 0) service_da = 1;
#endif
            TARGET_SERVICE_CHECK_HOOK;
        }
#if SERVICE_PathMap
        ini_path_map_service(host->protocol, bcg);
#  if ENABLE_DebugContext && ENABLE_ContextProxy
        if (service_pm) forward_pm = 1;
#  endif
#endif
#if SERVICE_LineNumbers
        if (!service_ln) ini_line_numbers_service(host->protocol);
#endif
#if SERVICE_Symbols
        if (!service_sm) ini_symbols_service(host->protocol);
#endif
#if SERVICE_Disassembly
        if (!service_da) ini_disassembly_service(host->protocol);
#endif
#if SERVICE_Expressions
        ini_expressions_service(host->protocol);
#endif
#if ENABLE_DebugContext && ENABLE_ContextProxy
        create_context_proxy(host, target, forward_pm);
#endif
    }
}

int ini_server(const char * url, Protocol * p, TCFBroadcastGroup * b) {
    ChannelServer * serv = NULL;
    PeerServer * ps = NULL;
    Trap trap;

    if (!set_trap(&trap)) {
        bcg = NULL;
        proto = NULL;
        if (ps != NULL) peer_server_free(ps);
        errno = trap.error;
        return -1;
    }

    bcg = b;
    proto = p;
    ps = channel_peer_from_url(url);
    if (ps == NULL) str_exception(ERR_OTHER, "Invalid server URL");
    peer_server_addprop(ps, loc_strdup("Name"), loc_strdup(PROXY_NAME));
    peer_server_addprop(ps, loc_strdup("Proxy"), loc_strdup(""));
    SERVER_ADDPROP_HOOK;
    serv = channel_server(ps);
    if (serv == NULL) exception(errno);
    serv->new_conn = channel_new_connection;

    clear_trap(&trap);
    add_channel_redirection_listener(channel_redirection_listener);
    return 0;
}
