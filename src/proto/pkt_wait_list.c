// -*- c-basic-offset: 4; c-backslash-column: 79; indent-tabs-mode: nil -*-
// vim:sw=4 ts=4 sts=4 expandtab
/* Copyright 2010, SecurActive.
 *
 * This file is part of Junkie.
 *
 * Junkie is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Junkie is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with Junkie.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "junkie/proto/pkt_wait_list.h"
#include "junkie/tools/log.h"
#include "junkie/tools/ext.h"
#include "junkie/tools/objalloc.h"

#undef LOG_CAT
#define LOG_CAT pkt_wait_list_log_category

LOG_CATEGORY_DEF(pkt_wait_list);

/*
 * Destruction of a pending packet
 */

/* There is no such thing as a destructor for proto_info, since they are constructed on the stack.
 * Also, notice that normaly the pointer to parser is not a counted ref since these proto_info are normaly
 * build on the stack, but for our copies on the heap we need a proper ref, so we unref them here. */
static void proto_info_del_rec(struct proto_info *info)
{
    void *start = (void *)info->parser->proto->ops->info_addr(info, NULL);

    parser_unref(&info->parser);

    if (info->parent) {
        proto_info_del_rec(info->parent);
        info->parent = NULL;
    }

    objfree(start);
}

// caller must own list->mutex
static void pkt_wait_dtor(struct pkt_wait *pkt, struct pkt_wait_list *pkt_wl)
{
    SLOG(LOG_DEBUG, "Destruct pkt@%p", pkt);

    assert(pkt_wl->nb_pkts > 0);
    assert(pkt_wl->tot_payload >= pkt->cap_len);
    LIST_REMOVE(pkt, entry);
    pkt_wl->nb_pkts --;
    pkt_wl->tot_payload -= pkt->cap_len;

    if (pkt->parent) {
        proto_info_del_rec(pkt->parent);
        pkt->parent = NULL;
    }
}

// caller must own list->mutex
static void pkt_wait_del_nolock(struct pkt_wait *pkt, struct pkt_wait_list *pkt_wl)
{
    pkt_wait_dtor(pkt, pkt_wl);
    objfree(pkt);
}

void pkt_wait_del(struct pkt_wait *pkt, struct pkt_wait_list *pkt_wl)
{
    supermutex_lock_maydeadlock(&pkt_wl->list->mutex);
    pkt_wait_del_nolock(pkt, pkt_wl);
    supermutex_unlock(&pkt_wl->list->mutex);
}

// Call proto_parse for the given packet, with a subparser if possible
// caller must own list->mutex
static enum proto_parse_status pkt_wait_parse(struct pkt_wait *pkt, struct pkt_wait_list *pkt_wl, struct timeval const *now)
{
    if (
        pkt_wl->next_offset >= pkt->next_offset // the pkt content was completely covered
    ) {
        // forget it
        return proto_parse(NULL, pkt->parent, pkt->way, NULL, 0, 0, &pkt->cap_tv, pkt->tot_cap_len, pkt->packet);
    } else if (
        pkt->offset > pkt_wl->next_offset       // the pkt was supposed to come later,
    ) {
        // advertise the gap
        size_t const gap = pkt->offset - pkt_wl->next_offset;
        pkt_wl->next_offset = pkt->next_offset;
        pkt_wl->wait_since = *now;
        return proto_parse(pkt_wl->parser, NULL, pkt->way, NULL, 0, gap, &pkt->cap_tv, 0, NULL);
    }

    // So we must parse from pkt_wl->next_offset to pkt->next_offset
    assert(pkt->offset <= pkt_wl->next_offset);
    unsigned const trim = pkt_wl->next_offset - pkt->offset;  // This assumes that offsets _are_ bytes. If not, then there is no reason to trim.
    enum proto_parse_status const status =
        trim < pkt->cap_len ?
            proto_parse(pkt_wl->parser, pkt->parent, pkt->way, pkt->packet + pkt->start + trim, pkt->cap_len - trim, pkt->wire_len - trim, &pkt->cap_tv, pkt->tot_cap_len, pkt->packet) :
            // The parser may be able to parse this (if he just skip, for instance HTTP skipping a body)
            proto_parse(pkt_wl->parser, pkt->parent, pkt->way, NULL, 0, trim < pkt->wire_len ? pkt->wire_len - trim : 0, &pkt->cap_tv, pkt->tot_cap_len, pkt->packet);
    pkt_wl->next_offset = pkt->next_offset;
    if (timeval_is_set(now)) pkt_wl->wait_since = *now;
    return status;
}

