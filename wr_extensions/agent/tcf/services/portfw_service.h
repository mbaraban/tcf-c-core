/*
 * Copyright (c) 2011-2014 Wind River Systems, Inc.
 *
 * The right to copy, distribute, modify or otherwise make use
 * of this software may be licensed only pursuant to the terms
 * of an applicable Wind River license agreement.
 */

#ifndef D_portfw_service
#define D_portfw_service

#include <tcf/framework/protocol.h>   /* Protocol     */

#if !defined(SERVICE_PortForward)
#  define SERVICE_PortForward   0
#endif

#if SERVICE_PortForward
extern void ini_portfw_remote_config_service(Protocol *proto, TCFBroadcastGroup * bcg);
#endif

#endif
