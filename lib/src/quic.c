// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2016-2019, NetApp, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <inttypes.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>

// IWYU pragma: no_include <picotls/../picotls.h>

#include <picotls.h> // IWYU pragma: keep
#include <quant/quant.h>
#include <timeout.h>

#ifdef HAVE_ASAN
#include <sanitizer/asan_interface.h>
#else
#define ASAN_POISON_MEMORY_REGION(x, y)
#define ASAN_UNPOISON_MEMORY_REGION(x, y)
#endif

#if !defined(NDEBUG) && !defined(FUZZING) &&                                   \
    !defined(NO_FUZZER_CORPUS_COLLECTION)
#include <errno.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "conn.h"
#include "loop.h"
#include "pkt.h"
#include "pn.h"
#include "qlog.h"
#include "quic.h"
#include "recovery.h"
#include "stream.h"
#include "tls.h"


// TODO: many of these globals should move to a per-engine struct

char __cid_str[CID_STR_LEN];
char __srt_str[hex_str_len(SRT_LEN)];
char __tok_str[hex_str_len(MAX_TOK_LEN)];


/// QUIC version supported by this implementation in order of preference.
const uint32_t ok_vers[] = {
#ifndef NDEBUG
    0xbabababa, // reserved version to trigger negotiation, TODO: randomize
#endif
    0x45474700 + DRAFT_VERSION, // quant private version -xx
    0xff000000 + DRAFT_VERSION, // draft-ietf-quic-transport-xx
};

/// Length of the @p ok_vers array.
const uint8_t ok_vers_len = sizeof(ok_vers) / sizeof(ok_vers[0]);


struct q_conn_sl accept_queue = sl_head_initializer(accept_queue);

static struct timeout api_alarm = TIMEOUT_INITIALIZER(0);

#if !defined(NDEBUG) && !defined(FUZZING) &&                                   \
    !defined(NO_FUZZER_CORPUS_COLLECTION)
int corpus_pkt_dir, corpus_frm_dir;
#endif


void alloc_off(struct w_engine * const w,
               struct w_iov_sq * const q,
               const int af,
               const uint32_t len,
               const uint16_t off)
{
    w_alloc_len(w, af, q, len, MAX_PKT_LEN - AEAD_LEN - off, off);
    struct w_iov * v;
    sq_foreach (v, q, next) {
        struct pkt_meta * const m = &meta(v);
        ASAN_UNPOISON_MEMORY_REGION(m, sizeof(*m));
        m->strm_data_pos = off;
    }
}


void free_iov(struct w_iov * const v, struct pkt_meta * const m)
{
    if (m->txed) {
        if (m->acked == false && m->lost == false &&
            m->pn->abandoned == false) {
            m->strm = 0;
            on_pkt_lost(m, false);
        }

        struct pkt_meta * m_rtx = sl_first(&m->rtx);
        if (unlikely(m_rtx)) {
            // this pkt has prior or later RTXs
            if (m->has_rtx) {
                // this pkt has an RTX
                sl_remove(&m_rtx->rtx, m, pkt_meta, rtx_next);

            } else {
                // this is the last ("real") RTX of a packet
                while (m_rtx) {
                    m_rtx->strm = 0;
                    ensure(m_rtx->has_rtx, "was RTX'ed");
                    sl_remove_head(&m->rtx, rtx_next);
                    sl_remove_head(&m_rtx->rtx, rtx_next);
                    m_rtx = sl_next(m_rtx, rtx_next);
                }
            }
        }
    }

    memset(m, 0, sizeof(*m));
    ASAN_POISON_MEMORY_REGION(m, sizeof(*m));
    w_free_iov(v);
}


struct w_iov * alloc_iov(struct w_engine * const w,
                         const int af,
                         const uint16_t len,
                         const uint16_t off,
                         struct pkt_meta ** const m)
{
    struct w_iov * const v = w_alloc_iov(w, af, len, off);
    ensure(v, "w_alloc_iov failed");
    *m = &meta(v);
    ASAN_UNPOISON_MEMORY_REGION(*m, sizeof(**m));
    (*m)->strm_data_pos = off;
    return v;
}