// Delete the packet after having called proto_parse on it
// caller must own list->mutex
static enum proto_parse_status pkt_wait_finalize(struct pkt_wait *pkt, struct pkt_wait_list *pkt_wl, struct timeval const *now)
{
    SLOG(LOG_DEBUG, "Finalizing parse of next packet @(%u:%u)", pkt->offset, pkt->next_offset);
    enum proto_parse_status status = pkt_wait_parse(pkt, pkt_wl, now);
    pkt_wait_del_nolock(pkt, pkt_wl);
    return status;
}

/*
 * Construction of a waiting packet
 */

static struct proto_info *copy_info_rec(struct proto_info *info)
{
    if (! info) return NULL;

    struct proto_info *parent = copy_info_rec(info->parent);

    size_t size;
    void *start = (void *)info->parser->proto->ops->info_addr(info, &size);
    void *copy = objalloc_nice(size, "waiting infos");
    if (! copy) {
        SLOG(LOG_WARNING, "Cannot alloc for pending info");
        if (parent) proto_info_del_rec(parent);
        return NULL;
    }
    memcpy(copy, start, size);

    struct proto_info *copy_info = (struct proto_info *)(((char *)copy) + ((char *)info - (char *)start));
    copy_info->parent = parent;
    copy_info->parser = parser_ref(info->parser);

    return copy_info;
}

// Construct it but does not insert it into the pkt_wait list yet
static int pkt_wait_ctor(struct pkt_wait *pkt, unsigned offset, unsigned next_offset, bool sync, unsigned sync_offset, struct proto_info *parent, unsigned way, uint8_t const *packet, size_t cap_len, size_t wire_len, size_t tot_cap_len, uint8_t const *tot_packet, struct timeval const *now)
{
    SLOG(LOG_DEBUG, "Construct pkt@%p", pkt);
    CHECK_LAST_FIELD(pkt_wait, packet, uint8_t);

    pkt->offset = offset;
    pkt->next_offset = next_offset;
    pkt->sync_offset = sync_offset;
    pkt->sync = sync;
    pkt->cap_len = cap_len;
    pkt->wire_len = wire_len;
    pkt->way = way;
    pkt->tot_cap_len = tot_cap_len;
    pkt->cap_tv = *now;
    if (packet < tot_packet || packet + cap_len > tot_packet + tot_cap_len) {
        // FIXME: May happen since packet does not always lies within tot_packet (see pkt_wait_list_reassemble)
        return -1;
    }
    pkt->start = packet - tot_packet;
    assert(pkt->start <= pkt->tot_cap_len);
    assert(pkt->cap_len <= pkt->tot_cap_len);
    assert(pkt->wire_len >= pkt->cap_len);

    // We save the original packet, assuming packet points within it.
    memcpy(pkt->packet, tot_packet, tot_cap_len);

    if (parent) {
        pkt->parent = copy_info_rec(parent);
        if (! pkt->parent) return -1;
    } else {
        pkt->parent = NULL;
    }

    return 0;
}

static struct pkt_wait *pkt_wait_new(unsigned offset, unsigned next_offset, bool sync, unsigned sync_offset, struct proto_info *parent, unsigned way, uint8_t const *packet, size_t cap_len, size_t wire_len, size_t tot_cap_len, uint8_t const *tot_packet, struct timeval const *now)
{
    struct pkt_wait *pkt = objalloc(sizeof(*pkt) + tot_cap_len, "pkt_waits");
    if (! pkt) return NULL;

