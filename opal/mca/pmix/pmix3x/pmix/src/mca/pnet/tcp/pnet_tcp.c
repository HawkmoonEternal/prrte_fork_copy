/*
 * Copyright (c) 2018      Intel, Inc. All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>

#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <time.h>

#include <pmix_common.h>

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_globals.h"
#include "src/class/pmix_list.h"
#include "src/util/alfg.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/parse_options.h"
#include "src/util/pmix_environ.h"
#include "src/mca/preg/preg.h"

#include "src/mca/pnet/base/base.h"
#include "pnet_tcp.h"

#define PMIX_TCP_SETUP_APP_KEY  "pmix.tcp.setup.app.key"

static pmix_status_t tcp_init(void);
static void tcp_finalize(void);
static pmix_status_t allocate(pmix_nspace_t *nptr,
                              pmix_info_t *info,
                              pmix_list_t *ilist);
static pmix_status_t setup_local_network(pmix_nspace_t *nptr,
                                         pmix_info_t info[],
                                         size_t ninfo);
static pmix_status_t setup_fork(pmix_nspace_t *nptr,
                                const pmix_proc_t *peer, char ***env);
static void child_finalized(pmix_peer_t *peer);
static void local_app_finalized(pmix_nspace_t *nptr);
static void deregister_nspace(pmix_nspace_t *nptr);
static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       pmix_info_cbfunc_t cbfunc, void *cbdata);

pmix_pnet_module_t pmix_tcp_module = {
    .init = tcp_init,
    .finalize = tcp_finalize,
    .allocate = allocate,
    .setup_local_network = setup_local_network,
    .setup_fork = setup_fork,
    .child_finalized = child_finalized,
    .local_app_finalized = local_app_finalized,
    .deregister_nspace = deregister_nspace,
    .collect_inventory = collect_inventory
};

/* local tracker objects */
typedef struct {
    pmix_list_item_t super;
    char *type;
    char *plane;
    char **ports;
    size_t nports;
} tcp_available_ports_t;

typedef struct {
    pmix_list_item_t super;
    char *nspace;
    char **ports;
    tcp_available_ports_t *src;  // source of the allocated ports
} tcp_port_tracker_t;


static pmix_list_t allocations, available;
static pmix_status_t process_request(pmix_nspace_t *nptr,
                                     char *idkey, int ports_per_node,
                                     tcp_port_tracker_t *trk,
                                     pmix_list_t *ilist);

static void tacon(tcp_available_ports_t *p)
{
    p->type = NULL;
    p->plane = NULL;
    p->ports = NULL;
    p->nports = 0;
}
static void tades(tcp_available_ports_t *p)
{
    if (NULL != p->type) {
        free(p->type);
    }
    if (NULL != p->plane) {
        free(p->plane);
    }
    if (NULL != p->ports) {
        pmix_argv_free(p->ports);
    }
}
static PMIX_CLASS_INSTANCE(tcp_available_ports_t,
                           pmix_list_item_t,
                           tacon, tades);

static void ttcon(tcp_port_tracker_t *p)
{
    p->nspace = NULL;
    p->ports = NULL;
    p->src = NULL;
}
static void ttdes(tcp_port_tracker_t *p)
{
    size_t n, m, mstart;

    if (NULL != p->nspace) {
        free(p->nspace);
    }
    if (NULL != p->src) {
        if (NULL != p->ports) {
            mstart = 0;
            for (n=0; NULL != p->ports[n]; n++) {
                /* find an empty position */
                for (m=mstart; m < p->src->nports; m++) {
                    if (NULL == p->src->ports[m]) {
                        p->src->ports[m] = strdup(p->ports[n]);
                        mstart = m + 1;
                        break;
                    }
                }
            }
            pmix_argv_free(p->ports);
        }
        PMIX_RELEASE(p->src);  // maintain accounting
    } else if (NULL != p->ports) {
        pmix_argv_free(p->ports);
    }
}
static PMIX_CLASS_INSTANCE(tcp_port_tracker_t,
                           pmix_list_item_t,
                           ttcon, ttdes);

