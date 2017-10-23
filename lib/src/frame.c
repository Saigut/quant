// Copyright (c) 2016-2017, NetApp, Inc.
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
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>

#ifdef __linux__
#include <byteswap.h>
#endif

#include <ev.h>
#include <quant/quant.h>
#include <warpcore/warpcore.h>

#include "conn.h"
#include "diet.h"
#include "frame.h"
#include "marshall.h"
#include "pkt.h"
#include "quic.h"
#include "stream.h"
#include "tls.h"


#define FRAM_TYPE_PAD 0x00
#define FRAM_TYPE_CNCL 0x02
#define FRAM_TYPE_MAX_DATA 0x04
#define FRAM_TYPE_MAX_STRM_DATA 0x05
#define FRAM_TYPE_PING 0x07
#define FRAM_TYPE_STRM_BLCK 0x09
#define FRAM_TYPE_STRM 0xC0
#define FRAM_TYPE_ACK 0xA0


// Convert stream ID length encoded in flags to bytes
static uint8_t __attribute__((const)) dec_sid_len(const uint8_t flags)
{
    const uint8_t l = (flags & 0x18) >> 3;
    ensure(l <= 3, "cannot decode stream ID length %u", l);
    const uint8_t dec[] = {1, 2, 3, 4};
    return dec[l];
}


// Convert stream offset length encoded in flags to bytes
static uint8_t __attribute__((const)) dec_off_len(const uint8_t flags)
{
    const uint8_t l = (flags & 0x06) >> 1;
    ensure(l <= 3, "cannot decode stream offset length %u", l);
    const uint8_t dec[] = {0, 2, 4, 8};
    return dec[l];
}


#define F_STREAM_FIN 0x20
#define F_STREAM_DATA_LEN 0x01


static uint16_t __attribute__((nonnull))
dec_stream_frame(struct q_conn * const c,
                 struct w_iov * const v,
                 const uint16_t pos,
                 uint16_t * const len,
                 bool * const new_data)
{
    uint16_t i = pos;

    uint8_t type = 0;
    dec(type, v->buf, v->len, i, 0, "0x%02x");

    uint32_t sid = 0;
    const uint8_t sid_len = dec_sid_len(type);
    dec(sid, v->buf, v->len, i, sid_len, "%u");

    const uint8_t off_len = dec_off_len(type);
    uint64_t off = 0;
    if (off_len)
        dec(off, v->buf, v->len, i, off_len, "%" PRIu64);
    // TODO: pay attention to offset when delivering data to app

    if (is_set(F_STREAM_DATA_LEN, type))
        dec(*len, v->buf, v->len, i, 0, "%u");
    else
        // stream data extends to end of packet
        *len = v->len - i;

    ensure(*len || is_set(F_STREAM_FIN, type), "len %u > 0 or FIN", *len);

    // deliver data into stream
    struct q_stream * s = get_stream(c, sid);
    if (s == 0) {
        if (diet_find(&c->closed_streams, sid)) {
            warn(WRN, "ignoring frame for closed str %u on %s conn %" PRIx64,
                 sid, conn_type(c), c->id);
            *new_data = false;
            return i;
        }
        s = new_stream(c, sid);
    }

    // best case: new in-order data
    if (off == s->in_off) {
        warn(NTE,
             "%u byte%s new data (off %" PRIu64 "-%" PRIu64
             ") on %s conn %" PRIx64 " str %u",
             *len, plural(*len), off, off + *len, conn_type(c), c->id, sid);
        s->in_off += *len;
        sq_insert_tail(&s->in, v, next);
        // s->state = STRM_STAT_OPEN;

        if (is_set(F_STREAM_FIN, type)) {
#ifndef NDEBUG
            const uint8_t old_state = s->state;
#endif
            s->state =
                s->state == STRM_STAT_OPEN ? STRM_STAT_HCRM : STRM_STAT_CLSD;
            warn(NTE,
                 "received FIN on %s conn %" PRIx64 " str %u, state %u -> %u",
                 conn_type(c), c->id, s->id, old_state, s->state);
            // if (s->state == STRM_STAT_CLSD)
            //     maybe_api_return(q_close_stream, s);
        }

        if (s->id != 0)
            maybe_api_return(q_read, s->c);
        else {
            // adjust w_iov start and len to stream frame data for TLS handshake
            uint8_t * const b = v->buf;
            const uint16_t l = v->len;
            v->buf = &v->buf[i];
            v->len = *len;
            if (tls_handshake(s) == 0)
                maybe_api_return(q_connect, c);
            // undo adjust
            v->buf = b;
            v->len = l;
        }
        *new_data = true;
        return i;
    }

    // data is a complete duplicate
    if (off + *len <= s->in_off) {
        warn(NTE,
             "%u byte%s dup data (off %" PRIu64 "-%" PRIu64
             ") on %s conn %" PRIx64 " str %u",
             *len, plural(*len), off, off + *len, conn_type(c), c->id, sid);
        q_free_iov(w_engine(c->sock), v);
        *new_data = false;
        return i;
    }

    die("TODO: handle partially new or reordered data: %u byte%s data (off "
        "%" PRIu64 "-%" PRIu64 "), expected %" PRIu64 " on %s conn %" PRIx64
        " str %u: %.*s",
        *len, plural(*len), off, off + *len, s->in_off, conn_type(c), c->id,
        sid, *len, &v->buf[i]);
}