    if (0 != pkt_wait_ctor(pkt, offset, next_offset, sync, sync_offset, parent, way, packet, cap_len, wire_len, tot_cap_len, tot_packet, now)) {
        objfree(pkt);
        return NULL;
    }

    return pkt;
}

/*
 * Waiting list management
 */

static SLIST_HEAD(pkt_wl_configs, pkt_wl_config) pkt_wl_configs = SLIST_HEAD_INITIALIZER(pkt_wls_configs);
static struct mutex pkt_wl_configs_mutex;

// caller must own list->mutex
static enum proto_parse_status pkt_wait_list_empty(struct pkt_wait_list *pkt_wl)
{
    enum proto_parse_status last_status = PROTO_OK;
    struct pkt_wait *pkt;
    while (NULL != (pkt = LIST_FIRST(&pkt_wl->pkts))) {
        last_status = pkt_wait_finalize(pkt, pkt_wl, &timeval_unset);
    }
    assert(pkt_wl->nb_pkts == 0);
    assert(pkt_wl->tot_payload == 0);

    return last_status;
}

enum proto_parse_status pkt_wait_list_flush(struct pkt_wait_list *pkt_wl, uint8_t *payload, size_t cap_len, size_t wire_len)
{
    enum proto_parse_status last_status = PROTO_OK;
    if (0 != supermutex_lock(&pkt_wl->list->mutex)) return PROTO_PARSE_ERR;

    if (! payload) {
        // start by cleaning the parser so that the subparse method won't be called
        parser_unref(&pkt_wl->parser);
        last_status = pkt_wait_list_empty(pkt_wl); // may deadlock
    } else { // slightly different version
        struct parser *parser = pkt_wl->parser; // transfert the ref to this local variable
        pkt_wl->parser = NULL;
        struct pkt_wait *pkt;
        while (NULL != (pkt = LIST_FIRST(&pkt_wl->pkts))) {
            if (LIST_IS_LAST(pkt, entry)) {
                last_status = proto_parse(parser, pkt->parent, pkt->way, payload, cap_len, wire_len, &pkt->cap_tv, pkt->tot_cap_len, pkt->packet);   // FIXME: once again, payload not within pkt->packet !
                pkt_wait_del_nolock(pkt, pkt_wl);
            } else {
                last_status = pkt_wait_finalize(pkt, pkt_wl, &timeval_unset);  // may deadlock
            }
        }
        parser_unref(&parser);
        assert(pkt_wl->nb_pkts == 0);
        assert(pkt_wl->tot_payload == 0);
    }

    supermutex_unlock(&pkt_wl->list->mutex);
    return last_status;
}

int pkt_wait_list_ctor(struct pkt_wait_list *pkt_wl, unsigned next_offset, struct pkt_wl_config *config, struct parser *parser, struct timeval const *now, struct pkt_wait_list *restrict sync_with)
{
    SLOG(LOG_DEBUG, "Construct pkt_wait_list @%p", pkt_wl);

    LIST_INIT(&pkt_wl->pkts);
    pkt_wl->nb_pkts = 0;
    pkt_wl->tot_payload = 0;
    pkt_wl->next_offset = next_offset;
    pkt_wl->wait_since = *now;
    pkt_wl->parser = parser_ref(parser);
    pkt_wl->config = config;
    pkt_wl->sync_with = sync_with;
    pkt_wl->list = config->lists + (config->list_seqnum % NB_ELEMS(config->lists));
    config->list_seqnum ++; // No need for atomicity for this usage
    if (0 != supermutex_lock(&pkt_wl->list->mutex)) return -1;
    TAILQ_INSERT_TAIL(&pkt_wl->list->list, pkt_wl, entry);
    supermutex_unlock(&pkt_wl->list->mutex);

    return 0;
}