static pmix_status_t tcp_init(void)
{
    tcp_available_ports_t *trk;
    char *p, **grps;
    size_t n;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: tcp init");

    /* if we are not the "gateway", then there is nothing
     * for us to do */
    if (!PMIX_PROC_IS_GATEWAY(pmix_globals.mypeer)) {
        return PMIX_SUCCESS;
    }

    PMIX_CONSTRUCT(&allocations, pmix_list_t);
    PMIX_CONSTRUCT(&available, pmix_list_t);

    /* if we have no static ports, then disqualify ourselves
     * as there is nothing for us to manage
     *
     * NOTE: need to check inventory in addition to MCA param as
     * the inventory may have reported back static ports */
    if (NULL == mca_pnet_tcp_component.static_ports) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* split on semi-colons */
    grps = pmix_argv_split(mca_pnet_tcp_component.static_ports, ';');
    for (n=0; NULL != grps[n]; n++) {
        trk = PMIX_NEW(tcp_available_ports_t);
        if (NULL == trk) {
            pmix_argv_free(grps);
            return PMIX_ERR_NOMEM;
        }
        /* there must be at least one colon */
        if (NULL == (p = strrchr(grps[n], ':'))) {
            pmix_argv_free(grps);
            return PMIX_ERR_BAD_PARAM;
        }
        /* extract the ports */
        *p = '\0';
        ++p;
        pmix_util_parse_range_options(p, &trk->ports);
        trk->nports = pmix_argv_count(trk->ports);
        /* see if they provided a plane */
        if (NULL != (p = strchr(grps[n], ':'))) {
            /* yep - save the plane */
            *p = '\0';
            ++p;
            trk->plane = strdup(p);
        }
        /* the type is just what is left at the front */
        trk->type = strdup(grps[n]);
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "TYPE: %s PLANE %s", trk->type,
                            (NULL == trk->plane) ? "NULL" : trk->plane);
        pmix_list_append(&available, &trk->super);
    }
    pmix_argv_free(grps);

    return PMIX_SUCCESS;
}

static void tcp_finalize(void)
{
    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: tcp finalize");
    if (PMIX_PROC_IS_GATEWAY(pmix_globals.mypeer)) {
        PMIX_LIST_DESTRUCT(&allocations);
        PMIX_LIST_DESTRUCT(&available);
    }
}

/* some network users may want to encrypt their communications
 * as a means of securing them, or include a token in their
 * messaging headers for some minimal level of security. This
 * is far from perfect, but is provided to illustrate how it
 * can be done. The resulting info is placed into the
 * app_context's env array so it will automatically be pushed
 * into the environment of every MPI process when launched.
 *
 * In a more perfect world, there would be some privileged place
 * to store the crypto key and the encryption would occur
 * in a non-visible driver - but we don't have a mechanism
 * for doing so.
 */

static inline void generate_key(uint64_t* unique_key) {
    pmix_rng_buff_t rng;
    pmix_srand(&rng,(unsigned int)time(NULL));
    unique_key[0] = pmix_rand(&rng);
    unique_key[1] = pmix_rand(&rng);
}

/* when allocate is called, we look at our table of available static addresses
 * and assign an address to each process on a node based on its node rank.
 * This will prevent collisions as the host RM is responsible for correctly
 * setting the node rank. Note that node ranks will "rollover" when they
 * hit whatever maximum value the host RM supports, and that they will
 * increase monotonically as new jobs are launched until hitting that
 * max value. So we need to take into account the number of static
 * ports we were given and check to ensure we have enough to hand out
 *
 * NOTE: this implementation is offered as an example that can
 * undoubtedly be vastly improved/optimized */