struct w_iov * dup_iov(const struct w_iov * const v,
                       struct pkt_meta ** const mdup,
                       const uint16_t off)
{
    struct w_iov * const vdup = w_alloc_iov(v->w, v->wv_af, v->len - off, 0);
    ensure(vdup, "w_alloc_iov failed");
    if (mdup) {
        *mdup = &meta(vdup);
        ASAN_UNPOISON_MEMORY_REGION(*mdup, sizeof(**mdup));
    }
    memcpy(vdup->buf, v->buf + off, v->len - off);
    memcpy(&vdup->saddr, &v->saddr, sizeof(v->saddr));
    vdup->flags = v->flags;
    return vdup;
}


void q_alloc(struct w_engine * const w,
             struct w_iov_sq * const q,
             const int af,
             const size_t len)
{
    ensure(len <= UINT32_MAX, "len %zu too long", len);
    alloc_off(w, q, af, (uint32_t)len, DATA_OFFSET);
}


void q_free(struct w_iov_sq * const q)
{
    while (!sq_empty(q)) {
        struct w_iov * const v = sq_first(q);
        sq_remove_head(q, next);
        sq_next(v, next) = 0;
        free_iov(v, &meta(v));
    }
}


static void __attribute__((nonnull)) mark_fin(struct w_iov_sq * const q)
{
    struct w_iov * const last = sq_last(q, w_iov, next);
    ensure(last, "got last buffer");
    meta(last).is_fin = true;
}


struct q_conn * q_connect(struct w_engine * const w,
                          const struct sockaddr * const peer,
                          const char * const peer_name,
                          struct w_iov_sq * const early_data,
                          struct q_stream ** const early_data_stream,
                          const bool fin,
                          const char * const alpn,
                          const struct q_conn_conf * const conf)
{
    // make new connection
    struct w_sockaddr p;

    uint16_t addr_idx = UINT16_MAX;
    if (peer->sa_family == AF_INET && w->have_ip4) {
        addr_idx = w->addr4_pos;
        p.port = ((const struct sockaddr_in *)(const void *)peer)->sin_port;
    } else if (peer->sa_family == AF_INET6 && w->have_ip6) {
        addr_idx = 0;
        p.port = ((const struct sockaddr_in6 *)(const void *)peer)->sin6_port;
    }

    if (unlikely(addr_idx == UINT16_MAX ||
                 w_to_waddr(&p.addr, peer) == false)) {
        warn(CRT, "address family error");
        return 0;
    }

    struct q_conn * const c =
        new_conn(w, addr_idx, 0, 0, &p, peer_name, 0, conf);

    // init TLS
    init_tls(c, peer_name, alpn);
    init_tp(c);

    // if we have no early data, we're not trying 0-RTT
    c->try_0rtt &= early_data && early_data_stream;

    warn(WRN, "new %u-RTT %s conn %s to %s:%u, %" PRIu " byte%s queued for TX",
         c->try_0rtt ? 0 : 1, conn_type(c), cid_str(c->scid),
         w_ntop(&p.addr, ip_tmp), bswap16(p.port),
         early_data ? w_iov_sq_len(early_data) : 0,
         plural(early_data ? w_iov_sq_len(early_data) : 0));

    restart_idle_alarm(c);
    w_connect(c->sock, peer);

    // start TLS handshake
    tls_io(c->cstrms[ep_init], 0);

    if (early_data && !sq_empty(early_data)) {
        ensure(early_data_stream, "early data without stream pointer");
        // queue up early data
        if (fin)
            mark_fin(early_data);
        *early_data_stream = new_stream(c, c->next_sid_bidi);
        concat_out(*early_data_stream, early_data);
    } else if (early_data_stream)
        *early_data_stream = 0;

    timeouts_add(ped(w)->wheel, &c->tx_w, 0);

    warn(DBG, "waiting for connect on %s conn %s to %s:%u", conn_type(c),
         cid_str(c->scid), w_ntop(&p.addr, ip_tmp), bswap16(p.port));
    conn_to_state(c, conn_opng);
    loop_run(w, (func_ptr)q_connect, c, 0);

    if (fin && early_data_stream && *early_data_stream)
        strm_to_state(*early_data_stream,
                      (*early_data_stream)->state == strm_hcrm ? strm_clsd
                                                               : strm_hclo);

    if (c->state != conn_estb) {
        warn(WRN, "%s conn %s not connected", conn_type(c), cid_str(c->scid));
        return 0;
    }

    warn(WRN, "%s conn %s connected%s, cipher %s", conn_type(c),
         cid_str(c->scid), c->did_0rtt ? " after 0-RTT" : "",
         c->pns[pn_data]
             .data.out_1rtt[c->pns[pn_data].data.out_kyph]
             .aead->algo->name);

    return c;
}


