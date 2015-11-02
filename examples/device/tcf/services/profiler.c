/*******************************************************************************
 * Copyright (c) 2013, 2014 Xilinx, Inc. and others.
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
 *     Xilinx - initial API and implementation
 *******************************************************************************/

#include <tcf/config.h>

#if SERVICE_Profiler

#include <stdio.h>
#include <assert.h>
#include <tcf/framework/json.h>
#include <tcf/framework/context.h>
#include <tcf/framework/exceptions.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/cache.h>
#include <tcf/services/runctrl.h>
#include <tcf/services/symbols.h>
#include <tcf/services/linenumbers.h>
#include <tcf/services/memorymap.h>
#include <tcf/services/profiler.h>

typedef struct {
    LINK link_cfg;
    ProfilerClass * cls;
    void * obj;
} ProfilerInstance;

typedef struct {
    LINK link_all;
    char id[256];
    unsigned params_cnt;
    ProfilerParams params;
    LINK list; /* List of ProfilerInstance */
} ProfilerConfiguration;

static const char * PROFILER = "Profiler";


typedef struct cpu_stat {
    long unsigned int cpu_total_time;
    long unsigned int cpu_idle_time;
    long unsigned int cpu_system_time;
    long unsigned int cpu_irq_time;
    long unsigned int cpu_time[10];
} cpu_stat;

typedef struct mem_info {
    long unsigned int mem_total;
    long unsigned int mem_free;
    long unsigned int buffers;
    long unsigned int cached;
    long unsigned int active;
    long unsigned int inactive;

    long unsigned int swap_total;
    long unsigned int swap_free;
    long unsigned int swap_cached;
} mem_info;

static cpu_stat cstat, last_stat;
static long ncores;
static long freq;
static unsigned long multiplier = 1;
static unsigned long divider = 1;
static mem_info minfo;

void add_profiler(Context * ctx, ProfilerClass * cls) {
}

static int get_usage(struct cpu_stat* result) {
    FILE *fstat = fopen("/proc/stat", "r");
    long unsigned int cpu_time[10];
    int i;
    if (fstat == NULL) {
        perror("FOPEN ERROR ");
        fclose(fstat);
        return -1;
    }
    bzero(cpu_time, sizeof(cpu_time));
    if (fscanf(fstat, "%*s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu", &result->cpu_time[0], &result->cpu_time[1], &result->cpu_time[2], &result->cpu_time[3], &result->cpu_time[4], &result->cpu_time[5], &result->cpu_time[6], &result->cpu_time[7], &result->cpu_time[8], &result->cpu_time[9]) == EOF) {
        fclose(fstat);
        return -1;
    }
    fclose(fstat);
    result->cpu_idle_time = result->cpu_time[3] / ncores;
    result->cpu_system_time = result->cpu_time[2] / ncores;
    result->cpu_irq_time = result->cpu_time[5] / ncores;
    result->cpu_total_time = 0;
    for(i=0; i < 10;i++)
        result->cpu_total_time += result->cpu_time[i];
    result->cpu_total_time /= ncores;
    return 0;
}

static int get_meminfo(struct mem_info* result) {
    char name[30];
    long unsigned int value;
    int i;

    FILE *f = fopen("/proc/meminfo", "r");
    if (f == NULL) {
        perror("FOPEN ERROR ");
        fclose(f);
        return -1;
    }

    do {
        bzero(name, sizeof(name));
        value = -1;
        i = fscanf(f, "%s %lu %*s", name, &value);
        if (i != EOF && strlen(name) > 0 && value >= 0) {
            value = value * 1024;
            if (strcmp(name, "MemTotal:") == 0) {
                result->mem_total = value;
            }
            else if (strcmp(name, "MemFree:") == 0) {
                result->mem_free = value;
            }
            else if (strcmp(name, "Buffers:") == 0) {
                result->buffers = value;
            }
            else if (strcmp(name, "Cached:") == 0) {
                result->cached = value;
            }
            else if (strcmp(name, "Active:") == 0) {
                result->active = value;
            }
            else if (strcmp(name, "Inactive:") == 0) {
                result->inactive = value;
            }
            else if (strcmp(name, "SwapTotal:") == 0) {
                result->swap_total = value;
            }
            else if (strcmp(name, "SwapFree:") == 0) {
                result->swap_free = value;
            }
            else if (strcmp(name, "SwapCached:") == 0) {
                result->swap_cached = value;
            }
        }
    } while (i != EOF);
    fclose(f);

    return 0;
}

static void profiler_read(OutputStream * out) {
    static int initialized = 0;
    static unsigned long system_ticks, irq_ticks, cpu_ticks, idle_ticks;
    if (!initialized) {
        get_usage(&last_stat);
        initialized = 1;
    }
    get_usage(&cstat);

    system_ticks = (cstat.cpu_system_time - last_stat.cpu_system_time);
    irq_ticks = cstat.cpu_irq_time - last_stat.cpu_irq_time;
    idle_ticks = cstat.cpu_idle_time - last_stat.cpu_idle_time;
    cpu_ticks = cstat.cpu_total_time - last_stat.cpu_total_time;

    irq_ticks = (irq_ticks * multiplier) / divider;
    cpu_ticks = (cpu_ticks * multiplier) / divider;
    system_ticks = (system_ticks * multiplier) / divider;
    idle_ticks = (idle_ticks * multiplier) / divider;

    write_stream(out, '{');
    json_write_string(out, "Format");
    write_stream(out, ':');
    json_write_string(out, "Spy");
    write_stream(out, ',');
    json_write_string(out, "KernelTotalTicks");
    write_stream(out, ':');
    json_write_ulong(out, system_ticks);
    write_stream(out, ',');
    json_write_string(out, "InterruptTotalTicks");
    write_stream(out, ':');
    json_write_ulong(out, irq_ticks);
    write_stream(out, ',');
    json_write_string(out, "IdleTotalTicks");
    write_stream(out, ':');
    json_write_ulong(out, idle_ticks);
    write_stream(out, ',');
    json_write_string(out, "TotalTicks");
    write_stream(out, ':');
    json_write_ulong(out, cpu_ticks);
    write_stream(out, '}');
}