static pmix_status_t allocate(pmix_nspace_t *nptr,
                              pmix_info_t *info,
                              pmix_list_t *ilist)
{
    uint64_t unique_key[2];
    size_t n, nreqs=0;
    int ports_per_node=0;
    pmix_kval_t *kv;
    pmix_status_t rc;
    pmix_info_t *requests = NULL;
    char **reqs, *cptr;
    bool allocated = false, seckey = false;
    tcp_port_tracker_t *trk;
    tcp_available_ports_t *avail, *aptr;
    pmix_list_t mylist;
    pmix_buffer_t buf;
    char *type = NULL, *plane = NULL, *idkey = NULL;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:tcp:allocate for nspace %s", nptr->nspace);

    /* if I am not the gateway, then ignore this call - should never
     * happen, but check to be safe */
    if (!PMIX_PROC_IS_GATEWAY(pmix_globals.mypeer)) {
        return PMIX_SUCCESS;
    }

    /* check directives to see if a crypto key and/or
     * network resource allocations requested */
    PMIX_CONSTRUCT(&mylist, pmix_list_t);
    if (0 == strncmp(info->key, PMIX_SETUP_APP_ENVARS, PMIX_MAX_KEYLEN) ||
        0 == strncmp(info->key, PMIX_SETUP_APP_ALL, PMIX_MAX_KEYLEN)) {
        if (NULL != mca_pnet_tcp_component.include) {
            rc = pmix_pnet_base_harvest_envars(mca_pnet_tcp_component.include,
                                               mca_pnet_tcp_component.exclude,
                                               ilist);
            return rc;
        }
        return PMIX_SUCCESS;
    } else if (0 != strncmp(info->key, PMIX_ALLOC_NETWORK, PMIX_MAX_KEYLEN)) {
        /* not a network allocation request */
        return PMIX_SUCCESS;
    }

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:tcp:allocate alloc_network for nspace %s",
                        nptr->nspace);
    /* this info key includes an array of pmix_info_t, each providing
     * a key (that is to be used as the key for the allocated ports) and
     * a number of ports to allocate for that key */
    if (PMIX_DATA_ARRAY != info->value.type ||
        NULL == info->value.data.darray ||
        PMIX_INFO != info->value.data.darray->type ||
        NULL == info->value.data.darray->array) {
        /* they made an error */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }
    requests = (pmix_info_t*)info->value.data.darray->array;
    nreqs = info->value.data.darray->size;
    /* cycle thru the provided array and see if this refers to
     * tcp/udp-based resources - there is no required ordering
     * of the keys, so just have to do a search */
    for (n=0; n < nreqs; n++) {
        if (0 == strncasecmp(requests[n].key, PMIX_ALLOC_NETWORK_TYPE, PMIX_MAX_KEYLEN)) {
            /* check for bozo error */
            if (PMIX_STRING != requests[n].value.type ||
                NULL == requests[n].value.data.string) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            type = requests[n].value.data.string;
        } else if (0 == strncasecmp(requests[n].key, PMIX_ALLOC_NETWORK_PLANE, PMIX_MAX_KEYLEN)) {
            /* check for bozo error */
            if (PMIX_STRING != requests[n].value.type ||
                NULL == requests[n].value.data.string) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            plane = requests[n].value.data.string;
        } else if (0 == strncasecmp(requests[n].key, PMIX_ALLOC_NETWORK_ENDPTS, PMIX_MAX_KEYLEN)) {
            PMIX_VALUE_GET_NUMBER(rc, &requests[n].value, ports_per_node, int);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        } else if (0 == strncmp(requests[n].key, PMIX_ALLOC_NETWORK_ID, PMIX_MAX_KEYLEN)) {
            /* check for bozo error */
            if (PMIX_STRING != requests[n].value.type ||
                NULL == requests[n].value.data.string) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            idkey = requests[n].value.data.string;
        } else if (0 == strncasecmp(requests[n].key, PMIX_ALLOC_NETWORK_SEC_KEY, PMIX_MAX_KEYLEN)) {
            seckey = PMIX_INFO_TRUE(&requests[n]);
        }
    }

    /* we at least require an attribute key for the response */
    if (NULL == idkey) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* must include the idkey */
    kv = PMIX_NEW(pmix_kval_t);
    if (NULL == kv) {
        return PMIX_ERR_NOMEM;
    }
    kv->key = strdup(PMIX_ALLOC_NETWORK_ID);
    kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
    if (NULL == kv->value) {
        PMIX_RELEASE(kv);
        return PMIX_ERR_NOMEM;
    }
    kv->value->type = PMIX_STRING;
    kv->value->data.string = strdup(idkey);
    pmix_list_append(&mylist, &kv->super);

    /* note that they might not provide
     * the network type (letting it fall to a default component
     * based on priority), and they are not required to provide
     * a plane. In addition, they are allowed to simply request
     * a network security key without asking for endpts */

    if (NULL != type) {
        /* if it is tcp or udp, then this is something we should process */
        if (0 == strcasecmp(type, "tcp")) {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:tcp:allocate allocating TCP ports for nspace %s",
                                nptr->nspace);
            /* do we have static tcp ports? */
            avail = NULL;
            PMIX_LIST_FOREACH(aptr, &available, tcp_available_ports_t) {
                if (0 == strcmp(aptr->type, "tcp")) {
                    /* if they specified a plane, then require it */
                    if (NULL != plane && (NULL == aptr->plane || 0 != strcmp(aptr->plane, plane))) {
                        continue;
                    }
                    avail = aptr;
                    break;
                }
            }
            /* nope - they asked for something that we cannot do */
            if (NULL == avail) {
                return PMIX_ERR_NOT_AVAILABLE;
            }
            /* setup to track the assignment */
            trk = PMIX_NEW(tcp_port_tracker_t);
            if (NULL == trk) {
                return PMIX_ERR_NOMEM;
            }
            trk->nspace = strdup(nptr->nspace);
            PMIX_RETAIN(avail);
            trk->src = avail;
            pmix_list_append(&allocations, &trk->super);
            rc = process_request(nptr, idkey, ports_per_node, trk, &mylist);
            if (PMIX_SUCCESS != rc) {
                /* return the allocated ports */
                pmix_list_remove_item(&allocations, &trk->super);
                PMIX_RELEASE(trk);
                return rc;
            }
            allocated = true;

        } else if (0 == strcasecmp(requests[n].value.data.string, "udp")) {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:tcp:allocate allocating UDP ports for nspace %s",
                                nptr->nspace);
            /* do we have static udp ports? */
            avail = NULL;
            PMIX_LIST_FOREACH(aptr, &available, tcp_available_ports_t) {
                if (0 == strcmp(aptr->type, "udp")) {
                    /* if they specified a plane, then require it */
                    if (NULL != plane && (NULL == aptr->plane || 0 != strcmp(aptr->plane, plane))) {
                        continue;
                    }
                    avail = aptr;
                    break;
                }
            }
            /* nope - they asked for something that we cannot do */
            if (NULL == avail) {
                return PMIX_ERR_NOT_AVAILABLE;
            }
            /* setup to track the assignment */
            trk = PMIX_NEW(tcp_port_tracker_t);
            if (NULL == trk) {
                return PMIX_ERR_NOMEM;
            }
            trk->nspace = strdup(nptr->nspace);
            PMIX_RETAIN(avail);
            trk->src = avail;
            pmix_list_append(&allocations, &trk->super);
            rc = process_request(nptr, idkey, ports_per_node, trk, &mylist);
            if (PMIX_SUCCESS != rc) {
                /* return the allocated ports */
                pmix_list_remove_item(&allocations, &trk->super);
                PMIX_RELEASE(trk);
                return rc;
            }
            allocated = true;
        } else {
            /* unsupported type */
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:tcp:allocate unsupported type %s for nspace %s",
                                type, nptr->nspace);
            return PMIX_ERR_TAKE_NEXT_OPTION;
        }

    } else {
        if (NULL != plane) {
            /* if they didn't specify a type, but they did specify a plane, we can
             * see if that is a plane we recognize */
            PMIX_LIST_FOREACH(aptr, &available, tcp_available_ports_t) {
                if (0 != strcmp(aptr->plane, plane)) {
                    continue;
                }
                /* setup to track the assignment */
                trk = PMIX_NEW(tcp_port_tracker_t);
                if (NULL == trk) {
                    return PMIX_ERR_NOMEM;
                }
                trk->nspace = strdup(nptr->nspace);
                PMIX_RETAIN(aptr);
                trk->src = aptr;
                pmix_list_append(&allocations, &trk->super);
                rc = process_request(nptr, idkey, ports_per_node, trk, &mylist);
                if (PMIX_SUCCESS != rc) {
                    /* return the allocated ports */
                    pmix_list_remove_item(&allocations, &trk->super);
                    PMIX_RELEASE(trk);
                    return rc;
                }
                allocated = true;
                break;
            }
        } else {
            /* if they didn't specify either type or plane, then we got here because
             * nobody of a higher priority could act as a default transport - so try
             * to provide something here, starting by looking at any provided setting */
            if (NULL != mca_pnet_tcp_component.default_request) {
                pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                    "pnet:tcp:allocate allocating default ports %s for nspace %s",
                                    mca_pnet_tcp_component.default_request, nptr->nspace);
                reqs = pmix_argv_split(mca_pnet_tcp_component.default_request, ';');
                for (n=0; NULL != reqs[n]; n++) {
                    /* if there is no colon, then it is just
                     * a number of ports to use */
                    type = NULL;
                    plane = NULL;
                    if (NULL == (cptr = strrchr(reqs[n], ':'))) {
                        avail = (tcp_available_ports_t*)pmix_list_get_first(&available);
                    } else {
                        *cptr = '\0';
                        ++cptr;
                        ports_per_node = strtoul(cptr, NULL, 10);
                        /* look for the plane */
                        cptr -= 2;
                        if (NULL != (cptr = strrchr(cptr, ':'))) {
                            *cptr = '\0';
                            ++cptr;
                            plane = cptr;
                        }
                        type = reqs[n];
                        avail = NULL;
                        PMIX_LIST_FOREACH(aptr, &available, tcp_available_ports_t) {
                            if (0 == strcmp(aptr->type, type)) {
                                /* if they specified a plane, then require it */
                                if (NULL != plane && (NULL == aptr->plane || 0 != strcmp(aptr->plane, plane))) {
                                    continue;
                                }
                                avail = aptr;
                                break;
                            }
                        }
                        /* if we didn't find it, that isn't an error - just ignore */
                        if (NULL == avail) {
                            continue;
                        }
                    }
                    /* setup to track the assignment */
                    trk = PMIX_NEW(tcp_port_tracker_t);
                    if (NULL == trk) {
                        pmix_argv_free(reqs);
                        return PMIX_ERR_NOMEM;
                    }
                    trk->nspace = strdup(nptr->nspace);
                    PMIX_RETAIN(avail);
                    trk->src = avail;
                    pmix_list_append(&allocations, &trk->super);
                    rc = process_request(nptr, idkey, ports_per_node, trk, &mylist);
                    if (PMIX_SUCCESS != rc) {
                        /* return the allocated ports */
                        pmix_list_remove_item(&allocations, &trk->super);
                        PMIX_RELEASE(trk);
                        return rc;
                    }
                    allocated = true;
                }
            } else {
                avail = (tcp_available_ports_t*)pmix_list_get_first(&available);
                if (NULL != avail) {
                    /* setup to track the assignment */
                    trk = PMIX_NEW(tcp_port_tracker_t);
                    if (NULL == trk) {
                        return PMIX_ERR_NOMEM;
                    }
                    trk->nspace = strdup(nptr->nspace);
                    PMIX_RETAIN(avail);
                    trk->src = avail;
                    pmix_list_append(&allocations, &trk->super);
                    rc = process_request(nptr, idkey, ports_per_node, trk, &mylist);
                    if (PMIX_SUCCESS != rc) {
                        /* return the allocated ports */
                        pmix_list_remove_item(&allocations, &trk->super);
                        PMIX_RELEASE(trk);
                    } else {
                        allocated = true;
                    }
                }
            }
        }
        if (!allocated) {
            /* nope - we cannot help */
            return PMIX_ERR_TAKE_NEXT_OPTION;
        }
    }

    if (seckey) {
        generate_key(unique_key);
        kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            return PMIX_ERR_NOMEM;
        }
        kv->key = strdup(PMIX_ALLOC_NETWORK_SEC_KEY);
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            return PMIX_ERR_NOMEM;
        }
        kv->value->type = PMIX_BYTE_OBJECT;
        kv->value->data.bo.bytes = (char*)malloc(2 * sizeof(uint64_t));
        if (NULL == kv->value->data.bo.bytes) {
            PMIX_RELEASE(kv);
            return PMIX_ERR_NOMEM;
        }
        memcpy(kv->value->data.bo.bytes, unique_key, 2 * sizeof(uint64_t));
        kv->value->data.bo.size = 2 * sizeof(uint64_t);
        pmix_list_append(&mylist, &kv->super);
    }


    n = pmix_list_get_size(&mylist);
    if (0 < n) {
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        /* pack the number of kvals for ease on the remote end */
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &n, 1, PMIX_SIZE);
        /* cycle across the list and pack the kvals */
        while (NULL != (kv = (pmix_kval_t*)pmix_list_remove_first(&mylist))) {
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, kv, 1, PMIX_KVAL);
            PMIX_RELEASE(kv);
            if (PMIX_SUCCESS != rc) {
                PMIX_DESTRUCT(&buf);
                PMIX_LIST_DESTRUCT(&mylist);
                return rc;
            }
        }
        PMIX_LIST_DESTRUCT(&mylist);
        kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup(PMIX_TCP_SETUP_APP_KEY);
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            PMIX_DESTRUCT(&buf);
            return PMIX_ERR_NOMEM;
        }
        kv->value->type = PMIX_BYTE_OBJECT;
        PMIX_UNLOAD_BUFFER(&buf, kv->value->data.bo.bytes, kv->value->data.bo.size);
        PMIX_DESTRUCT(&buf);
        pmix_list_append(ilist, &kv->super);
    }

    /* if we got here, then we processed this specific request, so
     * indicate that by returning success */
    return PMIX_SUCCESS;
}

