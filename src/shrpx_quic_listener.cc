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
#include "shrpx_quic_listener.h"
#include "shrpx_worker.h"
#include "shrpx_config.h"
#include "shrpx_log.h"

namespace shrpx {

namespace {
void readcb(struct ev_loop *loop, ev_io *w, int revent) {
  auto l = static_cast<QUICListener *>(w->data);
  l->on_read();
}
} // namespace

QUICListener::QUICListener(const UpstreamAddr *faddr, Worker *worker)
    : faddr_(faddr), worker_(worker) {
  ev_io_init(&rev_, readcb, faddr_->fd, EV_READ);
  rev_.data = this;
  ev_io_start(worker_->get_loop(), &rev_);
}

QUICListener::~QUICListener() {
  ev_io_stop(worker_->get_loop(), &rev_);
  close(faddr_->fd);
}

void QUICListener::on_read() {
  sockaddr_union su;
  std::array<uint8_t, 64_k> buf;
  size_t pktcnt = 0;
  iovec msg_iov{buf.data(), buf.size()};

  msghdr msg{};
  msg.msg_name = &su;
  msg.msg_iov = &msg_iov;
  msg.msg_iovlen = 1;

  uint8_t msg_ctrl[CMSG_SPACE(sizeof(in6_pktinfo))];
  msg.msg_control = msg_ctrl;

  for (; pktcnt < 10;) {
    msg.msg_namelen = sizeof(su);
    msg.msg_controllen = sizeof(msg_ctrl);

    auto nread = recvmsg(faddr_->fd, &msg, 0);
    if (nread == -1) {
      return;
    }

    ++pktcnt;

    Address local_addr{};
    if (util::msghdr_get_local_addr(local_addr, &msg, su.storage.ss_family) !=
        0) {
      continue;
    }

    util::set_port(local_addr, faddr_->port);

    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "QUIC received packet: local="
                << util::to_numeric_addr(&local_addr)
                << " remote=" << util::to_numeric_addr(&su.sa, msg.msg_namelen)
                << " " << nread << " bytes";
    }

    if (nread == 0) {
      continue;
    }
  }
}

} // namespace shrpx
