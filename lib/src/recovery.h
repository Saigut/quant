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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <warpcore/warpcore.h>

// IWYU pragma: no_include "../deps/libev/ev.h"

#include "event.h" // IWYU pragma: keep
#include "quic.h"

struct q_conn;
struct pn_space;


struct recovery {
    // LD state
    ev_timer ld_alarm;   // loss_detection_timer
    uint16_t crypto_cnt; // crypto_count
    uint16_t pto_cnt;    // pto_count

    uint8_t _unused2[4];

    ev_tstamp last_sent_ack_elicit_t; // time_of_last_sent_ack_eliciting_packet
    ev_tstamp last_sent_crypto_t;     // time_of_last_sent_crypto_packet

    // largest_sent_packet -> pn->lg_sent
    // largest_acked_packet -> pn->lg_acked

    ev_tstamp latest_rtt; // latest_rtt
    ev_tstamp srtt;       // smoothed_rtt
    ev_tstamp rttvar;     // rttvar
    ev_tstamp min_rtt;    // min_rtt

    // max_ack_delay -> c->tp_out.max_ack_del

    // CC state
    ev_tstamp rec_start_t; // recovery_start_time
#ifndef PARTICLE
    uint64_t ce_cnt;       // ecn_ce_counter
    uint64_t in_flight;    // bytes_in_flight
    uint64_t ae_in_flight; // nr of ACK-eliciting pkts inflight
    uint64_t cwnd;         // congestion_window
    uint64_t ssthresh;     // sshtresh
#else
    uint32_t ce_cnt;
    uint32_t in_flight;
    uint32_t ae_in_flight;
    uint32_t cwnd;
    uint32_t ssthresh;
#endif

#ifndef NDEBUG
    // these are only used in log_cc below
    uint64_t prev_in_flight;
    uint64_t prev_cwnd;
    uint64_t prev_ssthresh;
    ev_tstamp prev_srtt;
    ev_tstamp prev_rttvar;
#endif
};


#ifndef NDEBUG
extern void __attribute__((nonnull)) log_cc(struct q_conn * const c);
#else
#define log_cc(...)
#endif

extern void __attribute__((nonnull)) init_rec(struct q_conn * const c);

extern void __attribute__((nonnull)) on_pkt_sent(struct pkt_meta * const m);

extern void __attribute__((nonnull))
on_ack_received_1(struct pkt_meta * const lg_ack, const uint64_t ack_del);

extern void __attribute__((nonnull))
on_ack_received_2(struct pn_space * const pn);

extern void __attribute__((nonnull))
on_pkt_acked(struct w_iov * const v, struct pkt_meta * m);

extern void __attribute__((nonnull))
congestion_event(struct q_conn * const c, const ev_tstamp sent_t);

extern void __attribute__((nonnull)) set_ld_timer(struct q_conn * const c);

extern void __attribute__((nonnull))
on_pkt_lost(struct pkt_meta * const m, const bool is_lost);