/* upon receipt of the launch message, each daemon adds the
 * static address assignments to the job-level info cache
 * for that job */
static pmix_status_t setup_local_network(pmix_nspace_t *nptr,
                                         pmix_info_t info[],
                                         size_t ninfo)
{
    size_t n, m, nkvals;
    pmix_buffer_t bkt;
    int32_t cnt;
    pmix_kval_t *kv;
    pmix_status_t rc;
    pmix_info_t *jinfo, stinfo;
    char *idkey = NULL;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:tcp:setup_local_network");

    if (NULL != info) {
        for (n=0; n < ninfo; n++) {
            /* look for my key */
            if (0 == strncmp(info[n].key, PMIX_TCP_SETUP_APP_KEY, PMIX_MAX_KEYLEN)) {
                /* this macro NULLs and zero's the incoming bo */
                PMIX_LOAD_BUFFER(pmix_globals.mypeer, &bkt,
                                 info[n].value.data.bo.bytes,
                                 info[n].value.data.bo.size);
                /* unpack the number of kvals */
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer,
                                   &bkt, &nkvals, &cnt, PMIX_SIZE);
                /* setup the info array */
                PMIX_INFO_CREATE(jinfo, nkvals);
                /* cycle thru the blob and extract the kvals */
                kv = PMIX_NEW(pmix_kval_t);
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer,
                                   &bkt, kv, &cnt, PMIX_KVAL);
                m = 0;
                while (PMIX_SUCCESS == rc) {
                    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                        "recvd KEY %s %s", kv->key,
                                        (PMIX_STRING == kv->value->type) ? kv->value->data.string : "NON-STRING");
                    /* xfer the value to the info */
                    (void)strncpy(jinfo[m].key, kv->key, PMIX_MAX_KEYLEN);
                    PMIX_BFROPS_VALUE_XFER(rc, pmix_globals.mypeer,
                                           &jinfo[m].value, kv->value);
                    /* if this is the ID key, save it */
                    if (NULL == idkey &&
                        0 == strncmp(kv->key, PMIX_ALLOC_NETWORK_ID, PMIX_MAX_KEYLEN)) {
                        idkey = strdup(kv->value->data.string);
                    }
                    ++m;
                    PMIX_RELEASE(kv);
                    kv = PMIX_NEW(pmix_kval_t);
                    cnt = 1;
                    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer,
                                       &bkt, kv, &cnt, PMIX_KVAL);
                }
                /* restore the incoming data */
                info[n].value.data.bo.bytes = bkt.base_ptr;
                info[n].value.data.bo.size = bkt.bytes_used;
                bkt.base_ptr = NULL;
                bkt.bytes_used = 0;

                /* if they didn't include a network ID, then this is an error */
                if (NULL == idkey) {
                    PMIX_INFO_FREE(jinfo, nkvals);
                    return PMIX_ERR_BAD_PARAM;
                }
                /* the data gets stored as a pmix_data_array_t on the provided key */
                PMIX_INFO_CONSTRUCT(&stinfo);
                (void)strncpy(stinfo.key, idkey, PMIX_MAX_KEYLEN);
                stinfo.value.type = PMIX_DATA_ARRAY;
                PMIX_DATA_ARRAY_CREATE(stinfo.value.data.darray, nkvals, PMIX_INFO);
                stinfo.value.data.darray->array = jinfo;

                /* cache the info on the job */
                PMIX_GDS_CACHE_JOB_INFO(rc, pmix_globals.mypeer, nptr,
                                        &stinfo, 1);
                PMIX_INFO_DESTRUCT(&stinfo);
            }
        }
    }
    if (NULL != idkey) {
        free(idkey);
    }
    return PMIX_SUCCESS;
}