bool q_write(struct q_stream * const s,
             struct w_iov_sq * const q,
             const bool fin)
{
    struct q_conn * const c = s->c;
    if (unlikely(c->state == conn_qlse || c->state == conn_drng ||
                 c->state == conn_clsd)) {
        warn(ERR, "%s conn %s is in state %s, can't write", conn_type(c),
             cid_str(c->scid), conn_state_str[c->state]);
        return false;
    }

    if (unlikely(s->state == strm_hclo || s->state == strm_clsd)) {
        warn(ERR, "%s conn %s strm " FMT_SID " is in state %s, can't write",
             conn_type(c), cid_str(c->scid), s->id, strm_state_str[s->state]);
        return false;
    }

    // add to stream
    if (fin) {
        if (sq_empty(q))
            alloc_off(c->w, q, q_conn_af(s->c), 1, DATA_OFFSET);
        mark_fin(q);
        // strm_to_state(s, s->state == strm_hcrm ? strm_clsd : strm_hclo);
    }

    warn(WRN,
         "writing %" PRIu " byte%s %sin %" PRIu
         " buf%s on %s conn %s strm " FMT_SID,
         w_iov_sq_len(q), plural(w_iov_sq_len(q)), fin ? "(and FIN) " : "",
         w_iov_sq_cnt(q), plural(w_iov_sq_cnt(q)), conn_type(c),
         cid_str(c->scid), s->id);

    concat_out(s, q);

    // kick TX watcher
    timeouts_add(ped(c->w)->wheel, &c->tx_w, 0);
    return true;
}


struct q_stream *
q_read(struct q_conn * const c, struct w_iov_sq * const q, const bool all)
{
    struct q_stream * s = 0;
    do {
        kh_foreach_value(&c->strms_by_id, s, {
            if (!sq_empty(&s->in) || s->state == strm_clsd)
                // we found a stream with queued data
                break;
        });

        if (s == 0 && all) {
            // no data queued on any stream, wait for new data
            warn(WRN, "waiting to read on any strm on %s conn %s", conn_type(c),
                 cid_str(c->scid));
            loop_run(c->w, (func_ptr)q_read, c, 0);
        }
    } while (s == 0 && all);

    if (s && s->state != strm_clsd)
        q_read_stream(s, q, false);

    return s;
}


bool q_read_stream(struct q_stream * const s,
                   struct w_iov_sq * const q,
                   const bool all)
{
    struct q_conn * const c = s->c;
    if (unlikely(c->state != conn_estb))
        return 0;

    if (q_peer_closed_stream(s) == false && all) {
        warn(WRN, "reading all on %s conn %s strm " FMT_SID, conn_type(c),
             cid_str(c->scid), s->id);
    again:
        loop_run(c->w, (func_ptr)q_read_stream, c, s);
    }

    if (sq_empty(&s->in))
        return false;

    struct w_iov * const last = sq_last(&s->in, w_iov, next);
    const struct pkt_meta * const m_last = &meta(last);

    warn(WRN,
         "read %" PRIu " new byte%s %sin %" PRIu " buf%s on %s "
         "conn %s strm " FMT_SID,
         w_iov_sq_len(&s->in), plural(w_iov_sq_len(&s->in)),
         m_last->is_fin ? "(and FIN) " : "", w_iov_sq_cnt(&s->in),
         plural(w_iov_sq_cnt(&s->in)), conn_type(c), cid_str(c->scid), s->id);

    sq_concat(q, &s->in);
    if (all && m_last->is_fin == false)
        goto again;

    return true;
}


