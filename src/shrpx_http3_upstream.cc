/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2021 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "shrpx_http3_upstream.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/udp.h>

#include <cstdio>

#include <ngtcp2/ngtcp2_crypto.h>

#include "shrpx_client_handler.h"
#include "shrpx_downstream.h"
#include "shrpx_downstream_connection.h"
#include "shrpx_log.h"
#include "shrpx_quic.h"
#include "shrpx_worker.h"
#include "shrpx_http.h"
#include "shrpx_connection_handler.h"
#ifdef HAVE_MRUBY
#  include "shrpx_mruby.h"
#endif // HAVE_MRUBY
#include "http3.h"
#include "util.h"

namespace shrpx {

namespace {
void idle_timeoutcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto upstream = static_cast<Http3Upstream *>(w->data);

  if (LOG_ENABLED(INFO)) {
    ULOG(INFO, upstream) << "QUIC idle timeout";
  }

  upstream->idle_close();

  auto handler = upstream->get_client_handler();

  delete handler;
}
} // namespace

namespace {
void timeoutcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto upstream = static_cast<Http3Upstream *>(w->data);

  if (upstream->handle_expiry() != 0 || upstream->on_write() != 0) {
    goto fail;
  }

  return;

fail:
  auto handler = upstream->get_client_handler();

  delete handler;
}
} // namespace

namespace {
void shutdown_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto upstream = static_cast<Http3Upstream *>(w->data);
  auto handler = upstream->get_client_handler();

  if (upstream->submit_goaway() != 0) {
    delete handler;
  }
}
} // namespace

namespace {
void prepare_cb(struct ev_loop *loop, ev_prepare *w, int revent) {
  auto upstream = static_cast<Http3Upstream *>(w->data);
  auto handler = upstream->get_client_handler();

  if (upstream->check_shutdown() != 0) {
    delete handler;
  }
}
} // namespace

namespace {
size_t downstream_queue_size(Worker *worker) {
  auto &downstreamconf = *worker->get_downstream_config();

  if (get_config()->http2_proxy) {
    return downstreamconf.connections_per_host;
  }

  return downstreamconf.connections_per_frontend;
}
} // namespace

Http3Upstream::Http3Upstream(ClientHandler *handler)
    : handler_{handler},
      max_udp_payload_size_{SHRPX_QUIC_MAX_UDP_PAYLOAD_SIZE},
      qlog_fd_{-1},
      hashed_scid_{},
      conn_{nullptr},
      tls_alert_{0},
      httpconn_{nullptr},
      downstream_queue_{downstream_queue_size(handler->get_worker()),
                        !get_config()->http2_proxy},
      idle_close_{false},
      retry_close_{false} {
  ev_timer_init(&timer_, timeoutcb, 0., 0.);
  timer_.data = this;

  auto config = get_config();
  auto &quicconf = config->quic;

  ev_timer_init(&idle_timer_, idle_timeoutcb, 0.,
                quicconf.upstream.timeout.idle);
  idle_timer_.data = this;

  ev_timer_init(&shutdown_timer_, shutdown_timeout_cb, 0., 0.);
  shutdown_timer_.data = this;

  ev_prepare_init(&prep_, prepare_cb);
  prep_.data = this;
  ev_prepare_start(handler_->get_loop(), &prep_);
}

Http3Upstream::~Http3Upstream() {
  auto loop = handler_->get_loop();

  ev_prepare_stop(loop, &prep_);
  ev_timer_stop(loop, &shutdown_timer_);
  ev_timer_stop(loop, &idle_timer_);
  ev_timer_stop(loop, &timer_);

  nghttp3_conn_del(httpconn_);

  ngtcp2_conn_del(conn_);

  if (qlog_fd_ != -1) {
    close(qlog_fd_);
  }
}

namespace {
void log_printf(void *user_data, const char *fmt, ...) {
  va_list ap;
  std::array<char, 4096> buf;

  va_start(ap, fmt);
  auto nwrite = vsnprintf(buf.data(), buf.size(), fmt, ap);
  va_end(ap);

  if (static_cast<size_t>(nwrite) >= buf.size()) {
    nwrite = buf.size() - 1;
  }

  buf[nwrite++] = '\n';

  while (write(fileno(stderr), buf.data(), nwrite) == -1 && errno == EINTR)
    ;
}
} // namespace

namespace {
void qlog_write(void *user_data, uint32_t flags, const void *data,
                size_t datalen) {
  auto upstream = static_cast<Http3Upstream *>(user_data);

  upstream->qlog_write(data, datalen, flags & NGTCP2_QLOG_WRITE_FLAG_FIN);
}
} // namespace

void Http3Upstream::qlog_write(const void *data, size_t datalen, bool fin) {
  assert(qlog_fd_ != -1);

  while (write(qlog_fd_, data, datalen) == -1 && errno == EINTR)
    ;

  if (fin) {
    close(qlog_fd_);
    qlog_fd_ = -1;
  }
}

namespace {
void rand(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *rand_ctx) {
  util::random_bytes(dest, dest + destlen,
                     *static_cast<std::mt19937 *>(rand_ctx->native_handle));
}
} // namespace

namespace {
int get_new_connection_id(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token,
                          size_t cidlen, void *user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);
  auto handler = upstream->get_client_handler();
  auto worker = handler->get_worker();
  auto conn_handler = worker->get_connection_handler();
  auto &qkms = conn_handler->get_quic_keying_materials();
  auto &qkm = qkms->keying_materials.front();

  if (generate_quic_connection_id(*cid, cidlen, worker->get_cid_prefix(),
                                  qkm.id, qkm.cid_encryption_key.data()) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  if (generate_quic_stateless_reset_token(token, *cid, qkm.secret.data(),
                                          qkm.secret.size()) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  auto quic_connection_handler = worker->get_quic_connection_handler();

  quic_connection_handler->add_connection_id(*cid, handler);

  return 0;
}
} // namespace

namespace {
int remove_connection_id(ngtcp2_conn *conn, const ngtcp2_cid *cid,
                         void *user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);
  auto handler = upstream->get_client_handler();
  auto worker = handler->get_worker();
  auto quic_conn_handler = worker->get_quic_connection_handler();

  quic_conn_handler->remove_connection_id(*cid);

  return 0;
}
} // namespace

void Http3Upstream::http_begin_request_headers(int64_t stream_id) {
  auto downstream =
      std::make_unique<Downstream>(this, handler_->get_mcpool(), stream_id);
  nghttp3_conn_set_stream_user_data(httpconn_, stream_id, downstream.get());

  downstream->reset_upstream_rtimer();

  handler_->repeat_read_timer();

  auto &req = downstream->request();
  req.http_major = 3;
  req.http_minor = 0;

  add_pending_downstream(std::move(downstream));
}

void Http3Upstream::add_pending_downstream(
    std::unique_ptr<Downstream> downstream) {
  downstream_queue_.add_pending(std::move(downstream));
}

namespace {
int recv_stream_data(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
                     uint64_t offset, const uint8_t *data, size_t datalen,
                     void *user_data, void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);

  if (upstream->recv_stream_data(flags, stream_id, data, datalen) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Http3Upstream::recv_stream_data(uint32_t flags, int64_t stream_id,
                                    const uint8_t *data, size_t datalen) {
  assert(httpconn_);

  auto nconsumed = nghttp3_conn_read_stream(
      httpconn_, stream_id, data, datalen, flags & NGTCP2_STREAM_DATA_FLAG_FIN);
  if (nconsumed < 0) {
    ULOG(ERROR, this) << "nghttp3_conn_read_stream: "
                      << nghttp3_strerror(nconsumed);
    last_error_ = quic::err_application(nconsumed);
    return -1;
  }

  ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, nconsumed);
  ngtcp2_conn_extend_max_offset(conn_, nconsumed);

  return 0;
}

namespace {
int stream_close(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
                 uint64_t app_error_code, void *user_data,
                 void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);

