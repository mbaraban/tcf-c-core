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
 * Agent main module.
 */

#include <tcf/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <tcf/framework/asyncreq.h>
#include <tcf/framework/events.h>
#include <tcf/framework/errors.h>
#include <tcf/framework/trace.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/exceptions.h>
#include <tcf/framework/channel_tcp.h>
#include <tcf/framework/channel_np.h>
#include <tcf/framework/plugins.h>
#include <tcf/framework/proxy.h>
#include <tcf/services/discovery.h>
#include <tcf/services/streamsservice.h>
#include <tcf/main/test.h>
#include <tcf/main/cmdline.h>
#include <tcf/main/services.h>
#include <tcf/main/server.h>
#include <tcf/main/main_hooks.h>
//#include <tcf/services/device_manager.h>
#include <tcf/services/WR_DeviceManagement_proxy.h>

#if SERVICE_PortForward
#include <tcf/services/portfw_service.h>
#endif

#if SERVICE_PortServer
#include <tcf/services/portfw_proxy.h>
#endif

#ifndef ENABLE_SignalHandlers
#  define ENABLE_SignalHandlers 1
#endif

#ifndef DEFAULT_SERVER_URL
#  define DEFAULT_SERVER_URL "TCP:"
#endif

/* Hook before all TCF initialization.  This hook can add local variables. */
#ifndef PRE_INIT_HOOK
#define PRE_INIT_HOOK do {} while(0)
#endif

/* Hook before any TCF threads are created.  This hook can do
 * initialization that will affect all threads and call most basic TCF
 * functions, like post_event. */
#ifndef PRE_THREADING_HOOK
#define PRE_THREADING_HOOK do {} while(0)
#endif

/* Hook before becoming a daemon process.  This hook can output
 * banners and other information. */
#ifndef PRE_DAEMON_HOOK
#define PRE_DAEMON_HOOK do {} while(0)
#endif

/* Hook to add help text. */
#ifndef HELP_TEXT_HOOK
#define HELP_TEXT_HOOK
#endif

/* Hook for illegal option case.  This hook allows for handling off
 * additional options. */
#ifndef ILLEGAL_OPTION_HOOK
#define ILLEGAL_OPTION_HOOK  do {} while(0)
#endif

/* Signal handler. This hook extends behavior when process
 * exits due to received signal. */
#ifndef SIGNAL_HANDLER_HOOK
#define SIGNAL_HANDLER_HOOK do {} while(0)
#endif

/* Hook run immediatly after option parsing loop. */
#ifndef POST_OPTION_HOOK
#define POST_OPTION_HOOK do {} while(0)
#endif

/* Hook for USAGE string */
#ifndef USAGE_STRING_HOOK
#define USAGE_STRING_HOOK                               \
    "Usage: device [OPTION]...",                         \
    "Start Target Communication Framework agent."
#endif

static const char * progname;
static unsigned int idle_timeout;
static unsigned int idle_count;
static const char * dev_cfg;

static void device_register(const char * dev_cfg);

static void check_idle_timeout(void * args) {
    if (list_is_empty(&channel_root)) {
        idle_count++;
        if (idle_count > idle_timeout) {
            trace(LOG_ALWAYS, "No connections for %d seconds, shutting down", idle_timeout);
            discovery_stop();
            cancel_event_loop();
            return;
        }
    }
    post_event_with_delay(check_idle_timeout, NULL, 1000000);
}

static void channel_closed(Channel *c) {
    /* Reset idle_count if there are short lived connections */
    idle_count = 0;
}

#if ENABLE_SignalHandlers

static void shutdown_event(void * args) {
    discovery_stop();
    cancel_event_loop();
}

static void signal_handler(int sig) {
    SIGNAL_HANDLER_HOOK;
    if (is_dispatch_thread()) {
        discovery_stop();
        signal(sig, SIG_DFL);
        raise(sig);
    }
    else {
        post_event(shutdown_event, NULL);
    }
}

