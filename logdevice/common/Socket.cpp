/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "Socket.h"

#include <algorithm>
#include <errno.h>
#include <memory>

#include <folly/Random.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "event2/bufferevent_ssl.h"
#include "event2/event.h"
#include "logdevice/common/AdminCommandTable.h"
#include "logdevice/common/BWAvailableCallback.h"
#include "logdevice/common/ConstructorFailed.h"
#include "logdevice/common/EventHandler.h"
#include "logdevice/common/FlowGroup.h"
#include "logdevice/common/LegacyPluginPack.h"
#include "logdevice/common/PrincipalParser.h"
#include "logdevice/common/Processor.h"
#include "logdevice/common/ResourceBudget.h"
#include "logdevice/common/SSLFetcher.h"
#include "logdevice/common/Sender.h"
#include "logdevice/common/SocketCallback.h"
#include "logdevice/common/TimeoutMap.h"
#include "logdevice/common/UpdateableSecurityInfo.h"
#include "logdevice/common/Worker.h"
#include "logdevice/common/configuration/Configuration.h"
#include "logdevice/common/debug.h"
#include "logdevice/common/libevent/compat.h"
#include "logdevice/common/protocol/ACK_Message.h"
#include "logdevice/common/protocol/Compatibility.h"
#include "logdevice/common/protocol/HELLO_Message.h"
#include "logdevice/common/protocol/Message.h"
#include "logdevice/common/protocol/MessageDeserializers.h"
#include "logdevice/common/protocol/MessageTypeNames.h"
#include "logdevice/common/protocol/ProtocolHeader.h"
#include "logdevice/common/protocol/ProtocolReader.h"
#include "logdevice/common/protocol/ProtocolWriter.h"
#include "logdevice/common/protocol/SHUTDOWN_Message.h"
#include "logdevice/common/settings/Settings.h"
#include "logdevice/common/stats/Histogram.h"
#include "logdevice/common/stats/Stats.h"
#include "logdevice/common/util.h"

#ifdef __linux__
#ifndef TCP_USER_TIMEOUT
#define TCP_USER_TIMEOUT 18
#endif
#endif

namespace facebook { namespace logdevice {
using folly::SSLContext;

const char* socketTypeToString(SocketType sock_type) {
  switch (sock_type) {
    case SocketType::DATA:
      return "DATA";
    case SocketType::GOSSIP:
      return "GOSSIP";
  }
  return "";
}

const char* connectionTypeToString(ConnectionType conn_type) {
  switch (conn_type) {
    case ConnectionType::NONE:
      return "NONE";
    case ConnectionType::PLAIN:
      return "PLAIN";
    case ConnectionType::SSL:
      return "SSL";
  }
  return "";
}

class SocketImpl {
 public:
  SocketImpl() {}

  // an intrusive list of callback functors to call when the socket closes
  folly::IntrusiveList<SocketCallback, &SocketCallback::listHook_> on_close_;

