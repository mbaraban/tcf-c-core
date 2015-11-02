/*
 * Copyright (c) 2014 Wind River Systems, Inc.
 *
 * The right to copy, distribute, modify or otherwise make use
 * of this software may be licensed only pursuant to the terms
 * of an applicable Wind River license agreement.
 */
/*
 * This module implements port forwarding service.
 */

#ifndef D_port_fw
#define D_port_fw

#include <tcf/framework/context.h>
#include <tcf/framework/protocol.h>

#if !defined(ENABLE_PortForwardProxy)
#  define ENABLE_PortForwardProxy   0
#endif
#if !defined(SERVICE_PortServer)
#  define SERVICE_PortServer        0
#endif

#if ENABLE_PortForwardProxy
typedef struct PortServer PortServer;

typedef void (*PortConnectCallback)(struct PortServer * /* server */, void * /* hook data */);
typedef void (*PortDisconnectCallback)(struct PortServer * server/* server */, void * /* hook data */);
typedef void (*PortRecvCallback)(struct PortServer * server/* server */, char * /* buffer */, size_t * /* size */, size_t /* buffer size */, void * /* hook data */);


typedef struct PortAttribute {
    struct PortAttribute * next;
    char * name;        /* Attribute name */
    char * value;       /* Attribute value as JSON string */
} PortAttribute;

typedef struct PortRedirectionInfo {
    int target_port;                        /* target port number */
    int local_port;                         /* target port number */
    int auto_connect;                       /* auto connect to target port? */
    int auto_connect_period;                /* auto connect period in seconds */
    Channel * c;                            /* channel to use for port redirection */
    PortAttribute * attrs;
    PortConnectCallback connect_callback;         /* connect hook */
    PortDisconnectCallback disconnect_callback;   /* disconnect hook */
    PortRecvCallback recv_callback;               /* receive hook */
    void * callback_data;                         /* callback data */
} PortRedirectionInfo;


/* Create a port redirection. The PortRedirectionInfo and its attributes
 * must have been allocated through the loc_xxx TCF apis. This structure
 * will be automatically freed when the port redirection is deleted; it is
 * not the role of the caller to free it.
 */
extern PortServer * create_port_redirection(PortRedirectionInfo * port);
#if defined(SERVICE_PortServer)
extern void ini_portforward_server(int service_id, Protocol *proto, TCFBroadcastGroup * bcg);
#endif
#endif

#endif /* D_port_fw */