struct q_conn *
q_bind(struct w_engine * const w, const uint16_t addr_idx, const uint16_t port)
{
    // bind socket and create new embryonic server connection
    struct q_conn * const c =
        new_conn(w, addr_idx, 0, 0, 0, 0, bswap16(port), 0);
    if (likely(c))
        warn(INF, "bound %s socket to %s:%u", conn_type(c),
             w_ntop(&c->sock->ws_laddr, ip_tmp), port);
    return c;
}


static void cancel_api_call(void)
{
#ifdef DEBUG_EXTRA
    warn(DBG, "canceling API call");
#endif
    timeout_del(&api_alarm);
    maybe_api_return(q_accept, 0, 0);
    maybe_api_return(q_ready, 0, 0);
}


static void __attribute__((nonnull))
restart_api_alarm(struct w_engine * const w, const uint64_t nsec)
{
#ifdef DEBUG_TIMERS
    warn(DBG, "next API alarm in %f sec", nsec / (double)NS_PER_S);
#endif

    timeouts_add(ped(w)->wheel, &api_alarm, nsec);
}


struct q_conn * q_accept(struct w_engine * const w,
                         const struct q_conn_conf * const conf)
{
    if (sl_first(&accept_queue))
        goto accept;

    const uint_t idle_to = get_conf(w, conf, idle_timeout);
    warn(WRN, "waiting for conn on any serv sock (timeout %" PRIu " ms)",
         idle_to);
    if (idle_to)
        restart_api_alarm(w, idle_to * NS_PER_MS);

    loop_run(w, (func_ptr)q_accept, 0, 0);

    if (sl_empty(&accept_queue)) {
        warn(ERR, "no conn ready for accept");
        return 0;
    }

accept:;
    struct q_conn * const c = sl_first(&accept_queue);
    sl_remove_head(&accept_queue, node_aq);
    restart_idle_alarm(c);
    c->needs_accept = false;

    warn(WRN, "%s conn %s accepted from clnt %s:%u%s, cipher %s", conn_type(c),
         cid_str(c->scid), w_ntop(&c->peer.addr, ip_tmp), bswap16(c->peer.port),
         c->did_0rtt ? " after 0-RTT" : "",
         c->pns[pn_data]
             .data.out_1rtt[c->pns[pn_data].data.out_kyph]
             .aead->algo->name);

    update_conf(c, conf);
    return c;
}


struct q_stream * q_rsv_stream(struct q_conn * const c, const bool bidi)
{
    if (unlikely(c->state == conn_drng || c->state == conn_clsd))
        return 0;

    const uint_t * const max_streams =
        bidi ? &c->tp_out.max_strms_bidi : &c->tp_out.max_strms_uni;

    if (unlikely(*max_streams == 0))
        warn(WRN, "peer hasn't allowed %s streams", bidi ? "bi" : "uni");

    dint_t * const next_sid = bidi ? &c->next_sid_bidi : &c->next_sid_uni;
    const uint_t next = (uint_t)(*next_sid >> 2);
    if (unlikely(next >= *max_streams)) {
        // we hit the max stream limit, wait for MAX_STREAMS frame
        warn(WRN, "need %s MAX_STREAMS increase (%" PRIu " >= %" PRIu ")",
             bidi ? "bi" : "uni", next, *max_streams);
        if (bidi)
            c->sid_blocked_bidi = true;
        else
            c->sid_blocked_uni = true;
        loop_run(c->w, (func_ptr)q_rsv_stream, c, 0);
    }

    // stream blocking is handled by new_stream
    return new_stream(c, *next_sid);
}


#if !defined(NDEBUG) && !defined(FUZZING) &&                                   \
    !defined(NO_FUZZER_CORPUS_COLLECTION)