void pkt_wait_list_dtor(struct pkt_wait_list *pkt_wl)
{
    SLOG(LOG_DEBUG, "Destruct pkt_wait_list @%p", pkt_wl);

    enum proto_parse_status status;
    pkt_wait_list_try_both(pkt_wl, &status, &timeval_unset, true);  // force parse of everything that can be

    // Now we are done with the parser
    parser_unref(&pkt_wl->parser);

    struct supermutex *const mutex = &pkt_wl->list->mutex;
    supermutex_lock_maydeadlock(mutex);
    TAILQ_REMOVE(&pkt_wl->list->list, pkt_wl, entry);
    pkt_wl->list = NULL;
    supermutex_unlock(mutex);

    // In case there's something left we couldn't parse (for instance if the parser returned PROTO_PARSE_ERR) then call the callback for each pending packet
    pkt_wait_list_empty(pkt_wl);
}

void pkt_wl_config_ctor(struct pkt_wl_config *config, char const *name, unsigned acceptable_gap, unsigned nb_pkts_max, size_t payload_max, unsigned timeout)
{
    config->name = name;
    config->acceptable_gap = acceptable_gap;
    config->nb_pkts_max = nb_pkts_max;
    config->payload_max = payload_max;
    config->timeout = timeout;
    config->list_seqnum = 0;
#   ifndef __GNUC__
    mutex_ctor(&config->atomic, "pkt_wl_config");
#   endif

    for (unsigned l = 0; l < NB_ELEMS(config->lists); l++) {
        TAILQ_INIT(&config->lists[l].list);
        supermutex_ctor(&config->lists[l].mutex, "pkt wl config");
    }

    mutex_lock(&pkt_wl_configs_mutex);
    SLIST_INSERT_HEAD(&pkt_wl_configs, config, entry);
    mutex_unlock(&pkt_wl_configs_mutex);
}

void pkt_wl_config_dtor(struct pkt_wl_config *config)
{
    mutex_lock(&pkt_wl_configs_mutex);
    SLIST_REMOVE(&pkt_wl_configs, config, pkt_wl_config, entry);
    mutex_unlock(&pkt_wl_configs_mutex);

    for (unsigned l = 0; l < NB_ELEMS(config->lists); l++) {
        if (! TAILQ_EMPTY(&config->lists[l].list)) {
            /* We cannot destruct the pkt_wait_lists since this may trigger the deletion of the parser
             * still owning it, which would then certainly also destruct the list.
             * Emptying the list would have the same result, ie deleting this list (and
             * probably others as well) while we are scanning the list. So be it. */
            SLOG(LOG_INFO, "Packet waiting list config@%p is not empty!", config);
        }
        supermutex_dtor(&config->lists[l].mutex);
    }

#   ifndef __GNUC__
    mutex_dtor(&config->atomic);
#   endif
}

// caller must own list->mutex (obviously)
static bool pkt_wait_list_try_locked(struct pkt_wait_list *pkt_wl, enum proto_parse_status *status, struct timeval const *now, bool force_timeout)
{
    unsigned const timeout = pkt_wl->config->timeout;
    struct timeval oldest = *now;
    if (timeout != 0 && timeval_is_set(&oldest)) timeval_sub_sec(&oldest, timeout);
    bool ret = false;

    while (1) {
        struct pkt_wait *pkt = LIST_FIRST(&pkt_wl->pkts);
        if (! pkt) break;

        if (! force_timeout) {
            if (pkt->offset > pkt_wl->next_offset) {
                // Timeout maybe?
                if (timeout == 0) break;
                if (!timeval_is_set(&oldest)) break;
                if (timeval_cmp(&pkt_wl->wait_since, &oldest) >= 0) break;
                // Ok we are tired with this segment, consider we won't receive it
            } else if (pkt_wl->sync_with && pkt->sync && pkt_wl->sync_with->next_offset < pkt->sync_offset) {
                // Must wait for the other direction
                break;
            }
        }
        force_timeout = false;  // this works only once (so that caller has a chance to advance the reciprocal waiting_list)
        // Advance this direction (gaps will be signaled)
        *status = pkt_wait_finalize(pkt, pkt_wl, now);
        ret = true;
    }

    return ret;
}

