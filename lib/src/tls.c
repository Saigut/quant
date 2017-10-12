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
#include <string.h>

// IWYU pragma: no_include <picotls/../picotls.h>
#include <picotls/minicrypto.h>
#include <picotls/openssl.h>

#include <warpcore/warpcore.h>

#include "cert.h"
#include "conn.h"
#include "marshall.h"
#include "pkt.h"
#include "quic.h"
#include "stream.h"
#include "tls.h"


ptls_context_t tls_ctx = {0};

static ptls_minicrypto_secp256r1sha256_sign_certificate_t sign_cert = {0};
static ptls_iovec_t tls_certs = {0};
static ptls_openssl_verify_certificate_t verifier = {0};

#define TLS_EXT_TYPE_TRANSPORT_PARAMETERS 26
static uint8_t tp_buf[64];
static ptls_raw_extension_t tp_ext[2];
static ptls_handshake_properties_t tls_hshake_prop = {.additional_extensions =
                                                          tp_ext};

static uint32_t initial_max_stream_data = 1000001; // units of octets
static uint32_t initial_max_data = 2000002;        // units of 1024 octets
static uint32_t initial_max_stream_id = 3000003;   // as is
static uint16_t idle_timeout = 595; // units of seconds (max 600 seconds)


#define PTLS_CLNT_LABL "EXPORTER-QUIC client 1-RTT Secret"
#define PTLS_SERV_LABL "EXPORTER-QUIC server 1-RTT Secret"


static void __attribute__((nonnull))
conn_setup_1rtt_secret(struct q_conn * const c,
                       ptls_cipher_suite_t * const cipher,
                       ptls_aead_context_t ** aead,
                       uint8_t * const sec,
                       const char * const label,
                       uint8_t is_enc)
{
    int ret = ptls_export_secret(c->tls, sec, cipher->hash->digest_size, label,
                                 ptls_iovec_init(0, 0));
    ensure(ret == 0, "ptls_export_secret");
    *aead = ptls_aead_new(cipher->aead, cipher->hash, is_enc, sec);
    ensure(aead, "ptls_aead_new");
}


static void __attribute__((nonnull)) conn_setup_1rtt(struct q_conn * const c)
{
    ptls_cipher_suite_t * const cipher = ptls_get_cipher(c->tls);
    conn_setup_1rtt_secret(c, cipher, &c->in_kp0, c->in_sec,
                           is_clnt(c) ? PTLS_SERV_LABL : PTLS_CLNT_LABL, 0);
    conn_setup_1rtt_secret(c, cipher, &c->out_kp0, c->out_sec,
                           is_clnt(c) ? PTLS_CLNT_LABL : PTLS_SERV_LABL, 1);

    c->state = CONN_STAT_VERS_OK;
    warn(DBG, "%s conn %" PRIx64 " now in state %u", conn_type(c), c->id,
         c->state);
}


void init_tp(struct q_conn * const c)
{
    uint16_t i = 0;
    const uint16_t len = sizeof(tp_buf);

    enc(tp_buf, len, i, &c->vers, 0, "0x%08x");
    enc(tp_buf, len, i, &c->vers_initial, 0, "0x%08x");

    uint16_t l = is_serv(c) ? 50 : 30; // size of rest of parameters
    enc(tp_buf, len, i, &l, 2, "%u");

    uint16_t p = TP_INITIAL_MAX_STREAM_DATA;
    enc(tp_buf, len, i, &p, 0, "%u");
    l = 4;
    enc(tp_buf, len, i, &l, 0, "%u");
    enc(tp_buf, len, i, &initial_max_stream_data, 0, "%u");

    p = TP_INITIAL_MAX_DATA;
    enc(tp_buf, len, i, &p, 0, "%u");
    l = 4;
    enc(tp_buf, len, i, &l, 0, "%u");
    enc(tp_buf, len, i, &initial_max_data, 0, "%u");

    p = TP_INITIAL_MAX_STREAM_ID;
    enc(tp_buf, len, i, &p, 0, "%u");
    l = 4;
    enc(tp_buf, len, i, &l, 0, "%u");
    enc(tp_buf, len, i, &initial_max_stream_id, 0, "%u");

    p = TP_IDLE_TIMEOUT;
    enc(tp_buf, len, i, &p, 0, "%u");
    l = 2;
    enc(tp_buf, len, i, &l, 0, "%u");
    enc(tp_buf, len, i, &idle_timeout, 0, "%u");

    if (is_serv(c)) {
        p = TP_STATELESS_RESET_TOKEN;
        enc(tp_buf, len, i, &p, 0, "%u");
        l = 16;
        enc(tp_buf, len, i, &l, 0, "%u");
        enc(tp_buf, len, i, c->stateless_reset_token, 16, "");
    }

    tp_ext[0] =
        (ptls_raw_extension_t){TLS_EXT_TYPE_TRANSPORT_PARAMETERS, {tp_buf, i}};
    tp_ext[1] = (ptls_raw_extension_t){UINT16_MAX};
}


