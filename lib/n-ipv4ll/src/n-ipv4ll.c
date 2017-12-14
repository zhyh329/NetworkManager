/*
 * Dynamic IPv4 Link-Local Address Configuration
 *
 * This implements the main n-ipv4ll API. It is built around n-acd, with
 * analogus lifetime rules. The engine is started on demand, and stopped if no
 * longer needed. The parameters are all set at the time the engine is started.
 * During the entire lifetime the context can be dispatched. That is, the
 * dispatcher does not have to be aware of the context state.
 *
 * If a conflict is detected, the IPv4LL engine reports to the caller and stops
 * the engine. The caller can now modify parameters and restart the engine, if
 * required.
 */

#include <assert.h>
#include <c-list.h>
#include <errno.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <n-acd.h>
#include <stdlib.h>
#include <string.h>
#include "n-ipv4ll.h"

#define _public_ __attribute__((__visibility__("default")))

#define IPV4LL_NETWORK UINT32_C(0xa9fe0000)

enum {
        N_IPV4LL_STATE_INIT,
        N_IPV4LL_STATE_RUNNING,
};

typedef struct NIpv4llEventNode {
        NIpv4llEvent event;
        uint8_t sender[ETH_ALEN];
        CList link;
} NIpv4llEventNode;

struct NIpv4ll {
        /* context */
        struct drand48_data enumeration_state;

        /* runtime */
        NAcd *acd;
        NAcdConfig config;
        uint8_t mac[ETH_ALEN];
        unsigned int state;

        /* pending events */
        CList events;
        NIpv4llEventNode *current;
};

static int n_ipv4ll_event_node_new(NIpv4llEventNode **nodep, unsigned int event) {
        NIpv4llEventNode *node;

        node = calloc(1, sizeof(*node));
        if (!node)
                return -ENOMEM;

        node->event.event = event;
        node->link = (CList)C_LIST_INIT(node->link);

        *nodep = node;

        return 0;
}

static NIpv4llEventNode *n_ipv4ll_event_node_free(NIpv4llEventNode *node) {
        if (!node)
                return NULL;

        c_list_unlink(&node->link);
        free(node);

        return NULL;
}

/**
 * n_ipv4ll_new() - create a new IPv4LL context
 * @llp:        output argument for context
 *
 * Create a new IPv4LL context and return it in @llp.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
_public_ int n_ipv4ll_new(NIpv4ll **llp) {
        NIpv4ll *ll;
        int r;

        ll = calloc(1, sizeof(*ll));
        if (!ll)
                return -ENOMEM;

        r = n_acd_new(&ll->acd);
        if (r) {
                if (r > 0)
                        r = -ENOTRECOVERABLE;
                goto error;
        }

        ll->state = N_IPV4LL_STATE_INIT;
        ll->events = (CList)C_LIST_INIT(ll->events);

        *llp = ll;
        return 0;

error:
        n_ipv4ll_free(ll);
        return r;
}

/**
 * n_acd_free() - free an IPv4LL context
 * @ll:         IPv4LL context
 *
 * Frees all resources held by the context. This may be called at any time,
 * but doing so invalidates all data owned by the context.
 *
 * Return: NULL.
 */
_public_ NIpv4ll *n_ipv4ll_free(NIpv4ll *ll) {
        NIpv4llEventNode *node;

        if (!ll)
                return NULL;

        n_ipv4ll_stop(ll);

        while ((node = c_list_first_entry(&ll->events, NIpv4llEventNode, link)))
                n_ipv4ll_event_node_free(node);

        n_acd_free(ll->acd);

        free(ll);

        return NULL;
}

/**
 * n_ipv4ll_get_fd() - get pollable file descriptor
 * @ll:         IPv4LL context
 * @fdp:        output argument for file descriptor
 *
 * Returns a file descriptor in @fdp. This file descriptor can be polled by
 * the caller to indicate when the IPv4LL context can be dispatched.
 */
_public_ void n_ipv4ll_get_fd(NIpv4ll *ll, int *fdp) {
        n_acd_get_fd(ll->acd, fdp);
}