enum proto_parse_status pkt_wait_list_add(struct pkt_wait_list *pkt_wl, unsigned offset, unsigned next_offset, bool sync, unsigned sync_offset, bool can_parse, struct proto_info *parent, unsigned way, uint8_t const *packet, size_t cap_len, size_t wire_len, struct timeval const *now, size_t tot_cap_len, uint8_t const *tot_packet)
{
    enum proto_parse_status ret = PROTO_OK;

    if (! pkt_wl->list) return PROTO_PARSE_ERR;

    // TODO: how much efficient if pkt_wl and pkt_wl->sync_with were on the same lock... in pkt_wait_list_ctor, add a parameter to force the list we are on?
    if (0 != supermutex_lock(&pkt_wl->list->mutex)) return PROTO_PARSE_ERR;

    while (
        (pkt_wl->config->nb_pkts_max && pkt_wl->nb_pkts >= pkt_wl->config->nb_pkts_max) ||
        (pkt_wl->config->payload_max && pkt_wl->tot_payload >= pkt_wl->config->payload_max)
    ) {
        SLOG(LOG_DEBUG, "Waiting list too big (%u pkts, %zu bytes), force timeout", pkt_wl->nb_pkts, pkt_wl->tot_payload);
        enum proto_parse_status status = PROTO_OK;
        pkt_wait_list_try_locked(pkt_wl, &status, now, true);
        if (status == PROTO_OK && pkt_wl->sync_with) pkt_wait_list_try(pkt_wl->sync_with, &status, now, false); // TODO: see above about mutex
    }

    SLOG(LOG_DEBUG, "Add a packet of %zu bytes at offset %u to waiting list @%p", wire_len, offset, pkt_wl);
    if (sync) SLOG(LOG_DEBUG, "  ...waiting for reciprocal waiting list to reach offset %u (currently at %u)", sync_offset, pkt_wl->sync_with->next_offset);

    // Find its location and the previous pkt
    struct pkt_wait *prev = NULL;
    struct pkt_wait *next;
    LIST_FOREACH(next, &pkt_wl->pkts, entry) {
        // Stop whenever the next packet must be sent after (try to preserve packet numbers and order of arrival for subscribers)
        if (offset < next->offset) {
            break;
        }
        prev = next;
    }

    // if previous == NULL and pkt_wl->next_offset == offset _and_ we don't wait for another list then we can call proto_parse directly and then advance next_offset.
    if (! prev && pkt_wl->next_offset == offset && can_parse && (!pkt_wl->sync_with || !sync || pkt_wl->sync_with->next_offset >= sync_offset)) {
        /* Beware: calling a parser with this list config locked can deadlock!
         * Not if the parser called create a new list on the same config (since the mutex is recursive),
         * but if he wants to create a new list on another config which is already locked by another thread
         * who also want to lock the one we already own!
         * For instance, when several FTP parsers create simultaneously new TCP parsers because of contracking.
         * Yes, this does happen :-( */
        SLOG(LOG_DEBUG, "Parsing packet at once since we were waiting for it");
        ret = proto_parse(pkt_wl->parser, parent, way, packet, cap_len, wire_len, now, tot_cap_len, tot_packet);

        // Now parse as much as we can while advancing next_offset, returning the first error we obtain
        pkt_wl->next_offset = next_offset;
        pkt_wl->wait_since = *now;
        while (ret == PROTO_OK) {
            struct pkt_wait *pkt = LIST_FIRST(&pkt_wl->pkts);
            if (! pkt) break;
            if (pkt->offset > pkt_wl->next_offset) break;
            if (pkt_wl->sync_with && sync && pkt_wl->sync_with->next_offset < pkt->sync_offset) break;
            ret = pkt_wait_finalize(pkt, pkt_wl, &timeval_unset);   // no need to touch wait_since since we've just set it
        }
        goto quit;
    }

    // else if gap after previous > acceptable_gap then call subscribers directly and we are done
    unsigned prev_offset = prev ? prev->next_offset : pkt_wl->next_offset;
    if (
        (pkt_wl->config->acceptable_gap > 0 && (int)(offset - prev_offset) > (int)pkt_wl->config->acceptable_gap)
    ) {
        ret = proto_parse(NULL, parent, way, packet, cap_len, wire_len, now, tot_cap_len, tot_packet);
        goto quit;
    }