void init_tls(struct q_conn * const c, const char * const peer_name)
{
    ensure((c->tls = ptls_new(&tls_ctx, peer_name == 0)) != 0,
           "alloc TLS state");
    if (peer_name)
        ensure(ptls_set_server_name(c->tls, peer_name, strlen(peer_name)) == 0,
               "ptls_set_server_name");
}


uint32_t tls_handshake(struct q_stream * const s)
{
    // get pointer to any received handshake data
    // XXX there is an assumption here that we only have one inbound packet
    struct w_iov * const iv = sq_first(&s->i);
    size_t in_len = iv ? iv->len : 0;

    // allocate a new w_iov
    struct w_iov * ov =
        w_alloc_iov(w_engine(s->c->sock), MAX_PKT_LEN, Q_OFFSET);
    ptls_buffer_init(&meta(ov).tb, ov->buf, ov->len);
    const int ret = ptls_handshake(s->c->tls, &meta(ov).tb, iv ? iv->buf : 0,
                                   &in_len, &tls_hshake_prop);
    ov->len = (uint16_t)meta(ov).tb.off;
    warn(INF, "TLS handshake: recv %u, gen %u, in_len %lu, ret %u: %.*s",
         iv ? iv->len : 0, ov->len, in_len, ret, ov->len, ov->buf);
    ensure(ret == 0 || ret == PTLS_ERROR_IN_PROGRESS, "TLS error: %u", ret);
    ensure(iv == 0 || iv->len && iv->len == in_len, "TLS data remaining");

    if (iv)
        // the assumption is that ptls_handshake has consumed all stream-0 data
        w_free(w_engine(s->c->sock), &s->i);
    else {
        s->c->state = CONN_STAT_VERS_SENT;
        // warn(DBG, "%s conn %" PRIx64 " now in state %u", conn_type(s->c),
        //      s->c->id, s->c->state);
    }

    if ((ret == 0 || ret == PTLS_ERROR_IN_PROGRESS) && ov->len != 0)
        // enqueue for TX
        sq_insert_tail(&s->o, ov, next);
    else
        // we are done with the handshake, no need to TX after all
        w_free_iov(w_engine(s->c->sock), ov);

    if (ret == 0)
        conn_setup_1rtt(s->c);

    return (uint32_t)ret;
}


void init_tls_ctx(void)
{
    // warn(DBG, "TLS: key %u byte%s, cert %u byte%s", tls_key_len,
    //      plural(tls_key_len), tls_cert_len, plural(tls_cert_len));
    tls_ctx.random_bytes = ptls_minicrypto_random_bytes;

    // allow secp256r1 and x25519
    static ptls_key_exchange_algorithm_t * my_own_key_exchanges[] = {
        &ptls_minicrypto_secp256r1, &ptls_minicrypto_x25519, NULL};

    tls_ctx.key_exchanges = my_own_key_exchanges;
    tls_ctx.cipher_suites = ptls_minicrypto_cipher_suites;

    ensure(ptls_minicrypto_init_secp256r1sha256_sign_certificate(
               &sign_cert, ptls_iovec_init(tls_key, tls_key_len)) == 0,
           "ptls_minicrypto_init_secp256r1sha256_sign_certificate");
    tls_ctx.sign_certificate = &sign_cert.super;

    tls_certs = ptls_iovec_init(tls_cert, tls_cert_len);
    tls_ctx.certificates.list = &tls_certs;
    tls_ctx.certificates.count = 1;

    ensure(ptls_openssl_init_verify_certificate(&verifier, 0) == 0,
           "ptls_openssl_init_verify_certificate");
    tls_ctx.verify_certificate = &verifier.super;
}