static int __attribute__((nonnull))
mk_or_open_dir(const char * const path, mode_t mode)
{
    int fd = mkdir(path, mode);
    ensure(fd == 0 || (fd == -1 && errno == EEXIST), "mkdir %s", path);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    ensure(fd != -1, "open %s", path);
    return fd;
}
#endif


struct w_engine * q_init(const char * const ifname,
                         const struct q_conf * const conf)
{
    // initialize warpcore on the given interface
    const uint32_t num_bufs = conf && conf->num_bufs ? conf->num_bufs : 10000;
    struct w_engine * const w = w_init(ifname, 0, num_bufs);
    const uint_t num_bufs_ok = w_iov_sq_cnt(&w->iov);
    if (num_bufs_ok < num_bufs)
        warn(WRN, "only allocated %" PRIu "/%" PRIu32 " warpcore buffers ",
             num_bufs_ok, num_bufs);

    w->data = calloc(1, sizeof(struct per_engine_data) + w->mtu);
    ensure(w->data, "could not calloc");
    ped(w)->scratch_len = w->mtu;

    ped(w)->pkt_meta = calloc(num_bufs, sizeof(*ped(w)->pkt_meta));
    ensure(ped(w)->pkt_meta, "could not calloc");
    ASAN_POISON_MEMORY_REGION(ped(w)->pkt_meta,
                              num_bufs * sizeof(*ped(w)->pkt_meta));
    ped(w)->num_bufs = num_bufs;

    ped(w)->default_conn_conf =
        (struct q_conn_conf){.idle_timeout = 10,
                             .enable_udp_zero_checksums = true,
                             .tls_key_update_frequency = 3,
                             .version = ok_vers[0],
                             .enable_spinbit =
#ifndef NDEBUG
                                 true
#else
                                 false
#endif
        };

    if (conf && conf->conn_conf) {
        // update default connection configuration
        ped(w)->default_conn_conf.version =
            get_conf(w, conf->conn_conf, version);
        ped(w)->default_conn_conf.idle_timeout =
            get_conf(w, conf->conn_conf, idle_timeout);
        ped(w)->default_conn_conf.tls_key_update_frequency =
            get_conf(w, conf->conn_conf, tls_key_update_frequency);
        ped(w)->default_conn_conf.enable_spinbit =
            get_conf_uncond(w, conf->conn_conf, enable_spinbit);
        ped(w)->default_conn_conf.enable_udp_zero_checksums =
            get_conf_uncond(w, conf->conn_conf, enable_udp_zero_checksums);
        ped(w)->default_conn_conf.enable_tls_key_updates =
            get_conf_uncond(w, conf->conn_conf, enable_tls_key_updates);
        ped(w)->default_conn_conf.disable_active_migration =
            get_conf_uncond(w, conf->conn_conf, disable_active_migration);
        ped(w)->default_conn_conf.enable_zero_len_cid =
            get_conf_uncond(w, conf->conn_conf, enable_zero_len_cid);
    }

    // initialize the event loop
    loop_init();
    int err;
    ped(w)->wheel = timeouts_open(TIMEOUT_nHZ, &err);
    timeouts_update(ped(w)->wheel, loop_now());
    timeout_setcb(&api_alarm, cancel_api_call, 0);

    warn(INF, "%s/%s (%s) %s/%s ready", quant_name, w->backend_name,
         w->backend_variant, quant_version, QUANT_COMMIT_HASH_ABBREV_STR);
    warn(DBG, "submit bug reports at https://github.com/NTAP/quant/issues");

    // initialize TLS context
    init_tls_ctx(conf, &ped(w)->tls_ctx);

#if !defined(NDEBUG) && !defined(NO_FUZZER_CORPUS_COLLECTION)
#ifdef FUZZING
    warn(CRT, "%s compiled for fuzzing - will not communicate", quant_name);
#else
    // create the directories for exporting fuzzer corpus data
    warn(NTE, "debug build, storing fuzzer corpus data");
    corpus_pkt_dir = mk_or_open_dir("../corpus_pkt", 0755);
    corpus_frm_dir = mk_or_open_dir("../corpus_frm", 0755);
#endif
#endif

#ifndef NO_QLOG
    if (conf && conf->qlog && *conf->qlog) {
        qlog = fopen(conf->qlog, "we");
        ensure(qlog, "fopen %s", conf->qlog);
    }
#endif

    return w;
}