    // In all other more complex cases, insert the packet
    struct pkt_wait *pkt = pkt_wait_new(offset, next_offset, sync, sync_offset, parent, way, packet, cap_len, wire_len, tot_cap_len, tot_packet, now);
    if (! pkt) {
        ret = proto_parse(NULL, parent, way, NULL, 0, 0, now, tot_cap_len, tot_packet); // silently discard
        goto quit;
    }

    if (prev) {
        LIST_INSERT_AFTER(prev, pkt, entry);
    } else {
        LIST_INSERT_HEAD(&pkt_wl->pkts, pkt, entry);
    }
    pkt_wl->nb_pkts ++;
    pkt_wl->tot_payload += pkt->cap_len;

    // Maybe this packet content is enough to allow parsing (we end here in case its content overlap what's already there)
    if (can_parse && pkt->offset <= pkt_wl->next_offset && (! pkt_wl->sync_with || !sync || pkt_wl->sync_with->next_offset >= pkt->sync_offset)) {
        ret = pkt_wait_finalize(pkt, pkt_wl, now);  // may deadlock
    }   // else just wait

quit:
    supermutex_unlock(&pkt_wl->list->mutex);
    return ret;
}

bool pkt_wait_list_try(struct pkt_wait_list *pkt_wl, enum proto_parse_status *status, struct timeval const *now, bool force_timeout)
{
    if (! pkt_wl->list) return false;
    if (0 != supermutex_lock(&pkt_wl->list->mutex)) return false;

    bool ret = pkt_wait_list_try_locked(pkt_wl, status, now, force_timeout);

    supermutex_unlock(&pkt_wl->list->mutex);
    return ret;
}

void pkt_wait_list_try_both(struct pkt_wait_list *pkt_wl, enum proto_parse_status *status, struct timeval const *now, bool force_timeout)
{
    *status = PROTO_OK;

    if (pkt_wl->sync_with) {
        /* Synchronize two waiting lists, ie do not advance one before what the other acked.
         * Notice that even if we ultimately want to timeout each packet we still want to parse
         * them in correct order. */
        while (*status == PROTO_OK) {
            // try first without timeout *
            if (
                ! pkt_wait_list_try(pkt_wl, status, now, false) &&
                ! pkt_wait_list_try(pkt_wl->sync_with, status, now, false)
            ) { // ok so we need to timeout one of these
                if (! force_timeout) break;
                /* If pkt_wl ack_num is beyond pkt_wl->sync_with seq_num then we must start by pkt_wl->sync_with.
                 * In the other way araound we must start by pkt_wl. If no ack_num comes after any seq_num then
                 * we don't care. */
                struct pkt_wait *const pkt = LIST_FIRST(&pkt_wl->pkts);
                struct pkt_wait *const sync_with_pkt = LIST_FIRST(&pkt_wl->sync_with->pkts);
                if (! sync_with_pkt) return;    // why were we here in the first place?
                if (pkt && pkt->sync_offset > sync_with_pkt->offset) {
                    if (! pkt_wait_list_try(pkt_wl->sync_with, status, now, true)) assert(!"Low battery");
                } else {
                    if (! pkt_wait_list_try(pkt_wl, status, now, true)) break;
                }
            }
        }
    } else {
        /* Nothing to synchronize with */
        while (
            *status == PROTO_OK &&
            pkt_wait_list_try(pkt_wl, status, now, force_timeout)
        ) ;
    }
}

bool pkt_wait_list_is_complete(struct pkt_wait_list *pkt_wl, unsigned start_offset, unsigned end_offset)
{
    unsigned end = start_offset;
    struct pkt_wait *pkt;
    bool ret = false;

    if (! pkt_wl->list) return false;
    if (0 != supermutex_lock(&pkt_wl->list->mutex)) return false;   // will retry later

    LIST_FOREACH(pkt, &pkt_wl->pkts, entry) {
        if (pkt->next_offset <= end) continue;
        if (pkt->offset > end) break;
        end = pkt->next_offset;
        if (end >= end_offset) {
            ret = true;
            break;
        }
    }

    supermutex_unlock(&pkt_wl->list->mutex);
    return ret;
}