// Convert largest ACK length encoded in flags to bytes
static uint8_t __attribute__((const)) dec_lg_ack_len(const uint8_t flags)
{
    const uint8_t l = (flags & 0x0C) >> 2;
    ensure(l <= 3, "cannot decode largest ACK length %u", l);
    const uint8_t dec[] = {1, 2, 4, 8};
    return dec[l];
}


// Convert ACK block length encoded in flags to bytes
static uint8_t __attribute__((const)) dec_ack_block_len(const uint8_t flags)
{
    const uint8_t l = flags & 0x03;
    ensure(l <= 3, "cannot decode largest ACK length %u", l);
    const uint8_t dec[] = {1, 2, 4, 8};
    return dec[l];
}


#define F_ACK_N 0x10
#define PN_GEQ(a, b) (((int64_t)(a) - (int64_t)(b)) >= 0)

static uint16_t __attribute__((nonnull))
dec_ack_frame(struct q_conn * const c,
              const struct w_iov * const v,
              const uint16_t pos,
              void (*op)(struct q_conn * const,
                         const uint64_t,
                         const uint64_t,
                         const uint16_t)

)
{
    uint16_t i = pos;
    uint8_t type = 0;
    dec(type, v->buf, v->len, i, 0, "0x%02x");

    uint8_t num_blocks = 1;
    if (is_set(F_ACK_N, type)) {
        dec(num_blocks, v->buf, v->len, i, 0, "%u");
        num_blocks++;
    }

    uint8_t num_ts = 0;
    dec(num_ts, v->buf, v->len, i, 0, "%u");

    const uint8_t lg_ack_len = dec_lg_ack_len(type);
    uint64_t lg_ack = 0;
    dec(lg_ack, v->buf, v->len, i, lg_ack_len, "%" PRIu64);

    uint16_t ack_delay = 0;
    dec(ack_delay, v->buf, v->len, i, 0, "%u");

    const uint8_t ack_block_len = dec_ack_block_len(type);
    uint64_t ack = lg_ack;
    for (uint8_t b = 0; b < num_blocks; b++) {
        uint64_t l = 0;
        dec(l, v->buf, v->len, i, ack_block_len, "%" PRIu64);

        while (PN_GEQ(ack, lg_ack - l)) {
            // warn(INF, "pkt %" PRIu64 " had ACK for %" PRIu64, meta(v).nr,
            // ack);
            op(c, ack, lg_ack, ack_delay);
            ack--;
        }

        // skip ACK gap (no gap after last ACK block)
        if (b < num_blocks - 1) {
            uint8_t gap = 0;
            dec(gap, v->buf, v->len, i, 0, "%u");
            lg_ack = ack -= gap - 1;
        }
    }

    for (uint8_t b = 0; b < num_ts; b++) {
        warn(DBG, "decoding timestamp block #%u", b);
        uint8_t delta_lg_obs = 0;
        dec(delta_lg_obs, v->buf, v->len, i, 0, "%u");
        uint32_t ts = 0;
        dec(ts, v->buf, v->len, i, b == 0 ? 4 : 2, "%u");
    }
    return i;
}