  if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET)) {
    app_error_code = NGHTTP3_H3_NO_ERROR;
  }

  if (upstream->stream_close(stream_id, app_error_code) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Http3Upstream::stream_close(int64_t stream_id, uint64_t app_error_code) {
  if (!httpconn_) {
    return 0;
  }

  auto rv = nghttp3_conn_close_stream(httpconn_, stream_id, app_error_code);
  switch (rv) {
  case 0:
    break;
  case NGHTTP3_ERR_STREAM_NOT_FOUND:
    if (ngtcp2_is_bidi_stream(stream_id)) {
      ngtcp2_conn_extend_max_streams_bidi(conn_, 1);
    }
    break;
  default:
    ULOG(ERROR, this) << "nghttp3_conn_close_stream: " << nghttp3_strerror(rv);
    last_error_ = quic::err_application(rv);
    return -1;
  }

  return 0;
}

namespace {
int acked_stream_data_offset(ngtcp2_conn *conn, int64_t stream_id,
                             uint64_t offset, uint64_t datalen, void *user_data,
                             void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);

  if (upstream->acked_stream_data_offset(stream_id, datalen) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Http3Upstream::acked_stream_data_offset(int64_t stream_id,
                                            uint64_t datalen) {
  if (!httpconn_) {
    return 0;
  }

  auto rv = nghttp3_conn_add_ack_offset(httpconn_, stream_id, datalen);
  if (rv != 0) {
    ULOG(ERROR, this) << "nghttp3_conn_add_ack_offset: "
                      << nghttp3_strerror(rv);
    return -1;
  }

  return 0;
}

namespace {
int extend_max_stream_data(ngtcp2_conn *conn, int64_t stream_id,
                           uint64_t max_data, void *user_data,
                           void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);

  if (upstream->extend_max_stream_data(stream_id) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Http3Upstream::extend_max_stream_data(int64_t stream_id) {
  if (!httpconn_) {
    return 0;
  }

  auto rv = nghttp3_conn_unblock_stream(httpconn_, stream_id);
  if (rv != 0) {
    ULOG(ERROR, this) << "nghttp3_conn_unblock_stream: "
                      << nghttp3_strerror(rv);
    return -1;
  }

  return 0;
}

namespace {
int extend_max_remote_streams_bidi(ngtcp2_conn *conn, uint64_t max_streams,
                                   void *user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);

  upstream->extend_max_remote_streams_bidi(max_streams);

  return 0;
}
} // namespace

void Http3Upstream::extend_max_remote_streams_bidi(uint64_t max_streams) {
  nghttp3_conn_set_max_client_streams_bidi(httpconn_, max_streams);
}

namespace {
int stream_reset(ngtcp2_conn *conn, int64_t stream_id, uint64_t final_size,
                 uint64_t app_error_code, void *user_data,
                 void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);

  if (upstream->http_shutdown_stream_read(stream_id) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Http3Upstream::http_shutdown_stream_read(int64_t stream_id) {
  if (!httpconn_) {
    return 0;
  }

  auto rv = nghttp3_conn_shutdown_stream_read(httpconn_, stream_id);
  if (rv != 0) {
    ULOG(ERROR, this) << "nghttp3_conn_shutdown_stream_read: "
                      << nghttp3_strerror(rv);
    return -1;
  }

  return 0;
}

namespace {
int stream_stop_sending(ngtcp2_conn *conn, int64_t stream_id,
                        uint64_t app_error_code, void *user_data,
                        void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);

  if (upstream->http_shutdown_stream_read(stream_id) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int handshake_completed(ngtcp2_conn *conn, void *user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);

  if (upstream->handshake_completed() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Http3Upstream::handshake_completed() {
  handler_->set_alpn_from_conn();

  auto alpn = handler_->get_alpn();
  if (alpn.empty()) {
    ULOG(ERROR, this) << "NO ALPN was negotiated";
    return -1;
  }

  std::array<uint8_t, NGTCP2_CRYPTO_MAX_REGULAR_TOKENLEN> token;
  size_t tokenlen;

  auto path = ngtcp2_conn_get_path(conn_);
  auto worker = handler_->get_worker();
  auto conn_handler = worker->get_connection_handler();
  auto &qkms = conn_handler->get_quic_keying_materials();
  auto &qkm = qkms->keying_materials.front();

  if (generate_token(token.data(), tokenlen, path->remote.addr,
                     path->remote.addrlen, qkm.secret.data(),
                     qkm.secret.size()) != 0) {
    return 0;
  }

  auto rv = ngtcp2_conn_submit_new_token(conn_, token.data(), tokenlen);
  if (rv != 0) {
    ULOG(ERROR, this) << "ngtcp2_conn_submit_new_token: "
                      << ngtcp2_strerror(rv);
    return -1;
  }

  return 0;
}

int Http3Upstream::init(const UpstreamAddr *faddr, const Address &remote_addr,
                        const Address &local_addr,
                        const ngtcp2_pkt_hd &initial_hd,
                        const ngtcp2_cid *odcid, const uint8_t *token,
                        size_t tokenlen) {
  int rv;

  auto worker = handler_->get_worker();
  auto conn_handler = worker->get_connection_handler();

  auto callbacks = ngtcp2_callbacks{
      nullptr, // client_initial
      ngtcp2_crypto_recv_client_initial_cb,
      ngtcp2_crypto_recv_crypto_data_cb,
      shrpx::handshake_completed,
      nullptr, // recv_version_negotiation
      ngtcp2_crypto_encrypt_cb,
      ngtcp2_crypto_decrypt_cb,
      ngtcp2_crypto_hp_mask_cb,
      shrpx::recv_stream_data,
      shrpx::acked_stream_data_offset,
      nullptr, // stream_open
      shrpx::stream_close,
      nullptr, // recv_stateless_reset
      nullptr, // recv_retry
      nullptr, // extend_max_local_streams_bidi
      nullptr, // extend_max_local_streams_uni
      rand,
      get_new_connection_id,
      remove_connection_id,
      ngtcp2_crypto_update_key_cb,
      nullptr, // path_validation
      nullptr, // select_preferred_addr
      shrpx::stream_reset,
      shrpx::extend_max_remote_streams_bidi,
      nullptr, // extend_max_remote_streams_uni
      shrpx::extend_max_stream_data,
      nullptr, // dcid_status
      nullptr, // handshake_confirmed
      nullptr, // recv_new_token
      ngtcp2_crypto_delete_crypto_aead_ctx_cb,
      ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
      nullptr, // recv_datagram
      nullptr, // ack_datagram
      nullptr, // lost_datagram
      ngtcp2_crypto_get_path_challenge_data_cb,
      shrpx::stream_stop_sending,
  };

  auto config = get_config();
  auto &quicconf = config->quic;
  auto &http3conf = config->http3;

  auto &qkms = conn_handler->get_quic_keying_materials();
  auto &qkm = qkms->keying_materials.front();

  ngtcp2_cid scid;

  if (generate_quic_connection_id(scid, SHRPX_QUIC_SCIDLEN,
                                  worker->get_cid_prefix(), qkm.id,
                                  qkm.cid_encryption_key.data()) != 0) {
    return -1;
  }

  ngtcp2_settings settings;
  ngtcp2_settings_default(&settings);
  if (quicconf.upstream.debug.log) {
    settings.log_printf = log_printf;
  }

  if (!quicconf.upstream.qlog.dir.empty()) {
    auto fd = open_qlog_file(quicconf.upstream.qlog.dir, scid);
    if (fd != -1) {
      qlog_fd_ = fd;
      settings.qlog.odcid = initial_hd.dcid;
      settings.qlog.write = shrpx::qlog_write;
    }
  }

  settings.initial_ts = quic_timestamp();
  settings.initial_rtt = static_cast<ngtcp2_tstamp>(
      quicconf.upstream.initial_rtt * NGTCP2_SECONDS);
  settings.cc_algo = quicconf.upstream.congestion_controller;
  settings.max_window = http3conf.upstream.max_connection_window_size;
  settings.max_stream_window = http3conf.upstream.max_window_size;
  settings.max_udp_payload_size = SHRPX_QUIC_MAX_UDP_PAYLOAD_SIZE;
  settings.assume_symmetric_path = 1;
  settings.rand_ctx.native_handle = &worker->get_randgen();
  settings.token = ngtcp2_vec{const_cast<uint8_t *>(token), tokenlen};

  ngtcp2_transport_params params;
  ngtcp2_transport_params_default(&params);
  params.initial_max_streams_bidi = http3conf.upstream.max_concurrent_streams;
  // The minimum number of unidirectional streams required for HTTP/3.
  params.initial_max_streams_uni = 3;
  params.initial_max_data = http3conf.upstream.connection_window_size;
  params.initial_max_stream_data_bidi_remote = http3conf.upstream.window_size;
  params.initial_max_stream_data_uni = http3conf.upstream.window_size;
  params.max_idle_timeout = static_cast<ngtcp2_tstamp>(
      quicconf.upstream.timeout.idle * NGTCP2_SECONDS);

#ifdef OPENSSL_IS_BORINGSSL
  if (quicconf.upstream.early_data) {
    ngtcp2_transport_params early_data_params{
        .initial_max_stream_data_bidi_local =
            params.initial_max_stream_data_bidi_local,
        .initial_max_stream_data_bidi_remote =
            params.initial_max_stream_data_bidi_remote,
        .initial_max_stream_data_uni = params.initial_max_stream_data_uni,
        .initial_max_data = params.initial_max_data,
        .initial_max_streams_bidi = params.initial_max_streams_bidi,
        .initial_max_streams_uni = params.initial_max_streams_uni,
    };

    // TODO include HTTP/3 SETTINGS

    std::array<uint8_t, 128> quic_early_data_ctx;

    auto quic_early_data_ctxlen = ngtcp2_encode_transport_params(
        quic_early_data_ctx.data(), quic_early_data_ctx.size(),
        NGTCP2_TRANSPORT_PARAMS_TYPE_ENCRYPTED_EXTENSIONS, &early_data_params);

    assert(quic_early_data_ctxlen > 0);
    assert(static_cast<size_t>(quic_early_data_ctxlen) <=
           quic_early_data_ctx.size());

    if (SSL_set_quic_early_data_context(handler_->get_ssl(),
                                        quic_early_data_ctx.data(),
                                        quic_early_data_ctxlen) != 1) {
      ULOG(ERROR, this) << "SSL_set_quic_early_data_context failed";
      return -1;
    }
  }
#endif // OPENSSL_IS_BORINGSSL

  if (odcid) {
    params.original_dcid = *odcid;
    params.retry_scid = initial_hd.dcid;
    params.retry_scid_present = 1;
  } else {
    params.original_dcid = initial_hd.dcid;
  }

  rv = generate_quic_stateless_reset_token(
      params.stateless_reset_token, scid, qkm.secret.data(), qkm.secret.size());
  if (rv != 0) {
    ULOG(ERROR, this) << "generate_quic_stateless_reset_token failed";
    return -1;
  }
  params.stateless_reset_token_present = 1;

  auto path = ngtcp2_path{
      {
          const_cast<sockaddr *>(&local_addr.su.sa),
          local_addr.len,
      },
      {
          const_cast<sockaddr *>(&remote_addr.su.sa),
          remote_addr.len,
      },
      const_cast<UpstreamAddr *>(faddr),
  };

  rv = ngtcp2_conn_server_new(&conn_, &initial_hd.scid, &scid, &path,
                              initial_hd.version, &callbacks, &settings,
                              &params, nullptr, this);
  if (rv != 0) {
    ULOG(ERROR, this) << "ngtcp2_conn_server_new: " << ngtcp2_strerror(rv);
    return -1;
  }

  ngtcp2_conn_set_tls_native_handle(conn_, handler_->get_ssl());

  auto quic_connection_handler = worker->get_quic_connection_handler();

  if (generate_quic_hashed_connection_id(hashed_scid_, remote_addr, local_addr,
                                         initial_hd.dcid) != 0) {
    return -1;
  }

  quic_connection_handler->add_connection_id(hashed_scid_, handler_);
  quic_connection_handler->add_connection_id(scid, handler_);

  return 0;
}

int Http3Upstream::on_read() { return 0; }

int Http3Upstream::on_write() {
  if (write_streams() != 0) {
    return -1;
  }

  reset_timer();

  return 0;
}

int Http3Upstream::write_streams() {
  std::array<nghttp3_vec, 16> vec;
  std::array<uint8_t, 64_k> buf;
  auto max_udp_payload_size = std::min(
      max_udp_payload_size_, ngtcp2_conn_get_path_max_udp_payload_size(conn_));
  size_t max_pktcnt =
      std::min(static_cast<size_t>(64_k), ngtcp2_conn_get_send_quantum(conn_)) /
      max_udp_payload_size;
  ngtcp2_pkt_info pi, prev_pi;
  uint8_t *bufpos = buf.data();
  ngtcp2_path_storage ps, prev_ps;
  size_t pktcnt = 0;
  int rv;
  auto ts = quic_timestamp();

  ngtcp2_path_storage_zero(&ps);
  ngtcp2_path_storage_zero(&prev_ps);

  auto config = get_config();
  auto &quicconf = config->quic;

  if (quicconf.upstream.congestion_controller != NGTCP2_CC_ALGO_BBR) {
    max_pktcnt = std::min(max_pktcnt, static_cast<size_t>(10));
  }

  for (;;) {
    int64_t stream_id = -1;
    int fin = 0;
    nghttp3_ssize sveccnt = 0;

    if (httpconn_ && ngtcp2_conn_get_max_data_left(conn_)) {
      sveccnt = nghttp3_conn_writev_stream(httpconn_, &stream_id, &fin,
                                           vec.data(), vec.size());
      if (sveccnt < 0) {
        ULOG(ERROR, this) << "nghttp3_conn_writev_stream: "
                          << nghttp3_strerror(sveccnt);
        last_error_ = quic::err_application(sveccnt);
        return handle_error();
      }
    }

    ngtcp2_ssize ndatalen;
    auto v = vec.data();
    auto vcnt = static_cast<size_t>(sveccnt);

    uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
    if (fin) {
      flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
    }

    auto nwrite = ngtcp2_conn_writev_stream(
        conn_, &ps.path, &pi, bufpos, max_udp_payload_size, &ndatalen, flags,
        stream_id, reinterpret_cast<const ngtcp2_vec *>(v), vcnt, ts);
    if (nwrite < 0) {
      switch (nwrite) {
      case NGTCP2_ERR_STREAM_DATA_BLOCKED:
        assert(ndatalen == -1);
        rv = nghttp3_conn_block_stream(httpconn_, stream_id);
        if (rv != 0) {
          ULOG(ERROR, this)
              << "nghttp3_conn_block_stream: " << nghttp3_strerror(rv);
          last_error_ = quic::err_application(rv);
          return handle_error();
        }
        continue;
      case NGTCP2_ERR_STREAM_SHUT_WR:
        assert(ndatalen == -1);
        rv = nghttp3_conn_shutdown_stream_write(httpconn_, stream_id);
        if (rv != 0) {
          ULOG(ERROR, this)
              << "nghttp3_conn_shutdown_stream_write: " << nghttp3_strerror(rv);
          last_error_ = quic::err_application(rv);
          return handle_error();
        }
        continue;
      case NGTCP2_ERR_WRITE_MORE:
        assert(ndatalen >= 0);
        rv = nghttp3_conn_add_write_offset(httpconn_, stream_id, ndatalen);
        if (rv != 0) {
          ULOG(ERROR, this)
              << "nghttp3_conn_add_write_offset: " << nghttp3_strerror(rv);
          last_error_ = quic::err_application(rv);
          return handle_error();
        }
        continue;
      }

      assert(ndatalen == -1);

      ULOG(ERROR, this) << "ngtcp2_conn_writev_stream: "
                        << ngtcp2_strerror(nwrite);

      last_error_ = quic::err_transport(nwrite);

      handler_->get_connection()->wlimit.stopw();

      return handle_error();
    } else if (ndatalen >= 0) {
      rv = nghttp3_conn_add_write_offset(httpconn_, stream_id, ndatalen);
      if (rv != 0) {
        ULOG(ERROR, this) << "nghttp3_conn_add_write_offset: "
                          << nghttp3_strerror(rv);
        last_error_ = quic::err_application(rv);
        return handle_error();
      }
    }

    if (nwrite == 0) {
      if (bufpos - buf.data()) {
        send_packet(static_cast<UpstreamAddr *>(prev_ps.path.user_data),
                    prev_ps.path.remote.addr, prev_ps.path.remote.addrlen,
                    prev_ps.path.local.addr, prev_ps.path.local.addrlen,
                    prev_pi, buf.data(), bufpos - buf.data(),
                    max_udp_payload_size);

        reset_idle_timer();
      }

      ngtcp2_conn_update_pkt_tx_time(conn_, ts);

      handler_->get_connection()->wlimit.stopw();

      return 0;
    }

    bufpos += nwrite;

#ifdef UDP_SEGMENT
    if (pktcnt == 0) {
      ngtcp2_path_copy(&prev_ps.path, &ps.path);
      prev_pi = pi;
    } else if (!ngtcp2_path_eq(&prev_ps.path, &ps.path) ||
               prev_pi.ecn != pi.ecn) {
      send_packet(static_cast<UpstreamAddr *>(prev_ps.path.user_data),
                  prev_ps.path.remote.addr, prev_ps.path.remote.addrlen,
                  prev_ps.path.local.addr, prev_ps.path.local.addrlen, prev_pi,
                  buf.data(), bufpos - buf.data() - nwrite,
                  max_udp_payload_size);

      send_packet(static_cast<UpstreamAddr *>(ps.path.user_data),
                  ps.path.remote.addr, ps.path.remote.addrlen,
                  ps.path.local.addr, ps.path.local.addrlen, pi,
                  bufpos - nwrite, nwrite, max_udp_payload_size);

      ngtcp2_conn_update_pkt_tx_time(conn_, ts);
      reset_idle_timer();

      handler_->signal_write();

      return 0;
    }

    if (++pktcnt == max_pktcnt ||
        static_cast<size_t>(nwrite) < max_udp_payload_size) {
      send_packet(static_cast<UpstreamAddr *>(ps.path.user_data),
                  ps.path.remote.addr, ps.path.remote.addrlen,
                  ps.path.local.addr, ps.path.local.addrlen, pi, buf.data(),
                  bufpos - buf.data(), max_udp_payload_size);

      ngtcp2_conn_update_pkt_tx_time(conn_, ts);
      reset_idle_timer();

      handler_->signal_write();

      return 0;
    }
#else  // !UDP_SEGMENT
    send_packet(static_cast<UpstreamAddr *>(ps.path.user_data),
                ps.path.remote.addr, ps.path.remote.addrlen, ps.path.local.addr,
                ps.path.local.addrlen, pi, buf.data(), bufpos - buf.data(), 0);

    if (++pktcnt == max_pktcnt) {
      ngtcp2_conn_update_pkt_tx_time(conn_, ts);
      reset_idle_timer();

      handler_->signal_write();

      return 0;
    }

    bufpos = buf.data();
#endif // !UDP_SEGMENT
  }

  return 0;
}

int Http3Upstream::on_timeout(Downstream *downstream) { return 0; }

int Http3Upstream::on_downstream_abort_request(Downstream *downstream,
                                               unsigned int status_code) {
  int rv;

  rv = error_reply(downstream, status_code);

  if (rv != 0) {
    return -1;
  }

  handler_->signal_write();

  return 0;
}

int Http3Upstream::on_downstream_abort_request_with_https_redirect(
    Downstream *downstream) {
  int rv;

  rv = redirect_to_https(downstream);
  if (rv != 0) {
    return -1;
  }

  handler_->signal_write();
  return 0;
}

namespace {
uint64_t
infer_upstream_shutdown_stream_error_code(uint32_t downstream_error_code) {
  // NGHTTP2_REFUSED_STREAM is important because it tells upstream
  // client to retry.
  switch (downstream_error_code) {
  case NGHTTP2_NO_ERROR:
    return NGHTTP3_H3_NO_ERROR;
  case NGHTTP2_REFUSED_STREAM:
    return NGHTTP3_H3_REQUEST_REJECTED;
  default:
    return NGHTTP3_H3_INTERNAL_ERROR;
  }
}
} // namespace

int Http3Upstream::downstream_read(DownstreamConnection *dconn) {
  auto downstream = dconn->get_downstream();

  if (downstream->get_response_state() == DownstreamState::MSG_RESET) {
    // The downstream stream was reset (canceled). In this case,
    // RST_STREAM to the upstream and delete downstream connection
    // here. Deleting downstream will be taken place at
    // on_stream_close_callback.
    shutdown_stream(downstream,
                    infer_upstream_shutdown_stream_error_code(
                        downstream->get_response_rst_stream_error_code()));
    downstream->pop_downstream_connection();
    // dconn was deleted
    dconn = nullptr;
  } else if (downstream->get_response_state() ==
             DownstreamState::MSG_BAD_HEADER) {
    if (error_reply(downstream, 502) != 0) {
      return -1;
    }
    downstream->pop_downstream_connection();
    // dconn was deleted
    dconn = nullptr;
  } else {
    auto rv = downstream->on_read();
    if (rv == SHRPX_ERR_EOF) {
      if (downstream->get_request_header_sent()) {
        return downstream_eof(dconn);
      }
      return SHRPX_ERR_RETRY;
    }
    if (rv == SHRPX_ERR_DCONN_CANCELED) {
      downstream->pop_downstream_connection();
      handler_->signal_write();
      return 0;
    }
    if (rv != 0) {
      if (rv != SHRPX_ERR_NETWORK) {
        if (LOG_ENABLED(INFO)) {
          DCLOG(INFO, dconn) << "HTTP parser failure";
        }
      }
      return downstream_error(dconn, Downstream::EVENT_ERROR);
    }

    if (downstream->can_detach_downstream_connection()) {
      // Keep-alive
      downstream->detach_downstream_connection();
    }
  }

  handler_->signal_write();

  // At this point, downstream may be deleted.

  return 0;
}

int Http3Upstream::downstream_write(DownstreamConnection *dconn) {
  int rv;
  rv = dconn->on_write();
  if (rv == SHRPX_ERR_NETWORK) {
    return downstream_error(dconn, Downstream::EVENT_ERROR);
  }
  if (rv != 0) {
    return rv;
  }
  return 0;
}

int Http3Upstream::downstream_eof(DownstreamConnection *dconn) {
  auto downstream = dconn->get_downstream();

  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, dconn) << "EOF. stream_id=" << downstream->get_stream_id();
  }

  // Delete downstream connection. If we don't delete it here, it will
  // be pooled in on_stream_close_callback.
  downstream->pop_downstream_connection();
  // dconn was deleted
  dconn = nullptr;
  // downstream will be deleted in on_stream_close_callback.
  if (downstream->get_response_state() == DownstreamState::HEADER_COMPLETE) {
    // Server may indicate the end of the request by EOF
    if (LOG_ENABLED(INFO)) {
      ULOG(INFO, this) << "Downstream body was ended by EOF";
    }
    downstream->set_response_state(DownstreamState::MSG_COMPLETE);

    // For tunneled connection, MSG_COMPLETE signals
    // downstream_read_data_callback to send RST_STREAM after pending
    // response body is sent. This is needed to ensure that RST_STREAM
    // is sent after all pending data are sent.
    if (on_downstream_body_complete(downstream) != 0) {
      return -1;
    }
  } else if (downstream->get_response_state() !=
             DownstreamState::MSG_COMPLETE) {
    // If stream was not closed, then we set MSG_COMPLETE and let
    // on_stream_close_callback delete downstream.
    if (error_reply(downstream, 502) != 0) {
      return -1;
    }
  }
  handler_->signal_write();
  // At this point, downstream may be deleted.
  return 0;
}

int Http3Upstream::downstream_error(DownstreamConnection *dconn, int events) {
  auto downstream = dconn->get_downstream();

  if (LOG_ENABLED(INFO)) {
    if (events & Downstream::EVENT_ERROR) {
      DCLOG(INFO, dconn) << "Downstream network/general error";
    } else {
      DCLOG(INFO, dconn) << "Timeout";
    }
    if (downstream->get_upgraded()) {
      DCLOG(INFO, dconn) << "Note: this is tunnel connection";
    }
  }

  // Delete downstream connection. If we don't delete it here, it will
  // be pooled in on_stream_close_callback.
  downstream->pop_downstream_connection();
  // dconn was deleted
  dconn = nullptr;

  if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
    // For SSL tunneling, we issue RST_STREAM. For other types of
    // stream, we don't have to do anything since response was
    // complete.
    if (downstream->get_upgraded()) {
      shutdown_stream(downstream, NGHTTP3_H3_NO_ERROR);
    }
  } else {
    if (downstream->get_response_state() == DownstreamState::HEADER_COMPLETE) {
      if (downstream->get_upgraded()) {
        if (on_downstream_body_complete(downstream) != 0) {
          return -1;
        }
      } else {
        shutdown_stream(downstream, NGHTTP3_H3_INTERNAL_ERROR);
      }
    } else {
      unsigned int status;
      if (events & Downstream::EVENT_TIMEOUT) {
        if (downstream->get_request_header_sent()) {
          status = 504;
        } else {
          status = 408;
        }
      } else {
        status = 502;
      }
      if (error_reply(downstream, status) != 0) {
        return -1;
      }
    }
    downstream->set_response_state(DownstreamState::MSG_COMPLETE);
  }
  handler_->signal_write();
  // At this point, downstream may be deleted.
  return 0;
}

ClientHandler *Http3Upstream::get_client_handler() const { return handler_; }

namespace {
nghttp3_ssize downstream_read_data_callback(nghttp3_conn *conn,
                                            int64_t stream_id, nghttp3_vec *vec,
                                            size_t veccnt, uint32_t *pflags,
                                            void *conn_user_data,
                                            void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(conn_user_data);
  auto downstream = static_cast<Downstream *>(stream_user_data);

  assert(downstream);

  auto body = downstream->get_response_buf();

  assert(body);

  if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
  } else if (body->rleft_mark() == 0) {
    downstream->disable_upstream_wtimer();
    return NGHTTP3_ERR_WOULDBLOCK;
  }

  downstream->reset_upstream_wtimer();

  veccnt = body->riovec_mark(reinterpret_cast<struct iovec *>(vec), veccnt);

  assert((*pflags & NGHTTP3_DATA_FLAG_EOF) || veccnt);

  downstream->response_sent_body_length += nghttp3_vec_len(vec, veccnt);

  if ((*pflags & NGHTTP3_DATA_FLAG_EOF) &&
      upstream->shutdown_stream_read(stream_id, NGHTTP3_H3_NO_ERROR) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  return veccnt;
}
} // namespace

int Http3Upstream::on_downstream_header_complete(Downstream *downstream) {
  int rv;

  const auto &req = downstream->request();
  auto &resp = downstream->response();

  auto &balloc = downstream->get_block_allocator();

  if (LOG_ENABLED(INFO)) {
    if (downstream->get_non_final_response()) {
      DLOG(INFO, downstream) << "HTTP non-final response header";
    } else {
      DLOG(INFO, downstream) << "HTTP response header completed";
    }
  }

  auto config = get_config();
  auto &httpconf = config->http;

  if (!config->http2_proxy && !httpconf.no_location_rewrite) {
    downstream->rewrite_location_response_header(req.scheme);
  }

#ifdef HAVE_MRUBY
  if (!downstream->get_non_final_response()) {
    auto dconn = downstream->get_downstream_connection();
    const auto &group = dconn->get_downstream_addr_group();
    if (group) {
      const auto &dmruby_ctx = group->shared_addr->mruby_ctx;

      if (dmruby_ctx->run_on_response_proc(downstream) != 0) {
        if (error_reply(downstream, 500) != 0) {
          return -1;
        }
        // Returning -1 will signal deletion of dconn.
        return -1;
      }

      if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
        return -1;
      }
    }

    auto worker = handler_->get_worker();
    auto mruby_ctx = worker->get_mruby_context();

    if (mruby_ctx->run_on_response_proc(downstream) != 0) {
      if (error_reply(downstream, 500) != 0) {
        return -1;
      }
      // Returning -1 will signal deletion of dconn.
      return -1;
    }

    if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
      return -1;
    }
  }
#endif // HAVE_MRUBY

  auto nva = std::vector<nghttp3_nv>();
  // 4 means :status and possible server, via, and set-cookie (for
  // affinity cookie) header field.
  nva.reserve(resp.fs.headers().size() + 4 +
              httpconf.add_response_headers.size());

  if (downstream->get_non_final_response()) {
    auto response_status = http2::stringify_status(balloc, resp.http_status);

    nva.push_back(http3::make_nv_ls_nocopy(":status", response_status));

    http3::copy_headers_to_nva_nocopy(nva, resp.fs.headers(),
                                      http2::HDOP_STRIP_ALL);

    if (LOG_ENABLED(INFO)) {
      log_response_headers(downstream, nva);
    }

    rv = nghttp3_conn_submit_info(httpconn_, downstream->get_stream_id(),
                                  nva.data(), nva.size());

    resp.fs.clear_headers();

    if (rv != 0) {
      ULOG(FATAL, this) << "nghttp3_conn_submit_info() failed";
      return -1;
    }

    return 0;
  }

  auto striphd_flags = http2::HDOP_STRIP_ALL & ~http2::HDOP_STRIP_VIA;
  StringRef response_status;

  if (req.connect_proto == ConnectProto::WEBSOCKET && resp.http_status == 101) {
    response_status = http2::stringify_status(balloc, 200);
    striphd_flags |= http2::HDOP_STRIP_SEC_WEBSOCKET_ACCEPT;
  } else {
    response_status = http2::stringify_status(balloc, resp.http_status);
  }

  nva.push_back(http3::make_nv_ls_nocopy(":status", response_status));

  http3::copy_headers_to_nva_nocopy(nva, resp.fs.headers(), striphd_flags);

  if (!config->http2_proxy && !httpconf.no_server_rewrite) {
    nva.push_back(http3::make_nv_ls_nocopy("server", httpconf.server_name));
  } else {
    auto server = resp.fs.header(http2::HD_SERVER);
    if (server) {
      nva.push_back(http3::make_nv_ls_nocopy("server", (*server).value));
    }
  }

  if (!req.regular_connect_method() || !downstream->get_upgraded()) {
    auto affinity_cookie = downstream->get_affinity_cookie_to_send();
    if (affinity_cookie) {
      auto dconn = downstream->get_downstream_connection();
      assert(dconn);
      auto &group = dconn->get_downstream_addr_group();
      auto &shared_addr = group->shared_addr;
      auto &cookieconf = shared_addr->affinity.cookie;
      auto secure =
          http::require_cookie_secure_attribute(cookieconf.secure, req.scheme);
      auto cookie_str = http::create_affinity_cookie(
          balloc, cookieconf.name, affinity_cookie, cookieconf.path, secure);
      nva.push_back(http3::make_nv_ls_nocopy("set-cookie", cookie_str));
    }
  }

  auto via = resp.fs.header(http2::HD_VIA);
  if (httpconf.no_via) {
    if (via) {
      nva.push_back(http3::make_nv_ls_nocopy("via", (*via).value));
    }
  } else {
    // we don't create more than 16 bytes in
    // http::create_via_header_value.
    size_t len = 16;
    if (via) {
      len += via->value.size() + 2;
    }

    auto iov = make_byte_ref(balloc, len + 1);
    auto p = iov.base;
    if (via) {
      p = std::copy(std::begin(via->value), std::end(via->value), p);
      p = util::copy_lit(p, ", ");
    }
    p = http::create_via_header_value(p, resp.http_major, resp.http_minor);
    *p = '\0';

    nva.push_back(http3::make_nv_ls_nocopy("via", StringRef{iov.base, p}));
  }

  for (auto &p : httpconf.add_response_headers) {
    nva.push_back(http3::make_nv_nocopy(p.name, p.value));
  }

  if (LOG_ENABLED(INFO)) {
    log_response_headers(downstream, nva);
  }

  nghttp3_data_reader data_read;
  data_read.read_data = downstream_read_data_callback;

  nghttp3_data_reader *data_readptr;

  if (downstream->expect_response_body() ||
      downstream->expect_response_trailer()) {
    data_readptr = &data_read;
  } else {
    data_readptr = nullptr;
  }

  rv = nghttp3_conn_submit_response(httpconn_, downstream->get_stream_id(),
                                    nva.data(), nva.size(), data_readptr);
  if (rv != 0) {
    ULOG(FATAL, this) << "nghttp3_conn_submit_response() failed";
    return -1;
  }

  if (data_readptr) {
    downstream->reset_upstream_wtimer();
  } else if (shutdown_stream_read(downstream->get_stream_id(),
                                  NGHTTP3_H3_NO_ERROR) != 0) {
    return -1;
  }

  return 0;
}

int Http3Upstream::on_downstream_body(Downstream *downstream,
                                      const uint8_t *data, size_t len,
                                      bool flush) {
  auto body = downstream->get_response_buf();
  body->append(data, len);

  if (flush) {
    nghttp3_conn_resume_stream(httpconn_, downstream->get_stream_id());

    downstream->ensure_upstream_wtimer();
  }

  return 0;
}

int Http3Upstream::on_downstream_body_complete(Downstream *downstream) {
  if (LOG_ENABLED(INFO)) {
    DLOG(INFO, downstream) << "HTTP response completed";
  }

  auto &resp = downstream->response();

  if (!downstream->validate_response_recv_body_length()) {
    shutdown_stream(downstream, NGHTTP3_H3_GENERAL_PROTOCOL_ERROR);
    resp.connection_close = true;
    return 0;
  }

  if (!downstream->get_upgraded()) {
    const auto &trailers = resp.fs.trailers();
    if (!trailers.empty()) {
      std::vector<nghttp3_nv> nva;
      nva.reserve(trailers.size());
      http3::copy_headers_to_nva_nocopy(nva, trailers, http2::HDOP_STRIP_ALL);
      if (!nva.empty()) {
        auto rv = nghttp3_conn_submit_trailers(
            httpconn_, downstream->get_stream_id(), nva.data(), nva.size());
        if (rv != 0) {
          ULOG(FATAL, this) << "nghttp3_conn_submit_trailers() failed: "
                            << nghttp3_strerror(rv);
          return -1;
        }
      }
    }
  }

  nghttp3_conn_resume_stream(httpconn_, downstream->get_stream_id());
  downstream->ensure_upstream_wtimer();

  return 0;
}

void Http3Upstream::on_handler_delete() {
  for (auto d = downstream_queue_.get_downstreams(); d; d = d->dlnext) {
    if (d->get_dispatch_state() == DispatchState::ACTIVE &&
        d->accesslog_ready()) {
      handler_->write_accesslog(d);
    }
  }

  auto worker = handler_->get_worker();
  auto quic_conn_handler = worker->get_quic_connection_handler();

  std::vector<ngtcp2_cid> scids(ngtcp2_conn_get_num_scid(conn_) + 1);
  ngtcp2_conn_get_scid(conn_, scids.data());
  scids.back() = hashed_scid_;

  for (auto &cid : scids) {
    quic_conn_handler->remove_connection_id(cid);
  }

  if (idle_close_ || retry_close_) {
    return;
  }

  // If this is not idle close, send CONNECTION_CLOSE.
  if (!ngtcp2_conn_is_in_closing_period(conn_) &&
      !ngtcp2_conn_is_in_draining_period(conn_)) {
    ngtcp2_path_storage ps;
    ngtcp2_pkt_info pi;
    conn_close_.resize(SHRPX_QUIC_CONN_CLOSE_PKTLEN);

    ngtcp2_path_storage_zero(&ps);

    auto nwrite = ngtcp2_conn_write_connection_close(
        conn_, &ps.path, &pi, conn_close_.data(), conn_close_.size(),
        NGTCP2_NO_ERROR, nullptr, 0, quic_timestamp());
    if (nwrite < 0) {
      if (nwrite != NGTCP2_ERR_INVALID_STATE) {
        ULOG(ERROR, this) << "ngtcp2_conn_write_connection_close: "
                          << ngtcp2_strerror(nwrite);
      }

      return;
    }

    conn_close_.resize(nwrite);

    send_packet(static_cast<UpstreamAddr *>(ps.path.user_data),
                ps.path.remote.addr, ps.path.remote.addrlen, ps.path.local.addr,
                ps.path.local.addrlen, pi, conn_close_.data(), nwrite, 0);
  }

  auto d =
      static_cast<ev_tstamp>(ngtcp2_conn_get_pto(conn_) * 3) / NGTCP2_SECONDS;

  if (LOG_ENABLED(INFO)) {
    ULOG(INFO, this) << "Enter close-wait period " << d << "s with "
                     << conn_close_.size() << " bytes sentinel packet";
  }

  auto cw = std::make_unique<CloseWait>(worker, std::move(scids),
                                        std::move(conn_close_), d);

  quic_conn_handler->add_close_wait(cw.get());

  cw.release();
}

int Http3Upstream::on_downstream_reset(Downstream *downstream, bool no_retry) {
  int rv;

  if (downstream->get_dispatch_state() != DispatchState::ACTIVE) {
    // This is error condition when we failed push_request_headers()
    // in initiate_downstream().  Otherwise, we have
    // DispatchState::ACTIVE state, or we did not set
    // DownstreamConnection.
    downstream->pop_downstream_connection();
    handler_->signal_write();

    return 0;
  }

  if (!downstream->request_submission_ready()) {
    if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
      // We have got all response body already.  Send it off.
      downstream->pop_downstream_connection();
      return 0;
    }
    // pushed stream is handled here
    shutdown_stream(downstream, NGHTTP3_H3_INTERNAL_ERROR);
    downstream->pop_downstream_connection();

    handler_->signal_write();

    return 0;
  }

  downstream->pop_downstream_connection();

  downstream->add_retry();

  std::unique_ptr<DownstreamConnection> dconn;

  rv = 0;

  if (no_retry || downstream->no_more_retry()) {
    goto fail;
  }

  // downstream connection is clean; we can retry with new
  // downstream connection.

  for (;;) {
    auto dconn = handler_->get_downstream_connection(rv, downstream);
    if (!dconn) {
      goto fail;
    }

    rv = downstream->attach_downstream_connection(std::move(dconn));
    if (rv == 0) {
      break;
    }
  }

  rv = downstream->push_request_headers();
  if (rv != 0) {
    goto fail;
  }

  return 0;

fail:
  if (rv == SHRPX_ERR_TLS_REQUIRED) {
    rv = on_downstream_abort_request_with_https_redirect(downstream);
  } else {
    rv = on_downstream_abort_request(downstream, 502);
  }
  if (rv != 0) {
    shutdown_stream(downstream, NGHTTP3_H3_INTERNAL_ERROR);
  }
  downstream->pop_downstream_connection();

  handler_->signal_write();

  return 0;
}

void Http3Upstream::pause_read(IOCtrlReason reason) {}

int Http3Upstream::resume_read(IOCtrlReason reason, Downstream *downstream,
                               size_t consumed) {
  consume(downstream->get_stream_id(), consumed);

  auto &req = downstream->request();

  req.consume(consumed);

  handler_->signal_write();

  return 0;
}

int Http3Upstream::send_reply(Downstream *downstream, const uint8_t *body,
                              size_t bodylen) {
  int rv;

  nghttp3_data_reader data_read, *data_read_ptr = nullptr;

  if (bodylen) {
    data_read.read_data = downstream_read_data_callback;
    data_read_ptr = &data_read;
  }

  const auto &resp = downstream->response();
  auto config = get_config();
  auto &httpconf = config->http;

  auto &balloc = downstream->get_block_allocator();

  const auto &headers = resp.fs.headers();
  auto nva = std::vector<nghttp3_nv>();
  // 2 for :status and server
  nva.reserve(2 + headers.size() + httpconf.add_response_headers.size());

  auto response_status = http2::stringify_status(balloc, resp.http_status);

  nva.push_back(http3::make_nv_ls_nocopy(":status", response_status));

  for (auto &kv : headers) {
    if (kv.name.empty() || kv.name[0] == ':') {
      continue;
    }
    switch (kv.token) {
    case http2::HD_CONNECTION:
    case http2::HD_KEEP_ALIVE:
    case http2::HD_PROXY_CONNECTION:
    case http2::HD_TE:
    case http2::HD_TRANSFER_ENCODING:
    case http2::HD_UPGRADE:
      continue;
    }
    nva.push_back(http3::make_nv_nocopy(kv.name, kv.value, kv.no_index));
  }

  if (!resp.fs.header(http2::HD_SERVER)) {
    nva.push_back(http3::make_nv_ls_nocopy("server", config->http.server_name));
  }

  for (auto &p : httpconf.add_response_headers) {
    nva.push_back(http3::make_nv_nocopy(p.name, p.value));
  }

  rv = nghttp3_conn_submit_response(httpconn_, downstream->get_stream_id(),
                                    nva.data(), nva.size(), data_read_ptr);
  if (nghttp3_err_is_fatal(rv)) {
    ULOG(FATAL, this) << "nghttp3_conn_submit_response() failed: "
                      << nghttp3_strerror(rv);
    return -1;
  }

  auto buf = downstream->get_response_buf();

  buf->append(body, bodylen);

  downstream->set_response_state(DownstreamState::MSG_COMPLETE);

  if (data_read_ptr) {
    downstream->reset_upstream_wtimer();
  }

  if (shutdown_stream_read(downstream->get_stream_id(), NGHTTP3_H3_NO_ERROR) !=
      0) {
    return -1;
  }

  return 0;
}

int Http3Upstream::initiate_push(Downstream *downstream, const StringRef &uri) {
  return 0;
}

int Http3Upstream::response_riovec(struct iovec *iov, int iovcnt) const {
  return 0;
}

void Http3Upstream::response_drain(size_t n) {}

bool Http3Upstream::response_empty() const { return false; }

Downstream *
Http3Upstream::on_downstream_push_promise(Downstream *downstream,
                                          int32_t promised_stream_id) {
  return nullptr;
}

int Http3Upstream::on_downstream_push_promise_complete(
    Downstream *downstream, Downstream *promised_downstream) {
  return 0;
}

bool Http3Upstream::push_enabled() const { return false; }

void Http3Upstream::cancel_premature_downstream(
    Downstream *promised_downstream) {}

int Http3Upstream::on_read(const UpstreamAddr *faddr,
                           const Address &remote_addr,
                           const Address &local_addr, const ngtcp2_pkt_info &pi,
                           const uint8_t *data, size_t datalen) {
  int rv;

  auto path = ngtcp2_path{
      {
          const_cast<sockaddr *>(&local_addr.su.sa),
          local_addr.len,
      },
      {
          const_cast<sockaddr *>(&remote_addr.su.sa),
          remote_addr.len,
      },
      const_cast<UpstreamAddr *>(faddr),
  };

  rv = ngtcp2_conn_read_pkt(conn_, &path, &pi, data, datalen, quic_timestamp());
  if (rv != 0) {
    switch (rv) {
    case NGTCP2_ERR_DRAINING:
      return -1;
    case NGTCP2_ERR_RETRY: {
      auto worker = handler_->get_worker();
      auto quic_conn_handler = worker->get_quic_connection_handler();

      uint32_t version;
      const uint8_t *dcid, *scid;
      size_t dcidlen, scidlen;

      rv = ngtcp2_pkt_decode_version_cid(&version, &dcid, &dcidlen, &scid,
                                         &scidlen, data, datalen,
                                         SHRPX_QUIC_SCIDLEN);
      if (rv != 0) {
        return -1;
      }

      if (worker->get_graceful_shutdown()) {
        ngtcp2_cid ini_dcid, ini_scid;

        ngtcp2_cid_init(&ini_dcid, dcid, dcidlen);
        ngtcp2_cid_init(&ini_scid, scid, scidlen);

        quic_conn_handler->send_connection_close(
            faddr, version, ini_dcid, ini_scid, remote_addr, local_addr,
            NGTCP2_CONNECTION_REFUSED);

        return -1;
      }

      retry_close_ = true;

      quic_conn_handler->send_retry(handler_->get_upstream_addr(), version,
                                    dcid, dcidlen, scid, scidlen, remote_addr,
                                    local_addr);

      return -1;
    }
    case NGTCP2_ERR_REQUIRED_TRANSPORT_PARAM:
    case NGTCP2_ERR_MALFORMED_TRANSPORT_PARAM:
    case NGTCP2_ERR_TRANSPORT_PARAM:
      // If rv indicates transport_parameters related error, we should
      // send TRANSPORT_PARAMETER_ERROR even if last_error_.code is
      // already set.  This is because OpenSSL might set Alert.
      last_error_ = quic::err_transport(rv);
      break;
    case NGTCP2_ERR_DROP_CONN:
      return -1;
    default:
      if (!last_error_.code) {
        last_error_ = quic::err_transport(rv);
      }
    }

    ULOG(ERROR, this) << "ngtcp2_conn_read_pkt: " << ngtcp2_strerror(rv);

    return handle_error();
  }

  reset_idle_timer();

  return 0;
}

int Http3Upstream::send_packet(const UpstreamAddr *faddr,
                               const sockaddr *remote_sa, size_t remote_salen,
                               const sockaddr *local_sa, size_t local_salen,
                               const ngtcp2_pkt_info &pi, const uint8_t *data,
                               size_t datalen, size_t gso_size) {
  auto rv = quic_send_packet(faddr, remote_sa, remote_salen, local_sa,
                             local_salen, pi, data, datalen, gso_size);
  switch (rv) {
  case 0:
    return 0;
    // With GSO, sendmsg may fail with EINVAL if UDP payload is too
    // large.
  case -EINVAL:
  case -EMSGSIZE:
    max_udp_payload_size_ = NGTCP2_MAX_UDP_PAYLOAD_SIZE;
    break;
  default:
    break;
  }

  return -1;
}

int Http3Upstream::handle_error() {
  if (ngtcp2_conn_is_in_closing_period(conn_)) {
    return -1;
  }

  ngtcp2_path_storage ps;
  ngtcp2_pkt_info pi;

  ngtcp2_path_storage_zero(&ps);

  auto ts = quic_timestamp();

  conn_close_.resize(SHRPX_QUIC_CONN_CLOSE_PKTLEN);

  ngtcp2_ssize nwrite;

  if (last_error_.type == quic::ErrorType::Transport) {
    nwrite = ngtcp2_conn_write_connection_close(
        conn_, &ps.path, &pi, conn_close_.data(), conn_close_.size(),
        last_error_.code, nullptr, 0, ts);
    if (nwrite < 0) {
      ULOG(ERROR, this) << "ngtcp2_conn_write_connection_close: "
                        << ngtcp2_strerror(nwrite);
      return -1;
    }
  } else {
    nwrite = ngtcp2_conn_write_application_close(
        conn_, &ps.path, &pi, conn_close_.data(), conn_close_.size(),
        last_error_.code, nullptr, 0, ts);
    if (nwrite < 0) {
      ULOG(ERROR, this) << "ngtcp2_conn_write_application_close: "
                        << ngtcp2_strerror(nwrite);
      return -1;
    }
  }

  conn_close_.resize(nwrite);

  send_packet(static_cast<UpstreamAddr *>(ps.path.user_data),
              ps.path.remote.addr, ps.path.remote.addrlen, ps.path.local.addr,
              ps.path.local.addrlen, pi, conn_close_.data(), nwrite, 0);

  return -1;
}

int Http3Upstream::on_rx_secret(ngtcp2_crypto_level level,
                                const uint8_t *secret, size_t secretlen) {
  if (ngtcp2_crypto_derive_and_install_rx_key(conn_, nullptr, nullptr, nullptr,
                                              level, secret, secretlen) != 0) {
    ULOG(ERROR, this) << "ngtcp2_crypto_derive_and_install_rx_key failed";
    return -1;
  }

  return 0;
}

int Http3Upstream::on_tx_secret(ngtcp2_crypto_level level,
                                const uint8_t *secret, size_t secretlen) {
  if (ngtcp2_crypto_derive_and_install_tx_key(conn_, nullptr, nullptr, nullptr,
                                              level, secret, secretlen) != 0) {
    ULOG(ERROR, this) << "ngtcp2_crypto_derive_and_install_tx_key failed";
    return -1;
  }

  if (level == NGTCP2_CRYPTO_LEVEL_APPLICATION && setup_httpconn() != 0) {
    return -1;
  }

  return 0;
}

int Http3Upstream::add_crypto_data(ngtcp2_crypto_level level,
                                   const uint8_t *data, size_t datalen) {
  int rv = ngtcp2_conn_submit_crypto_data(conn_, level, data, datalen);

  if (rv != 0) {
    ULOG(ERROR, this) << "ngtcp2_conn_submit_crypto_data: "
                      << ngtcp2_strerror(rv);
    return -1;
  }

  return 0;
}

void Http3Upstream::set_tls_alert(uint8_t alert) { tls_alert_ = alert; }

int Http3Upstream::handle_expiry() {
  int rv;

  auto ts = quic_timestamp();

  rv = ngtcp2_conn_handle_expiry(conn_, ts);
  if (rv != 0) {
    ULOG(ERROR, this) << "ngtcp2_conn_handle_expiry: " << ngtcp2_strerror(rv);
    last_error_ = quic::err_transport(rv);
    return handle_error();
  }

  return 0;
}

void Http3Upstream::reset_idle_timer() {
  auto ts = quic_timestamp();
  auto idle_ts = ngtcp2_conn_get_idle_expiry(conn_);

  idle_timer_.repeat =
      idle_ts > ts ? static_cast<ev_tstamp>(idle_ts - ts) / NGTCP2_SECONDS
                   : 1e-9;

  ev_timer_again(handler_->get_loop(), &idle_timer_);
}

void Http3Upstream::reset_timer() {
  auto ts = quic_timestamp();
  auto expiry_ts = ngtcp2_conn_get_expiry(conn_);

  timer_.repeat = expiry_ts > ts
                      ? static_cast<ev_tstamp>(expiry_ts - ts) / NGTCP2_SECONDS
                      : 1e-9;

  ev_timer_again(handler_->get_loop(), &timer_);
}

namespace {
int http_deferred_consume(nghttp3_conn *conn, int64_t stream_id,
                          size_t nconsumed, void *user_data,
                          void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);

  upstream->consume(stream_id, nconsumed);

  return 0;
}
} // namespace

namespace {
int http_acked_stream_data(nghttp3_conn *conn, int64_t stream_id,
                           uint64_t datalen, void *user_data,
                           void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);
  auto downstream = static_cast<Downstream *>(stream_user_data);

  assert(downstream);

  if (upstream->http_acked_stream_data(downstream, datalen) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Http3Upstream::http_acked_stream_data(Downstream *downstream,
                                          uint64_t datalen) {
  if (LOG_ENABLED(INFO)) {
    ULOG(INFO, this) << "Stream " << downstream->get_stream_id() << " "
                     << datalen << " bytes acknowledged";
  }

  auto body = downstream->get_response_buf();
  auto drained = body->drain_mark(datalen);
  (void)drained;

  assert(datalen == drained);

  if (downstream->resume_read(SHRPX_NO_BUFFER, datalen) != 0) {
    return -1;
  }

  return 0;
}

namespace {
int http_begin_request_headers(nghttp3_conn *conn, int64_t stream_id,
                               void *user_data, void *stream_user_data) {
  if (!ngtcp2_is_bidi_stream(stream_id)) {
    return 0;
  }

  auto upstream = static_cast<Http3Upstream *>(user_data);
  upstream->http_begin_request_headers(stream_id);

  return 0;
}
} // namespace

namespace {
int http_recv_request_header(nghttp3_conn *conn, int64_t stream_id,
                             int32_t token, nghttp3_rcbuf *name,
                             nghttp3_rcbuf *value, uint8_t flags,
                             void *user_data, void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);
  auto downstream = static_cast<Downstream *>(stream_user_data);

  if (!downstream || downstream->get_stop_reading()) {
    return 0;
  }

  if (upstream->http_recv_request_header(downstream, token, name, value, flags,
                                         /* trailer = */ false) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int http_recv_request_trailer(nghttp3_conn *conn, int64_t stream_id,
                              int32_t token, nghttp3_rcbuf *name,
                              nghttp3_rcbuf *value, uint8_t flags,
                              void *user_data, void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);
  auto downstream = static_cast<Downstream *>(stream_user_data);

  if (!downstream || downstream->get_stop_reading()) {
    return 0;
  }

  if (upstream->http_recv_request_header(downstream, token, name, value, flags,
                                         /* trailer = */ true) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Http3Upstream::http_recv_request_header(Downstream *downstream,
                                            int32_t h3token,
                                            nghttp3_rcbuf *name,
                                            nghttp3_rcbuf *value, uint8_t flags,
                                            bool trailer) {
  auto namebuf = nghttp3_rcbuf_get_buf(name);
  auto valuebuf = nghttp3_rcbuf_get_buf(value);
  auto &req = downstream->request();
  auto config = get_config();
  auto &httpconf = config->http;

  if (req.fs.buffer_size() + namebuf.len + valuebuf.len >
          httpconf.request_header_field_buffer ||
      req.fs.num_fields() >= httpconf.max_request_header_fields) {
    downstream->set_stop_reading(true);

    if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
      return 0;
    }

    if (LOG_ENABLED(INFO)) {
      ULOG(INFO, this) << "Too large or many header field size="
                       << req.fs.buffer_size() + namebuf.len + valuebuf.len
                       << ", num=" << req.fs.num_fields() + 1;
    }

    // just ignore if this is a trailer part.
    if (trailer) {
      return 0;
    }

    if (error_reply(downstream, 431) != 0) {
      return -1;
    }

    return 0;
  }

  auto token = http2::lookup_token(namebuf.base, namebuf.len);
  auto no_index = flags & NGHTTP3_NV_FLAG_NEVER_INDEX;

  downstream->add_rcbuf(name);
  downstream->add_rcbuf(value);

  if (trailer) {
    req.fs.add_trailer_token(StringRef{namebuf.base, namebuf.len},
                             StringRef{valuebuf.base, valuebuf.len}, no_index,
                             token);
    return 0;
  }

  req.fs.add_header_token(StringRef{namebuf.base, namebuf.len},
                          StringRef{valuebuf.base, valuebuf.len}, no_index,
                          token);
  return 0;
}

namespace {
int http_end_request_headers(nghttp3_conn *conn, int64_t stream_id, int fin,
                             void *user_data, void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);
  auto handler = upstream->get_client_handler();
  auto downstream = static_cast<Downstream *>(stream_user_data);

  if (!downstream || downstream->get_stop_reading()) {
    return 0;
  }

  if (upstream->http_end_request_headers(downstream, fin) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  downstream->reset_upstream_rtimer();
  handler->stop_read_timer();

  return 0;
}
} // namespace

int Http3Upstream::http_end_request_headers(Downstream *downstream, int fin) {
  auto lgconf = log_config();
  lgconf->update_tstamp(std::chrono::system_clock::now());
  auto &req = downstream->request();
  req.tstamp = lgconf->tstamp;

  if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
    return 0;
  }

  auto &nva = req.fs.headers();

  if (LOG_ENABLED(INFO)) {
    std::stringstream ss;
    for (auto &nv : nva) {
      if (nv.name == "authorization") {
        ss << TTY_HTTP_HD << nv.name << TTY_RST << ": <redacted>\n";
        continue;
      }
      ss << TTY_HTTP_HD << nv.name << TTY_RST << ": " << nv.value << "\n";
    }
    ULOG(INFO, this) << "HTTP request headers. stream_id="
                     << downstream->get_stream_id() << "\n"
                     << ss.str();
  }

  auto content_length = req.fs.header(http2::HD_CONTENT_LENGTH);
  if (content_length) {
    // libnghttp2 guarantees this can be parsed
    req.fs.content_length = util::parse_uint(content_length->value);
  }

  // presence of mandatory header fields are guaranteed by libnghttp2.
  auto authority = req.fs.header(http2::HD__AUTHORITY);
  auto path = req.fs.header(http2::HD__PATH);
  auto method = req.fs.header(http2::HD__METHOD);
  auto scheme = req.fs.header(http2::HD__SCHEME);

  auto method_token = http2::lookup_method_token(method->value);
  if (method_token == -1) {
    if (error_reply(downstream, 501) != 0) {
      return -1;
    }
    return 0;
  }

  auto faddr = handler_->get_upstream_addr();

  auto config = get_config();

  // For HTTP/2 proxy, we require :authority.
  if (method_token != HTTP_CONNECT && config->http2_proxy &&
      faddr->alt_mode == UpstreamAltMode::NONE && !authority) {
    shutdown_stream(downstream, NGHTTP3_H3_GENERAL_PROTOCOL_ERROR);
    return 0;
  }

  req.method = method_token;
  if (scheme) {
    req.scheme = scheme->value;
  }

  // nghttp2 library guarantees either :authority or host exist
  if (!authority) {
    req.no_authority = true;
    authority = req.fs.header(http2::HD_HOST);
  }

  if (authority) {
    req.authority = authority->value;
  }

  if (path) {
    if (method_token == HTTP_OPTIONS &&
        path->value == StringRef::from_lit("*")) {
      // Server-wide OPTIONS request.  Path is empty.
    } else if (config->http2_proxy &&
               faddr->alt_mode == UpstreamAltMode::NONE) {
      req.path = path->value;
    } else {
      req.path = http2::rewrite_clean_path(downstream->get_block_allocator(),
                                           path->value);
    }
  }

  auto connect_proto = req.fs.header(http2::HD__PROTOCOL);
  if (connect_proto) {
    if (connect_proto->value != "websocket") {
      if (error_reply(downstream, 400) != 0) {
        return -1;
      }
      return 0;
    }
    req.connect_proto = ConnectProto::WEBSOCKET;
  }

  if (!fin) {
    req.http2_expect_body = true;
  } else if (req.fs.content_length == -1) {
    req.fs.content_length = 0;
  }

  downstream->inspect_http2_request();

  downstream->set_request_state(DownstreamState::HEADER_COMPLETE);

#ifdef HAVE_MRUBY
  auto upstream = downstream->get_upstream();
  auto handler = upstream->get_client_handler();
  auto worker = handler->get_worker();
  auto mruby_ctx = worker->get_mruby_context();

  if (mruby_ctx->run_on_request_proc(downstream) != 0) {
    if (error_reply(downstream, 500) != 0) {
      return -1;
    }
    return 0;
  }
#endif // HAVE_MRUBY

  if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
    return 0;
  }

  start_downstream(downstream);

  return 0;
}

void Http3Upstream::start_downstream(Downstream *downstream) {
  if (downstream_queue_.can_activate(downstream->request().authority)) {
    initiate_downstream(downstream);
    return;
  }

  downstream_queue_.mark_blocked(downstream);
}

void Http3Upstream::initiate_downstream(Downstream *downstream) {
  int rv;

  DownstreamConnection *dconn_ptr;

  for (;;) {
    auto dconn = handler_->get_downstream_connection(rv, downstream);
    if (!dconn) {
      if (rv == SHRPX_ERR_TLS_REQUIRED) {
        rv = redirect_to_https(downstream);
      } else {
        rv = error_reply(downstream, 502);
      }
      if (rv != 0) {
        shutdown_stream(downstream, NGHTTP3_H3_INTERNAL_ERROR);
      }

      downstream->set_request_state(DownstreamState::CONNECT_FAIL);
      downstream_queue_.mark_failure(downstream);

      return;
    }

#ifdef HAVE_MRUBY
    dconn_ptr = dconn.get();
#endif // HAVE_MRUBY
    rv = downstream->attach_downstream_connection(std::move(dconn));
    if (rv == 0) {
      break;
    }
  }

#ifdef HAVE_MRUBY
  const auto &group = dconn_ptr->get_downstream_addr_group();
  if (group) {
    const auto &mruby_ctx = group->shared_addr->mruby_ctx;
    if (mruby_ctx->run_on_request_proc(downstream) != 0) {
      if (error_reply(downstream, 500) != 0) {
        shutdown_stream(downstream, NGHTTP3_H3_INTERNAL_ERROR);
      }

      downstream_queue_.mark_failure(downstream);

      return;
    }

    if (downstream->get_response_state() == DownstreamState::MSG_COMPLETE) {
      return;
    }
  }
#endif // HAVE_MRUBY

  rv = downstream->push_request_headers();
  if (rv != 0) {

    if (error_reply(downstream, 502) != 0) {
      shutdown_stream(downstream, NGHTTP3_H3_INTERNAL_ERROR);
    }

    downstream_queue_.mark_failure(downstream);

    return;
  }

  downstream_queue_.mark_active(downstream);

  auto &req = downstream->request();
  if (!req.http2_expect_body) {
    rv = downstream->end_upload_data();
    if (rv != 0) {
      shutdown_stream(downstream, NGHTTP3_H3_INTERNAL_ERROR);
    }
  }
}

namespace {
int http_recv_data(nghttp3_conn *conn, int64_t stream_id, const uint8_t *data,
                   size_t datalen, void *user_data, void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);
  auto downstream = static_cast<Downstream *>(stream_user_data);

  if (upstream->http_recv_data(downstream, data, datalen) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Http3Upstream::http_recv_data(Downstream *downstream, const uint8_t *data,
                                  size_t datalen) {
  downstream->reset_upstream_rtimer();

  if (downstream->push_upload_data_chunk(data, datalen) != 0) {
    if (downstream->get_response_state() != DownstreamState::MSG_COMPLETE) {
      shutdown_stream(downstream, NGHTTP3_H3_INTERNAL_ERROR);
    }

    consume(downstream->get_stream_id(), datalen);

    return 0;
  }

  return 0;
}

namespace {
int http_end_stream(nghttp3_conn *conn, int64_t stream_id, void *user_data,
                    void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);
  auto downstream = static_cast<Downstream *>(stream_user_data);

  if (!downstream || downstream->get_stop_reading()) {
    return 0;
  }

  if (upstream->http_end_stream(downstream) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Http3Upstream::http_end_stream(Downstream *downstream) {
  downstream->disable_upstream_rtimer();

  if (downstream->end_upload_data() != 0) {
    if (downstream->get_response_state() != DownstreamState::MSG_COMPLETE) {
      shutdown_stream(downstream, NGHTTP3_H3_INTERNAL_ERROR);
    }
  }

  downstream->set_request_state(DownstreamState::MSG_COMPLETE);

  return 0;
}

namespace {
int http_stream_close(nghttp3_conn *conn, int64_t stream_id,
                      uint64_t app_error_code, void *conn_user_data,
                      void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(conn_user_data);
  auto downstream = static_cast<Downstream *>(stream_user_data);

  if (!downstream) {
    return 0;
  }

  if (upstream->http_stream_close(downstream, app_error_code) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Http3Upstream::http_stream_close(Downstream *downstream,
                                     uint64_t app_error_code) {
  auto stream_id = downstream->get_stream_id();

  if (LOG_ENABLED(INFO)) {
    ULOG(INFO, this) << "Stream stream_id=" << stream_id
                     << " is being closed with app_error_code="
                     << app_error_code;

    auto body = downstream->get_response_buf();

    ULOG(INFO, this) << "response unacked_left=" << body->rleft()
                     << " not_sent=" << body->rleft_mark();
  }

  auto &req = downstream->request();

  consume(stream_id, req.unconsumed_body_length);

  req.unconsumed_body_length = 0;

  ngtcp2_conn_extend_max_streams_bidi(conn_, 1);

  if (downstream->get_request_state() == DownstreamState::CONNECT_FAIL) {
    remove_downstream(downstream);
    // downstream was deleted

    return 0;
  }

  if (downstream->can_detach_downstream_connection()) {
    // Keep-alive
    downstream->detach_downstream_connection();
  }

  downstream->set_request_state(DownstreamState::STREAM_CLOSED);

  // At this point, downstream read may be paused.

  // If shrpx_downstream::push_request_headers() failed, the
  // error is handled here.
  remove_downstream(downstream);
  // downstream was deleted

  return 0;
}

namespace {
int http_stop_sending(nghttp3_conn *conn, int64_t stream_id,
                      uint64_t app_error_code, void *user_data,
                      void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);

  if (upstream->http_stop_sending(stream_id, app_error_code) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Http3Upstream::http_stop_sending(int64_t stream_id,
                                     uint64_t app_error_code) {
  auto rv = ngtcp2_conn_shutdown_stream_read(conn_, stream_id, app_error_code);
  if (ngtcp2_err_is_fatal(rv)) {
    ULOG(ERROR, this) << "ngtcp2_conn_shutdown_stream_read: "
                      << ngtcp2_strerror(rv);
    return -1;
  }

  return 0;
}

namespace {
int http_reset_stream(nghttp3_conn *conn, int64_t stream_id,
                      uint64_t app_error_code, void *user_data,
                      void *stream_user_data) {
  auto upstream = static_cast<Http3Upstream *>(user_data);

  if (upstream->http_reset_stream(stream_id, app_error_code) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Http3Upstream::http_reset_stream(int64_t stream_id,
                                     uint64_t app_error_code) {
  auto rv = ngtcp2_conn_shutdown_stream_write(conn_, stream_id, app_error_code);
  if (ngtcp2_err_is_fatal(rv)) {
    ULOG(ERROR, this) << "ngtcp2_conn_shutdown_stream_write: "
                      << ngtcp2_strerror(rv);
    return -1;
  }

  return 0;
}

int Http3Upstream::setup_httpconn() {
  int rv;

  if (ngtcp2_conn_get_max_local_streams_uni(conn_) < 3) {
    return -1;
  }

  nghttp3_callbacks callbacks{
      shrpx::http_acked_stream_data,
      shrpx::http_stream_close,
      shrpx::http_recv_data,
      http_deferred_consume,
      shrpx::http_begin_request_headers,
      shrpx::http_recv_request_header,
      shrpx::http_end_request_headers,
      nullptr, // begin_trailers
      shrpx::http_recv_request_trailer,
      nullptr, // end_trailers
      shrpx::http_stop_sending,
      shrpx::http_end_stream,
      shrpx::http_reset_stream,
  };

  auto config = get_config();

  nghttp3_settings settings;
  nghttp3_settings_default(&settings);
  settings.qpack_max_dtable_capacity = 4_k;

  if (!config->http2_proxy) {
    settings.enable_connect_protocol = 1;
  }

  auto mem = nghttp3_mem_default();

  rv = nghttp3_conn_server_new(&httpconn_, &callbacks, &settings, mem, this);
  if (rv != 0) {
    ULOG(ERROR, this) << "nghttp3_conn_server_new: " << nghttp3_strerror(rv);
    return -1;
  }

  ngtcp2_transport_params params;
  ngtcp2_conn_get_local_transport_params(conn_, &params);

  nghttp3_conn_set_max_client_streams_bidi(httpconn_,
                                           params.initial_max_streams_bidi);

  int64_t ctrl_stream_id;

  rv = ngtcp2_conn_open_uni_stream(conn_, &ctrl_stream_id, nullptr);
  if (rv != 0) {
    ULOG(ERROR, this) << "ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv);
    return -1;
  }

  rv = nghttp3_conn_bind_control_stream(httpconn_, ctrl_stream_id);
  if (rv != 0) {
    ULOG(ERROR, this) << "nghttp3_conn_bind_control_stream: "
                      << nghttp3_strerror(rv);
    return -1;
  }

  int64_t qpack_enc_stream_id, qpack_dec_stream_id;

  rv = ngtcp2_conn_open_uni_stream(conn_, &qpack_enc_stream_id, nullptr);
  if (rv != 0) {
    ULOG(ERROR, this) << "ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv);
    return -1;
  }

  rv = ngtcp2_conn_open_uni_stream(conn_, &qpack_dec_stream_id, nullptr);
  if (rv != 0) {
    ULOG(ERROR, this) << "ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv);
    return -1;
  }

  rv = nghttp3_conn_bind_qpack_streams(httpconn_, qpack_enc_stream_id,
                                       qpack_dec_stream_id);
  if (rv != 0) {
    ULOG(ERROR, this) << "nghttp3_conn_bind_qpack_streams: "
                      << nghttp3_strerror(rv);
    return -1;
  }

  return 0;
}

int Http3Upstream::error_reply(Downstream *downstream,
                               unsigned int status_code) {
  int rv;
  auto &resp = downstream->response();

  auto &balloc = downstream->get_block_allocator();

  auto html = http::create_error_html(balloc, status_code);
  resp.http_status = status_code;
  auto body = downstream->get_response_buf();
  body->append(html);
  downstream->set_response_state(DownstreamState::MSG_COMPLETE);

  nghttp3_data_reader data_read;
  data_read.read_data = downstream_read_data_callback;

  auto lgconf = log_config();
  lgconf->update_tstamp(std::chrono::system_clock::now());

  auto response_status = http2::stringify_status(balloc, status_code);
  auto content_length = util::make_string_ref_uint(balloc, html.size());
  auto date = make_string_ref(balloc, lgconf->tstamp->time_http);

  auto nva = std::array<nghttp3_nv, 5>{
      {http3::make_nv_ls_nocopy(":status", response_status),
       http3::make_nv_ll("content-type", "text/html; charset=UTF-8"),
       http3::make_nv_ls_nocopy("server", get_config()->http.server_name),
       http3::make_nv_ls_nocopy("content-length", content_length),
       http3::make_nv_ls_nocopy("date", date)}};

  rv = nghttp3_conn_submit_response(httpconn_, downstream->get_stream_id(),
                                    nva.data(), nva.size(), &data_read);
  if (nghttp3_err_is_fatal(rv)) {
    ULOG(FATAL, this) << "nghttp3_conn_submit_response() failed: "
                      << nghttp3_strerror(rv);
    return -1;
  }

  downstream->reset_upstream_wtimer();

  if (shutdown_stream_read(downstream->get_stream_id(), NGHTTP3_H3_NO_ERROR) !=
      0) {
    return -1;
  }

  return 0;
}

int Http3Upstream::shutdown_stream(Downstream *downstream,
                                   uint64_t app_error_code) {
  auto stream_id = downstream->get_stream_id();

  if (LOG_ENABLED(INFO)) {
    ULOG(INFO, this) << "Shutdown stream_id=" << stream_id
                     << " with app_error_code=" << app_error_code;
  }

  auto rv = ngtcp2_conn_shutdown_stream(conn_, stream_id, app_error_code);
  if (rv != 0) {
    ULOG(FATAL, this) << "ngtcp2_conn_shutdown_stream() failed: "
                      << ngtcp2_strerror(rv);
    return -1;
  }

  return 0;
}

int Http3Upstream::shutdown_stream_read(int64_t stream_id,
                                        uint64_t app_error_code) {
  auto rv =
      ngtcp2_conn_shutdown_stream_read(conn_, stream_id, NGHTTP3_H3_NO_ERROR);
  if (ngtcp2_err_is_fatal(rv)) {
    ULOG(FATAL, this) << "ngtcp2_conn_shutdown_stream_read: "
                      << ngtcp2_strerror(rv);
    return -1;
  }

  return 0;
}

int Http3Upstream::redirect_to_https(Downstream *downstream) {
  auto &req = downstream->request();
  if (req.regular_connect_method() || req.scheme != "http") {
    return error_reply(downstream, 400);
  }

  auto authority = util::extract_host(req.authority);
  if (authority.empty()) {
    return error_reply(downstream, 400);
  }

  auto &balloc = downstream->get_block_allocator();
  auto config = get_config();
  auto &httpconf = config->http;

  StringRef loc;
  if (httpconf.redirect_https_port == StringRef::from_lit("443")) {
    loc = concat_string_ref(balloc, StringRef::from_lit("https://"), authority,
                            req.path);
  } else {
    loc = concat_string_ref(balloc, StringRef::from_lit("https://"), authority,
                            StringRef::from_lit(":"),
                            httpconf.redirect_https_port, req.path);
  }

  auto &resp = downstream->response();
  resp.http_status = 308;
  resp.fs.add_header_token(StringRef::from_lit("location"), loc, false,
                           http2::HD_LOCATION);

  return send_reply(downstream, nullptr, 0);
}

void Http3Upstream::consume(int64_t stream_id, size_t nconsumed) {
  ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, nconsumed);
  ngtcp2_conn_extend_max_offset(conn_, nconsumed);
}

void Http3Upstream::remove_downstream(Downstream *downstream) {
  if (downstream->accesslog_ready()) {
    handler_->write_accesslog(downstream);
  }

  nghttp3_conn_set_stream_user_data(httpconn_, downstream->get_stream_id(),
                                    nullptr);

  auto next_downstream = downstream_queue_.remove_and_get_blocked(downstream);

  if (next_downstream) {
    initiate_downstream(next_downstream);
  }

  if (downstream_queue_.get_downstreams() == nullptr) {
    // There is no downstream at the moment.  Start idle timer now.
    handler_->repeat_read_timer();
  }
}

void Http3Upstream::log_response_headers(
    Downstream *downstream, const std::vector<nghttp3_nv> &nva) const {
  std::stringstream ss;
  for (auto &nv : nva) {
    ss << TTY_HTTP_HD << StringRef{nv.name, nv.namelen} << TTY_RST << ": "
       << StringRef{nv.value, nv.valuelen} << "\n";
  }
  ULOG(INFO, this) << "HTTP response headers. stream_id="
                   << downstream->get_stream_id() << "\n"
                   << ss.str();
}

int Http3Upstream::check_shutdown() {
  auto worker = handler_->get_worker();

  if (!worker->get_graceful_shutdown()) {
    return 0;
  }

  ev_prepare_stop(handler_->get_loop(), &prep_);

  return start_graceful_shutdown();
}

int Http3Upstream::start_graceful_shutdown() {
  int rv;

  if (ev_is_active(&shutdown_timer_)) {
    return 0;
  }

  rv = nghttp3_conn_submit_shutdown_notice(httpconn_);
  if (rv != 0) {
    ULOG(FATAL, this) << "nghttp3_conn_submit_shutdown_notice: "
                      << nghttp3_strerror(rv);
    return -1;
  }

  handler_->signal_write();

  auto t = ngtcp2_conn_get_pto(conn_);

  ev_timer_set(&shutdown_timer_, static_cast<ev_tstamp>(t * 3) / NGTCP2_SECONDS,
               0.);
  ev_timer_start(handler_->get_loop(), &shutdown_timer_);

  return 0;
}

int Http3Upstream::submit_goaway() {
  int rv;

  rv = nghttp3_conn_shutdown(httpconn_);
  if (rv != 0) {
    ULOG(FATAL, this) << "nghttp3_conn_shutdown: " << nghttp3_strerror(rv);
    return -1;
  }

  handler_->signal_write();

  return 0;
}

void Http3Upstream::idle_close() { idle_close_ = true; }

int Http3Upstream::open_qlog_file(const StringRef &dir,
                                  const ngtcp2_cid &scid) const {
  std::array<char, sizeof("20141115T125824.741+0900")> buf;

  auto path = dir.str();
  path += '/';
  path +=
      util::format_iso8601_basic(buf.data(), std::chrono::system_clock::now());
  path += '-';
  path += util::format_hex(scid.data, scid.datalen);
  path += ".sqlog";

  int fd;

#ifdef O_CLOEXEC
  while ((fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                    S_IRUSR | S_IWUSR | S_IRGRP)) == -1 &&
         errno == EINTR)
    ;
#else  // !O_CLOEXEC
  while ((fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                    S_IRUSR | S_IWUSR | S_IRGRP)) == -1 &&
         errno == EINTR)
    ;

  if (fd != -1) {
    util::make_socket_closeonexec(fd);
  }
#endif // !O_CLOEXEC

  if (fd == -1) {
    auto error = errno;
    ULOG(ERROR, this) << "Failed to open qlog file " << path
                      << ": errno=" << error;
    return -1;
  }

  return fd;
}

} // namespace shrpx