uint8_t *pkt_wait_list_reassemble(struct pkt_wait_list *pkt_wl, unsigned start_offset, unsigned end_offset)
{
    assert(end_offset >= start_offset);

    if (! pkt_wl->list) return NULL;

    SLOG(LOG_DEBUG, "Reassemble pkt_wl@%p from offset %u to %u", pkt_wl, start_offset, end_offset);

    uint8_t *payload = objalloc_nice(end_offset - start_offset, "waiting plds");
    if (! payload) {
        SLOG(LOG_DEBUG, "Cannot alloc for packet reassembly of %zu bytes", pkt_wl->tot_payload);
        return NULL;
    }

    if (0 != supermutex_lock(&pkt_wl->list->mutex)) return NULL;

    unsigned end = start_offset;   // we filled payload up to there
    struct pkt_wait *pkt;
    LIST_FOREACH(pkt, &pkt_wl->pkts, entry) {
        if (end == end_offset) break;
        if (pkt->next_offset <= end) continue;
        if (pkt->offset > end) break;
        unsigned const trim_left = end - pkt->offset;
        if (trim_left >= pkt->cap_len) break;
        unsigned next_end = pkt->offset + pkt->cap_len;
        if (next_end > end_offset) next_end = end_offset;
        SLOG(LOG_DEBUG, "  Copy from pkt@%p from offset %u (%u) %u bytes at location %u", pkt, trim_left, pkt->offset + trim_left, next_end - end, end-start_offset);
        assert(next_end <= end_offset); // no buffer overrun
        assert(trim_left + (next_end-end) <= pkt->cap_len); // don't read out of pkt->packet
        assert(pkt->start + trim_left + (next_end-end) <= pkt->tot_cap_len); // really, I mean it!
        memcpy(payload + (end-start_offset), pkt->packet + pkt->start + trim_left, next_end - end);
        end = next_end;
    }

    if (end != end_offset) {
        objfree(payload);
        payload = NULL;
    }

    supermutex_unlock(&pkt_wl->list->mutex);
    return payload;
}

/*
 * Extensions
 */

static struct ext_function sg_wait_list_names;
static SCM g_wait_list_names(void)
{
    SCM ret = SCM_EOL;
    mutex_lock(&pkt_wl_configs_mutex);
    struct pkt_wl_config *config;
    SLIST_FOREACH(config, &pkt_wl_configs, entry) {
        ret = scm_cons(scm_from_locale_string(config->name), ret);
    }
    mutex_unlock(&pkt_wl_configs_mutex);
    return ret;
}

static struct pkt_wl_config *pkt_wl_config_of_scm_name(SCM name_)
{
    char *name = scm_to_tempstr(name_);
    struct pkt_wl_config *config;
    SLIST_LOOKUP_LOCKED(config, &pkt_wl_configs, entry, 0 == strcasecmp(name, config->name), &pkt_wl_configs_mutex);
    return config;
}

static SCM timeout_sym;
static SCM max_payload_sym;
static SCM max_packets_sym;
static SCM acceptable_gap_sym;

static struct ext_function sg_wait_list_stats;
static SCM g_wait_list_stats(SCM name_)
{
    struct pkt_wl_config *config = pkt_wl_config_of_scm_name(name_);
    if (! config) return SCM_UNSPECIFIED;

    return scm_list_4(
        scm_cons(timeout_sym,        scm_from_uint(config->timeout)),
        scm_cons(max_payload_sym,    scm_from_size_t(config->payload_max)),
        scm_cons(max_packets_sym,    scm_from_uint(config->nb_pkts_max)),
        scm_cons(acceptable_gap_sym, scm_from_uint(config->acceptable_gap)));
}