static void __attribute__((nonnull))
track_acked_pkts(struct q_conn * const c,
                 const uint64_t ack,
                 const uint64_t lg_ack __attribute__((unused)),
                 const uint16_t ack_delay __attribute__((unused)))
{
    // warn(DBG, "no longer need to ACK pkt %" PRIu64, ack);
    diet_remove(&c->recv, ack);
}


static void __attribute__((nonnull)) process_ack(struct q_conn * const c,
                                                 const uint64_t ack,
                                                 const uint64_t lg_ack,
                                                 const uint16_t ack_delay)
{
    struct w_iov * p;
    const bool found = find_sent_pkt(c, ack, &p);
    if (!found) {
        warn(CRT, "got ACK for pkt %" PRIu64 " that we never sent", ack);
        return;
    }
    if (p == 0) {
        // we already had a previous ACK for this packet
        warn(INF, "already have ACK for pkt %" PRIu64, ack);
        return;
    }

    // only act on first-time ACKs
    if (meta(p).ack_cnt)
        die("repeated ACK (%u times)", meta(p).ack_cnt);

    meta(p).ack_cnt++;
    warn(NTE, "first ACK for %" PRIu64, ack);

    // first clause from OnAckReceived pseudo code:
    // if the largest ACKed is newly ACKed, update the RTT
    if (ack == lg_ack) {
        c->lg_acked = MAX(c->lg_acked, ack);

        c->latest_rtt = ev_now(loop) - meta(p).time;
        if (c->latest_rtt > ack_delay)
            c->latest_rtt -= ack_delay;

        // see UpdateRtt pseudo code:
        if (fpclassify(c->srtt) == FP_ZERO) {
            c->srtt = c->latest_rtt;
            c->rttvar = c->latest_rtt / 2;
        } else {
            c->rttvar = .75 * c->rttvar + .25 * (c->srtt - c->latest_rtt);
            c->srtt = .875 * c->srtt + .125 * c->latest_rtt;
        }
        warn(DBG, "%s conn %" PRIx64 " srtt = %f, rttvar = %f", conn_type(c),
             c->id, c->srtt, c->rttvar);
    }

    // second clause from OnAckReceived pseudo code:
    // the sender may skip packets for detecting optimistic ACKs
    // TODO: if (packets ACKed that the sender skipped): abortConnection()


    // see OnPacketAcked pseudo code (for LD):

    // If a packet sent prior to RTO was ACKed, then the RTO
    // was spurious.  Otherwise, inform congestion control.
    if (c->rto_cnt && ack > c->lg_sent_before_rto)
        // see OnRetransmissionTimeoutVerified pseudo code
        c->cwnd = kMinimumWindow;
    c->hshake_cnt = c->tlp_cnt = c->rto_cnt = 0;

    // this packet is no no longer unACKed
    splay_remove(pm_splay, &c->unacked_pkts, &meta(p));
    diet_insert(&c->acked_pkts, ack);
    if (splay_empty(&c->unacked_pkts))
        maybe_api_return(q_close, c);

    // stop ACKing packets that were contained in the ACK frame of this packet
    if (meta(p).ack_header_pos) {
        warn(DBG, "decoding ACK info from pkt %" PRIu64 " from pos %u", ack,
             meta(p).ack_header_pos);
        p->buf -= Q_OFFSET;
        p->len += Q_OFFSET;
        dec_ack_frame(c, p, meta(p).ack_header_pos, &track_acked_pkts);
        p->buf += Q_OFFSET;
        p->len -= Q_OFFSET;
    } else
        warn(DBG, "pkt %" PRIu64 " did not contain an ACK frame", ack);

    // see OnPacketAcked pseudo code (for CC):
    if (ack >= c->rec_end) {
        if (c->cwnd < c->ssthresh)
            c->cwnd += p->len;
        else
            c->cwnd += p->len / c->cwnd;
        warn(INF, "cwnd now %" PRIu64, c->cwnd);
    }

    if (is_rtxable(&meta(p))) {
        // adjust in_flight
        c->in_flight -= meta(p).stream_data_end;
        warn(INF, "in_flight -%u = %" PRIu64, meta(p).stream_data_end,
             c->in_flight);

        // check if a q_write is done
        struct q_stream * const s = meta(p).str;
        if (s && ++s->out_ack_cnt == sq_len(&s->out))
            // all packets are ACKed
            maybe_api_return(q_write, s);

        // // check if a q_close_stream is done
        // p->buf -= Q_OFFSET;
        // p->len += Q_OFFSET;
        // if (is_set(F_STREAM_FIN, p->buf[meta(p).stream_header_pos])) {
        //     // our FIN got ACKed
        //     s->state = s->state == STRM_STAT_HCLO ? STRM_STAT_CLSD :
        //     s->state; warn(CRT, "ACK of FIN, state now %u", s->state);
        //     maybe_api_return(q_close, s);
        // }
        // p->buf += Q_OFFSET;
        // p->len -= Q_OFFSET;
    }

    if (ack == lg_ack) {
        // detect_lost_pkts(c);
        set_ld_alarm(c);
    }
}