#if defined(_WIN32) || defined(__CYGWIN__)
static LONG NTAPI VectoredExceptionHandler(PEXCEPTION_POINTERS x) {
    if (is_dispatch_thread()) {
        DWORD exception_code = x->ExceptionRecord->ExceptionCode;
        if (exception_code == EXCEPTION_IN_PAGE_ERROR) {
            int error = ERR_OTHER;
            if (x->ExceptionRecord->NumberParameters >= 3) {
                ULONG status = (ULONG)x->ExceptionRecord->ExceptionInformation[2];
                if (status != 0) error = set_nt_status_errno(status);
            }
            str_exception(error, "In page error");
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static BOOL CtrlHandler(DWORD ctrl) {
    switch(ctrl) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        post_event(shutdown_event, NULL);
        return TRUE;
    }
    return FALSE;
}
#endif

#endif /* ENABLE_SignalHandlers */

#if !defined(_WRS_KERNEL)
static const char * help_text[] = {
    USAGE_STRING_HOOK,
    "  -d               run in daemon mode (output is sent to system logger)",
#if ENABLE_Cmdline
    "  -i               run in interactive mode",
#endif
#if ENABLE_RCBP_TEST
    "  -t               run in diagnostic mode",
#endif
    "  -L<file>         enable logging, use -L- to send log to stderr",
#if ENABLE_Trace
    "  -l<level>        set log level, the level is comma separated list of:",
    "@",
#endif
    "  -s<url>          set agent listening port and protocol, default is " DEFAULT_SERVER_URL,
    "  -S               print server properties in Json format to stdout",
    "  -I<idle-seconds> exit if there are no connections for the specified time",
#if ENABLE_Plugins
    "  -P<dir>          set agent plugins directory name",
#endif
#if ENABLE_SSL
    "  -c               generate SSL certificate and exit",
#endif
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
                fprintf(stderr, "      %-12s %s (%#x)\n", tm->name, tm->description, tm->mode);
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
static void device_register_cb(void * dev_cfg) {
    device_register((const char *)dev_cfg);
}

static void device_register(const char * dev_cfg) {
    const char * user = NULL;
    const char * platform = NULL;
    const char * id = NULL;
    char * dev_url = NULL;
    const char * s;
    int err = 0;

    if (strncmp(dev_cfg, "file:", strlen("file:")) == 0) {
        struct stat st;
        const char * dev_file = dev_cfg + strlen("file:");
        FILE * fp = NULL;

        if (stat(dev_file, &st) == 0 &&
            ((fp = fopen(dev_file, "r")) != NULL)) 
            {
            dev_url = loc_alloc_zero(st.st_size + 1);
            if (fgets(dev_url, st.st_size, fp) == NULL || strlen(dev_url) == 0) {
                loc_free(dev_url);
                dev_url = NULL;
            }
            else {
                size_t length = strlen(dev_url);
                while (dev_url[length - 1] == '\n' || dev_url[length - 1] == '\r' || dev_url[length - 1] == ' ') {
                    dev_url[length-1] = '\0';
                    length--;
                }
            }
        }
        if (fp != NULL) fclose(fp);
        if (err) {
            loc_free(dev_url);
            dev_url = NULL;
        }
    }
    else dev_url = loc_strdup(dev_cfg);

    if (dev_url != NULL && strlen(dev_url) == 0) {
        loc_free(dev_url);
        dev_url = NULL;
    }

    if (dev_url == NULL) {
        post_event_with_delay(device_register_cb, (void *)dev_cfg, 1000000);
        return;
    }

    s = (const char *) dev_url;
    while (*s) {
        while (*s && *s != ';') s++;
        if (*s == ';') {
            char * name;
            char * value;
            const char * url;
            s++;
            url = s;
            while (*s && *s != '=') s++;
            if (*s != '=' || s == url) {
                s = url - 1;
                break;
            }
            name = loc_strndup(url, s - url);
            s++;
            url = s;
            while (*s && *s != ';') s++;
            value = loc_strndup(url, s - url);
            if (strcmp(name, "User") == 0) {
                user = value;
            } else if (strcmp(name, "Platform") == 0) {
                platform = value;
            } else if (strcmp(name, "ID") == 0) {
                id = value;
            } else loc_free(value);
            loc_free(name);
        }
    }
    deviceRegister(dev_url, user, id, platform);
    loc_free(user);
    loc_free(id);
    loc_free(platform);
    loc_free(dev_url);
}

static void channel_redirection_listener(Channel * host, Channel * target) {
    if (target->state == ChannelStateConnected) {
#if SERVICE_PortForward
        ini_portfw_remote_config_service(target->protocol, NULL);
#endif

#if SERVICE_PortServer
        {
        unsigned int i;
        int service_id = 0;
        const size_t len = strlen("PortServer");
        for (i = 0; i < (unsigned int)target->peer_service_cnt; i++) {
            char * nm = target->peer_service_list[i];
            if (strncmp(nm, "PortServer", len) == 0) {
                if (nm[len] == '\0') {
                    if (service_id == 0) service_id = 1;
                    continue;
                }
                if (nm[len] == '@') {
                    char *endptr;
                    int id;
                    id = (int) strtol(nm + len + 1, &endptr, 10);
                    if (nm + len + 1 < endptr && *endptr == '\0') {
                        if (service_id <= id)
                            service_id = id + 1;
                        continue;
                    }
                }
            }
        }
        ini_portforward_server(service_id, host->protocol, NULL);
        ini_streams_service (target->protocol);
        }
#endif
    }
}

#if defined(_WRS_KERNEL)
int tcf(void);
int tcf(void) {
#else
int main(int argc, char ** argv) {
    int c;
    int ind;
    int daemon = 0;
    const char * log_name = NULL;
    const char * log_level = NULL;
#endif
#if ENABLE_WebSocket_SOCKS_V5
    const char * socks_v5_proxy = NULL;
#endif
    int interactive = 0;
    int print_server_properties = 0;
    const char * url = NULL;
    Protocol * proto;
    TCFBroadcastGroup * bcg;

    PRE_INIT_HOOK;
    ini_mdep();
    ini_trace();
    ini_events_queue();
    ini_asyncreq();
    PRE_THREADING_HOOK;

#if defined(_WRS_KERNEL)

    progname = "tcf";
    open_log_file("-");
    log_mode = 0;

#else
    /* Unset any LD_PRELOAD environment variable that may have been set
     * to start the device binary (tsocks for example). We do not want the
     * spawned processes to inherit this environment variable.
     */
    putenv ("LD_PRELOAD=");

    progname = argv[0];

    /* Parse arguments */
    for (ind = 1; ind < argc; ind++) {
        char * s = argv[ind];
        if (*s++ != '-') break;
        while (s && (c = *s++) != '\0') {
            switch (c) {
            case 'i':
                interactive = 1;
                break;

            case 't':
#if ENABLE_RCBP_TEST
                test_proc();
#endif
                exit(0);
                break;

            case 'd':
#if defined(_WIN32) || defined(__CYGWIN__)
                /* For Windows the only way to detach a process is to
                 * create a new process, so we patch the -d option to
                 * -D for the second time we get invoked so we don't
                 * keep on creating new processes forever. */
                s[-1] = 'D';
                daemon = 2;
                break;

            case 'D':
#endif
                daemon = 1;
                break;

            case 'c':
                generate_ssl_certificate();
                exit(0);
                break;

            case 'S':
                print_server_properties = 1;
                break;

            case 'h':
                show_help();
                exit(0);

            case 'I':
#if ENABLE_Trace
            case 'l':
#endif
            case 'L':
            case 's':
#if ENABLE_WebSocket_SOCKS_V5
            case 'p':
#endif
#if ENABLE_Plugins
            case 'P':
#endif
                if (*s == '\0') {
                    if (++ind >= argc) {
                        fprintf(stderr, "%s: error: no argument given to option '%c'\n", progname, c);
                        exit(1);
                    }
                    s = argv[ind];
                }
                switch (c) {
                case 'I':
                    idle_timeout = strtol(s, 0, 0);
                    break;

#if ENABLE_Trace
                case 'l':
                    log_level = s;
                    parse_trace_mode(log_level, &log_mode);
                    break;
#endif

                case 'L':
                    log_name = s;
                    break;

                case 's':
                    url = s;
                    break;

#if ENABLE_Plugins
                case 'P':
                    plugins_path = s;
                    break;
#endif
#if ENABLE_WebSocket_SOCKS_V5
                case 'p':
                    socks_v5_proxy = s;
                    break;
#endif
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

    if (ind < argc) {
        dev_cfg = argv[ind++];
    }

    POST_OPTION_HOOK;
    if (daemon) {
#if defined(_WIN32) || defined(__CYGWIN__)
        become_daemon(daemon > 1 ? argv : NULL);
#else
        become_daemon();
#endif
    }
    open_log_file(log_name);

#endif

    bcg = broadcast_group_alloc();
    proto = protocol_alloc();

    /* The static services must be initialised before the plugins */
#if ENABLE_Cmdline
    if (interactive) ini_cmdline_handler(interactive, proto);
#else
    if (interactive) fprintf(stderr, "Warning: This version does not support interactive mode.\n");
#endif

#if ENABLE_WebSocket
#if ENABLE_WebSocket_SOCKS_V5
    if (socks_v5_proxy != NULL) {
        if (parse_socks_v5_proxy(socks_v5_proxy) != 0) {
            fprintf(stderr, "Cannot parse SOCKS V5 proxy string: %s\n", socks_v5_proxy);
            exit(1);
        }
    }
#endif
    ini_np_channel();
#endif
    ini_services(proto, bcg);
#if SERVICE_PortForward
    ini_portfw_remote_config_service(proto, bcg);
#endif
#if SERVICE_PortServer
    ini_portforward_server(0, proto, NULL);
#endif
    ini_device_mgt_lib(proto, bcg, 1, 10, 10);

#if !defined(_WRS_KERNEL)
    /* Reparse log level in case initialization cause additional
     * levels to be registered */
    if (log_level != NULL && parse_trace_mode(log_level, &log_mode) != 0) {
        fprintf(stderr, "Cannot parse log level: %s\n", log_level);
        exit(1);
    }
#endif
    if (url != NULL) {
        if (ini_server(url, proto, bcg) < 0) {
            fprintf(stderr, "Cannot create TCF server: %s\n", errno_to_str(errno));
            exit(1);
        }
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
    }

    if (dev_cfg) device_register(dev_cfg);

    PRE_DAEMON_HOOK;
#if !defined(_WRS_KERNEL)
    if (daemon)
        close_out_and_err();
#endif

#if ENABLE_SignalHandlers
    signal(SIGABRT, signal_handler);
    signal(SIGILL, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#if defined(_WIN32) || defined(__CYGWIN__)
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlHandler, TRUE);
    AddVectoredExceptionHandler(1, VectoredExceptionHandler);
#endif
#endif /* ENABLE_SignalHandlers */

    if (idle_timeout != 0) {
        add_channel_close_listener(channel_closed);
        check_idle_timeout(NULL);
    }
    add_channel_redirection_listener(channel_redirection_listener);

    /* Process events - must run on the initial thread since ptrace()
     * returns ECHILD otherwise, thinking we are not the owner. */
    run_event_loop();

#if ENABLE_Plugins
    plugins_destroy();
#endif /* ENABLE_Plugins */

    return 0;
}
