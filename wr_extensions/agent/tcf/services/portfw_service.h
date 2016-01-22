/*******************************************************************************
 * Copyright (c) 2015, 2016 Wind River Systems, Inc. and others.
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