  // an intrusive list of the pending bandwidth available callbacks for
  // state machines waiting to run on this socket. These callbacks must
  // be cleaned up when the socket is closed.
  folly::IntrusiveList<BWAvailableCallback, &BWAvailableCallback::socket_links_>
      pending_bw_cbs_;
};

// Setting the env forces all sockets to be SSL-enabled. Aiming to load the
// env just once.
static bool forceSSLSockets() {
  static std::atomic<int> force_ssl{-1};
  int val = force_ssl.load();
  if (val == -1) {
    const char* env = getenv("LOGDEVICE_TEST_FORCE_SSL");
    // Return false for null, "" and "0", true otherwise.
    val = env != nullptr && strlen(env) > 0 && strcmp(env, "0") != 0;

    force_ssl.store(val);
  }
  return val;
}

Socket::Socket(std::unique_ptr<SocketDependencies>& deps,
               Address peer_name,
               const Sockaddr& peer_sockaddr,
               SocketType type,
               ConnectionType conntype,
               FlowGroup& flow_group)
    : peer_name_(peer_name),
      peer_sockaddr_(peer_sockaddr),
      flow_group_(flow_group),
      type_(type),
      ref_holder_(this),
      impl_(new SocketImpl),
      deps_(std::move(deps)),
      next_pos_(0),
      drain_pos_(0),
      bev_(nullptr),
      connected_(false),
      handshaken_(false),
      proto_(getSettings().max_protocol),
      our_name_at_peer_(ClientID::INVALID),
      connect_throttle_(this),
      outbuf_overflow_(getSettings().outbuf_overflow_kb * 1024),
      retries_so_far_(0),
      first_attempt_(true),
      tcp_sndbuf_cache_({128 * 1024, std::chrono::steady_clock::now()}),
      tcp_rcvbuf_size_(128 * 1024),
      close_reason_(E::UNKNOWN),
      num_messages_sent_(0),
      num_messages_received_(0),
      num_bytes_received_(0) {
  if ((conntype == ConnectionType::SSL) ||
      (forceSSLSockets() && type != SocketType::GOSSIP)) {
    conntype_ = ConnectionType::SSL;
  } else {
    conntype_ = ConnectionType::PLAIN;
  }

  if (!peer_sockaddr.valid()) {
    ld_check(!peer_name.isClientAddress());
    if (conntype_ == ConnectionType::SSL) {
      err = E::NOSSLCONFIG;
      RATELIMIT_ERROR(std::chrono::seconds(10),
                      2,
                      "Recipient %s is not configured for SSL connections.",
                      peer_name_.toString().c_str());
    } else {
      err = E::NOTINCONFIG;
      RATELIMIT_ERROR(std::chrono::seconds(10),
                      2,
                      "Invalid address for %s.",
                      peer_name_.toString().c_str());
    }
    throw ConstructorFailed();
  }

  int rv =
      deps_->evtimerAssign(&read_more_, EventHandler<readMoreCallback>, this);
  if (rv != 0) {
    err = E::INTERNAL;
    throw ConstructorFailed();
  }

  rv = deps_->evtimerAssign(&connect_timeout_event_,
                            EventHandler<connectAttemptTimeoutCallback>,
                            this);
  if (rv != 0) {
    err = E::INTERNAL;
    throw ConstructorFailed();
  }

  rv = deps_->evtimerAssign(
      &handshake_timeout_event_, EventHandler<handshakeTimeoutCallback>, this);
  if (rv != 0) {
    err = E::INTERNAL;
    throw ConstructorFailed();
  }

  rv = deps_->evtimerAssign(&deferred_event_queue_event_,
                            EventHandler<deferredEventQueueEventCallback>,
                            reinterpret_cast<void*>(this));

  if (rv != 0) {
    err = E::INTERNAL;
    throw ConstructorFailed();
  }

  rv = deps_->eventAssign(&end_stream_rewind_event_,
                          EventHandler<endStreamRewindCallback>,
                          reinterpret_cast<void*>(this));
  if (rv != 0) {
    err = E::INTERNAL;
    throw ConstructorFailed();
  }

  rv = deps_->eventPrioritySet(
      &end_stream_rewind_event_, EventLoop::PRIORITY_HIGH);
  if (rv != 0) {
    err = E::INTERNAL;
    throw ConstructorFailed();
  }

  rv = deps_->evtimerAssign(&buffered_output_flush_event_,
                            EventHandler<onBufferedOutputTimerEvent>,
                            reinterpret_cast<void*>(this));
  if (rv != 0) {
    err = E::INTERNAL;
    throw ConstructorFailed();
  }
}

Socket::Socket(NodeID server_name,
               SocketType sock_type,
               ConnectionType conntype,
               FlowGroup& flow_group,
               std::unique_ptr<SocketDependencies> deps)
    : Socket(deps,
             Address(server_name),
             deps->getNodeSockaddr(server_name, sock_type, conntype),
             sock_type,
             conntype,
             flow_group) {}

Socket::Socket(int fd,
               ClientID client_name,
               const Sockaddr& client_addr,
               ResourceBudget::Token conn_token,
               SocketType type,
               ConnectionType conntype,
               FlowGroup& flow_group,
               std::unique_ptr<SocketDependencies> deps)
    : Socket(deps,
             Address(client_name),
             client_addr,
             type,
             conntype,
             flow_group) {
  ld_check(fd >= 0);
  ld_check(client_name.valid());
  ld_check(client_addr.valid());

  // note that caller (Sender.addClient()) does not close(fd) on error.
  // If you add code here that throws ConstructorFailed you must close(fd)!

  bev_ = newBufferevent(fd,
                        client_addr.family(),
                        &tcp_sndbuf_cache_.size,
                        &tcp_rcvbuf_size_,
                        // This is only used if conntype_ == SSL, tells libevent
                        // we are in a server context
                        BUFFEREVENT_SSL_ACCEPTING);
  if (!bev_) {
    throw ConstructorFailed(); // err is already set
  }

  conn_incoming_token_ = std::move(conn_token);

  addHandshakeTimeoutEvent();
  expectProtocolHeader();

  if (isSSL()) {
    expecting_ssl_handshake_ = true;
  }
  connected_ = true;
  peer_shuttingdown_ = false;
  fd_ = fd;

  STAT_INCR(deps_->getStats(), num_connections);
  STAT_DECR(deps_->getStats(), num_backlog_connections);
  if (isSSL()) {
    STAT_INCR(deps_->getStats(), num_ssl_connections);
  }
}

void Socket::onBufferedOutputWrite(struct evbuffer* buffer,
                                   const struct evbuffer_cb_info* info,
                                   void* arg) {
  Socket* self = reinterpret_cast<Socket*>(arg);

  ld_check(self);
  ld_check(self->bev_);
  ld_check(self->buffered_output_);
  ld_check(buffer == self->buffered_output_);

  if (info->n_added) {
    self->deps_->evtimerAdd(
        &self->buffered_output_flush_event_, self->deps_->getZeroTimeout());
  }
}

void Socket::flushBufferedOutput() {
  ld_check(buffered_output_);
  ld_check(bev_);
  // Moving buffer chains into bev's output
  int rv = LD_EV(evbuffer_add_buffer)(deps_->getOutput(bev_), buffered_output_);
  if (rv != 0) {
    ld_error("evbuffer_add_buffer() failed. error %d", rv);
    err = E::NOMEM;
    close(E::NOMEM);
  }

  // the buffered_output_ size might not be 0 because of minor size limit
  // differences with the actual outbuf. We also have to check if we are still
  // connected here, because the socket might have been closed above, or if we
  // flushed the last bytes (see flushOutputandClose())
  if (connected_ && LD_EV(evbuffer_get_length)(buffered_output_) != 0) {
    deps_->evtimerAdd(&buffered_output_flush_event_, deps_->getZeroTimeout());
  }
}

void Socket::onBufferedOutputTimerEvent(void* instance, short) {
  auto self = reinterpret_cast<Socket*>(instance);
  ld_check(self);
  self->flushBufferedOutput();
}

Socket::~Socket() {
  ld_debug(
      "Destroying Socket %s", deps_->describeConnection(peer_name_).c_str());
  close(E::SHUTDOWN);
}

struct bufferevent* Socket::newBufferevent(int sfd,
                                           sa_family_t sa_family,
                                           size_t* sndbuf_size_out,
                                           size_t* rcvbuf_size_out,
                                           bufferevent_ssl_state ssl_state) {
  int rv;

  ld_check(sa_family == AF_INET || sa_family == AF_INET6 ||
           sa_family == AF_UNIX);

  if (sfd < 0) {
    sfd = socket(sa_family, SOCK_STREAM, 0);
    if (sfd < 0) {
      ld_error("socket() failed. errno=%d (%s)", errno, strerror(errno));
      switch (errno) {
        case EMFILE:
        case ENFILE:
          err = E::SYSLIMIT;
          break;
        case ENOBUFS:
        case ENOMEM:
          err = E::NOMEM;
          break;
        default:
          err = E::INTERNAL;
      }
      return nullptr;
    }
  }

  rv = deps_->evUtilMakeSocketNonBlocking(sfd);
  if (rv != 0) { // unlikely
    ld_error("Failed to make fd %d non-blocking. errno=%d (%s)",
             sfd,
             errno,
             strerror(errno));
    ::close(sfd);
    err = E::INTERNAL;
    return nullptr;
  }

  int tcp_sndbuf_size = 0, tcp_rcvbuf_size = 0;

  deps_->configureSocket(
      !peer_sockaddr_.isUnixAddress(), sfd, &tcp_sndbuf_size, &tcp_rcvbuf_size);

  if (isSSL()) {
    ld_check(!ssl_context_);
    ssl_context_ = deps_->getSSLContext(ssl_state, null_ciphers_only_);
  }

  struct bufferevent* bev = deps_->buffereventSocketNew(
      sfd, BEV_OPT_CLOSE_ON_FREE, isSSL(), ssl_state, ssl_context_.get());
  if (!bev) { // unlikely
    ld_error("bufferevent_socket_new() failed. errno=%d (%s)",
             errno,
             strerror(errno));
    err = E::NOMEM;
    ::close(sfd);
    return nullptr;
  }

  struct evbuffer* outbuf = deps_->getOutput(bev);
  ld_check(outbuf);

  struct evbuffer_cb_entry* outbuf_cbe = LD_EV(evbuffer_add_cb)(
      outbuf, &EvBufferEventHandler<Socket::bytesSentCallback>, (void*)this);

  if (!outbuf_cbe) { // unlikely
    ld_error("evbuffer_add_cb() failed. errno=%d (%s)", errno, strerror(errno));
    err = E::NOMEM;
    ::close(sfd);
    return nullptr;
  }

  // At this point, we are convinced the socket we are using is legit.
  fd_ = sfd;

  if (tcp_sndbuf_size > 0) {
    deps_->buffereventSetMaxSingleWrite(bev, tcp_sndbuf_size);
    if (sndbuf_size_out) {
      *sndbuf_size_out = tcp_sndbuf_size;
    }
  }

  if (tcp_rcvbuf_size > 0) {
    deps_->buffereventSetMaxSingleRead(bev, tcp_rcvbuf_size);
    if (rcvbuf_size_out) {
      *rcvbuf_size_out = tcp_rcvbuf_size;
    }
  }

  deps_->buffereventSetCb(bev,
                          BufferEventHandler<Socket::dataReadCallback>,
                          nullptr,
                          BufferEventHandler<Socket::eventCallback>,
                          (void*)this);

  if (isSSL()) {
    // The buffer may already exist if we're making another attempt at a
    // connection
    if (!buffered_output_) {
      // creating an evbuffer that would batch up SSL writes
      buffered_output_ = LD_EV(evbuffer_new)();
      LD_EV(evbuffer_add_cb)
      (buffered_output_,
       &EvBufferEventHandler<Socket::onBufferedOutputWrite>,
       (void*)this);
    }
  } else {
    buffered_output_ = nullptr;
  }

  deps_->buffereventEnable(bev, EV_READ | EV_WRITE);

  return bev;
}

int Socket::connect() {
  if (peer_name_.isClientAddress()) {
    if (bev_) {
      ld_check(connected_);
      err = E::ISCONN;
    } else {
      err = E::UNREACHABLE;
    }
    return -1;
  }

  // it's a server socket

  if (bev_) {
    err = connected_ ? E::ISCONN : E::ALREADY;
    return -1;
  }

  // it's an unconnected server socket

  ld_check(!connected_);
  ld_check(pendingq_.empty());
  ld_check(serializeq_.empty());
  ld_check(sendq_.empty());
  ld_check(getBytesPending() == 0);

  if (!connect_throttle_.mayConnect()) {
    err = E::DISABLED;
    return -1;
  }

  retries_so_far_ = 0;

  int rv = doConnectAttempt();
  if (rv != 0) {
    return -1; // err set by doConnectAttempt
  }
  if (isSSL()) {
    ld_check(bev_);
    ld_assert(bufferevent_get_openssl_error(bev_) == 0);
  }

  next_pos_ = 0;
  drain_pos_ = 0;

  sendHello(); // queue up HELLO, to be sent when we connect

  STAT_INCR(deps_->getStats(), num_connections);
  if (isSSL()) {
    STAT_INCR(deps_->getStats(), num_ssl_connections);
  }

  RATELIMIT_DEBUG(std::chrono::seconds(1),
                  10,
                  "Connected %s socket via %s channel to %s",
                  getSockType() == SocketType::DATA ? "DATA" : "GOSSIP",
                  getConnType() == ConnectionType::SSL ? "SSL" : "PLAIN",
                  peerSockaddr().toString().c_str());

  return 0;
}

int Socket::doConnectAttempt() {
  ld_check(!connected_);
  ld_check(!bev_);
  bev_ = newBufferevent(-1,
                        peer_sockaddr_.family(),
                        &tcp_sndbuf_cache_.size,
                        &tcp_rcvbuf_size_,
                        // This is only used if conntype_ == SSL, tells libevent
                        // we are in a client context
                        BUFFEREVENT_SSL_CONNECTING);

  if (!bev_) {
    return -1; // err is already set
  }

  expectProtocolHeader();

  struct sockaddr_storage ss;
  int len = peer_sockaddr_.toStructSockaddr(&ss);
  if (len == -1) {
    // This can only fail if node->address is an invalid Sockaddr.  Since the
    // address comes from Configuration, it must have been validated already.
    err = E::INTERNAL;
    ld_check(false);
    return -1;
  }
  const int rv = deps_->buffereventSocketConnect(
      bev_, reinterpret_cast<struct sockaddr*>(&ss), len);

  if (rv != 0) {
    if (isSSL() && bev_) {
      unsigned long ssl_err = 0;
      char ssl_err_string[120];
      while ((ssl_err = bufferevent_get_openssl_error(bev_))) {
        ERR_error_string_n(ssl_err, ssl_err_string, sizeof(ssl_err_string));
        RATELIMIT_ERROR(
            std::chrono::seconds(10), 10, "SSL error: %s", ssl_err_string);
      }
    }

    ld_error("Failed to initiate connection to %s errno=%d (%s)",
             deps_->describeConnection(peer_name_).c_str(),
             errno,
             strerror(errno));
    switch (errno) {
      case ENOMEM:
        err = E::NOMEM;
        break;
      case ENETUNREACH:
      case ENOENT: // for unix domain sockets.
        err = E::UNROUTABLE;
        break;
      case EAGAIN:
        err = E::SYSLIMIT; // out of ephemeral ports
        break;
      case ECONNREFUSED: // TODO: verify
      case EADDRINUSE:   // TODO: verify
      default:
        // Linux does not report ECONNREFUSED for non-blocking TCP
        // sockets even if connecting over loopback. Other errno values
        // can only be explained by an internal error such as memory
        // corruption or a bug in libevent.
        err = E::INTERNAL;
        break;
    }
    return -1;
  }
  if (isSSL()) {
    ld_assert(bufferevent_get_openssl_error(bev_) == 0);
  }

  // Start a timer for this connection attempt. When the timer triggers, this
  // function will be called again until we reach the maximum amount of
  // connection retries.
  addConnectAttemptTimeoutEvent();
  return 0;
}

size_t Socket::getTotalOutbufLength() {
  auto pending_bytes = LD_EV(evbuffer_get_length)(deps_->getOutput(bev_));
  if (buffered_output_) {
    pending_bytes += LD_EV(evbuffer_get_length)(buffered_output_);
  }
  return pending_bytes;
}

void Socket::onOutputEmpty(struct bufferevent*, void* arg, short) {
  Socket* self = reinterpret_cast<Socket*>(arg);
  ld_check(self);
  // Write watermark has been set to zero so the output buffer should be
  // empty when this callback gets called, but we could still have bytes
  // pending in buffered_output_
  auto pending_bytes = self->getTotalOutbufLength();
  if (pending_bytes == 0) {
    self->close(self->close_reason_);
  } else {
    ld_info("Not closing socket because %lu bytes are still pending",
            pending_bytes);
  }
}

void Socket::flushOutputAndClose(Status reason) {
  auto pending_bytes = getTotalOutbufLength();

  if (pending_bytes == 0) {
    close(reason);
    return;
  }

  ld_spew("Flushing %lu bytes of output before closing connection to %s",
          pending_bytes,
          deps_->describeConnection(peer_name_).c_str());

  close_reason_ = reason;

  // - Remove the read callback as we are not processing any more message
  //   since we are about to close the connection.
  // - Set up the write callback and the low write watermark to 0 so that the
  //   callback will be called when the output buffer is flushed and will close
  //   the connection using close_reason_ for the error code.
  deps_->buffereventSetWatermark(bev_, EV_WRITE, 0, 0);
  deps_->buffereventSetCb(bev_,
                          nullptr,
                          BufferEventHandler<Socket::onOutputEmpty>,
                          BufferEventHandler<Socket::eventCallback>,
                          (void*)this);
}

void Socket::onBytesAvailable(bool fresh) {
  // process up to this many messages
  unsigned process_max = getSettings().incoming_messages_max_per_socket;

  size_t available = LD_EV(evbuffer_get_length)(deps_->getInput(bev_));

  // if this function was called by bev_ in response to "input buffer
  // length is above low watermark" event, we must have at least as
  // many bytes available as Socket is expecting. Otherwise the
  // function was called by read_more_ event, which may run after
  // "bev_ is readable" if TCP socket becomes readable after
  // read_more_ was activated. If that happens this callback may find
  // fewer bytes in bev_'s input buffer than dataReadCallback() expects.
  ld_assert(!fresh || available >= bytesExpected());

  for (unsigned i = 0;; i++) {
    if (available >= bytesExpected()) {
      if (i / 2 < process_max) {
        // it's i/2 because we need 2 calls to receiveMessage() to consume
        // a message: one for the protocol header, the other for message body
        int rv = receiveMessage();
        if (rv != 0) {
          if (!peer_name_.isClientAddress()) {
            RATELIMIT_ERROR(std::chrono::seconds(10),
                            10,
                            "receiveMessage() failed with %s from %s.",
                            error_name(err),
                            deps_->describeConnection(peer_name_).c_str());
            connect_throttle_.connectFailed();
          }
          if ((err == E::PROTONOSUPPORT || err == E::INVALID_CLUSTER ||
               err == E::ACCESS || err == E::DESTINATION_MISMATCH) &&
              isHELLOMessage(recv_message_ph_.type)) {
            // Make sure the ACK message with E::PROTONOSUPPORT, E::ACCESS,
            // E::DESTINATION_MISMATCH or E::INVALID_CLUSTER error is sent to
            // the client before the socket is closed.
            flushOutputAndClose(err);
          } else {
            close(err);
          }
          break;
        }
      } else {
        // We reached the limit of how many messages we are allowed to
        // process before returning control to libevent. schedule
        // read_more_ to fire in the next iteration of event loop and
        // return control to libevent so that we can run other events
        deps_->evtimerAdd(&read_more_, deps_->getZeroTimeout());
        break;
      }
    } else {
      deps_->evtimerDel(&read_more_);
      break;
    }

    ld_check(!isClosed());
    available = LD_EV(evbuffer_get_length)(deps_->getInput(bev_));
  }
}

void Socket::dataReadCallback(struct bufferevent* bev, void* arg, short) {
  Socket* self = reinterpret_cast<Socket*>(arg);

  ld_check(self);
  ld_check(bev == self->bev_);

  self->onBytesAvailable(/*fresh=*/true);
}

void Socket::readMoreCallback(void* arg, short what) {
  Socket* self = reinterpret_cast<Socket*>(arg);
  ld_check(what & EV_TIMEOUT);
  ld_check(self->bev_);
  ld_spew("Socket %s remains above low watermark",
          self->deps_->describeConnection(self->peer_name_).c_str());
  self->onBytesAvailable(/*fresh=*/false);
}

size_t Socket::bytesExpected() {
  size_t protohdr_bytes =
      ProtocolHeader::bytesNeeded(recv_message_ph_.type, proto_);

  if (expectingProtocolHeader()) {
    return protohdr_bytes;
  } else {
    return recv_message_ph_.len - protohdr_bytes;
  }
}

void Socket::eventCallback(struct bufferevent* bev, void* arg, short what) {
  Socket* self = reinterpret_cast<Socket*>(arg);

  ld_check(self);
  ld_check(bev == self->bev_);

  SocketEvent e{what, errno};

  if (self->isSSL() && !(e.what & BEV_EVENT_CONNECTED)) {
    // libevent's SSL handlers will call this before calling the
    // bytesSentCallback(), which breaks assumptions in our code. To avoid
    // that, we place the callback on a queue instead of calling it
    // immediately.

    // Not deferring onConnected(), as otherwise onConnectAttemptTimeout()
    // might be triggered after the connection has been established (and the
    // BEV_EVENT_CONNECTED processed), but before onConnected() callback is
    // hit.
    self->enqueueDeferredEvent(e);
  } else {
    self->eventCallbackImpl(e);
  }
}

void Socket::eventCallbackImpl(SocketEvent e) {
  if (e.what & BEV_EVENT_CONNECTED) {
    onConnected();
  } else if (e.what & BEV_EVENT_ERROR) {
    onError(e.what & (BEV_EVENT_READING | BEV_EVENT_WRITING), e.socket_errno);
  } else if (e.what & BEV_EVENT_EOF) {
    onPeerClosed();
  } else {
    // BEV_EVENT_TIMEOUT must not be reported yet
    ld_critical("INTERNAL ERROR: unexpected event bitset in a bufferevent "
                "callback: 0x%hx",
                e.what);
    ld_check(0);
  }
}

void Socket::flushNextInSerializeQueue() {
  ld_check(!serializeq_.empty());

  std::unique_ptr<Envelope> next_envelope(&serializeq_.front());
  serializeq_.pop_front();
  send(std::move(next_envelope));
}

void Socket::flushSerializeQueue() {
  while (!serializeq_.empty()) {
    flushNextInSerializeQueue();
  }
}

void Socket::onConnected() {
  ld_check(bev_);
  if (expecting_ssl_handshake_) {
    ld_check(connected_);
    // we receive a BEV_EVENT_CONNECTED for an _incoming_ connection after the
    // handshake is done.
    ld_check(isSSL());
    ld_debug("SSL handshake with %s completed",
             deps_->describeConnection(peer_name_).c_str());
    expecting_ssl_handshake_ = false;
    expectProtocolHeader();
    return;
  }
  ld_check(!connected_);
  ld_check(!peer_name_.isClientAddress());

  deps_->evtimerDel(&connect_timeout_event_);
  addHandshakeTimeoutEvent();
  connected_ = true;
  peer_shuttingdown_ = false;

  ld_debug("Socket(%p) to node %s has connected",
           this,
           deps_->describeConnection(peer_name_).c_str());

  // Send the first message enqueued in serializeq_, which should be a HELLO
  // message. The rest of the content of the queue will be serialized only once
  // we are handshaken and we know the protocol that will be used for
  // communicating with the other end.
  ld_check(!serializeq_.empty());
  auto& first_envelope = serializeq_.front();
  ld_check(dynamic_cast<const HELLO_Message*>(&first_envelope.message()));
  flushNextInSerializeQueue();
}

void Socket::onSent(std::unique_ptr<Envelope> e,
                    Status reason,
                    Message::CompletionMethod cm) {
  // Do not call onSent() of pending messages if our Worker is getting
  // destroyed. This is to guarantee that onSent() code and the methods
  // it calls do not try to access a partially destroyed Worker, with some
  // members already destroyed and free'd.
  ld_check(!e->socket_links_.is_linked());

  if (reason == Status::OK) {
    FLOW_GROUP_MSG_STAT_INCR(
        Worker::stats(), flow_group_, &e->message(), sent_ok);
    FLOW_GROUP_MSG_STAT_ADD(
        Worker::stats(), flow_group_, &e->message(), sent_bytes, e->cost());
  } else {
    FLOW_GROUP_MSG_STAT_INCR(
        Worker::stats(), flow_group_, &e->message(), sent_error);
  }

  if (!deps_->shuttingDown()) {
    deps_->noteBytesDrained(e->cost());
    deps_->onSent(e->moveMessage(), peer_name_, reason, e->birthTime(), cm);
    ld_check(!e->haveMessage());
  }
}

void Socket::onError(short direction, int socket_errno) {
  if (!bev_) {
    ld_critical("INTERNAL ERROR: got a libevent error on disconnected socket "
                "with peer %s. errno=%d (%s)",
                deps_->describeConnection(peer_name_).c_str(),
                socket_errno,
                strerror(socket_errno));
    ld_check(0);
    return;
  }

  if (isSSL()) {
    unsigned long ssl_err = 0;
    char ssl_err_string[120];
    while ((ssl_err = bufferevent_get_openssl_error(bev_))) {
      ERR_error_string_n(ssl_err, ssl_err_string, sizeof(ssl_err_string));
      RATELIMIT_ERROR(
          std::chrono::seconds(10), 10, "SSL error: %s", ssl_err_string);
    }
  }

  if (connected_) {
    const bool severe = socket_errno != ECONNRESET && socket_errno != ETIMEDOUT;
    ld_log(severe ? facebook::logdevice::dbg::Level::ERROR
                  : facebook::logdevice::dbg::Level::WARNING,
           "Got an error on socket connected to %s while %s. "
           "errno=%d (%s)",
           deps_->describeConnection(peer_name_).c_str(),
           (direction & BEV_EVENT_WRITING) ? "writing" : "reading",
           socket_errno,
           strerror(socket_errno));
  } else {
    ld_check(!peer_name_.isClientAddress());
    connect_throttle_.connectFailed();
    RATELIMIT_LEVEL(
        errno == ECONNREFUSED ? dbg::Level::DEBUG : dbg::Level::WARNING,
        std::chrono::seconds(10),
        10,
        "Failed to connect to node %s. errno=%d (%s)",
        deps_->describeConnection(peer_name_).c_str(),
        socket_errno,
        strerror(socket_errno));
  }

  close(E::CONNFAILED);
}

void Socket::onPeerClosed() {
  ld_spew("Peer %s closed.", deps_->describeConnection(peer_name_).c_str());
  ld_check(bev_);
  if (!isSSL()) {
    // an SSL socket can be in a state where the TCP connection is established,
    // but the SSL handshake hasn't finished, this isn't considered connected.
    ld_check(connected_);
  }

  Status reason = E::PEER_CLOSED;

  if (!peer_name_.isClientAddress()) {
    connect_throttle_.connectFailed();
    if (peer_shuttingdown_) {
      reason = E::SHUTDOWN;
    }
  }

  close(reason);
}

void Socket::onConnectTimeout() {
  ld_spew("Connection timeout connecting to %s",
          deps_->describeConnection(peer_name_).c_str());
  if (!peer_name_.isClientAddress()) {
    connect_throttle_.connectFailed();
  }

  close(E::TIMEDOUT);
}

void Socket::onHandshakeTimeout() {
  ld_warning("Handshake timeout occurred (peer: %s).",
             deps_->describeConnection(peer_name_).c_str());
  onConnectTimeout();
  STAT_INCR(deps_->getStats(), handshake_timeouts);
}

void Socket::onConnectAttemptTimeout() {
  ld_check(!connected_);

  RATELIMIT_DEBUG(std::chrono::seconds(5),
                  5,
                  "Connection timeout occurred (peer: %s). Attempt %lu.",
                  deps_->describeConnection(peer_name_).c_str(),
                  retries_so_far_);
  ld_check(!connected_);
  if (retries_so_far_ >= getSettings().connection_retries) {
    onConnectTimeout();
    STAT_INCR(deps_->getStats(), connection_timeouts);
  } else {
    // Nothing should be written in the output buffer of an unconnected socket.
    ld_check(getTotalOutbufLength() == 0);
    deps_->buffereventFree(bev_); // this also closes the TCP socket
    bev_ = nullptr;
    ssl_context_.reset();

    // Try connecting again.
    if (doConnectAttempt() != 0) {
      ld_warning("Connect attempt #%lu failed (peer:%s), err=%s",
                 retries_so_far_ + 1,
                 deps_->describeConnection(peer_name_).c_str(),
                 strerror(errno));
      onConnectTimeout();
    } else {
      STAT_INCR(deps_->getStats(), connection_retries);
      ++retries_so_far_;
    }
  }
}

void Socket::setDSCP(uint8_t dscp) {
  int rc = 0;

  switch (peer_sockaddr_.family()) {
    case AF_INET: {
      int ip_tos = dscp << 2;
      rc = setsockopt(fd_, IPPROTO_IP, IP_TOS, &ip_tos, sizeof(ip_tos));
      break;
    }
    case AF_INET6: {
      int tclass = dscp << 2;
      rc = setsockopt(fd_, IPPROTO_IPV6, IPV6_TCLASS, &tclass, sizeof(tclass));
      break;
    }
    default:
      break;
  }

  // DSCP is used for external traffic shaping. Allow the connection to
  // continue to operate, but warn about the failure.
  if (rc != 0) {
    RATELIMIT_WARNING(std::chrono::seconds(1),
                      10,
                      "DSCP(0x%x) configuration failed: %s",
                      dscp,
                      strerror(errno));
  }
}

void Socket::close(Status reason) {
  // Checking and setting this here to prevent recursive closes
  if (closing_) {
    return;
  }
  closing_ = true;
  SCOPE_EXIT {
    closing_ = false;
  };

  if (!bev_) {
    ld_check(!connected_);
    return;
  }

  RATELIMIT_LEVEL((reason == E::CONNFAILED || reason == E::TIMEDOUT)
                      ? dbg::Level::DEBUG
                      : dbg::Level::INFO,
                  std::chrono::seconds(10),
                  10,
                  "Closing socket %s. Reason: %s",
                  deps_->describeConnection(peer_name_).c_str(),
                  error_description(reason));

  if (getBytesPending() > 0) {
    ld_debug("Socket %s had %zu bytes pending when closed.",
             deps_->describeConnection(peer_name_).c_str(),
             getBytesPending());

    ld_debug("Worker #%i now has %zu total bytes pending",
             int(deps_->getWorkerId()),
             deps_->getBytesPending() - getBytesPending());
  }

  endStreamRewind();

  if (!deferred_event_queue_.empty()) {
    // Process outstanding deferred events since they may inform us that
    // connection throttling is appropriate against future connections. But
    // if we are shutting down and won't be accepting new connections, don't
    // bother.
    if (!deps_->shuttingDown()) {
      processDeferredEventQueue();
    } else {
      deferred_event_queue_.clear();
    }
  }

  if (buffered_output_) {
    deps_->evtimerDel(&buffered_output_flush_event_);
    LD_EV(evbuffer_free)(buffered_output_);
    buffered_output_ = nullptr;
  }

  if (isSSL()) {
    deps_->buffereventShutDownSSL(bev_);
  }

  if (!deps_->shuttingDown()) {
    deps_->noteBytesDrained(LD_EV(evbuffer_get_length)(deps_->getOutput(bev_)));
  }
  deps_->buffereventFree(bev_); // this also closes the TCP socket
  bev_ = nullptr;

  // socket was just closed; make sure it's properly accounted for
  conn_incoming_token_.release();
  conn_external_token_.release();

  our_name_at_peer_ = ClientID::INVALID;
  connected_ = false;
  handshaken_ = false;
  ssl_context_.reset();
  peer_config_version_ = config_version_t(0);

  STAT_DECR(deps_->getStats(), num_connections);
  if (isSSL()) {
    STAT_DECR(deps_->getStats(), num_ssl_connections);
  }

  deps_->evtimerDel(&read_more_);
  deps_->evtimerDel(&connect_timeout_event_);
  deps_->evtimerDel(&handshake_timeout_event_);
  deps_->evtimerDel(&deferred_event_queue_event_);
  deps_->eventDel(&end_stream_rewind_event_);

  // Move everything here so that this Socket object has a clean state
  // before we call any callback.
  PendingQueue moved_pendingq = std::move(pendingq_);
  std::vector<EnvelopeQueue> moved_queues;
  moved_queues.emplace_back(std::move(serializeq_));
  moved_queues.emplace_back(std::move(sendq_));
  folly::IntrusiveList<SocketCallback, &SocketCallback::listHook_>
      on_close_moved = std::move(impl_->on_close_);
  folly::IntrusiveList<BWAvailableCallback, &BWAvailableCallback::socket_links_>
      pending_bw_cbs_moved = std::move(impl_->pending_bw_cbs_);

  ld_check(pendingq_.empty());
  ld_check(serializeq_.empty());
  ld_check(sendq_.empty());
  ld_check(impl_->on_close_.empty());
  ld_check(impl_->pending_bw_cbs_.empty());
  ld_check(deferred_event_queue_.empty());

  trim(moved_pendingq, Priority::MAX, reason, moved_pendingq.cost());
  for (auto& queue : moved_queues) {
    while (!queue.empty()) {
      std::unique_ptr<Envelope> e(&queue.front());
      queue.pop_front();
      onSent(std::move(e), reason);
    }
  }

  // Clients expect all outstanding messages to be completed prior to
  // delivering "on close" callbacks.
  if (!deps_->shuttingDown()) {
    deps_->processDeferredMessageCompletions();
  }

  while (!pending_bw_cbs_moved.empty()) {
    auto& cb = pending_bw_cbs_moved.front();
    cb.deactivate();
    cb.cancelled(reason);
  }

  while (!on_close_moved.empty()) {
    auto& cb = on_close_moved.front();
    on_close_moved.pop_front();

    // on_close_ is an intrusive list, pop_front() removes cb from list but
    // does not call any destructors. cb is now not on any callback lists.
    cb(reason, peer_name_);
  }
}

bool Socket::isClosed() const {
  if (bev_) {
    return false;
  }

  ld_check(!connected_);
  ld_check(sendq_.empty());
  ld_check(serializeq_.empty());
  ld_check(getBytesPending() == 0);
  return true;
}

bool Socket::sizeLimitsExceeded() const {
  return deps_->bytesPendingLimitReached() ||
      (getBytesPending() > outbuf_overflow_);
}

bool Socket::isChecksummingEnabled(MessageType msgtype) {
  if (!getSettings().checksumming_enabled) {
    return false;
  }

  auto& msg_checksum_set = getSettings().checksumming_blacklisted_messages;
  return msg_checksum_set.find((char)msgtype) == msg_checksum_set.end();
}

int Socket::serializeMessageWithoutChecksum(const Message& msg,
                                            size_t msglen,
                                            struct evbuffer* outbuf) {
  ProtocolHeader protohdr;
  protohdr.len = msglen;
  protohdr.type = msg.type_;
  size_t protohdr_bytes = ProtocolHeader::bytesNeeded(msg.type_, proto_);