static pmix_status_t setup_fork(pmix_nspace_t *nptr,
                                const pmix_proc_t *peer, char ***env)
{
    return PMIX_SUCCESS;
}

/* when a local client finalizes, the server gives us a chance
 * to do any required local cleanup for that peer. We don't
 * have anything we need to do */
static void child_finalized(pmix_peer_t *peer)
{

}

/* when all local clients for a given job finalize, the server
 * provides an opportunity for the local network to cleanup
 * any resources consumed locally by the clients of that job.
 * We don't have anything we need to do */
static void local_app_finalized(pmix_nspace_t *nptr)
{

}

/* when the job completes, the scheduler calls the "deregister nspace"
 * PMix function, which in turn calls my TCP component to release the
 * assignments for that job. The addresses are marked as "available"
 * for reuse on the next job. */
static void deregister_nspace(pmix_nspace_t *nptr)
{
    tcp_port_tracker_t *trk;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:tcp deregister nspace %s", nptr->nspace);

    /* if we are not the "gateway", then there is nothing
     * for us to do */
    if (!PMIX_PROC_IS_GATEWAY(pmix_globals.mypeer)) {
        return;
    }

    /* find this tracker */
    PMIX_LIST_FOREACH(trk, &allocations, tcp_port_tracker_t) {
        if (0 == strcmp(nptr->nspace, trk->nspace)) {
            pmix_list_remove_item(&allocations, &trk->super);
            PMIX_RELEASE(trk);
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:tcp released tracker for nspace %s", nptr->nspace);
            return;
        }
    }
}