void q_close_stream(struct q_stream * const s)
{
    warn(WRN, "closing strm " FMT_SID " on %s conn %s", s->id, conn_type(s->c),
         cid_str(s->c->scid));
    struct w_iov_sq q = w_iov_sq_initializer(q);
    q_write(s, &q, true);
}


void q_free_stream(struct q_stream * const s)
{
    free_stream(s);
}


void q_stream_get_written(struct q_stream * const s, struct w_iov_sq * const q)
{
    if (s->out_una == 0) {
        sq_concat(q, &s->out);
        return;
    }

    struct w_iov * v = sq_first(&s->out);
    while (v != s->out_una) {
        sq_remove_head(&s->out, next);
        sq_next(v, next) = 0;
        sq_insert_tail(q, v, next);
        v = sq_first(&s->out);
    }
}


void q_close(struct q_conn * const c,
             const uint64_t code,
             const char * const reason
#if defined(NO_ERR_REASONS) && defined(NDEBUG)
             __attribute__((unused))
#endif
)
{
    if (c->scid)
        warn(WRN,
             "closing %s conn %s on port %u w/err %s0x%" PRIx64 "%s%s%s" NRM,
             conn_type(c), cid_str(c->scid), bswap16(c->sock->ws_lport),
             code ? RED : NRM, code, reason ? " (" : "", reason ? reason : "",
             reason ? ")" : "");

    c->err_code = (uint_t)code;
#ifndef NO_ERR_REASONS
    if (reason) {
        strncpy(c->err_reason, reason, MAX_ERR_REASON_LEN);
        c->err_reason_len = (uint8_t)strnlen(reason, MAX_ERR_REASON_LEN);
    }
#endif

    if (c->state == conn_idle || c->state == conn_clsd ||
        (!is_clnt(c) && c->holds_sock))
        // we don't need to do the closing dance in these cases
        goto done;

    if (c->state != conn_drng) {
        conn_to_state(c, conn_qlse);
        timeouts_add(ped(c->w)->wheel, &c->tx_w, 0);
    }

    loop_run(c->w, (func_ptr)q_close, c, 0);

done:
#ifndef NO_QINFO
    if (c->scid && c->i.pkts_in_valid > 0) {
        conn_info_populate(c);
        warn(INF, "%s conn %s stats:", conn_type(c), cid_str(c->scid));
        warn(INF, "\tpkts_in_valid = %s%" PRIu NRM,
             c->i.pkts_in_valid ? NRM : BLD RED, c->i.pkts_in_valid);
        warn(INF, "\tpkts_in_invalid = %s%" PRIu NRM,
             c->i.pkts_in_invalid ? BLD RED : NRM, c->i.pkts_in_invalid);
        warn(INF, "\tpkts_out = %" PRIu, c->i.pkts_out);
        warn(INF, "\tpkts_out_lost = %" PRIu, c->i.pkts_out_lost);
        warn(INF, "\tpkts_out_rtx = %" PRIu, c->i.pkts_out_rtx);
        warn(INF, "\trtt = %.3f", c->i.rtt / (float)NS_PER_S);
        warn(INF, "\trttvar = %.3f", c->i.rttvar / (float)NS_PER_S);
        warn(INF, "\tcwnd = %" PRIu, c->i.cwnd);
        warn(INF, "\tssthresh = %" PRIu, c->i.ssthresh);
        warn(INF, "\tpto_cnt = %" PRIu, c->i.pto_cnt);
    }
#endif
    free_conn(c);
#ifndef NO_QLOG
    fflush(qlog);
#endif
}