static uint16_t __attribute__((nonnull))
dec_conn_close_frame(struct q_conn * const c __attribute__((unused)),
                     const struct w_iov * const v,
                     const uint16_t pos)
{
    uint16_t i = pos + 1;

    uint32_t err_code = 0;
    dec(err_code, v->buf, v->len, i, 0, "0x%08x");

    uint16_t reas_len = 0;
    dec(reas_len, v->buf, v->len, i, 0, "%u");
    ensure(reas_len <= v->len - i, "reason_len invalid");

    if (reas_len) {
        char reas_phr[UINT16_MAX];
        memcpy(reas_phr, &v->buf[i], reas_len);
        i += reas_len;
        warn(NTE, "%u-byte conn close reason: %.*s", reas_len, reas_len,
             reas_phr);
    }

    // maybe_api_return(q_read, c);

    return i;
}


static uint16_t __attribute__((nonnull))
dec_max_stream_data_frame(struct q_conn * const c,
                          const struct w_iov * const v,
                          const uint16_t pos)
{
    uint16_t i = pos + 1;

    uint32_t sid = 0;
    dec(sid, v->buf, v->len, i, 0, "%u");
    struct q_stream * const s = get_stream(c, sid);
    ensure(s, "have stream %u", sid);
    dec(s->max_stream_data, v->buf, v->len, i, 0, "%" PRIu64);

    return i;
}


static uint16_t __attribute__((nonnull))
dec_max_data_frame(struct q_conn * const c,
                   const struct w_iov * const v,
                   const uint16_t pos)
{
    uint16_t i = pos + 1;
    dec(c->max_data, v->buf, v->len, i, 0, "%" PRIu64);
    return i;
}


static uint16_t __attribute__((nonnull))
dec_stream_blocked(struct q_conn * const c,
                   const struct w_iov * const v,
                   const uint16_t pos)
{
    uint16_t i = pos + 1;

    uint32_t sid = 0;
    dec(sid, v->buf, v->len, i, 0, "%u");
    struct q_stream * const s = get_stream(c, sid);
    ensure(s, "have stream %u", sid);

    // TODO: handle this

    return i;
}


bool dec_frames(struct q_conn * const c, struct w_iov * v)
{
    uint16_t i = pkt_hdr_len(v->buf, v->len);
    uint16_t pad_start = 0;
    uint16_t dpos = 0;
    uint16_t dlen = 0;
    bool tx_needed = false;

    while (i < v->len) {
        const uint8_t type = ((const uint8_t * const)(v->buf))[i];
        if (pad_start && (type != FRAM_TYPE_PAD || i == v->len - 1)) {
            warn(DBG, "skipped padding in [%u..%u]", pad_start, i);
            pad_start = 0;
        }

        if (is_set(FRAM_TYPE_STRM, type)) {
            if (dpos) {
                // already had at least one stream frame in this packet,
                // generate (another) copy
                warn(INF, "more than one stream frame in pkt, copy");
                struct w_iov * const vdup =
                    w_alloc_iov(w_engine(c->sock), MAX_PKT_LEN, Q_OFFSET);
                memcpy(vdup->buf, v->buf, v->len);
                meta(vdup) = meta(v);
                vdup->len = v->len;
                // adjust w_iov start and len to stream frame data
                v->buf = &v->buf[dpos];
                v->len = dlen;
                // continue parsing in the copied w_iov
                v = vdup;
            }

            // this is the first stream frame in this packet
            dpos = dec_stream_frame(c, v, i, &dlen, &tx_needed);
            i = dpos + dlen;

        } else if (is_set(FRAM_TYPE_ACK, type)) {
            i = dec_ack_frame(c, v, i, &process_ack);

        } else
            switch (type) {
            case FRAM_TYPE_PAD:
                pad_start = pad_start ? pad_start : i;
                i++;
                break;

            case FRAM_TYPE_CNCL:
                i = dec_conn_close_frame(c, v, i);
                tx_needed = true;
                break;

            case FRAM_TYPE_PING:
                warn(INF, "ping frame in [%u]", i);
                i++;
                tx_needed = true;
                break;

            case FRAM_TYPE_MAX_STRM_DATA:
                i = dec_max_stream_data_frame(c, v, i);
                break;

            case FRAM_TYPE_MAX_DATA:
                i = dec_max_data_frame(c, v, i);
                break;

            case FRAM_TYPE_STRM_BLCK:
                i = dec_stream_blocked(c, v, i);
                break;

            default:
                die("unknown frame type 0x%02x", type);
            }
    }

    if (dpos) {
        // adjust w_iov start and len to stream frame data
        v->buf = &v->buf[dpos];
        v->len = dlen;
    }

    return tx_needed;
}