static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t process_request(pmix_nspace_t *nptr,
                                     char *idkey, int ports_per_node,
                                     tcp_port_tracker_t *trk,
                                     pmix_list_t *ilist)
{
    char **plist;
    pmix_kval_t *kv;
    size_t m;
    int p, ppn;
    tcp_available_ports_t *avail = trk->src;

    kv = PMIX_NEW(pmix_kval_t);
    if (NULL == kv) {
        return PMIX_ERR_NOMEM;
    }
    kv->key = strdup(idkey);
    kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
    if (NULL == kv->value) {
        PMIX_RELEASE(kv);
        return PMIX_ERR_NOMEM;
    }
    kv->value->type = PMIX_STRING;
    kv->value->data.string = NULL;
    if (0 == ports_per_node) {
        /* find the maxprocs on the nodes in this nspace and
         * allocate that number of resources */
        return PMIX_ERR_NOT_SUPPORTED;
    } else {
        ppn = ports_per_node;
    }

    /* assemble the list of ports */
    p = 0;
    plist = NULL;
    for (m=0; p < ppn && m < avail->nports; m++) {
        if (NULL != avail->ports[m]) {
            pmix_argv_append_nosize(&trk->ports, avail->ports[m]);
            pmix_argv_append_nosize(&plist, avail->ports[m]);
            free(avail->ports[m]);
            avail->ports[m] = NULL;
            ++p;
        }
    }
    /* if we couldn't find enough, then that's an error */
    if (p < ppn) {
        PMIX_RELEASE(kv);
        /* the caller will release trk, and that will return
         * any allocated ports back to the available list */
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    /* pass the value */
    kv->value->data.string = pmix_argv_join(plist, ',');
    pmix_argv_free(plist);
    pmix_list_append(ilist, &kv->super);

    /* track where it came from */
    kv = PMIX_NEW(pmix_kval_t);
    if (NULL == kv) {
        return PMIX_ERR_NOMEM;
    }
    kv->key = strdup(idkey);
    kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
    if (NULL == kv->value) {
        PMIX_RELEASE(kv);
        return PMIX_ERR_NOMEM;
    }
    kv->value->type = PMIX_STRING;
    kv->value->data.string = strdup(trk->src->type);
    pmix_list_append(ilist, &kv->super);
    if (NULL != trk->src->plane) {
        kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            return PMIX_ERR_NOMEM;
        }
        kv->key = strdup(idkey);
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            return PMIX_ERR_NOMEM;
        }
        kv->value->type = PMIX_STRING;
        kv->value->data.string = strdup(trk->src->plane);
        pmix_list_append(ilist, &kv->super);
    }
    return PMIX_SUCCESS;
}