static void meminfo_read(OutputStream * out) {
    get_meminfo(&minfo);

    write_stream(out, '{');
    json_write_string(out, "Format");
    write_stream(out, ':');
    json_write_string(out, "MemInfo");
    write_stream(out, ',');
    json_write_string(out, "MemTotal");
    write_stream(out, ':');
    json_write_ulong(out, minfo.mem_total);
    write_stream(out, ',');
    json_write_string(out, "MemFree");
    write_stream(out, ':');
    json_write_ulong(out, minfo.mem_free);
    write_stream(out, ',');
    json_write_string(out, "Buffers");
    write_stream(out, ':');
    json_write_ulong(out, minfo.buffers);
    write_stream(out, ',');
    json_write_string(out, "Cached");
    write_stream(out, ':');
    json_write_ulong(out, minfo.cached);
    write_stream(out, ',');
    json_write_string(out, "Active");
    write_stream(out, ':');
    json_write_ulong(out, minfo.active);
    write_stream(out, ',');
    json_write_string(out, "Inactive");
    write_stream(out, ':');
    json_write_ulong(out, minfo.inactive);
    write_stream(out, ',');
    json_write_string(out, "SwapTotal");
    write_stream(out, ':');
    json_write_ulong(out, minfo.swap_total);
    write_stream(out, ',');
    json_write_string(out, "SwapFree");
    write_stream(out, ':');
    json_write_ulong(out, minfo.swap_free);
    write_stream(out, ',');
    json_write_string(out, "SwapCached");
    write_stream(out, ':');
    json_write_ulong(out, minfo.swap_cached);
    write_stream(out, '}');
}

static void read_cfg_param(InputStream * inp, const char * name, void * x) {
    ByteArrayInputStream buf;
    ProfilerConfiguration * cfg = (ProfilerConfiguration *)x;
    ProfilerParameter * p = (ProfilerParameter *)loc_alloc_zero(sizeof(ProfilerParameter));

    p->name = loc_strdup(name);
    p->value = json_read_object(inp);
    p->next = cfg->params.list;
    cfg->params.list = p;
    cfg->params_cnt++;

    if (strcmp(name, "FrameCnt") == 0) {
        inp = create_byte_array_input_stream(&buf, p->value, strlen(p->value));
        cfg->params.frame_cnt = json_read_ulong(inp);
    }
    else if (strcmp(name, "MaxSamples") == 0) {
        inp = create_byte_array_input_stream(&buf, p->value, strlen(p->value));
        cfg->params.max_samples = json_read_ulong(inp);
    }
}

static void free_params(ProfilerParams * params) {
    while (params->list != NULL) {
        ProfilerParameter * p = params->list;
        params->list = p->next;
        loc_free(p->name);
        loc_free(p->value);
        loc_free(p);
    }
}

static void command_get_capabilities(char * token, Channel * c) {
    char id[256];
    int error = 0;

    json_read_string(&c->inp, id, sizeof(id));
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);

    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, error);
    write_stream(&c->out, '{');

    json_write_string(&c->out, "CpuProfiler");
    write_stream(&c->out, ':');
    json_write_boolean(&c->out, 1);
    write_stream(&c->out, ',');

    json_write_string(&c->out, "MemMonitor");
    write_stream(&c->out, ':');
    json_write_boolean(&c->out, 1);

    write_stream(&c->out, '}');
    write_stream(&c->out, 0);
    write_stream(&c->out, MARKER_EOM);
}

static void command_configure(char * token, Channel * c) {
    char id[256];
    ProfilerConfiguration * cfg = (ProfilerConfiguration *)loc_alloc_zero(sizeof(ProfilerConfiguration));

    json_read_string(&c->inp, id, sizeof(id));
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    json_read_struct(&c->inp, read_cfg_param, cfg);
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);
    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, 0);
    write_stream(&c->out, MARKER_EOM);
    free_params(&cfg->params);
    loc_free(cfg);
}

static void command_read(char * token, Channel * c) {
    int error = 0;
    char id[256];

    json_read_string(&c->inp, id, sizeof(id));
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);

    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, error);
    if (error) {
        write_stringz(&c->out, "null");
    }
    else {
        write_stream(&c->out, '[');
        profiler_read(&c->out);
        write_stream(&c->out, ',');
        meminfo_read(&c->out);
        write_stream(&c->out, ']');
        write_stream(&c->out, MARKER_EOA);
    }
    write_stream(&c->out, MARKER_EOM);
}

void ini_profiler_service(Protocol * proto) {
    freq = (int) sysconf(_SC_CLK_TCK) ;
    ncores = (int) sysconf(_SC_NPROCESSORS_ONLN);

    /* Client expect a frequency of 100hz */

    if (freq == 100) {
        divider = 1;
        multiplier = 1;
    }
    else if (freq > 100) {
        divider = freq;
        multiplier = 100;
    }
    else {
        multiplier = freq;
        divider = 100;
    }

    add_command_handler(proto, PROFILER, "getCapabilities", command_get_capabilities);
    add_command_handler(proto, PROFILER, "configure", command_configure);
    add_command_handler(proto, PROFILER, "read", command_read);
}

#endif /* SERVICE_Profiler */