static int n_ipv4ll_push_event(NIpv4ll *ll, unsigned int event, struct in_addr *address, uint16_t *operation, uint8_t *sender, size_t n_sender, struct in_addr *target) {
        NIpv4llEventNode *node;
        int r;

        r = n_ipv4ll_event_node_new(&node, event);
        if (r < 0)
                return r;

        switch (event) {
                case N_IPV4LL_EVENT_DEFENDED:
                        node->event.defended.operation = *operation;
                        memcpy(node->sender, sender, sizeof(node->sender));
                        node->event.defended.sender = node->sender;
                        node->event.defended.n_sender = sizeof(node->sender);
                        node->event.defended.target = *target;
                        break;
                case N_IPV4LL_EVENT_CONFLICT:
                        node->event.conflict.operation = *operation;
                        memcpy(node->sender, sender, sizeof(node->sender));
                        node->event.conflict.sender = node->sender;
                        node->event.conflict.n_sender = sizeof(node->sender);
                        node->event.conflict.target = *target;
                        break;
                case N_IPV4LL_EVENT_READY:
                        node->event.ready.address = *address;
                        break;
                case N_IPV4LL_EVENT_DOWN:
                        break;
                default:
                        assert(0);
        }

        c_list_link_tail(&ll->events, &node->link);

        return 0;
}

static void n_ipv4ll_select_ip(NIpv4ll *ll, struct in_addr *ip) {
        for (;;) {
                long int result;
                uint16_t offset;

                (void) mrand48_r(&ll->enumeration_state, &result);

                offset = result ^ (result >> 16);

                /*
                 * The first and the last 256 addresses in the subnet are
                 * reserved.
                 */
                if (offset < 0x100 || offset > 0xfdff)
                        continue;

                ip->s_addr = htobe32(IPV4LL_NETWORK | offset);
                break;
        }
}

static int n_ipv4ll_handle_acd_event(NIpv4ll *ll, NAcdEvent *event) {
        int r;

        switch (event->event) {
        case N_ACD_EVENT_READY:
                r = n_ipv4ll_push_event(ll, N_IPV4LL_EVENT_READY, &ll->config.ip, NULL, NULL, 0, NULL);
                if (r < 0)
                        return r;

                break;

        case N_ACD_EVENT_DEFENDED:
                r = n_ipv4ll_push_event(ll, N_IPV4LL_EVENT_DEFENDED, NULL, &event->defended.operation, event->defended.sender, event->defended.n_sender, &event->defended.target);
                if (r < 0)
                        return r;

                break;

        case N_ACD_EVENT_CONFLICT:
                r = n_ipv4ll_push_event(ll, N_IPV4LL_EVENT_CONFLICT, NULL, &event->conflict.operation, event->conflict.sender, event->conflict.n_sender, &event->conflict.target);
                if (r < 0)
                        return r;

                /* fall-through */
        case N_ACD_EVENT_USED:
                n_ipv4ll_select_ip(ll, &ll->config.ip);
                r = n_acd_start(ll->acd, &ll->config);
                if (!r)
                        return 0;

                /*
                 * Failed to restart ACD. Give up and report the
                 * failure to the caller.
                 */

                /* fall-through */
        case N_ACD_EVENT_DOWN:
                r = n_ipv4ll_push_event(ll, N_IPV4LL_EVENT_DOWN, NULL, NULL, NULL, 0, NULL);
                if (r < 0)
                        return r;

                ll->state = N_IPV4LL_STATE_INIT;

                break;
        }

        return 0;
}

/**
 * n_ipv4ll_dispatch() - dispatch IPv4LL context
 * @ll:         IPv4LL context
 *
 * Return: 0 on successfull dispatch of all pending events. N_IPV4LL_E_PREEPMT
 *         in case there are still more events to be dispatched, or a negative
 *         error code on failure.
 */
_public_ int n_ipv4ll_dispatch(NIpv4ll *ll) {
        int r;

        r = n_acd_dispatch(ll->acd);
        if (r) {
                if (r > 0)
                        r = -ENOTRECOVERABLE;
                return r;
        }

        for (;;) {
                NAcdEvent *event;

                r = n_acd_pop_event(ll->acd, &event);
                if (r == N_ACD_E_STOPPED) {
                        ll->state = N_IPV4LL_STATE_INIT;
                        break;
                } else if (r == N_ACD_E_DONE) {
                        break;
                } else if (r) {
                        if (r > 0)
                                r = -ENOTRECOVERABLE;
                        return r;
                }

                r = n_ipv4ll_handle_acd_event(ll, event);
                if (r < 0)
                        return r;
        }

        return 0;
}