void q_cleanup(struct w_engine * const w)
{
    // close all connections
    struct q_conn * c;
    kh_foreach_value(&conns_by_id, c, { q_close(c, 0, 0); });
    kh_foreach_value(&conns_by_ipnp, c, { q_close(c, 0, 0); });
#ifndef NO_SRT_MATCHING
    kh_foreach_value(&conns_by_srt, c, { q_close(c, 0, 0); });
#endif

    // stop the event loop
    timeouts_close(ped(w)->wheel);

#ifndef NO_OOO_0RTT
    // free 0-RTT reordering cache
    while (!splay_empty(&ooo_0rtt_by_cid)) {
        struct ooo_0rtt * const zo =
            splay_min(ooo_0rtt_by_cid, &ooo_0rtt_by_cid);
        ensure(splay_remove(ooo_0rtt_by_cid, &ooo_0rtt_by_cid, zo), "removed");
        free(zo);
    }
#endif

#ifdef HAVE_ASAN
    for (uint_t i = 0; i < ped(w)->num_bufs; i++) {
        struct pkt_meta * const m = &ped(w)->pkt_meta[i];
        if (__asan_address_is_poisoned(m) == false) {
            warn(DBG, "buffer %" PRIu " still in use for %cX'ed %s pkt %" PRIu,
                 i, m->txed ? 'T' : 'R',
                 pkt_type_str(m->hdr.flags, &m->hdr.vers),
                 has_pkt_nr(m->hdr.flags, m->hdr.vers) ? m->hdr.nr : 0);
        }
    }
#endif

    kh_release(conns_by_id, &conns_by_id);
    kh_release(conns_by_ipnp, &conns_by_ipnp);
#ifndef NO_SRT_MATCHING
    kh_release(conns_by_srt, &conns_by_srt);
#endif

    free_tls_ctx(&ped(w)->tls_ctx);
    free(ped(w)->pkt_meta);
    free(w->data);
    w_cleanup(w);

#if !defined(NDEBUG) && !defined(FUZZING) &&                                   \
    !defined(NO_FUZZER_CORPUS_COLLECTION)
    close(corpus_pkt_dir);
    close(corpus_frm_dir);
#endif
    qlog_close();
}


const char *
q_cid(struct q_conn * const c, char * const buf, const size_t buf_len)
{
    ensure(buf_len >= hex_str_len(CID_LEN_MAX),
           "buf too short (need at least %lu)",
           (unsigned long)hex_str_len(CID_LEN_MAX));
    if (likely(c->scid))
        hex2str(c->scid->id, c->scid->len, buf, buf_len);
    else
        *buf = 0;
    return buf;
}


uint_t q_sid(const struct q_stream * const s)
{
    return (uint_t)s->id;
}


bool q_is_stream_closed(const struct q_stream * const s)
{
    return s->state == strm_clsd;
}


bool q_peer_closed_stream(const struct q_stream * const s)
{
    return s->state == strm_hcrm || s->state == strm_clsd;
}


bool q_is_conn_closed(const struct q_conn * const c)
{
    return c->state == conn_clsd;
}


#if !defined(NDEBUG) && !defined(FUZZING) &&                                   \
    !defined(NO_FUZZER_CORPUS_COLLECTION)
