
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <lo/lo.h>

#include "mapper_internal.h"
#include "types_internal.h"
#include <mapper/mapper.h>

mapper_router mapper_router_new(mapper_device device, const char *host,
                                int port, const char *name)
{
    char str[16];
    mapper_router router = (mapper_router) calloc(1, sizeof(struct _mapper_router));
    sprintf(str, "%d", port);
    router->addr = lo_address_new(host, str);
    router->dest_name = strdup(name);
    router->device = device;

    if (!router->addr) {
        mapper_router_free(router);
        return 0;
    }
    return router;
}

void mapper_router_free(mapper_router router)
{
    if (router) {
        if (router->addr)
            lo_address_free(router->addr);
        if (router->mappings) {
            mapper_signal_mapping sm = router->mappings;
            while (sm) {
                mapper_signal_mapping tmp = sm->next;
                if (sm->mapping) {

                    mapper_mapping m = sm->mapping;
                    while (m) {
                        mapper_mapping tmp = m->next;
                        if (tmp->props.src_name)
                            free(tmp->props.src_name);
                        if (tmp->props.dest_name)
                            free(tmp->props.dest_name);
                        free(m);
                        m = tmp;
                    }
                }
                free(sm);
                sm = tmp;
            }
        }
        free(router);
    }
}

void mapper_router_receive_signal(mapper_router router, mapper_signal sig,
                                  mapper_signal_value_t *value)
{
    // find this signal in list of mappings
    mapper_signal_mapping sm = router->mappings;
    while (sm && sm->signal != sig)
        sm = sm->next;

    // exit without failure if signal is not mapped
    if (!sm) {
        return;
    }
    // for each mapping, construct a mapped signal and send it
    mapper_mapping m = sm->mapping;
    while (m) {
        struct _mapper_signal signal;
        signal.props.name = m->props.dest_name;
        signal.props.type = m->props.dest_type;
        signal.props.length = m->props.dest_length;

        mapper_signal_value_t applied[signal.props.length];
        int i=0;
        int s=4;
        void *p = value;

        /* Currently expressions on vectors are not supported by the
         * evaluator.  For now, we half-support it by performing
         * element-wise operations on each item in the vector. */
        if (signal.props.type == 'i')
            s = sizeof(int);
        else if (signal.props.type == 'f')
            s = sizeof(float);

        for (i=0; i < signal.props.length; i++) {
            mapper_signal_value_t v, w;
            if (mapper_mapping_perform(m, sig, p, &v))
            {
                if (mapper_clipping_perform(m, &v, &w))
                    applied[i] = w;
                else
                    break;
            }
            else
                break;
            p += s;
        }
        if (i == signal.props.length)
            mapper_router_send_signal(router, &signal, applied);
        m = m->next;
    }
}

void mapper_router_send_signal(mapper_router router, mapper_signal sig,
                               mapper_signal_value_t *value)
{
    int i;
    lo_message m;
    if (!router->addr)
        return;

    m = lo_message_new();
    if (!m)
        return;

    for (i = 0; i < sig->props.length; i++)
        mval_add_to_message(m, sig, &value[i]);

    lo_send_message(router->addr, sig->props.name, m);
    lo_message_free(m);
    return;
}

int mapper_router_send_query(mapper_router router, mapper_signal sig,
                                const char *alias)
{
    // find this signal in list of mappings
    mapper_signal_mapping sm = router->mappings;
    while (sm && sm->signal != sig)
        sm = sm->next;

    // exit without failure if signal is not mapped
    if (!sm) {
        return 0;
    }
    // for each mapping, query the remote signal
    mapper_mapping m = sm->mapping;
    int count = 0;
    char query_string[1024];
    while (m) {
        strncpy(query_string, m->props.dest_name, 1024);
        strncat(query_string, "/get", 1024);
        if (alias) {
            lo_send_from(router->addr, router->device->server, 
                         LO_TT_IMMEDIATE, query_string, "s", alias);
        }
        else {
            lo_send_from(router->addr, router->device->server, 
                         LO_TT_IMMEDIATE, query_string, "");
        }
        count++;
        m = m->next;
    }
    return count;
}

mapper_mapping mapper_router_add_mapping(mapper_router router,
                                         mapper_signal sig,
                                         const char *dest_name,
                                         char dest_type,
                                         int dest_length)
{
    /* Currently, fail is lengths don't match.  TODO: In the future,
     * we'll have to examine the expresion to see if its input and
     * output lengths are compatible. */
    if (sig->props.length != dest_length) {
        char n[1024];
        msig_full_name(sig, n, 1024);
        trace("rejecting mapping %s -> %s%s because lengths don't match (not yet supported)\n",
              n, router->dest_name, dest_name);
        return 0;
    }

    mapper_mapping mapping = (mapper_mapping) calloc(1, sizeof(struct _mapper_mapping));
    
    mapping->props.src_name = strdup(sig->props.name);
    mapping->props.src_type = sig->props.type;
    mapping->props.src_length = sig->props.length;
    mapping->props.dest_name = strdup(dest_name);
    mapping->props.dest_type = dest_type;
    mapping->props.dest_length = dest_length;
    mapping->props.mode = MO_UNDEFINED;
    mapping->props.expression = strdup("y=x");
    mapping->props.clip_min = CT_NONE;
    mapping->props.clip_max = CT_NONE;
    mapping->props.muted = 0;
    
    // find signal in signal mapping list
    mapper_signal_mapping sm = router->mappings;
    while (sm && sm->signal != sig)
        sm = sm->next;

    // if not found, create a new list entry
    if (!sm) {
        sm = (mapper_signal_mapping)
            calloc(1, sizeof(struct _mapper_signal_mapping));
        sm->signal = sig;
        sm->next = router->mappings;
        router->mappings = sm;
    }
    // add new mapping to this signal's list
    mapping->next = sm->mapping;
    sm->mapping = mapping;
    
    return mapping;
}

int mapper_router_remove_mapping(mapper_router router, 
								 mapper_mapping mapping)
{
    // find signal in signal mapping list
    mapper_signal_mapping sm = router->mappings;
    while (sm) {
        mapper_mapping *m = &sm->mapping;
        while (*m) {
            if (*m == mapping) {
                *m = mapping->next;
                free(mapping);
                return 0;
            }
            m = &(*m)->next;
        }
        sm = sm->next;
    }
    return 1;
}

mapper_router mapper_router_find_by_dest_name(mapper_router router,
                                                const char* dest_name)
{
    int n = strlen(dest_name);
    const char *slash = strchr(dest_name+1, '/');
    if (slash)
        n = slash - dest_name;

    while (router) {
        if (strncmp(router->dest_name, dest_name, n)==0)
            return router;
        router = router->next;
    }
    return 0;
}