  if (LD_EV(evbuffer_add)(outbuf, &protohdr, protohdr_bytes) != 0) {
    // unlikely
    ld_critical("INTERNAL ERROR: failed to add message length and type "
                "to output evbuffer");
    ld_check(0);
    close(E::INTERNAL);
    err = E::INTERNAL;
    return -1;
  }

  ProtocolWriter writer(msg.type_, outbuf, proto_);
  msg.serialize(writer);
  ssize_t bodylen = writer.result();
  if (bodylen <= 0) { // unlikely
    ld_critical("INTERNAL ERROR: failed to serialize a message of "
                "type %s into an evbuffer",
                messageTypeNames[msg.type_].c_str());
    ld_check(0);
    close(E::INTERNAL);
    err = E::INTERNAL;
    return -1;
  }

  ld_check(bodylen + protohdr_bytes == protohdr.len);
  return 0;
}

// Serialization steps:
//  1. ProtocolWriter allocates a temp evbuffer and uses that in its
//     constructor
//  2. message.serialize() writes serialized version of the message
//     to the ProtocolWriter, which internally writes to temp evbuffer.
//  3. Checksum is computed on temp evbuffer and a ProtocolHeader
//     is generated. This ProtocolHeader is then written into output evbuffer.
//  4. Move temp evbuffer contents into output evbuffer (after the
//     ProtocolHeader)
int Socket::serializeMessageWithChecksum(const Message& msg,
                                         size_t msglen,
                                         struct evbuffer* outbuf) {
  struct evbuffer* cksum_evbuf = nullptr;
  SCOPE_EXIT {
    if (cksum_evbuf) {
      LD_EV(evbuffer_free)(cksum_evbuf);
      cksum_evbuf = nullptr;
    }
  };