uint16_t
enc_padding_frame(uint8_t * const buf, const uint16_t pos, const uint16_t len)
{
    warn(DBG, "encoding padding frame into [%u..%u]", pos, pos + len - 1);
    memset(&buf[pos], FRAM_TYPE_PAD, len);
    return len;
}


static const uint8_t enc_lg_ack_len[] = {0xFF,      0x00, 0x01 << 2, 0xFF,
                                         0x02 << 2, 0xFF, 0xFF,      0xFF,
                                         0x03 << 2}; // 0xFF = invalid


static uint8_t __attribute__((const)) needed_lg_ack_len(const uint64_t n)
{
    if (n < UINT8_MAX)
        return 1;
    if (n < UINT16_MAX)
        return 2;
    if (n < UINT32_MAX)
        return 4;
    return 8;
}


static const uint8_t enc_ack_block_len[] = {
    0xFF, 0x00, 0x01, 0xFF, 0x02, 0xFF, 0xFF, 0xFF, 0x03}; // 0xFF = invalid


static uint8_t __attribute__((nonnull))
needed_ack_block_len(struct q_conn * const c)
{
    const uint64_t max_block = diet_max(&c->recv) - diet_max(&c->recv);
    if (max_block < UINT8_MAX)
        return 1;
    if (max_block < UINT16_MAX)
        return 2;
    if (max_block < UINT32_MAX)
        return 4;
    return 8;
}


uint16_t enc_ack_frame(struct q_conn * const c,
                       uint8_t * const buf,
                       const uint16_t len,
                       const uint16_t pos)
{
    uint8_t type = FRAM_TYPE_ACK;

    uint8_t num_blocks = (uint8_t)MIN(c->recv.cnt, UINT8_MAX);
    if (num_blocks > 1) {
        num_blocks--;
        type |= F_ACK_N;
    }

    const uint64_t lg_recv = diet_max(&c->recv);
    const uint8_t lg_ack_len = needed_lg_ack_len(lg_recv);
    type |= enc_lg_ack_len[lg_ack_len];
    const uint8_t ack_block_len = needed_ack_block_len(c);
    type |= enc_ack_block_len[ack_block_len];

    uint16_t i = pos;
    enc(buf, len, i, &type, 0, "0x%02x");

    if (is_set(F_ACK_N, type))
        enc(buf, len, i, &num_blocks, 0, "%u");

    // TODO: send timestamps in protected packets
    const uint8_t num_ts = 0;
    enc(buf, len, i, &num_ts, 0, "%u");

    enc(buf, len, i, &lg_recv, lg_ack_len, "%" PRIu64);

    const uint16_t ack_delay = 0;
    enc(buf, len, i, &ack_delay, 0, "%u");

    struct ival * b;
    uint64_t prev_lo = 0;
    splay_foreach_rev (b, diet, &c->recv) {
        if (prev_lo) {
            const uint64_t gap = prev_lo - b->hi;
            ensure(gap <= UINT8_MAX, "TODO: gap %" PRIu64 " too large", gap);
            enc(buf, len, i, &gap, sizeof(uint8_t), "%" PRIu64);
        }
        prev_lo = b->lo;
        const uint64_t ack_block = b->hi - b->lo;
        warn(NTE, "ACKing %" PRIu64 "-%" PRIu64, b->lo, b->hi);
        enc(buf, len, i, &ack_block, ack_block_len, "%" PRIu64);
    }
    return i - pos;
}