static struct ext_function sg_wait_list_set_max_payload;
static SCM g_wait_list_set_max_payload(SCM name_, SCM payload_max_)
{
    struct pkt_wl_config *config = pkt_wl_config_of_scm_name(name_);
    if (! config) return SCM_BOOL_F;
    config->payload_max = scm_to_size_t(payload_max_);
    return SCM_BOOL_T;
}

static struct ext_function sg_wait_list_set_max_pkts;
static SCM g_wait_list_set_max_pkts(SCM name_, SCM pkts_max_)
{
    struct pkt_wl_config *config = pkt_wl_config_of_scm_name(name_);
    if (! config) return SCM_BOOL_F;
    config->nb_pkts_max = scm_to_uint(pkts_max_);
    return SCM_BOOL_T;
}

static struct ext_function sg_wait_list_set_max_gap;
static SCM g_wait_list_set_max_gap(SCM name_, SCM gap_max_)
{
    struct pkt_wl_config *config = pkt_wl_config_of_scm_name(name_);
    if (! config) return SCM_BOOL_F;
    config->acceptable_gap = scm_to_uint(gap_max_);
    return SCM_BOOL_T;
}

static struct ext_function sg_wait_list_set_timeout;
static SCM g_wait_list_set_timeout(SCM name_, SCM timeout_)
{
    struct pkt_wl_config *config = pkt_wl_config_of_scm_name(name_);
    if (! config) return SCM_BOOL_F;
    config->timeout = scm_to_uint(timeout_);
    return SCM_BOOL_T;
}

void pkt_wait_list_init(void)
{
    log_category_pkt_wait_list_init();
    mutex_ctor(&pkt_wl_configs_mutex, "pkt_wls_list");

    timeout_sym        = scm_permanent_object(scm_from_latin1_symbol("timeout"));
    max_payload_sym    = scm_permanent_object(scm_from_latin1_symbol("max-payload"));
    max_packets_sym    = scm_permanent_object(scm_from_latin1_symbol("max-packets"));
    acceptable_gap_sym = scm_permanent_object(scm_from_latin1_symbol("acceptable-gap"));

    ext_function_ctor(&sg_wait_list_names,
        "wait-list-names", 0, 0, 0, g_wait_list_names,
        "(wait-list-names): get the names of all existing waiting lists.\n"
        "See also (? 'wait-list-stats).\n");

    ext_function_ctor(&sg_wait_list_stats,
        "wait-list-stats", 1, 0, 0, g_wait_list_stats,
        "(wait-list-stats \"name\"): get stats about this waiting list.\n"
        "See also (? 'wait-list-names).\n");

    ext_function_ctor(&sg_wait_list_set_max_payload,
        "wait-list-set-max-payload", 2, 0, 0, g_wait_list_set_max_payload,
        "(wait-list-set-max-payload \"name\" bytes): sets the maximum kept payload per waiting list (0 for no limit - not advised!).\n"
        "See also (? 'wait-list-set-max-packets).\n");

    ext_function_ctor(&sg_wait_list_set_max_pkts,
        "wait-list-set-max-packets", 2, 0, 0, g_wait_list_set_max_pkts,
        "(wait-list-set-max-packets \"name\" packets): sets the maximum kept packets per waiting list (0 for no limit).\n"
        "See also (? 'wait-list-set-max-payload).\n");

    ext_function_ctor(&sg_wait_list_set_max_gap,
        "wait-list-set-gap-max", 2, 0, 0, g_wait_list_set_max_gap,
        "(wait-list-set-gap-max \"name\" bytes): sets the maximum acceptable gap between two kept packets (0 for no limit).\n");

    ext_function_ctor(&sg_wait_list_set_timeout,
        "wait-list-set-timeout", 2, 0, 0, g_wait_list_set_timeout,
        "(wait-list-set-timeout \"name\" seconds): sets the delay after which kept packets are droped (0 for no limit - not advised!).\n");
}

void pkt_wait_list_fini(void)
{
    log_category_pkt_wait_list_fini();
    mutex_dtor(&pkt_wl_configs_mutex);
}