  ProtocolHeader protohdr;
  protohdr.len = msglen;
  protohdr.type = msg.type_;
  cksum_evbuf = LD_EV(evbuffer_new)();

  // 1. Serialize the message into checksum buffer
  ProtocolWriter writer(msg.type_, cksum_evbuf, proto_);
  msg.serialize(writer);
  ssize_t bodylen = writer.result();
  if (bodylen <= 0) { // unlikely
    RATELIMIT_CRITICAL(std::chrono::seconds(1),
                       2,
                       "INTERNAL ERROR: Failed to serialize a message of "
                       "type %s into evbuffer",
                       messageTypeNames[msg.type_].c_str());
    ld_check(0);
    err = E::INTERNAL;
    close(err);
    return -1;
  }

  // 2. Compute checksum
  protohdr.cksum = writer.computeChecksum();
  /* For Tests only */
  if (shouldTamperChecksum()) {
    protohdr.cksum += 1;
  }

  // 3. Add proto header to evbuffer
  size_t protohdr_bytes = ProtocolHeader::bytesNeeded(msg.type_, proto_);
  if (LD_EV(evbuffer_add)(outbuf, &protohdr, protohdr_bytes) != 0) {
    // unlikely
    RATELIMIT_CRITICAL(
        std::chrono::seconds(1),
        2,
        "INTERNAL ERROR: Failed to add ProtocolHeader to output evbuffer");
    ld_check(0);
    err = E::INTERNAL;
    close(err);
    return -1;
  }

  // 4. Move the serialized message from cksum buffer to outbuf
  if (outbuf && cksum_evbuf) {
    // This moves all data b/w evbuffers without memory copy
    int rv = LD_EV(evbuffer_add_buffer)(outbuf, cksum_evbuf);
    ld_check(rv == 0);
  }