static const uint8_t enc_sid_len[] = {0xFF, 0x00, 0x01, 002,
                                      0x03}; // 0xFF = invalid

static uint8_t __attribute__((const)) needed_sid_len(const uint32_t n)
{
    if (n < UINT8_MAX)
        return 1;
    if (n < UINT16_MAX)
        return 2;
    if (n < 0x00FFFFFF) // UINT24_MAX :-)
        return 3;
    return 4;
}


static const uint8_t enc_off_len[] = {0x00,      0xFF, 0x01 << 1, 0xFF,
                                      0x02 << 1, 0xFF, 0xFF,      0xFF,
                                      0x03 << 1}; // 0xFF = invalid

static uint8_t __attribute__((const)) needed_off_len(const uint64_t n)
{
    if (n == 0)
        return 0;
    if (n < UINT16_MAX)
        return 2;
    if (n < UINT32_MAX)
        return 4;
    return 8;
}


uint16_t enc_stream_frame(struct q_stream * const s, struct w_iov * const v)
{
    const uint16_t dlen = v->len - Q_OFFSET; // TODO: support FIN bit
    ensure(dlen || s->state > STRM_STAT_OPEN,
           "no stream data or need to send FIN");

    warn(INF, "%u byte%s at off %" PRIu64 "-%" PRIu64 " on str %u", dlen,
         plural(dlen), s->out_off, dlen ? s->out_off + dlen - 1 : s->out_off,
         s->id);

    const uint8_t sid_len = needed_sid_len(s->id);
    uint8_t type =
        FRAM_TYPE_STRM | (dlen ? F_STREAM_DATA_LEN : 0) | enc_sid_len[sid_len];

    // if stream is closed locally and this is the last packet, include a FIN
    if ((s->state == STRM_STAT_HCLO || s->state == STRM_STAT_CLSD) &&
        v == sq_last(&s->out, w_iov, next)) {
        warn(NTE, "sending %s FIN on %s conn %" PRIx64 " str %u, state %u",
             dlen ? "" : "pure", conn_type(s->c), s->c->id, s->id, s->state);
        type |= F_STREAM_FIN;
        s->fin_sent = 1;
        // maybe_api_return(q_close_stream, s);
    }

    // prepend a stream frame header
    const uint8_t off_len = needed_off_len(s->out_off);
    type |= enc_off_len[off_len];

    // now that we know how long the stream frame header is, encode it
    uint16_t i = meta(v).stream_header_pos =
        Q_OFFSET - 1 - (dlen ? 2 : 0) - off_len - sid_len;
    enc(v->buf, v->len, i, &type, 0, "0x%02x");
    enc(v->buf, v->len, i, &s->id, sid_len, "%u");
    if (off_len)
        enc(v->buf, v->len, i, &s->out_off, off_len, "%" PRIu64);
    if (dlen)
        enc(v->buf, v->len, i, &dlen, 0, "%u");

    s->out_off += dlen; // increase the stream data offset
    meta(v).str = s;    // remember stream this buf belongs to

    return v->len;
}


uint16_t enc_conn_close_frame(struct w_iov * const v,
                              const uint16_t pos,
                              const uint32_t err_code,
                              const char * const reas,
                              const uint16_t reas_len)
{
    uint16_t i = meta(v).cc_header_pos = pos;

    const uint8_t type = FRAM_TYPE_CNCL;
    enc(v->buf, v->len, i, &type, 0, "0x%02x");

    enc(v->buf, v->len, i, &err_code, 0, "0x%08x");

    const uint16_t rlen = MIN(reas_len, v->len - i);
    enc(v->buf, v->len, i, &rlen, 0, "%u");

    memcpy(&v->buf[i], reas, rlen);
    warn(DBG, "enc %u-byte reason phrase into [%u..%u]", rlen, i, i + rlen - 1);

    return i + rlen - pos;
}