/**
 * n_ipv4ll_pop_event() - get the next pending event
 * @ll:         IPv4LL context
 * @eventp:     output argument for the event
 *
 * Returns a pointer to the next pending event. The event is still owned by
 * the context, and is only valid until the next call to n_acd_pop_event()
 * or until the context is freed.
 *
 * The possible events are:
 *  * N_IPV4LL_EVENT_READY:    The configured IP address was probed
 *                             successfully and is ready to be used. Once
 *                             configured on the interface, the caller must
 *                             call n_acd_announce() to announce and start
 *                             defending the address. No further events may
 *                             be received before n_acd_announce() has been
 *                             called.
 *  * N_IPV4LL_EVENT_DEFENDED: A conflict was detected for the announced IP
 *                             address, and the engine tried to defend it.
 *                             This is purely informational, and no action
 *                             is required from the caller.
 *  * N_IPV4LL_EVENT_CONFLICT: A conflict was detected for the announced IP
 *                             address, and the engine failed to defend it.
 *                             The engine was stopped, the caller must stop
 *                             using the address immediately, and may
 *                             restart the engine to retry.
 *  * N_IPV4LL_EVENT_DOWN:     A network error was detected. The engine was
 *                             stopped, and it is the responsibility of the
 *                             caller to restart it once the network may be
 *                             funcitonal again.
 *
 * Return: 0 on success, N_ACD_E_STOPPED if there are no more events and the
 *         engine has been stopped, N_IPV4LL_E_DONE if there are no more
 *         events, but the engine is still running, or a negative error code
 *         on failure.
 */
_public_ int n_ipv4ll_pop_event(NIpv4ll *ll, NIpv4llEvent **eventp) {
        ll->current = n_ipv4ll_event_node_free(ll->current);

        if (c_list_is_empty(&ll->events)) {
                if (ll->state == N_IPV4LL_STATE_INIT)
                        return N_IPV4LL_E_STOPPED;
                else
                        return N_IPV4LL_E_DONE;
        }

        ll->current = c_list_first_entry(&ll->events, NIpv4llEventNode, link);
        c_list_unlink(&ll->current->link);

        if (eventp)
                *eventp = &ll->current->event;

        return 0;
}

/**
 * n_ipv4ll_announce() - announce the configured IP address
 * @ll:         IPv4LL context
 *
 * Announce the IP address on the local link, and start defending it.
 *
 * This must be called in response to an N_IPV4LL_EVENT_READY event,
 * and only once the address has been configured on the given interface.
 *
 * Return: 0 on success, N_IPV4LL_E_BUSY if this is not in response to an
 *         N_IPV4LL_EVENT_READY event, or a negative error code on failure.
 */
_public_ int n_ipv4ll_announce(NIpv4ll *ll) {
        int r;

        r = n_acd_announce(ll->acd, N_ACD_DEFEND_ONCE);
        if (r) {
                if (r == N_ACD_E_BUSY)
                        r = N_IPV4LL_E_BUSY;
                else if (r > 0)
                        r = -ENOTRECOVERABLE;

                return r;
        }

        return 0;
}

/**
 * n_ipv4ll_start() - start the IPv4LL engine
 * @ll:         IPv4LL context
 * @config:     description of the interface
 *
 * Start probing for an address on the given interface.
 *
 * The engine must not already be running and there must not be any pending
 * events. The @config.enumeration argument uniquely determines the sequence
 * of addresses to be attempted for the given interface. It should to the
 * extent possible be kept constant between runs, and be selected so that
 * no two interfaces use the same number.
 *
 * Return: 0 on success, N_IPV4LL_E_BUSY if the engine is running, or there
 *         are pending events, or a negative error code on failure.
 */
_public_ int n_ipv4ll_start(NIpv4ll *ll, NIpv4llConfig *config) {
        NAcdConfig acd_config = {
                .ifindex = config->ifindex,
                .transport = N_ACD_TRANSPORT_ETHERNET,
                .mac = config->mac,
                .n_mac = config->n_mac,
                .timeout_msec = config->timeout_msec,
        };
        int r;

        if (config->transport != N_IPV4LL_TRANSPORT_ETHERNET)
                return N_IPV4LL_E_INVALID_ARGUMENT;

        (void) seed48_r((unsigned short int*) &config->enumeration, &ll->enumeration_state);

        if (config->requested_address) {
                if (be32toh(config->requested_address->s_addr) < (IPV4LL_NETWORK | 0x100) ||
                    be32toh(config->requested_address->s_addr) > (IPV4LL_NETWORK | 0xfdff))
                        return N_IPV4LL_E_INVALID_ARGUMENT;

                acd_config.ip = *config->requested_address;
        } else {
                n_ipv4ll_select_ip(ll, &acd_config.ip);
        }

        r = n_acd_start(ll->acd, &acd_config);
        if (r) {
                if (r == N_ACD_E_BUSY)
                        r = N_IPV4LL_E_BUSY;
                else if (r > 0)
                        r = -ENOTRECOVERABLE;

                return r;
        }

        ll->config = acd_config;
        ll->state = N_IPV4LL_STATE_RUNNING;

        return 0;
}

_public_ void n_ipv4ll_stop(NIpv4ll *ll) {
        n_acd_stop(ll->acd);
        ll->state = N_IPV4LL_STATE_INIT;
}