  ld_check(bodylen + protohdr_bytes == protohdr.len);
  return 0;
}

int Socket::serializeMessage(std::unique_ptr<Envelope>&& envelope,
                             size_t msglen) {
  // We should only write to the output buffer once connected.
  ld_check(connected_);

  const auto& msg = envelope->message();
  struct evbuffer* outbuf =
      buffered_output_ ? buffered_output_ : deps_->getOutput(bev_);
  ld_check(outbuf);

  bool compute_checksum = !buffered_output_ &&
      ProtocolHeader::needChecksumInHeader(msg.type_, proto_) &&
      isChecksummingEnabled(msg.type_);

  int rv = 0;
  if (compute_checksum) {
    rv = serializeMessageWithChecksum(msg, msglen, outbuf);
  } else {
    rv = serializeMessageWithoutChecksum(msg, msglen, outbuf);
  }

  if (rv != 0) {
    return rv;
  }

  MESSAGE_TYPE_STAT_INCR(deps_->getStats(), msg.type_, message_sent);
  TRAFFIC_CLASS_STAT_INCR(deps_->getStats(), msg.tc_, messages_sent);
  TRAFFIC_CLASS_STAT_ADD(deps_->getStats(), msg.tc_, bytes_sent, msglen);

  ld_check(!isHandshakeMessage(msg.type_) || next_pos_ == 0);
  ld_check(next_pos_ >= drain_pos_);

  next_pos_ += msglen;
  envelope->setDrainPos(next_pos_);

  sendq_.push_back(*envelope.release());
  ld_check(!envelope);

  deps_->noteBytesQueued(msglen);
  return 0;
}

bool Socket::injectAsyncMessageError(std::unique_ptr<Envelope>&& e) {
  auto error_chance_percent =
      getSettings().message_error_injection_chance_percent;
  auto error_status = getSettings().message_error_injection_status;
  if (error_chance_percent != 0 &&
      error_status != E::CBREGISTERED && // Must be synchronously delivered
      !isHandshakeMessage(e->message().type_) && !closing_ &&
      !message_error_injection_rewinding_stream_) {
    if (folly::Random::randDouble(0, 100.0) <= error_chance_percent) {
      message_error_injection_rewinding_stream_ = true;
      // Turn off the rewind when the deferred event queue is drained.
      // Ensure this happens even if no other deferred events are added
      // for this socket during the current event loop cycle.
      deps_->eventActive(&end_stream_rewind_event_, EV_WRITE, 0);
      ld_error("Rewinding Stream on Socket (%p) - %jd passed, %01.8f%% chance",
               this,
               (intmax_t)message_error_injection_pass_count_,
               error_chance_percent);
      message_error_injection_pass_count_ = 0;
    }
  }

  if (message_error_injection_rewinding_stream_) {
    message_error_injection_rewound_count_++;
    onSent(std::move(e), error_status, Message::CompletionMethod::DEFERRED);
    return true;
  }

  message_error_injection_pass_count_++;
  return false;
}

int Socket::preSendCheck(const Message& msg) {
  if (!bev_) {
    err = E::NOTCONN;
    return -1;
  }

  if (!handshaken_) {
    if (peer_name_.isClientAddress() && !isACKMessage(msg.type_)) {
      RATELIMIT_ERROR(std::chrono::seconds(1),
                      10,
                      "attempt to send a message of type %s to client %s "
                      "before handshake was completed",
                      messageTypeNames[msg.type_].c_str(),
                      deps_->describeConnection(peer_name_).c_str());
      err = E::UNREACHABLE;
      return -1;
    }
  } else if (msg.getMinProtocolVersion() > proto_) {
    if (msg.warnAboutOldProtocol()) {
      RATELIMIT_WARNING(
          std::chrono::seconds(1),
          10,
          "Could not serialize message of type %s to Socket %s "
          "because messages expects a protocol version >= %hu but "
          "the protocol used for that socket is %hu",
          messageTypeNames[msg.type_].c_str(),
          deps_->describeConnection(peer_name_).c_str(),
          msg.getMinProtocolVersion(),
          proto_);
    }

    if (isHandshakeMessage(msg.type_)) {
      ld_critical("INTERNAL ERROR: getMinProtocolVersion() is expected to "
                  "return a protocol version <= %hu for a message of type %s,"
                  " but it returns %hu instead.",
                  proto_,
                  messageTypeNames[msg.type_].c_str(),
                  msg.getMinProtocolVersion());
      close(E::INTERNAL);
      err = E::INTERNAL;
      ld_check(0);
    }

    err = E::PROTONOSUPPORT;
    return -1;
  }

  return 0;
}

void Socket::send(std::unique_ptr<Envelope> envelope) {
  const auto& msg = envelope->message();

  if (preSendCheck(msg)) {
    onSent(std::move(envelope), err);
    return;
  }

  if (msg.cancelled()) {
    onSent(std::move(envelope), E::CANCELLED);
    return;
  }

  // If we are handshaken, serialize the message directly to the output buffer.
  // Otherwise, push the message to the serializeq_ queue, it will be serialized
  // once we are handshaken. An exception is handshake messages, they can be
  // serialized as soon as we are connected.
  if (handshaken_ || (connected_ && isHandshakeMessage(msg.type_))) {
    // compute the message length only when 1) handshaken is completed and
    // negotiaged proto_ is known; or 2) message is a handshaken message
    // therefore its size does not depend on the protocol
    const auto msglen = msg.size(proto_);
    if (msglen > Message::MAX_LEN + sizeof(ProtocolHeader)) {
      RATELIMIT_ERROR(
          std::chrono::seconds(10),
          2,
          "Tried to send a message that's too long (%lu bytes) to %s",
          (size_t)msglen,
          deps_->describeConnection(peer_name_).c_str());
      err = E::TOOBIG;
      onSent(std::move(envelope), err);
      return;
    }

    // Offer up the message for error injection first. If the message
    // is accepted for injected error delivery, our responsibility for
    // sending the message ends.
    if (injectAsyncMessageError(std::move(envelope))) {
      return;
    }

    if (serializeMessage(std::move(envelope), msglen) != 0) {
      ld_check(err == E::INTERNAL || err == E::PROTONOSUPPORT);
      onSent(std::move(envelope), err);
      return;
    }
  } else {
    serializeq_.push_back(*envelope.release());
  }
}

bool Socket::trim(PendingQueue& pq,
                  Priority max_trim_priority,
                  Status reason,
                  size_t to_cut) {
  return pq.trim(max_trim_priority, to_cut, [&](Envelope& e_ref) {
    std::unique_ptr<Envelope> e(&e_ref);
    onSent(std::move(e), reason, Message::CompletionMethod::DEFERRED);
  });
}

Envelope* Socket::registerMessage(std::unique_ptr<Message>&& msg) {
  if (preSendCheck(*msg) != 0) {
    return nullptr;
  }

  // MessageType::HELLO and ::ACK are excluded from these limits because
  // we want to be able to establish connections even if we are out of
  // buffer space for messages. HELLO and ACK are a part of connection
  // establishment.
  if (!isHandshakeMessage(msg->type_) && sizeLimitsExceeded()) {
    RATELIMIT_WARNING(
        std::chrono::seconds(1),
        10,
        "ENOBUFS for Socket %s. Current socket usage: %zu, max: %zu",
        deps_->describeConnection(peer_name_).c_str(),
        getBytesPending(),
        outbuf_overflow_);

    RATELIMIT_INFO(std::chrono::seconds(60),
                   1,
                   "Messages queued to %s: %s",
                   peer_name_.toString().c_str(),
                   deps_->dumpQueuedMessages(peer_name_).c_str());
    err = E::NOBUFS;
    return nullptr;
  }

  auto envelope = std::make_unique<Envelope>(*this, std::move(msg));
  ld_check(!msg);

  pendingq_.push(*envelope);
  deps_->noteBytesQueued(envelope->cost());

  return envelope.release();
}

void Socket::releaseMessage(Envelope& envelope) {
  // This envelope should be in the pendingq_.
  ld_check(envelope.socket_links_.is_linked());

  // If this envelope was registered as a deferred callback on this
  // socket's FlowGroup, the code releasing the envelope should
  // have dequeued it.
  ld_check(!envelope.active());

  // Take ownership of the envelope
  std::unique_ptr<Envelope> pending_envelope(&envelope);
  pendingq_.erase(*pending_envelope);

  FLOW_GROUP_MSG_LATENCY_ADD(Worker::stats(), flow_group_, envelope);

  send(std::move(pending_envelope));
}

std::unique_ptr<Message> Socket::discardEnvelope(Envelope& envelope) {
  // This envelope should be in the pendingq_.
  ld_check(envelope.socket_links_.is_linked());

  deps_->noteBytesDrained(envelope.cost());

  // Take ownership of the envelope so it is deleted.
  std::unique_ptr<Envelope> pending_envelope(&envelope);
  pendingq_.erase(*pending_envelope);

  // The caller decides the disposition of the enclosed message.
  return pending_envelope->moveMessage();
}

void Socket::sendHello() {
  ld_check(bev_);
  ld_check(!connected_);
  ld_check(next_pos_ == 0);
  ld_check(drain_pos_ == 0);

  // HELLO should be the first message to be sent on this socket.
  ld_check(getBytesPending() == 0);

  uint16_t max_protocol = getSettings().max_protocol;
  ld_check(max_protocol >= Compatibility::MIN_PROTOCOL_SUPPORTED);
  ld_check(max_protocol <= Compatibility::MAX_PROTOCOL_SUPPORTED);
  HELLO_Header hdr{Compatibility::MIN_PROTOCOL_SUPPORTED,
                   max_protocol,
                   0,
                   request_id_t(0),
                   {}};
  NodeID source_node_id, destination_node_id;
  std::string cluster_name, extra_build_info, client_location;

  // If this is a LogDevice server, include its NodeID in the HELLO
  // message.
  if (getSettings().server) {
    hdr.flags |= HELLO_Header::SOURCE_NODE;
    source_node_id = deps_->getMyNodeID();
  } else {
    // if this is a client, include build information
    hdr.flags |= HELLO_Header::BUILD_INFO;
    extra_build_info = deps_->getClientBuildInfo();
  }

  // If include_destination_on_handshake is set, then include the
  // destination's NodeID in the HELLO message
  if (getSettings().include_destination_on_handshake) {
    hdr.flags |= HELLO_Header::DESTINATION_NODE;
    destination_node_id = peer_name_.id_.node_;
  }

  // Only include credentials when needed
  if (deps_->authenticationEnabled() && deps_->includeHELLOCredentials()) {
    const std::string& credentials = deps_->getHELLOCredentials();
    ld_check(credentials.size() < HELLO_Header::CREDS_SIZE_V1);

    // +1 for null terminator
    std::memcpy(hdr.credentials, credentials.c_str(), credentials.size() + 1);
  }

  // If include_cluster_name_on_handshake is set then include the cluster
  // name in the HELLOv2 message
  if (getSettings().include_cluster_name_on_handshake) {
    hdr.flags |= HELLO_Header::CLUSTER_NAME;
    cluster_name = deps_->getClusterName();
  }

  // If the client location is specified in settings, include it in the HELLOv2
  // message.
  auto client_location_opt = getSettings().client_location;
  if (client_location_opt.hasValue()) {
    client_location = client_location_opt.value().toString();
    hdr.flags |= HELLO_Header::CLIENT_LOCATION;
  }

  const std::string& csid = deps_->getCSID();
  ld_check(csid.size() < MAX_CSID_SIZE);
  if (!csid.empty()) {
    hdr.flags |= HELLO_Header::CSID;
  }

  std::unique_ptr<Message> hello = std::make_unique<HELLO_Message>(hdr);
  auto hello_v2 = static_cast<HELLO_Message*>(hello.get());
  hello_v2->source_node_id_ = source_node_id;
  hello_v2->destination_node_id_ = destination_node_id;
  hello_v2->cluster_name_ = cluster_name;
  hello_v2->csid_ = csid;
  hello_v2->build_info_ = extra_build_info;
  hello_v2->client_location_ = client_location;

  auto envelope = registerMessage(std::move(hello));
  ld_check(envelope);
  releaseMessage(*envelope);
}

void Socket::sendShutdown() {
  ld_check(bev_);

  auto serverInstanceId = deps_->getServerInstanceId();
  SHUTDOWN_Header hdr{E::SHUTDOWN, serverInstanceId};
  auto shutdown = std::make_unique<SHUTDOWN_Message>(hdr);
  auto envelope = registerMessage(std::move(shutdown));
  // envelope could be null if presend check failed (becasue
  // handshake is not complete) or there was no buffer space. In
  // either case, no shutdown will be sent.
  if (envelope) {
    releaseMessage(*envelope);
  }
}

const Settings& Socket::getSettings() {
  return deps_->getSettings();
}

void Socket::bytesSentCallback(struct evbuffer* buffer,
                               const struct evbuffer_cb_info* info,
                               void* arg) {
  Socket* self = reinterpret_cast<Socket*>(arg);

  ld_check(self);
  ld_check(self->bev_);
  ld_check(buffer == self->deps_->getOutput(self->bev_));

  if (info->n_deleted > 0) {
    self->onBytesPassedToTCP(info->n_deleted);
  }
}

void Socket::enqueueDeferredEvent(SocketEvent e) {
  deferred_event_queue_.push_back(e);

  if (!deps_->evtimerPending(&deferred_event_queue_event_)) {
    int rv = deps_->evtimerAdd(
        &deferred_event_queue_event_, deps_->getZeroTimeout());
    ld_check(rv == 0);
  }
}

void Socket::onBytesPassedToTCP(size_t nbytes) {
  message_pos_t next_drain_pos = drain_pos_ + nbytes;
  ld_check(next_pos_ >= next_drain_pos);

  while (!sendq_.empty() && sendq_.front().getDrainPos() <= next_drain_pos) {
    // All bytes of message at cur have been sent into the underlying socket.
    std::unique_ptr<Envelope> e(&sendq_.front());
    sendq_.pop_front();

    // Messages should be serialized only if we are handshaken_. The only
    // exception is the first message which is a handshake message. HELLO and
    // ACK messages are always at pos_ 0 since they are the first messages to
    // be sent on a connected socket.
    if (isHandshakeMessage(e->message().type_)) {
      ld_check_eq(drain_pos_, 0);
      // handshake was only message. A shutdown message can sneak in when we are
      // trying to shutdown all the client sockets and a ack was just sent out.
      ld_check(sendq_.empty() ||
               isShutdownMessage(sendq_.front().message().type_));
      // Handshake messages are resent on timeout so we can't check the state of
      // handshaken_. It is typically false, but a retransmits attempt could get
      // here just after we receive our peer's handshake reply.
    } else {
      ld_check(handshaken_);
    }

    onSent(std::move(e), E::OK);
    ++num_messages_sent_;
  }

  deps_->noteBytesDrained(nbytes);

  drain_pos_ = next_drain_pos;

  ld_spew("Socket %s passed %zu bytes to TCP. Worker #%i now has %zu total "
          "bytes pending",
          deps_->describeConnection(peer_name_).c_str(),
          nbytes,
          int(deps_->getWorkerId()),
          deps_->getBytesPending());
}

void Socket::deferredEventQueueEventCallback(void* instance, short) {
  auto self = reinterpret_cast<Socket*>(instance);
  self->processDeferredEventQueue();
}

void Socket::processDeferredEventQueue() {
  auto& queue = deferred_event_queue_;
  ld_check(!queue.empty());

  while (!queue.empty()) {
    // we have to remove the event from the queue before hitting callbacks, as
    // they might trigger calls into deferredEventQueueEventCallback() as well.
    SocketEvent event = queue.front();
    queue.pop_front();

    // Hitting the callbacks
    eventCallbackImpl(event);
  }

  if (deps_->evtimerPending(&deferred_event_queue_event_)) {
    deps_->evtimerDel(&deferred_event_queue_event_);
  }

  ld_check(queue.empty());
  ld_assert(!deps_->evtimerPending(&deferred_event_queue_event_));
}

void Socket::endStreamRewindCallback(void* instance, short) {
  auto self = reinterpret_cast<Socket*>(instance);
  self->endStreamRewind();
}

void Socket::endStreamRewind() {
  if (message_error_injection_rewinding_stream_) {
    ld_error("Ending Error Injection on Socket (%p) - %jd diverted",
             this,
             (intmax_t)message_error_injection_rewound_count_);
    message_error_injection_rewound_count_ = 0;
    message_error_injection_rewinding_stream_ = false;
  }
}

void Socket::expectProtocolHeader() {
  ld_check(bev_);
  size_t protohdr_bytes =
      ProtocolHeader::bytesNeeded(recv_message_ph_.type, proto_);

  // Set read watermarks. This tells bev_ to call dataReadCallback()
  // only after sizeof(ProtocolHeader) bytes are available in the input
  // evbuffer (low watermark). bev_ will stop reading from TCP socket after
  // the evbuffer hits tcp_rcvbuf_size_ (high watermark).
  deps_->buffereventSetWatermark(bev_,
                                 EV_READ,
                                 protohdr_bytes,
                                 std::max(protohdr_bytes, tcp_rcvbuf_size_));

  expecting_header_ = true;
}

void Socket::expectMessageBody() {
  ld_check(bev_);
  ld_check(expecting_header_);

  size_t protohdr_bytes =
      ProtocolHeader::bytesNeeded(recv_message_ph_.type, proto_);
  ld_check(recv_message_ph_.len > protohdr_bytes);
  ld_check(recv_message_ph_.len <= Message::MAX_LEN + protohdr_bytes);

  deps_->buffereventSetWatermark(
      bev_,
      EV_READ,
      recv_message_ph_.len - protohdr_bytes,
      std::max((size_t)recv_message_ph_.len, tcp_rcvbuf_size_));

  expecting_header_ = false;
}

int Socket::receiveMessage() {
  int nbytes;

  ld_check(bev_);
  ld_check(connected_);

  struct evbuffer* inbuf = deps_->getInput(bev_);

  static_assert(sizeof(recv_message_ph_) == sizeof(ProtocolHeader),
                "recv_message_ph_ type is not ProtocolHeader");

  if (expectingProtocolHeader()) {
    // 1. Read first 2 fields of ProtocolHeader to extract message type
    size_t min_protohdr_bytes =
        sizeof(ProtocolHeader) - sizeof(ProtocolHeader::cksum);
    nbytes =
        LD_EV(evbuffer_remove)(inbuf, &recv_message_ph_, min_protohdr_bytes);
    if (nbytes != min_protohdr_bytes) { // unlikely
      ld_critical("INTERNAL ERROR: got %d from evbuffer_remove() while "
                  "reading a protocol header from peer %s. "
                  "Expected %lu bytes.",
                  nbytes,
                  deps_->describeConnection(peer_name_).c_str(),
                  min_protohdr_bytes);
      err = E::INTERNAL; // TODO: make sure close() works as an error handler
      return -1;
    }
    if (recv_message_ph_.len <= min_protohdr_bytes) {
      ld_error("PROTOCOL ERROR: got message length %u from peer %s, expected "
               "at least %zu given sizeof(ProtocolHeader)=%zu",
               recv_message_ph_.len,
               deps_->describeConnection(peer_name_).c_str(),
               min_protohdr_bytes + 1,
               sizeof(ProtocolHeader));
      err = E::BADMSG;
      return -1;
    }

    size_t protohdr_bytes =
        ProtocolHeader::bytesNeeded(recv_message_ph_.type, proto_);

    if (recv_message_ph_.len > Message::MAX_LEN + protohdr_bytes) {
      err = E::BADMSG;
      ld_error("PROTOCOL ERROR: got invalid message length %u from peer %s "
               "for msg:%s. Expected at most %u. min_protohdr_bytes:%zu",
               recv_message_ph_.len,
               deps_->describeConnection(peer_name_).c_str(),
               messageTypeNames[recv_message_ph_.type].c_str(),
               Message::MAX_LEN,
               min_protohdr_bytes);
      return -1;
    }
    Message::deserializer_t* deserializer =
        messageDeserializers[recv_message_ph_.type];
    if (deserializer == nullptr) {
      ld_error("PROTOCOL ERROR: got an unknown message type '%c' (%d) from "
               "peer %s",
               int(recv_message_ph_.type),
               int(recv_message_ph_.type),
               deps_->describeConnection(peer_name_).c_str());
      err = E::BADMSG;
      return -1;
    }
    if (!handshaken_ && !isHandshakeMessage(recv_message_ph_.type)) {
      ld_error("PROTOCOL ERROR: got a message of type %s on a brand new "
               "connection to/from %s). Expected %s.",
               messageTypeNames[recv_message_ph_.type].c_str(),
               deps_->describeConnection(peer_name_).c_str(),
               peer_name_.isClientAddress() ? "HELLO" : "ACK");
      err = E::PROTO;
      return -1;
    }

    // 2. Now read checksum field if needed
    if (ProtocolHeader::needChecksumInHeader(recv_message_ph_.type, proto_)) {
      int cksum_nbytes = LD_EV(evbuffer_remove)(
          inbuf, &recv_message_ph_.cksum, sizeof(recv_message_ph_.cksum));

      if (cksum_nbytes != sizeof(recv_message_ph_.cksum)) { // unlikely
        ld_critical("INTERNAL ERROR: got %d from evbuffer_remove() while "
                    "reading checksum in protocol header from peer %s. "
                    "Expected %lu bytes.",
                    cksum_nbytes,
                    deps_->describeConnection(peer_name_).c_str(),
                    sizeof(recv_message_ph_.cksum));
        err = E::INTERNAL;
        return -1;
      }
    }

    expectMessageBody();

    ld_check(!expectingProtocolHeader());
  } else {
    // Got message body.

    ld_check(messageDeserializers[recv_message_ph_.type]);

    size_t protocol_bytes_already_read =
        ProtocolHeader::bytesNeeded(recv_message_ph_.type, proto_);
    std::string msgtype = messageTypeNames[recv_message_ph_.type];

    ProtocolReader reader(recv_message_ph_.type,
                          inbuf,
                          recv_message_ph_.len - protocol_bytes_already_read,
                          proto_);

    // 1. read checksum
    uint64_t cksum_recvd = 0;
    uint64_t cksum_computed = 0;
    auto checksumming_enabled = isChecksummingEnabled(recv_message_ph_.type);
    bool need_checksum_in_header =
        ProtocolHeader::needChecksumInHeader(recv_message_ph_.type, proto_);
    if (need_checksum_in_header) {
      // always read the checksum, we'll decide whether to verify it
      // or not based on other settings
      cksum_recvd = recv_message_ph_.cksum;
      if (checksumming_enabled && cksum_recvd != 0) {
        cksum_computed = reader.computeChecksum(recv_message_ph_.len -
                                                sizeof(ProtocolHeader));
      }
    }

    RATELIMIT_DEBUG(
        std::chrono::seconds(10),
        2,
        "msg:%s, cksum_recvd:%lu, cksum_computed:%lu, msg_len:%u, "
        "proto_:%hu, protocol_bytes_already_read:%zu, checksumming_enabled:%s",
        msgtype.c_str(),
        cksum_recvd,
        cksum_computed,
        recv_message_ph_.len,
        proto_,
        protocol_bytes_already_read,
        checksumming_enabled ? "yes" : "no");

    // 2. read actual message
    std::unique_ptr<Message> msg =
        messageDeserializers[recv_message_ph_.type](reader).msg;

    ++num_messages_received_;
    num_bytes_received_ += recv_message_ph_.len;

    bool cksum_failed = false;
    if (need_checksum_in_header && checksumming_enabled) {
      if (cksum_recvd != cksum_computed) {
        RATELIMIT_ERROR(
            std::chrono::seconds(1),
            2,
            "Checksum mismatch (recvd:%lu, computed:%lu) detected with peer %s"
            ", msgtype:%s",
            cksum_recvd,
            cksum_computed,
            deps_->describeConnection(peer_name_).c_str(),
            messageTypeNames[recv_message_ph_.type].c_str());

        cksum_failed = true;
        err = E::CHECKSUM_MISMATCH;
        STAT_INCR(deps_->getStats(), protocol_checksum_mismatch);
      } else {
        STAT_INCR(deps_->getStats(), protocol_checksum_matched);
      }
    }

    expectProtocolHeader();

    if (!msg || cksum_failed) {
      switch (err) {
        case E::TOOBIG:
          ld_error("PROTOCOL ERROR: message of type %s received from peer "
                   "%s is too large: %u bytes",
                   messageTypeNames[recv_message_ph_.type].c_str(),
                   deps_->describeConnection(peer_name_).c_str(),
                   recv_message_ph_.len);
          err = E::BADMSG;
          return -1;

        case E::BADMSG:
          ld_error("PROTOCOL ERROR: message of type %s received from peer "
                   "%s has invalid format",
                   messageTypeNames[recv_message_ph_.type].c_str(),
                   deps_->describeConnection(peer_name_).c_str());
          err = E::BADMSG;
          return -1;

        case E::CHECKSUM_MISMATCH:
          // converting error type since existing clients don't
          // handle E::CHECKSUM_MISMATCH
          err = E::BADMSG;
          return -1;

        case E::INTERNAL:
          ld_critical("INTERNAL ERROR while deserializing a message of type "
                      "%s received from peer %s",
                      messageTypeNames[recv_message_ph_.type].c_str(),
                      deps_->describeConnection(peer_name_).c_str());
          return 0;

        case E::NOTSUPPORTED:
          ld_critical(
              "INTERNAL ERROR: deserializer for message type %d (%s) not "
              "implemented.",
              int(recv_message_ph_.type),
              messageTypeNames[recv_message_ph_.type].c_str());
          ld_check(false);
          err = E::INTERNAL;
          return -1;

        default:
          ld_critical("INTERNAL ERROR: unexpected error code %d (%s) from "
                      "deserializer for message type %s received from peer %s",
                      static_cast<int>(err),
                      error_name(err),
                      messageTypeNames[recv_message_ph_.type].c_str(),
                      deps_->describeConnection(peer_name_).c_str());
          return 0;
      }

      ld_check(false); // must not get here
      return 0;
    }

    ld_check(msg);

    if (isHandshakeMessage(recv_message_ph_.type)) {
      if (handshaken_) {
        ld_error("PROTOCOL ERROR: got a duplicate %s from %s",
                 messageTypeNames[recv_message_ph_.type].c_str(),
                 deps_->describeConnection(peer_name_).c_str());
        err = E::PROTO;
        return -1;
      }

      handshaken_ = true;
      first_attempt_ = false;
      deps_->evtimerDel(&handshake_timeout_event_);
    }

    MESSAGE_TYPE_STAT_INCR(
        deps_->getStats(), recv_message_ph_.type, message_received);
    TRAFFIC_CLASS_STAT_INCR(deps_->getStats(), msg->tc_, messages_received);
    TRAFFIC_CLASS_STAT_ADD(
        deps_->getStats(), msg->tc_, bytes_received, recv_message_ph_.len);

    RunState run_state(msg->type_);
    deps_->onStartedRunning(run_state);

    /* verify that gossip sockets don't receive non-gossip messages
     * exceptions: handshake, config synchronization, shutdown
     */
    if (type_ == SocketType::GOSSIP) {
      if (!(msg->type_ == MessageType::SHUTDOWN ||
            Socket::allowedOnGossipConnection(msg->type_))) {
        RATELIMIT_WARNING(std::chrono::seconds(1),
                          1,
                          "Received invalid message(%u) on gossip socket",
                          static_cast<unsigned char>(msg->type_));
        err = E::BADMSG;
        return -1;
      }
    }
    ld_spew("Received message %s of size %u bytes from %s",
            messageTypeNames[recv_message_ph_.type].c_str(),
            recv_message_ph_.len,
            deps_->describeConnection(peer_name_).c_str());
    Message::Disposition disp = deps_->onReceived(msg.get(), peer_name_);
    Status onreceived_err = err;
    deps_->onStoppedRunning(run_state);

    // If this is a newly handshaken client connection, we might want to drop
    // it at this point if we're already over the limit. onReceived() of a
    // handshake message may set peer_node_id_ (if the client connection is in
    // fact from another node in the cluster), which is why the check is not
    // done earlier.
    if (isHandshakeMessage(recv_message_ph_.type)) {
      if (peerIsClient()) {
        conn_external_token_ = deps_->getConnBudgetExternal().acquireToken();
        if (!conn_external_token_) {
          RATELIMIT_WARNING(std::chrono::seconds(10),
                            1,
                            "Rejecting a client connection from %s because the "
                            "client connection limit has been reached.",
                            deps_->describeConnection(peer_name_).c_str());

          // Set to false to prevent close() from releasing even though
          // acquire() failed.
          handshaken_ = false;

          err = E::TOOMANY;
          return -1;
        }
      }
    }

    switch (disp) {
      case Message::Disposition::NORMAL:
        if (isHandshakeMessage(recv_message_ph_.type)) {
          switch (recv_message_ph_.type) {
            case MessageType::ACK: {
              ACK_Message* ack = static_cast<ACK_Message*>(msg.get());
              our_name_at_peer_ = ClientID(ack->getHeader().client_idx);
              proto_ = ack->getHeader().proto;
              connect_throttle_.connectSucceeded();
            } break;
            case MessageType::HELLO:
              proto_ = std::min(
                  static_cast<HELLO_Message*>(msg.get())->header_.proto_max,
                  getSettings().max_protocol);
              break;
            default:
              ld_check(false); // unreachable.
          };
          ld_check(proto_ >= Compatibility::MIN_PROTOCOL_SUPPORTED);
          ld_check(proto_ <= Compatibility::MAX_PROTOCOL_SUPPORTED);
          ld_assert(proto_ <= getSettings().max_protocol);
          ld_spew("%s negotiated protocol %d",
                  deps_->describeConnection(peer_name_).c_str(),
                  proto_);

          // Now that we know what protocol we are speaking with the other end,
          // we can serialize pending messages. Messages that are not compatible
          // with the protocol will not be sent.
          flushSerializeQueue();
        }
        break;
      case Message::Disposition::KEEP:
        // msg may have been deleted here, do not dereference
        ld_check(!isHandshakeMessage(recv_message_ph_.type));
        msg.release();
        break;
      case Message::Disposition::ERROR:
        // This should be in sync with comment in Message::Disposition enum.
        err = onreceived_err;
        ld_check_in(err,
                    ({E::ACCESS,
                      E::PROTONOSUPPORT,
                      E::PROTO,
                      E::BADMSG,
                      E::DESTINATION_MISMATCH,
                      E::INVALID_CLUSTER,
                      E::INTERNAL}));
        return -1;
    }
  } // processing message body

  return 0;
}

int Socket::pushOnCloseCallback(SocketCallback& cb) {
  if (cb.active()) {
    RATELIMIT_CRITICAL(
        std::chrono::seconds(1),
        10,
        "INTERNAL ERROR: attempt to push an active SocketCallback "
        "onto the on_close_ callback list of Socket %s",
        deps_->describeConnection(peer_name_).c_str());
    ld_check(false);
    err = E::INVALID_PARAM;
    return -1;
  }

  impl_->on_close_.push_back(cb);
  return 0;
}

int Socket::pushOnBWAvailableCallback(BWAvailableCallback& cb) {
  if (cb.socket_links_.is_linked()) {
    RATELIMIT_CRITICAL(std::chrono::seconds(1),
                       10,
                       "INTERNAL ERROR: attempt to push an active "
                       "BWAvailableCallback onto the pending_bw_cbs_ "
                       "callback list of Socket %s",
                       deps_->describeConnection(peer_name_).c_str());
    ld_check(false);
    err = E::INVALID_PARAM;
    return -1;
  }
  impl_->pending_bw_cbs_.push_back(cb);
  return 0;
}

size_t Socket::getTcpSendBufSize() const {
  if (!bev_) {
    return 0;
  }

  const std::chrono::seconds SNDBUF_CACHE_TTL(1);
  auto now = std::chrono::steady_clock::now();
  if (now - tcp_sndbuf_cache_.update_time >= SNDBUF_CACHE_TTL) {
    tcp_sndbuf_cache_.update_time = now;
    socklen_t optlen = sizeof(int);
    int fd = LD_EV(bufferevent_getfd)(bev_);
    ld_check(fd != -1);
    int prev_tcp_sndbuf_size_cache = tcp_sndbuf_cache_.size;
    int rv =
        getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &tcp_sndbuf_cache_.size, &optlen);
    if (rv == 0) {
      if (tcp_sndbuf_cache_.size > 0) {
        tcp_sndbuf_cache_.size /= 2;
      } else {
        ld_error("getsockopt() returned non-positive number %d: %s",
                 fd,
                 strerror(errno));
        tcp_sndbuf_cache_.size = prev_tcp_sndbuf_size_cache;
      }
    } else {
      ld_error("Failed to get sndbuf size for TCP socket %d: %s",
               fd,
               strerror(errno));
      tcp_sndbuf_cache_.size = prev_tcp_sndbuf_size_cache;
    }
  }