void write_to_corpus(const int dir, const void * const data, const size_t len)
{
    const uint64_t rand = w_rand64();
    char file[MAXPATHLEN];
    hex2str((const uint8_t *)&rand, sizeof(rand), file, sizeof(file));
    const int fd =
        openat(dir, file, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
    if (fd == -1) {
        warn(ERR, "cannot open corpus file %s", file);
        goto done;
    }
    if (write(fd, data, len) == -1) {
        warn(ERR, "cannot write corpus file %s", file);
        goto done;
    }
done:
    close(fd);
}
#endif


bool q_ready(struct w_engine * const w,
             const uint64_t nsec,
             struct q_conn ** const ready)
{
    if (sl_empty(&c_ready)) {
        if (nsec)
            restart_api_alarm(w, nsec);
#ifdef DEBUG_EXTRA
        warn(WRN, "waiting for conn to get ready");
#endif
        loop_run(w, (func_ptr)q_ready, 0, 0);
    }

    struct q_conn * const c = sl_first(&c_ready);
    if (c) {
        sl_remove_head(&c_ready, node_rx_ext);
        c->have_new_data = c->in_c_ready = false;
#if !defined(NDEBUG) && defined(DEBUG_EXTRA)
        char * op = "rx";
        if (c->needs_accept)
            op = "accept";
        else if (c->state == conn_clsd)
            op = "close";
        warn(WRN, "%s conn %s ready to %s", conn_type(c), cid_str(c->scid), op);
    } else {
        warn(WRN, "no conn ready to rx");
#endif
    }
    if (ready)
        *ready = c;
    return c;
}


bool q_is_new_serv_conn(const struct q_conn * const c)
{
    return c->needs_accept;
}


int q_conn_af(const struct q_conn * const c)
{
    return c->sock->ws_af;
}


#ifndef NO_MIGRATION
void q_rebind_sock(struct q_conn * const c, const bool use_new_dcid)
{
    ensure(is_clnt(c), "can only rebind w_sock on client");

    // find the index of the currently used local address
    uint16_t idx;
    for (idx = 0; idx < c->w->addr_cnt; idx++)
        if (w_addr_cmp(&c->w->ifaddr[idx].addr, &c->sock->ws_laddr))
            break;
    ensure(idx < c->w->addr_cnt, "could not find local address index");

    struct w_sock * const new_sock = w_bind(c->w, idx, 0, &c->sockopt);
    if (new_sock == 0)
        // could not open new w_sock, can't rebind
        return;

#ifndef NDEBUG
    char old_ip[IP_STRLEN];
    const uint16_t old_port = c->sock->ws_lport;
    w_ntop(&c->sock->ws_laddr, old_ip);
#endif

    // close the current w_sock
    if (c->scid == 0)
        conns_by_ipnp_del(c);
    w_close(c->sock);
    c->sock = new_sock;

    struct sockaddr_storage ss = {.ss_family = c->peer.addr.af};
    if (c->peer.addr.af == AF_INET) {
        struct sockaddr_in * const sin4 = (struct sockaddr_in *)&ss;
        sin4->sin_port = c->peer.port;
        memcpy(&sin4->sin_addr, &c->peer.addr.ip4, sizeof(sin4->sin_addr));
    } else {
        struct sockaddr_in6 * const sin6 = (struct sockaddr_in6 *)&ss;
        sin6->sin6_port = c->peer.port;
        memcpy(&sin6->sin6_addr, &c->peer.addr.ip4, sizeof(sin6->sin6_addr));
    }
    w_connect(c->sock, (struct sockaddr *)&ss);
    if (c->scid == 0)
        conns_by_ipnp_ins(c);

    if (use_new_dcid)
        // switch to new dcid
        use_next_dcid(c);

    warn(NTE, "simulated %s for %s conn %s from %s:%u to %s:%u",
         use_new_dcid ? "conn migration" : "NAT rebinding", conn_type(c),
         c->scid ? cid_str(c->scid) : "-", old_ip, old_port,
         w_ntop(&c->sock->ws_laddr, ip_tmp), c->sock->ws_lport);

    timeouts_add(ped(c->w)->wheel, &c->tx_w, 0);
}
#endif


#ifndef NO_QINFO
void q_info(struct q_conn * const c, struct q_conn_info * const ci)
{
    conn_info_populate(c);
    memcpy(ci, &c->i, sizeof(*ci));
}
#endif


char * hex2str(const uint8_t * const src,
               const size_t len_src,
               char * const dst,
               const size_t len_dst)
{
    ensure(len_dst >= hex_str_len(len_src), "overflow %lu < %lu",
           (unsigned long)len_dst, (unsigned long)hex_str_len(len_src));
    static const char hex[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len_src; i++) {
        dst[i * 2] = hex[(src[i] >> 4) & 0x0f];
        dst[i * 2 + 1] = hex[src[i] & 0x0f];
    }
    dst[i * 2] = 0;
    return dst;
}


const char *
cid2str(const struct cid * const cid, char * const dst, const size_t len_dst)
{
    const int n = snprintf(dst, len_dst, "%" PRIu ":", (cid)->seq);
    return hex2str((cid)->id, (cid)->len, &dst[n], len_dst - (size_t)n);
}