  return tcp_sndbuf_cache_.size;
}

void Socket::addHandshakeTimeoutEvent() {
  std::chrono::milliseconds timeout = getSettings().handshake_timeout;

  if (timeout.count() > 0) {
    deps_->evtimerAdd(
        &handshake_timeout_event_, deps_->getCommonTimeout(timeout));
  }
}

void Socket::addConnectAttemptTimeoutEvent() {
  std::chrono::milliseconds timeout = getSettings().connect_timeout;
  if (timeout.count() > 0) {
    timeout *=
        pow(getSettings().connect_timeout_retry_multiplier, retries_so_far_);
    deps_->evtimerAdd(
        &connect_timeout_event_, deps_->getCommonTimeout(timeout));
  }
}

size_t Socket::getBytesPending() const {
  size_t queued_bytes = pendingq_.cost() + serializeq_.cost() + sendq_.cost();

  size_t buffered_bytes = 0;
  if (bev_) {
    buffered_bytes += LD_EV(evbuffer_get_length)(deps_->getOutput(bev_));
  }
  if (buffered_output_) {
    buffered_bytes += LD_EV(evbuffer_get_length)(buffered_output_);
  }

  return queued_bytes + buffered_bytes;
}

void Socket::handshakeTimeoutCallback(void* arg, short) {
  reinterpret_cast<Socket*>(arg)->onHandshakeTimeout();
}

void Socket::connectAttemptTimeoutCallback(void* arg, short) {
  reinterpret_cast<Socket*>(arg)->onConnectAttemptTimeout();
}

int Socket::checkConnection(ClientID* our_name_at_peer) {
  if (!our_name_at_peer_.valid()) {
    // socket is either not connected or we're still waiting for a handshake
    // to complete
    if (!connect_throttle_.mayConnect()) {
      ld_check(!connected_);
      ld_check(!bev_);
      err = E::DISABLED;
    } else if (peer_name_.isClientAddress()) {
      err = E::INVALID_PARAM;
    } else {
      ld_check(!handshaken_);
      // Sender always initiates a connection attempt whenever a Socket is
      // created. Therefore, we're either still waiting on a connection to be
      // established or are expecting an ACK to complete the handshake. Set err
      // to NOTCONN only if we previously had a working connection to the node.
      err = first_attempt_ ? E::ALREADY : E::NOTCONN;
    }

    return -1;
  }

  if (our_name_at_peer) {
    *our_name_at_peer = our_name_at_peer_;
  }

  return 0;
}

void Socket::dumpQueuedMessages(std::map<MessageType, int>* out) const {
  for (const Envelope& e : sendq_) {
    ++(*out)[e.message().type_];
  }
}

void Socket::getDebugInfo(InfoSocketsTable& table) const {
  std::string state;
  // Connection state of the socket.
  if (!bev_) {
    state = "I";
  } else if (!connected_) {
    state = "C";
  } else if (!handshaken_) {
    state = "H";
  } else {
    state = "A";
  }

  const size_t available =
      bev_ ? LD_EV(evbuffer_get_length)(deps_->getInput(bev_)) : 0;

  table.next()
      .set<0>(state)
      .set<1>(peer_name_)
      .set<2>(getBytesPending() / 1024.0)
      .set<3>(available / 1024.0)
      .set<4>(num_bytes_received_ / 1048576.0)
      .set<5>(drain_pos_ / 1048576.0)
      .set<6>(num_messages_received_)
      .set<7>(num_messages_sent_)
      .set<8>(proto_)
      .set<9>(this->getTcpSendBufSize())
      .set<10>(getPeerConfigVersion().val())
      .set<11>(isSSL())
      .set<12>(fd_);
}

bool Socket::peerIsClient() const {
  return peer_name_.isClientAddress() && !peer_node_id_.isNodeID();
}

X509* Socket::getPeerCert() const {
  ld_check(isSSL());

  // This function should only be called when the socket is SSL enabled.
  // This means this should always return a valid ssl context.
  SSL* ctx = bufferevent_openssl_get_ssl(bev_);
  ld_check(ctx);

  return SSL_get_peer_certificate(ctx);
}

void Socket::limitCiphersToENULL() {
  ld_check(isSSL());
  null_ciphers_only_ = true;
}

// The following methods are overridden by tests.

const Settings& SocketDependencies::getSettings() const {
  return Worker::settings();
}

StatsHolder* SocketDependencies::getStats() {
  return Worker::stats();
}

void SocketDependencies::noteBytesQueued(size_t nbytes) {
  Worker::onThisThread()->sender().noteBytesQueued(nbytes);
}

void SocketDependencies::noteBytesDrained(size_t nbytes) {
  Worker::onThisThread()->sender().noteBytesDrained(nbytes);
}

size_t SocketDependencies::getBytesPending() const {
  return Worker::onThisThread()->sender().getBytesPending();
}

bool SocketDependencies::bytesPendingLimitReached() const {
  return Worker::onThisThread()->sender().bytesPendingLimitReached();
}

worker_id_t SocketDependencies::getWorkerId() const {
  return Worker::onThisThread()->idx_;
}

std::shared_ptr<SSLContext>
SocketDependencies::getSSLContext(bufferevent_ssl_state ssl_state,
                                  bool null_ciphers_only) const {
  // Servers are required to have a certificate so that the client can verify
  // them. If clients specify that they want to include their certificate, then
  // the server will also authenticate the client certificates.
  bool loadCert = getSettings().server || getSettings().ssl_load_client_cert;
  bool ssl_accepting = ssl_state == BUFFEREVENT_SSL_ACCEPTING;

  return Worker::onThisThread()->sslFetcher().getSSLContext(
      loadCert, ssl_accepting, null_ciphers_only);
}

bool SocketDependencies::shuttingDown() const {
  return Worker::onThisThread()->shuttingDown();
}

std::string SocketDependencies::dumpQueuedMessages(Address addr) const {
  return Worker::onThisThread()->sender().dumpQueuedMessages(addr);
}

const Sockaddr& SocketDependencies::getNodeSockaddr(NodeID nid,
                                                    SocketType type,
                                                    ConnectionType conntype) {
  std::shared_ptr<ServerConfig> cfg(Worker::getConfig()->serverConfig());
  const Configuration::Node* node_cfg = cfg->getNode(nid);

  if (node_cfg) {
    if (type == SocketType::GOSSIP && !Worker::settings().send_to_gossip_port) {
      return node_cfg->getSockaddr(SocketType::DATA, conntype);
    } else {
      return node_cfg->getSockaddr(type, conntype);
    }
  }

  return Sockaddr::INVALID;
}

int SocketDependencies::eventAssign(struct event* ev,
                                    void (*cb)(evutil_socket_t,
                                               short what,
                                               void* arg),
                                    void* arg) {
  return LD_EV(event_assign)(ev,
                             Worker::onThisThread()->getEventBase(),
                             -1,
                             EV_WRITE | EV_PERSIST,
                             cb,
                             arg);
}

void SocketDependencies::eventActive(struct event* ev, int what, short ncalls) {
  LD_EV(event_active)(ev, what, ncalls);
}

void SocketDependencies::eventDel(struct event* ev) {
  LD_EV(event_del)(ev);
}

int SocketDependencies::eventPrioritySet(struct event* ev, int priority) {
  return LD_EV(event_priority_set)(ev, priority);
}

int SocketDependencies::evtimerAssign(struct event* ev,
                                      void (*cb)(evutil_socket_t,
                                                 short what,
                                                 void* arg),
                                      void* arg) {
  return evtimer_assign(ev, Worker::onThisThread()->getEventBase(), cb, arg);
}

void SocketDependencies::evtimerDel(struct event* ev) {
  evtimer_del(ev);
}

const struct timeval*
SocketDependencies::getCommonTimeout(std::chrono::milliseconds timeout) {
  return Worker::onThisThread()->getCommonTimeout(timeout);
}

const struct timeval* SocketDependencies::getZeroTimeout() {
  return EventLoop::onThisThread()->zero_timeout_;
}

int SocketDependencies::evtimerAdd(struct event* ev,
                                   const struct timeval* timeout) {
  return evtimer_add(ev, timeout);
}

int SocketDependencies::evtimerPending(struct event* ev, struct timeval* tv) {
  return evtimer_pending(ev, tv);
}

struct bufferevent*
SocketDependencies::buffereventSocketNew(int sfd,
                                         int opts,
                                         bool secure,
                                         bufferevent_ssl_state ssl_state,
                                         SSLContext* ssl_ctx) {
  if (secure) {
    if (!ssl_ctx) {
      ld_error("Invalid SSLContext, can't create SSL socket");
      return nullptr;
    }

    SSL* ssl = ssl_ctx->createSSL();
    if (!ssl) {
      ld_error("Null SSL* returned, can't create SSL socket");
      return nullptr;
    }

    struct bufferevent* bev = bufferevent_openssl_socket_new(
        Worker::onThisThread()->getEventBase(), sfd, ssl, ssl_state, opts);
    ld_check(bufferevent_get_openssl_error(bev) == 0);
#if LIBEVENT_VERSION_NUMBER >= 0x02010100
    bufferevent_openssl_set_allow_dirty_shutdown(bev, 1);
#endif
    return bev;

  } else {
    return LD_EV(bufferevent_socket_new)(
        Worker::onThisThread()->getEventBase(), sfd, opts);
  }
}

struct evbuffer* SocketDependencies::getOutput(struct bufferevent* bev) {
  return LD_EV(bufferevent_get_output)(bev);
}

struct evbuffer* SocketDependencies::getInput(struct bufferevent* bev) {
  return LD_EV(bufferevent_get_input)(bev);
}

int SocketDependencies::buffereventSocketConnect(struct bufferevent* bev,
                                                 struct sockaddr* ss,
                                                 int len) {
  return LD_EV(bufferevent_socket_connect)(bev, ss, len);
}

void SocketDependencies::buffereventSetWatermark(struct bufferevent* bev,
                                                 short events,
                                                 size_t lowmark,
                                                 size_t highmark) {
  LD_EV(bufferevent_setwatermark)(bev, events, lowmark, highmark);
}

void SocketDependencies::buffereventSetCb(struct bufferevent* bev,
                                          bufferevent_data_cb readcb,
                                          bufferevent_data_cb writecb,
                                          bufferevent_event_cb eventcb,
                                          void* cbarg) {
  LD_EV(bufferevent_setcb)(bev, readcb, writecb, eventcb, cbarg);
}

void SocketDependencies::buffereventShutDownSSL(struct bufferevent* bev) {
  SSL* ctx = bufferevent_openssl_get_ssl(bev);
  ld_check(ctx);
  SSL_set_shutdown(ctx, SSL_RECEIVED_SHUTDOWN);
  SSL_shutdown(ctx);
  while (ERR_get_error()) {
    // flushing all SSL errors so they don't get misattributed to another
    // socket.
  }
}

void SocketDependencies::buffereventFree(struct bufferevent* bev) {
  LD_EV(bufferevent_free)(bev);
}

int SocketDependencies::evUtilMakeSocketNonBlocking(int sfd) {
  return LD_EV(evutil_make_socket_nonblocking)(sfd);
}

int SocketDependencies::buffereventSetMaxSingleWrite(struct bufferevent* bev,
                                                     size_t size) {
#if LIBEVENT_VERSION_NUMBER >= 0x02010000
  // In libevent >= 2.1 we can tweak the amount of data libevent sends to
  // the TCP stack at once
  return LD_EV(bufferevent_set_max_single_write)(bev, size);
#else
  // Let older libevent decide for itself.
  return 0;
#endif
}

int SocketDependencies::buffereventSetMaxSingleRead(struct bufferevent* bev,
                                                    size_t size) {
#if LIBEVENT_VERSION_NUMBER >= 0x02010000
  return LD_EV(bufferevent_set_max_single_read)(bev, size);
#else
  return 0;
#endif
}

int SocketDependencies::buffereventEnable(struct bufferevent* bev,
                                          short event) {
  return LD_EV(bufferevent_enable)(bev, event);
}

std::string SocketDependencies::describeConnection(const Address& addr) {
  return Sender::describeConnection(addr);
}

void SocketDependencies::onSent(std::unique_ptr<Message> msg,
                                const Address& to,
                                Status st,
                                SteadyTimestamp t,
                                Message::CompletionMethod cm) {
  RunState run_state(msg->type_);

  switch (cm) {
    case Message::CompletionMethod::IMMEDIATE: {
      auto prev_state = Worker::packRunState();
      Worker::onStartedRunning(run_state);
      Worker::onThisThread()->message_dispatch_->onSent(*msg, st, to, t);
      Worker::onStoppedRunning(run_state);
      Worker::unpackRunState(prev_state);
      break;
    }
    default:
      ld_check(false);
      // fallthrough
    case Message::CompletionMethod::DEFERRED:
      auto& sender = Worker::onThisThread()->sender();
      sender.queueMessageCompletion(std::move(msg), to, st, t);
      break;
  }
}

Message::Disposition SocketDependencies::onReceived(Message* msg,
                                                    const Address& from) {
  return Worker::onThisThread()->message_dispatch_->onReceived(msg, from);
}

void SocketDependencies::processDeferredMessageCompletions() {
  Worker::onThisThread()->sender().deliverCompletedMessages();
}

NodeID SocketDependencies::getMyNodeID() {
  return Worker::onThisThread()->getConfig()->serverConfig()->getMyNodeID();
}

/**
 * Attempt to set SO_SNDBUF, SO_RCVBUF, TCP_NODELAY, SO_KEEP_ALIVE,
 * TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT, TCP_USER_TIMEOUT options of
 * socket fd to values in getSettings().
 *
 * @param is_tcp If set to false, do not set TCP_NODELAY because this socket is
 * not a tcp socket.
 * @param snd_out  if non-nullptr, report the value of SO_SNDBUF through this.
 *                 Set value to -1 if getsockopt() fails.
 * @param rcv_out  same as snd_out, but for SO_RCVBUF.
 *
 * NOTE that the values reported by getsockopt() for buffer sizes are 2X what
 * is passed in through setsockopt() because that's how Linux does it. See
 * socket(7).
 * NOTE that KEEP_ALIVE options are used only for tcp sockets (when is_tcp is
 * true).
 */
void SocketDependencies::configureSocket(bool is_tcp,
                                         int fd,
                                         int* snd_out,
                                         int* rcv_out) {
  int sndbuf_size, rcvbuf_size;
  int rv;
  socklen_t optlen;

  sndbuf_size = getSettings().tcp_sendbuf_kb * 1024;
  if (sndbuf_size >= 0) {
    rv = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(int));
    if (rv != 0) {
      ld_error("Failed to set sndbuf size for TCP socket %d to %d: %s",
               fd,
               sndbuf_size,
               strerror(errno));
    }
  }

  if (snd_out) {
    optlen = sizeof(int);
    rv = getsockopt(fd, SOL_SOCKET, SO_SNDBUF, snd_out, &optlen);
    if (rv == 0) {
      *snd_out /= 2; // account for Linux doubling the value
    } else {
      ld_error("Failed to get sndbuf size for TCP socket %d: %s",
               fd,
               strerror(errno));
      *snd_out = -1;
    }
  }

  rcvbuf_size = getSettings().tcp_rcvbuf_kb * 1024;
  if (rcvbuf_size >= 0) {
    rv = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(int));
    if (rv != 0) {
      ld_error("Failed to set rcvbuf size for TCP socket %d to %d: %s",
               fd,
               rcvbuf_size,
               strerror(errno));
    }
  }

  if (rcv_out) {
    optlen = sizeof(int);
    rv = getsockopt(fd, SOL_SOCKET, SO_RCVBUF, rcv_out, &optlen);
    if (rv == 0) {
      *rcv_out /= 2; // account for Linux doubling the value
    } else {
      ld_error("Failed to get rcvbuf size for TCP socket %d: %s",
               fd,
               strerror(errno));
      *rcv_out = -1;
    }
  }

  if (is_tcp) {
    if (!getSettings().nagle) {
      int one = 1;
      rv = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      if (rv != 0) {
        ld_error("Failed to set TCP_NODELAY for TCP socket %d: %s",
                 fd,
                 strerror(errno));
      }
    }
  }

  bool keep_alive = getSettings().use_tcp_keep_alive;
  if (is_tcp && keep_alive) {
    int keep_alive_time = getSettings().tcp_keep_alive_time;
    int keep_alive_intvl = getSettings().tcp_keep_alive_intvl;
    int keep_alive_probes = getSettings().tcp_keep_alive_probes;

    rv = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keep_alive, sizeof(int));
    if (rv != 0) {
      ld_error("Failed to set SO_KEEPIDLE for TCP socket %d: %s",
               fd,
               strerror(errno));
    }

    if (keep_alive_time > 0) {
      rv = setsockopt(fd, SOL_TCP, TCP_KEEPIDLE, &keep_alive_time, sizeof(int));
      if (rv != 0) {
        ld_error("Failed to set TCP_KEEPIDLE for TCP socket %d: %s",
                 fd,
                 strerror(errno));
      }
    }

    if (keep_alive_intvl > 0) {
      rv = setsockopt(
          fd, SOL_TCP, TCP_KEEPINTVL, &keep_alive_intvl, sizeof(int));
      if (rv != 0) {
        ld_error("Failed to set TCP_KEEPINTVL for TCP socket %d: %s",
                 fd,
                 strerror(errno));
      }
    }

    if (keep_alive_probes > 0) {
      rv =
          setsockopt(fd, SOL_TCP, TCP_KEEPCNT, &keep_alive_probes, sizeof(int));
      if (rv != 0) {
        ld_error("Failed to set TCP_KEEPCNT for TCP socket %d: %s",
                 fd,
                 strerror(errno));
      }
    }
  }

#ifdef __linux__
  if (is_tcp) {
    int tcp_user_timeout = getSettings().tcp_user_timeout;

    if (tcp_user_timeout >= 0) {
      rv = setsockopt(
          fd, SOL_TCP, TCP_USER_TIMEOUT, &tcp_user_timeout, sizeof(int));
      if (rv != 0) {
        ld_error("Failed to set TCP_USER_TIMEOUT for TCP socket %d: %s",
                 fd,
                 strerror(errno));
      }
    }
  }
#endif
}

ResourceBudget& SocketDependencies::getConnBudgetExternal() {
  return Worker::onThisThread()->processor_->conn_budget_external_;
}

std::string SocketDependencies::getClusterName() {
  return Worker::getConfig()->serverConfig()->getClusterName();
}

ServerInstanceId SocketDependencies::getServerInstanceId() {
  return Worker::onThisThread()->processor_->getServerInstanceId();
}

const std::string& SocketDependencies::getHELLOCredentials() {
  return Worker::onThisThread()->processor_->HELLOCredentials_;
}

const std::string& SocketDependencies::getCSID() {
  return Worker::onThisThread()->processor_->csid_;
}

std::string SocketDependencies::getClientBuildInfo() {
  auto build_info = Worker::onThisThread()
                        ->processor_->getPluginRegistry()
                        ->getSinglePlugin<BuildInfo>(PluginType::BUILD_INFO);
  ld_check(build_info);
  return build_info->getBuildInfoJson();
}

bool SocketDependencies::authenticationEnabled() {
  Processor* const processor_ = Worker::onThisThread()->processor_;
  if (processor_->security_info_) {
    auto principal_parser = processor_->security_info_->getPrincipalParser();
    return principal_parser != nullptr;
  } else {
    return false;
  }
}

bool SocketDependencies::allowUnauthenticated() {
  return Worker::onThisThread()
      ->getConfig()
      ->serverConfig()
      ->allowUnauthenticated();
}

bool SocketDependencies::includeHELLOCredentials() {
  // Only include HELLOCredentials in HELLO_Message when the PrincipalParser
  // will use the data.
  auto principal_parser =
      Worker::onThisThread()->processor_->security_info_->getPrincipalParser();
  return principal_parser != nullptr &&
      (principal_parser->getAuthenticationType() ==
       AuthenticationType::SELF_IDENTIFICATION);
}

void SocketDependencies::onStartedRunning(RunState state) {
  Worker::onStartedRunning(state);
}

void SocketDependencies::onStoppedRunning(RunState prev_state) {
  Worker::onStoppedRunning(prev_state);
}

}} // namespace facebook::logdevice
