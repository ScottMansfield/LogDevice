/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "Sender.h"

#include <chrono>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <folly/DynamicConverter.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/json.h>
#include <folly/small_vector.h>

#include "event2/event.h"
#include "logdevice/common/ClientIdxAllocator.h"
#include "logdevice/common/ConstructorFailed.h"
#include "logdevice/common/EventHandler.h"
#include "logdevice/common/FlowGroup.h"
#include "logdevice/common/Processor.h"
#include "logdevice/common/ResourceBudget.h"
#include "logdevice/common/Sockaddr.h"
#include "logdevice/common/Socket.h"
#include "logdevice/common/SocketCallback.h"
#include "logdevice/common/Worker.h"
#include "logdevice/common/configuration/TrafficShapingConfig.h"
#include "logdevice/common/debug.h"
#include "logdevice/common/libevent/compat.h"
#include "logdevice/common/protocol/CONFIG_ADVISORY_Message.h"
#include "logdevice/common/protocol/CONFIG_CHANGED_Message.h"
#include "logdevice/common/protocol/Message.h"
#include "logdevice/common/settings/Settings.h"
#include "logdevice/common/stats/ServerHistograms.h"
#include "logdevice/common/stats/Stats.h"
#include "logdevice/common/util.h"

namespace facebook { namespace logdevice {

using steady_clock = std::chrono::steady_clock;

namespace {

// An object of this functor class is registered with every
// client Socket managed by this Sender and is called when the
// Socket closes.
class DisconnectedClientCallback : public SocketCallback {
 public:
  // calls noteDisconnectedClient() of the current thread's Sender
  void operator()(Status st, const Address& name) override;
};

} // namespace

class SenderImpl {
 public:
  SenderImpl(Sender& /*sender*/,
             size_t max_node_idx,
             int32_t /*num_workers*/,
             ClientIdxAllocator* client_id_allocator)
      : server_sockets_(max_node_idx + 1),
        client_id_allocator_(client_id_allocator) {}

  // a map of all Sockets that have been created on this Worker thread
  // in order to talk to LogDevice servers. The map is implemented as
  // a vector keyed by node_index_t's of those servers in the
  // Processor's configuration. Sockets are removed from this map only
  // when the corresponding server is no longer in the config (the
  // generation count of the Server's config record has
  // increased). Sockets are not removed when the connection breaks.
  // When a server goes down, its Socket's state changes to indicate
  // that it is now disconnected (.bev_ is nullptr, .connected_ is
  // false). The Socket object remains in the map. sendMessage() to
  // the node_index_t or NodeID of that Socket will try to reconnect.
  // The rate of reconnection attempts is controlled by a ConnectionThrottle.
  std::vector<std::unique_ptr<Socket>> server_sockets_;

  // Sockets get moved here from server_sockets_ to be closed. The
  // sockets_to_close_available_ event should be signalled when this vector is
  // not empty.
  std::vector<std::pair<std::unique_ptr<Socket>, Status>> sockets_to_close_;

  // a map of all Sockets wrapping connections that were accepted from
  // clients, keyed by 32-bit client ids. This map is empty on clients.
  std::unordered_map<ClientID, Socket, ClientID::Hash> client_sockets_;

  // Traffic Shaping
  std::array<FlowGroup, NodeLocation::NUM_ALL_SCOPES> flow_groups_;
  // Provides mutual exclusion between application of flow group updates
  // by the TrafficShaper thread and normal packet transmission on this
  // Sender.
  //
  // Note: Flow group updates only modify the FlowMeters within FlowGroups
  //       and perform thread safe tests to see if FlowGroups need to
  //       be run. For this reason, the flow_meters_mutex_ does not
  //       need to be held during operations that remove elements from
  //       a FlowGroup's priority queue. Operations such as trim or
  //       the cleanup of queued messages when a Socket is closed take
  //       advantage of this property to avoid having to reach up into
  //       the Sender to acquire this lock which, in many error paths,
  //       is already held.
  std::mutex flow_meters_mutex_;

  ClientIdxAllocator* client_id_allocator_;
};

bool SenderProxy::canSendToImpl(const Address& addr,
                                TrafficClass tc,
                                BWAvailableCallback& on_bw_avail) {
  auto w = Worker::onThisThread();
  return w->sender().canSendToImpl(addr, tc, on_bw_avail);
}

int SenderProxy::sendMessageImpl(std::unique_ptr<Message>&& msg,
                                 const Address& addr,
                                 BWAvailableCallback* on_bw_avail,
                                 SocketCallback* onclose) {
  auto w = Worker::onThisThread();
  return w->sender().sendMessageImpl(
      std::move(msg), addr, on_bw_avail, onclose);
}

void SenderBase::MessageCompletion::send() {
  auto prev_state = Worker::packRunState();
  RunState run_state(msg_->type_);
  Worker::onStartedRunning(run_state);
  Worker::onThisThread()->message_dispatch_->onSent(
      *msg_, status_, destination_, enqueue_time_);
  Worker::onStoppedRunning(run_state);
  Worker::unpackRunState(prev_state);
}

Sender::Sender(struct event_base* base,
               const configuration::TrafficShapingConfig& tsc,
               size_t max_node_idx,
               int32_t num_workers,
               ClientIdxAllocator* client_id_allocator)
    : impl_(new SenderImpl(*this,
                           max_node_idx,
                           num_workers,
                           client_id_allocator)),
      sockets_to_close_available_(
          LD_EV(event_new)(base,
                           -1,
                           EV_WRITE | EV_PERSIST,
                           EventHandler<Sender::onSocketsToCloseAvailable>,
                           this)),
      completed_messages_available_(
          LD_EV(event_new)(base,
                           -1,
                           EV_WRITE | EV_PERSIST,
                           EventHandler<Sender::onCompletedMessagesAvailable>,
                           this)),
      flow_groups_run_requested_(
          LD_EV(event_new)(base,
                           -1,
                           EV_WRITE | EV_PERSIST,
                           EventHandler<Sender::onFlowGroupsRunRequested>,
                           this)),
      flow_groups_run_deadline_exceeded_(
          LD_EV(event_new)(base,
                           -1,
                           0,
                           EventHandler<Sender::onFlowGroupsRunRequested>,
                           this)) {
  auto scope = NodeLocationScope::NODE;
  for (auto& fg : impl_->flow_groups_) {
    fg.setScope(this, scope);
    fg.configure(tsc.configured(scope));
    scope = NodeLocation::nextGreaterScope(scope);
  }

  if (!sockets_to_close_available_) { // unlikely
    ld_error("Failed to create 'sockets to close available' event for "
             "a Sender");
    err = E::NOMEM;
    throw ConstructorFailed();
  }

  if (!completed_messages_available_) { // unlikely
    ld_error("Failed to create 'completed messages available' event for "
             "a Sender");
    err = E::NOMEM;
    throw ConstructorFailed();
  }

  if (!flow_groups_run_requested_) { // unlikely
    ld_error("Failed to create 'flow groups run requested' event for "
             "a Sender");
    err = E::NOMEM;
    throw ConstructorFailed();
  }
  LD_EV(event_priority_set)
  (flow_groups_run_requested_, EventLoop::PRIORITY_LOW);

  if (!flow_groups_run_deadline_exceeded_) { // unlikely
    ld_error("Failed to create 'flow groups run deadline exceeded' event for "
             "a Sender");
    err = E::NOMEM;
    throw ConstructorFailed();
  }
  ld_check(num_workers != 0);
  ld_check(max_node_idx < std::numeric_limits<node_index_t>::max());
}

Sender::~Sender() {
  deliverCompletedMessages();
  LD_EV(event_free)(sockets_to_close_available_);
  LD_EV(event_free)(completed_messages_available_);
  LD_EV(event_free)(flow_groups_run_requested_);
  LD_EV(event_free)(flow_groups_run_deadline_exceeded_);
}

void Sender::onCompletedMessagesAvailable(void* arg, short) {
  Sender* self = reinterpret_cast<Sender*>(arg);
  self->deliverCompletedMessages();
}

void Sender::onFlowGroupsRunRequested(void* arg, short) {
  Sender* self = reinterpret_cast<Sender*>(arg);
  self->runFlowGroups(RunType::EVENTLOOP);
}

bool Sender::onMyWorker() const {
  Worker* w = Worker::onThisThread();
  ld_check(w);
  return (&w->sender() == this);
}

int Sender::addClient(int fd,
                      const Sockaddr& client_addr,
                      ResourceBudget::Token conn_token,
                      SocketType type,
                      ConnectionType conntype) {
  Worker* w = Worker::onThisThread();
  ld_check(&w->sender() == this);

  if (shutting_down_) {
    ld_check(false); // listeners are shut down before Senders.
    ld_error("Sender is shut down");
    err = E::SHUTDOWN;
    return -1;
  }

  eraseDisconnectedClients();

  ClientID client_name(
      impl_->client_id_allocator_->issueClientIdx(w->worker_type_, w->idx_));

  try {
    // Until we have better information (e.g. in a future update to the
    // HELLO message), assume clients are within our region unless they
    // have connected via SSL.
    NodeLocationScope flow_group_scope = conntype == ConnectionType::SSL
        ? NodeLocationScope::ROOT
        : NodeLocationScope::REGION;

    auto& flow_group = selectFlowGroup(flow_group_scope);
    auto res = impl_->client_sockets_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(client_name),
        std::forward_as_tuple(fd,
                              client_name,
                              client_addr,
                              std::move(conn_token),
                              type,
                              conntype,
                              flow_group));
    if (!res.second) {
      ld_critical("INTERNAL ERROR: attempt to add client %s (%s) that is "
                  "already in the client map",
                  client_name.toString().c_str(),
                  client_addr.toString().c_str());
      ld_check(0);
      err = E::EXISTS;
      return -1;
    }

    auto* cb = new DisconnectedClientCallback();
    // self-managed, destroyed by own operator()

    int rv = res.first->second.pushOnCloseCallback(*cb);

    if (rv != 0) { // unlikely
      ld_check(false);
      delete cb;
    }
  } catch (const ConstructorFailed&) {
    ld_error("Failed to construct a client socket: error %d (%s)",
             static_cast<int>(err),
             error_description(err));
    return -1;
  }

  return 0;
}

void Sender::noteBytesQueued(size_t nbytes) {
  // nbytes cannot exceed maximum message size
  ld_check(nbytes <= (size_t)Message::MAX_LEN + sizeof(ProtocolHeader));
  bytes_pending_ += nbytes;
  WORKER_STAT_ADD(evbuffer_total_size, nbytes);
  WORKER_STAT_ADD(evbuffer_max_size, nbytes);
}

void Sender::noteBytesDrained(size_t nbytes) {
  ld_check(bytes_pending_ >= nbytes);
  bytes_pending_ -= nbytes;
  WORKER_STAT_SUB(evbuffer_total_size, nbytes);
  WORKER_STAT_SUB(evbuffer_max_size, nbytes);
}

ssize_t Sender::getTcpSendBufSizeForClient(ClientID client_id) const {
  auto it = impl_->client_sockets_.find(client_id);
  if (it == impl_->client_sockets_.end()) {
    ld_error("client socket %s not found", client_id.toString().c_str());
    ld_check(false);
    return -1;
  }

  return it->second.getTcpSendBufSize();
}

void Sender::eraseDisconnectedClients() {
  for (const ClientID& cid : disconnected_clients_) {
    const auto pos = impl_->client_sockets_.find(cid);
    assert(pos != impl_->client_sockets_.end());
    assert(pos->second.isClosed());
    impl_->client_sockets_.erase(pos);
    impl_->client_id_allocator_->releaseClientIdx(cid);
  }

  disconnected_clients_.clear();
}

int Sender::notifyPeerConfigUpdated(Socket& sock) {
  auto server_config = Worker::onThisThread()->getServerConfig();
  config_version_t config_version = server_config->getVersion();
  config_version_t peer_config_version = sock.getPeerConfigVersion();
  if (peer_config_version >= config_version) {
    // The peer config is more recent. nothing to do here.
    return 0;
  }
  // The peer config version on the socket is outdated, so we need to notify
  // the peer and update it.
  std::unique_ptr<Message> msg;
  const Address& addr = sock.peer_name_;
  if (addr.isClientAddress()) {
    // The peer is a client, in the sense that it is the client-side of the
    // connection. It may very well be a node however.

    if (peer_config_version == CONFIG_VERSION_INVALID) {
      // We never received a CONFIG_ADVISORY message on this socket, so we can
      // assume that config synchronization is disabled on this client or it
      // hasn't got a chance to send the message yet. either way, don't do
      // anything yet.
      return 0;
    }

    ld_info("Detected stale peer config (%u < %u). "
            "Sending CONFIG_CHANGED to %s",
            peer_config_version.val(),
            config_version.val(),
            describeConnection(addr).c_str());
    ServerConfig::ConfigMetadata metadata =
        server_config->getMainConfigMetadata();

    // Send a CONFIG_CHANGED message to update the main config on the peer
    CONFIG_CHANGED_Header hdr = {
        static_cast<uint64_t>(metadata.modified_time.count()),
        config_version,
        server_config->getServerOrigin(),
        CONFIG_CHANGED_Header::ConfigType::MAIN_CONFIG,
        CONFIG_CHANGED_Header::Action::UPDATE};
    metadata.hash.copy(hdr.hash, sizeof hdr.hash);

    // We still send the Zookeeper section for backwards compatibility on
    // older servers, but on newer servers this is ignored
    // Clients already ignore / don't use the Zookeeper section
    // TODO deprecate in T32793726
    auto zk_config = Worker::onThisThread()->getZookeeperConfig();
    msg = std::make_unique<CONFIG_CHANGED_Message>(
        hdr, server_config->toString(nullptr, zk_config.get(), true));
  } else {
    // The peer is a server. Send a CONFIG_ADVISORY to let it know about our
    // config version. Upon receiving this message, if the server config hasn't
    // been updated already, it will either fetch it from us in another round
    // trip, or fetch the newest version directly from the source.
    CONFIG_ADVISORY_Header hdr = {config_version};
    msg = std::make_unique<CONFIG_ADVISORY_Message>(hdr);
  }
  int rv = sendMessageImpl(std::move(msg), sock);
  if (rv != 0) {
    RATELIMIT_INFO(
        std::chrono::seconds(10),
        10,
        "Failed to send %s with error %s.",
        addr.isClientAddress() ? "CONFIG_CHANGED" : "CONFIG_ADVISORY",
        error_description(err));
    return -1;
  }
  // Message was sent successfully. Peer should now have a version
  // greater than or equal to config_version.
  // Update peer version in socket to avoid sending CONFIG_ADVISORY or
  // CONFIG_CHANGED again on the same connection.
  sock.setPeerConfigVersion(config_version);
  return 0;
}

bool Sender::canSendToImpl(const Address& addr,
                           TrafficClass tc,
                           BWAvailableCallback& on_bw_avail) {
  ld_check(!on_bw_avail.active());

  Socket* sock = findSocket(addr);

  if (!sock) {
    if (addr.isClientAddress()) {
      // With no current client connection, the send is
      // guaranteeed to fail.
      return false;
    } else if (err == E::NOTCONN) {
      // sendMessage() will attempt to connect to a node
      // if no connection to that node currently exists.
      // Since we can't know for sure whether or not a
      // message will be throttled until after we're
      // connected and know the scope for that connection,
      // say that the send is expected to succeed so the
      // caller attempts to send and a connection attempt
      // will be made.
      return true;
    }
    return false;
  }

  Priority p = PriorityMap::fromTrafficClass()[tc];

  // Lock to prevent race between registering for bandwidth and
  // a bandwidth deposit from the TrafficShaper.
  std::unique_lock<std::mutex> lock(impl_->flow_meters_mutex_);
  if (!sock->flow_group_.canDrain(p)) {
    sock->flow_group_.push(on_bw_avail, p);
    maybeScheduleRunFlowGroups(sock->flow_group_);
    err = E::CBREGISTERED;
    FLOW_GROUP_STAT_INCR(Worker::stats(), sock->flow_group_, cbregistered);
    return false;
  }

  return true;
}

int Sender::sendMessageImpl(std::unique_ptr<Message>&& msg,
                            const Address& addr,
                            BWAvailableCallback* on_bw_avail,
                            SocketCallback* onclose) {
  Socket* sock = getSocket(addr, *msg);
  if (!sock) {
    // err set by getSocket()
    return -1;
  }

  // If the message is neither a handshake message nor a config sychronization
  // message, we need to update the client config version on the socket.
  if (!Socket::isHandshakeMessage(msg->type_) &&
      !Socket::isConfigSynchronizationMessage(msg->type_) &&
      Worker::settings().enable_config_synchronization) {
    int rv = notifyPeerConfigUpdated(*sock);
    if (rv != 0) {
      return -1;
    }
  }

  int rv = sendMessageImpl(std::move(msg), *sock, on_bw_avail, onclose);
  ld_check(rv != 0 ? (bool)msg : !msg); // take ownership on success only
  if (rv != 0) {
    bool no_warning =
        // Some messages are implemented to gracefully handle PROTONOSUPPORT,
        // avoid log spew for them
        err == E::PROTONOSUPPORT && !msg->warnAboutOldProtocol();
    if (!no_warning) {
      RATELIMIT_LEVEL(
          err == E::CBREGISTERED ? dbg::Level::SPEW : dbg::Level::WARNING,
          std::chrono::seconds(10),
          3,
          "Unable to send a message of type %s to %s: error %s",
          messageTypeNames[msg->type_].c_str(),
          Sender::describeConnection(addr).c_str(),
          error_description(err));
    }
  }
  return rv;
}

int Sender::sendMessageImpl(std::unique_ptr<Message>&& msg,
                            ClientID cid,
                            BWAvailableCallback* on_bw_avail,
                            SocketCallback* onclose) {
  return sendMessageImpl(std::move(msg), Address(cid), on_bw_avail, onclose);
}

int Sender::sendMessageImpl(std::unique_ptr<Message>&& msg,
                            NodeID nid,
                            BWAvailableCallback* on_bw_avail,
                            SocketCallback* onclose) {
  ld_check(on_bw_avail == nullptr || !on_bw_avail->active());
  return sendMessageImpl(std::move(msg), Address(nid), on_bw_avail, onclose);
}

int Sender::sendMessageImpl(std::unique_ptr<Message>&& msg,
                            Socket& sock,
                            BWAvailableCallback* on_bw_avail,
                            SocketCallback* onclose) {
  ld_check(!shutting_down_);
  ld_check(onMyWorker());

  /* verify that we only send allowed messages via gossip socket */
  if (sock.getSockType() == SocketType::GOSSIP) {
    if (!Socket::allowedOnGossipConnection(msg->type_)) {
      RATELIMIT_WARNING(std::chrono::seconds(1),
                        1,
                        "Unexpected msg type:%u",
                        static_cast<unsigned char>(msg->type_));
      ld_check(false);
      err = E::INTERNAL;
      return -1;
    }
  }

  auto envelope = sock.registerMessage(std::move(msg));
  if (!envelope) {
    ld_check(err == E::INTERNAL || err == E::NOBUFS || err == E::NOTCONN ||
             err == E::PROTONOSUPPORT || err == E::UNREACHABLE);
    return -1;
  }
  ld_check(!msg);

  if (onclose) {
    sock.pushOnCloseCallback(*onclose);
  }

  std::unique_lock<std::mutex> lock(impl_->flow_meters_mutex_);
  if (!injectTrafficShapingEvent(sock.flow_group_, envelope->priority()) &&
      sock.flow_group_.drain(*envelope)) {
    lock.unlock();
    FLOW_GROUP_STAT_INCR(Worker::stats(), sock.flow_group_, direct_dispatched);
    // Note: Some errors can only be detected during message serialization.
    //       If this occurs just after releaseMessage() and the onSent()
    //       handler for the message responds to the error by queuing another
    //       message, Sender::sendMessage() can be reentered.
    sock.releaseMessage(*envelope);
    return 0;
  }

  // Message has hit a traffic shaping limit.
  if (on_bw_avail == nullptr) {
    // Sender/FlowGroup will release the message once bandwidth is
    // available.
    FLOW_GROUP_STAT_INCR(Worker::stats(), sock.flow_group_, deferred);
    FLOW_GROUP_MSG_STAT_INCR(
        Worker::stats(), sock.flow_group_, &envelope->message(), deferred);
    sock.flow_group_.push(*envelope);
    maybeScheduleRunFlowGroups(sock.flow_group_);
    return 0;
  }

  // Message retransmission is the responsibility of the caller.  Return
  // message ownership to them.
  msg = sock.discardEnvelope(*envelope);

  FLOW_GROUP_STAT_INCR(Worker::stats(), sock.flow_group_, discarded);
  FLOW_GROUP_MSG_STAT_INCR(Worker::stats(), sock.flow_group_, msg, discarded);
  FLOW_GROUP_STAT_INCR(Worker::stats(), sock.flow_group_, cbregistered);
  sock.flow_group_.push(*on_bw_avail, msg->priority());
  sock.pushOnBWAvailableCallback(*on_bw_avail);
  maybeScheduleRunFlowGroups(sock.flow_group_);
  err = E::CBREGISTERED;
  return -1;
}

Socket* Sender::findServerSocket(node_index_t idx) {
  ld_check(idx >= 0);

  Socket* s;

  if (idx >= impl_->server_sockets_.size() ||
      !(s = impl_->server_sockets_[idx].get())) {
    return nullptr;
  }

  ld_check(s);
  ld_check(!s->peer_name_.isClientAddress());
  ld_check(s->peer_name_.id_.node_.index() == idx);

  return s;
}

void Sender::deliverCompletedMessages() {
  CompletionQueue moved_queue = std::move(completed_messages_);
  while (!moved_queue.empty()) {
    std::unique_ptr<MessageCompletion> completion(&moved_queue.front());
    moved_queue.pop_front();
    if (!shutting_down_) {
      completion->send();
    }
  }
}

void Sender::resetServerSocketConnectThrottle(NodeID node_id) {
  ld_check(node_id.isNodeID());

  auto socket = findServerSocket(node_id.index());
  if (socket != nullptr) {
    socket->resetConnectThrottle();
  }
}

void Sender::setPeerShuttingDown(NodeID node_id) {
  ld_check(node_id.isNodeID());

  auto socket = findServerSocket(node_id.index());
  if (socket != nullptr) {
    socket->setPeerShuttingDown();
  }
}

void Sender::runFlowGroups(RunType /*rt*/) {
  LD_EV(event_del)(flow_groups_run_requested_);
  evtimer_del(flow_groups_run_deadline_exceeded_);

  if (flow_groups_run_requested_time_ != SteadyTimestamp()) {
    auto queue_latency = std::chrono::duration_cast<std::chrono::microseconds>(
        SteadyTimestamp::now() - flow_groups_run_requested_time_);
    HISTOGRAM_ADD(Worker::stats(),
                  flow_groups_run_event_loop_delay,
                  queue_latency.count());
    flow_groups_run_requested_time_ = SteadyTimestamp();
  }

  auto run_start_time = SteadyTimestamp::now();
  auto run_deadline =
      run_start_time + Worker::settings().flow_groups_run_yield_interval;
  bool exceeded_deadline = false;

  // Shuffle FlowGroups so that all get a chance to run even if
  // only a subset take the majority of the allowed runtime.
  folly::small_vector<int, NodeLocation::NUM_ALL_SCOPES> fg_ids(
      impl_->flow_groups_.size());
  std::iota(fg_ids.begin(), fg_ids.end(), 0);
  std::shuffle(fg_ids.begin(), fg_ids.end(), folly::ThreadLocalPRNG());
  for (auto idx : fg_ids) {
    exceeded_deadline =
        impl_->flow_groups_[idx].run(impl_->flow_meters_mutex_, run_deadline);
    if (exceeded_deadline) {
      // Run again after yielding to the event loop.
      STAT_INCR(Worker::stats(), flow_groups_run_deadline_exceeded);
      flow_groups_run_requested_time_ = SteadyTimestamp::now();
      evtimer_add(flow_groups_run_deadline_exceeded_,
                  EventLoop::onThisThread()->zero_timeout_);
      break;
    }
  }

  HISTOGRAM_ADD(Worker::stats(),
                flow_groups_run_time,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    SteadyTimestamp::now() - run_start_time)
                    .count());
}

bool Sender::applyFlowGroupsUpdate(FlowGroupsUpdate& update,
                                   StatsHolder* stats) {
  std::unique_lock<std::mutex> lock(impl_->flow_meters_mutex_);
  bool run = false;
  for (size_t i = 0; i < NodeLocation::NUM_ALL_SCOPES; ++i) {
    if (impl_->flow_groups_[i].applyUpdate(update.group_entries[i], stats)) {
      run = true;
    }
  }
  return run;
}

int Sender::registerOnSocketClosed(const Address& addr, SocketCallback& cb) {
  Socket* sock;

  if (addr.isClientAddress()) {
    auto pos = impl_->client_sockets_.find(addr.id_.client_);
    if (pos == impl_->client_sockets_.end()) {
      err = E::NOTFOUND;
      return -1;
    }
    sock = &pos->second;
  } else { // addr is a server address
    sock = findServerSocket(addr.id_.node_.index());
    if (!sock) {
      err = E::NOTFOUND;
      return -1;
    }
  }

  return sock->pushOnCloseCallback(cb);
}

void Sender::flushOutputAndClose(Status reason) {
  auto open_socket_count = 0;
  for (const auto& socket : impl_->server_sockets_) {
    if (socket && !socket->isClosed()) {
      socket->flushOutputAndClose(reason);
      ++open_socket_count;
    }
  }

  for (auto& it : impl_->client_sockets_) {
    if (!it.second.isClosed()) {
      if (reason == E::SHUTDOWN) {
        it.second.sendShutdown();
      }
      it.second.flushOutputAndClose(reason);
      ++open_socket_count;
    }
  }

  ld_log(open_socket_count ? dbg::Level::INFO : dbg::Level::SPEW,
         "Number of open sockets : %d",
         open_socket_count);
}

int Sender::closeClientSocket(ClientID cid, Status reason) {
  ld_check(cid.valid());
  ld_check(onMyWorker());

  auto pos = impl_->client_sockets_.find(cid);
  if (pos == impl_->client_sockets_.end()) {
    err = E::NOTFOUND;
    return -1;
  }

  Socket& s = pos->second;
  s.close(reason);
  return 0;
}

int Sender::closeServerSocket(NodeID peer, Status reason) {
  ld_check(onMyWorker());

  Socket* s = findServerSocket(peer.index());
  if (!s) {
    err = E::NOTFOUND;
    return -1;
  }

  if (!s->isClosed()) {
    s->close(reason);
  }

  return 0;
}

std::pair<uint32_t, uint32_t> Sender::closeAllSockets() {
  ld_check(onMyWorker());

  std::pair<uint32_t, uint32_t> sockets_closed = {0, 0};

  for (auto& entry : impl_->server_sockets_) {
    if (entry && !entry->isClosed()) {
      sockets_closed.first++;
      entry->close(E::SHUTDOWN);
    }
  }

  for (auto& entry : impl_->client_sockets_) {
    if (!entry.second.isClosed()) {
      sockets_closed.second++;
      entry.second.close(E::SHUTDOWN);
    }
  }

  return sockets_closed;
}

bool Sender::isClosed() {
  // Go over all sockets at shutdown to find pending work. This could help in
  // figuring which sockets are slow in draining buffers.
  auto go_over_all_sockets = !Worker::onThisThread()->isAcceptingWork();

  auto num_open_server_sockets = 0;
  Socket* max_pending_work_server = nullptr;
  auto server_with_max_pending_bytes = 0;
  for (const auto& socket : impl_->server_sockets_) {
    if (socket && !socket->isClosed()) {
      if (!go_over_all_sockets) {
        return false;
      }

      ++num_open_server_sockets;
      auto pending_bytes = socket->getBytesPending();
      if (server_with_max_pending_bytes < pending_bytes) {
        max_pending_work_server = socket.get();
        server_with_max_pending_bytes = pending_bytes;
      }
    }
  }

  auto num_open_client_sockets = 0;
  auto max_pending_work_clientID = ClientID();
  Socket* max_pending_work_client = nullptr;
  auto client_with_max_pending_bytes = 0;
  for (auto& it : impl_->client_sockets_) {
    if (!it.second.isClosed()) {
      if (!go_over_all_sockets) {
        return false;
      }

      ++num_open_client_sockets;
      auto& socket = it.second;
      auto pending_bytes = socket.getBytesPending();

      if (client_with_max_pending_bytes < pending_bytes) {
        max_pending_work_client = &socket;
        max_pending_work_clientID = it.first;
        client_with_max_pending_bytes = pending_bytes;
      }
    }
  }

  // None of the sockets are open return true.
  if (!num_open_client_sockets && !num_open_server_sockets) {
    return true;
  }

  RATELIMIT_INFO(std::chrono::seconds(5),
                 5,
                 "Sockets still open: Server socket count %d max stats 0x%p "
                 "pending %d msgs, Client socket count %d max stats clientID "
                 "%s socket 0x%p pending %d msgs",
                 num_open_server_sockets,
                 (void*)max_pending_work_server,
                 server_with_max_pending_bytes,
                 num_open_client_sockets,
                 max_pending_work_clientID.toString().c_str(),
                 (void*)max_pending_work_client,
                 client_with_max_pending_bytes);

  return false;
}

int Sender::checkConnection(NodeID nid,
                            ClientID* our_name_at_peer,
                            bool allow_unencrypted) {
  if (!nid.isNodeID()) {
    ld_check(false);
    err = E::INVALID_PARAM;
    return -1;
  }

  Socket* s = findServerSocket(nid.index());
  if (!s || !s->peer_name_.id_.node_.equalsRelaxed(nid)) {
    err = E::NOTFOUND;
    return -1;
  }

  if (!s->isSSL() && !allow_unencrypted && useSSLWith(nid)) {
    // We have a plaintext connection, but we need an encrypted one.
    err = E::SSLREQUIRED;
    return -1;
  }

  // check if the socket to destination has reached its buffer limit
  if (s->sizeLimitsExceeded()) {
    err = E::NOBUFS;
    return -1;
  }

  return s->checkConnection(our_name_at_peer);
}

int Sender::connect(NodeID nid, bool allow_unencrypted) {
  if (shutting_down_) {
    err = E::SHUTDOWN;
    return -1;
  }

  Socket* s = initServerSocket(nid, SocketType::DATA, allow_unencrypted);
  if (!s) {
    return -1;
  }

  return s->connect();
}

bool Sender::useSSLWith(NodeID nid,
                        bool* cross_boundary_out,
                        bool* authentication_out) {
  if (nid.index() == getMyNodeIndex()) {
    // Don't use SSL for connections to self
    return false;
  }

  // Determine whether we need to use SSL by comparing our location with the
  // location of the target node.
  bool cross_boundary = false;
  NodeLocationScope diff_level = Worker::settings().ssl_boundary;

  std::shared_ptr<ServerConfig> cfg(Worker::getConfig()->serverConfig());
  cross_boundary = cfg->getNodeSSL(getMyLocation(), nid, diff_level);

  // Determine whether we need to use an SSL socket for authentication.
  // We will use a SSL socket for authentication when the client or server
  // want to load their certificate.
  bool authentication = false;
  if (Worker::settings().ssl_load_client_cert) {
    authentication = true;
  }

  if (cross_boundary_out) {
    *cross_boundary_out = cross_boundary;
  }
  if (authentication_out) {
    *authentication_out = authentication;
  }

  return cross_boundary || authentication;
}

void Sender::processSocketsToClose() {
  for (auto& s : impl_->sockets_to_close_) {
    s.first->close(s.second);
  }
  impl_->sockets_to_close_.clear();
}

bool Sender::injectTrafficShapingEvent(FlowGroup& fg, Priority p) {
  double chance_percent =
      Worker::settings().message_error_injection_chance_percent;
  if (chance_percent != 0 &&
      Worker::settings().message_error_injection_status == E::CBREGISTERED &&
      !fg.isRunningBacklog() && fg.configured() && fg.enabled() &&
      folly::Random::randDouble(0, 100.0) <= chance_percent) {
    // Empty the meter so that all subsequent messages see a shortage
    // until more bandwdith is added.
    fg.resetMeter(p);
    return true;
  }
  return false;
}

void Sender::onSocketsToCloseAvailable(void* arg, short) {
  Sender* self = reinterpret_cast<Sender*>(arg);
  self->processSocketsToClose();
}

Socket* Sender::initServerSocket(NodeID nid,
                                 SocketType sock_type,
                                 bool allow_unencrypted) {
  ld_check(!shutting_down_);

  std::unique_ptr<Socket>* sock_slot = findSocketSlot(nid);
  if (sock_slot == nullptr) {
    // err set by findSocketSlot().
    return nullptr;
  }

  std::shared_ptr<ServerConfig> cfg(Worker::getConfig()->serverConfig());
  auto node_cfg = cfg->getNode(nid.index());
  // Don't try to connect if the node is not in config.
  // If the socket was already connected but the node removed from config,
  // it will be closed once noteConfigurationChanged() executes.
  if (!node_cfg) {
    err = E::NOTINCONFIG;
    return nullptr;
  }

  std::unique_ptr<Socket>& s = *sock_slot;
  if (s) {
    if (!s->isSSL() && !allow_unencrypted && useSSLWith(nid)) {
      // We have a plaintext connection, but now we need an encrypted one.
      // Scheduling this socket to be closed and moving it out of
      // server_sockets_ to initialize an SSL connection in its place.
      impl_->sockets_to_close_.push_back({std::move(s), E::SSLREQUIRED});
      LD_EV(event_active)(sockets_to_close_available_, EV_WRITE, 0);
      ld_check(!s);
    }
  }

  if (!s) {
    // Don't try to connect if we expect a generation and it's different than
    // what is in the config.
    if (nid.generation() == 0) {
      nid = NodeID(nid.index(), node_cfg->generation);
    } else if (node_cfg->generation != nid.generation()) {
      err = E::NOTINCONFIG;
      return nullptr;
    }

    // Determine whether we should use SSL and what the flow group scope is
    auto flow_group_scope = NodeLocationScope::NODE;
    if (nid.index() != getMyNodeIndex()) {
      if (getMyLocation() && node_cfg && node_cfg->location) {
        flow_group_scope =
            getMyLocation()->closestSharedScope(*node_cfg->location);
      } else {
        // Assume within the same region for now, since cross region should
        // use SSL and have a location specified in the config.
        flow_group_scope = NodeLocationScope::REGION;
      }
    }

    bool cross_boundary = false;
    bool ssl_authentication = false;
    bool use_ssl = !allow_unencrypted &&
        useSSLWith(nid, &cross_boundary, &ssl_authentication);

    try {
      auto& flow_group = selectFlowGroup(flow_group_scope);
      if (sock_type == SocketType::GOSSIP) {
        Worker* w = Worker::onThisThread();
        ld_check(w->worker_type_ == WorkerType::FAILURE_DETECTOR);
        if (w->settings().send_to_gossip_port) {
          // disable ssl for connection to the gossip port
          use_ssl = false;
        }
      }

      s.reset(new Socket(nid,
                         sock_type,
                         use_ssl ? ConnectionType::SSL : ConnectionType::PLAIN,
                         flow_group));

      if (use_ssl && !cross_boundary) {
        // If the connection does not cross the ssl boundary, limit the ciphers
        // to eNULL ciphers to reduce overhead.
        s->limitCiphersToENULL();
      }
    } catch (ConstructorFailed&) {
      if (err == E::NOTINCONFIG || err == E::NOSSLCONFIG) {
        return nullptr;
      }
      ld_check(false);
      err = E::INTERNAL;
      return nullptr;
    }
  }

  ld_check(s != nullptr);
  return s.get();
}

FlowGroup& Sender::selectFlowGroup(NodeLocationScope starting_scope) {
  // Search for a configured FlowGroup with the smallest scope.
  // Note: Scope NODE and ROOT are always configured (defaulting to
  //       disabled -- i.e. no limits). So this search will always
  //       succeed even if in a cluster without any FlowGroups
  //       enforcing a policy.
  while (starting_scope < NodeLocationScope::ROOT &&
         !impl_->flow_groups_[static_cast<int>(starting_scope)].configured()) {
    starting_scope = NodeLocation::nextGreaterScope(starting_scope);
  }
  return impl_->flow_groups_[static_cast<int>(starting_scope)];
}

Sockaddr Sender::getSockaddr(const Address& addr) {
  if (addr.isClientAddress()) {
    auto pos = impl_->client_sockets_.find(addr.id_.client_);
    if (pos != impl_->client_sockets_.end()) {
      ld_check(pos->second.peer_name_ == addr);
      return pos->second.peer_sockaddr_;
    }
  } else { // addr is a server address
    Socket* s = findServerSocket(addr.id_.node_.index());
    if (s && s->peer_name_.id_.node_.equalsRelaxed(addr.id_.node_)) {
      return s->peer_sockaddr_;
    }
  }

  return Sockaddr::INVALID;
}

ConnectionType Sender::getSockConnType(const Address& addr) {
  if (addr.isClientAddress()) {
    auto pos = impl_->client_sockets_.find(addr.id_.client_);
    if (pos != impl_->client_sockets_.end()) {
      ld_check(pos->second.peer_name_ == addr);
      return pos->second.getConnType();
    }
  } else { // addr is a server address
    Socket* s = findServerSocket(addr.id_.node_.index());
    if (s && s->peer_name_.id_.node_.equalsRelaxed(addr.id_.node_)) {
      return s->getConnType();
    }
  }

  return ConnectionType::NONE;
}

Socket* FOLLY_NULLABLE Sender::getSocket(const ClientID& cid) {
  ld_check(onMyWorker());

  if (shutting_down_) {
    err = E::SHUTDOWN;
    return nullptr;
  }

  auto pos = impl_->client_sockets_.find(cid);
  if (pos == impl_->client_sockets_.end()) {
    err = E::UNREACHABLE;
    return nullptr;
  }
  return &pos->second;
}

Socket* FOLLY_NULLABLE Sender::getSocket(const NodeID& nid,
                                         const Message& msg) {
  ld_check(onMyWorker());

  if (shutting_down_) {
    err = E::SHUTDOWN;
    return nullptr;
  }

  SocketType sock_type;
  Worker* w = Worker::onThisThread();
  if (w->worker_type_ == WorkerType::FAILURE_DETECTOR) {
    ld_check(Socket::allowedOnGossipConnection(msg.type_));
    sock_type = SocketType::GOSSIP;
  } else {
    sock_type = SocketType::DATA;
  }

  Socket* sock = initServerSocket(nid, sock_type, msg.allowUnencrypted());
  if (!sock) {
    // err set by initServerSocket()
    return nullptr;
  }

  int rv = sock->connect();

  if (rv != 0 && err != E::ALREADY && err != E::ISCONN) {
    // err can't be UNREACHABLE because sock must be a server socket
    ld_check(err == E::UNROUTABLE || err == E::DISABLED || err == E::SYSLIMIT ||
             err == E::NOMEM || err == E::INTERNAL);
    return nullptr;
  }

  // sock is now connecting or connected, send msg
  ld_assert(sock->connect() == -1 && (err == E::ALREADY || err == E::ISCONN));
  return sock;
}

Socket* FOLLY_NULLABLE Sender::getSocket(const Address& addr,
                                         const Message& msg) {
  return addr.isClientAddress() ? getSocket(addr.asClientID())
                                : getSocket(addr.asNodeID(), msg);
}

Socket* FOLLY_NULLABLE Sender::findSocket(const Address& addr) {
  if (addr.isClientAddress()) {
    // err, if any, set by getSocket().
    return getSocket(addr.asClientID());
  }

  Socket* sock = nullptr;
  NodeID nid = addr.asNodeID();
  std::unique_ptr<Socket>* sock_slot = findSocketSlot(nid);
  if (sock_slot) {
    sock = sock_slot->get();
    if (!sock) {
      err = E::NOTINCONFIG;
    }
  }
  return sock;
}

std::unique_ptr<Socket>* FOLLY_NULLABLE
Sender::findSocketSlot(const NodeID& nid) {
  ld_check(nid.isNodeID());
  node_index_t idx = nid.index();
  ld_check(idx >= 0);
  if (idx >= impl_->server_sockets_.size()) {
    err = E::NOTINCONFIG;
    return nullptr;
  }

  std::unique_ptr<Socket>& sock = impl_->server_sockets_[nid.index()];
  if (sock) {
    ld_check(!sock->peer_name_.isClientAddress());
    ld_check(sock->peer_name_.id_.node_.index() == nid.index());
    if (!nid.equalsRelaxed(sock->peer_name_.id_.node_)) {
      // we assume that the address of a Socket in server_sockets_[]
      // is always in config. noteConfigurationChanged() keeps
      // server_sockets_[] in sync with config.
      err = E::NOTINCONFIG;
      return nullptr;
    }
  }
  return &sock;
}

const PrincipalIdentity* Sender::getPrincipal(const Address& addr) {
  if (addr.isClientAddress()) {
    auto pos = impl_->client_sockets_.find(addr.id_.client_);
    if (pos != impl_->client_sockets_.end()) {
      ld_check(pos->second.peer_name_ == addr);
      return &pos->second.principal_;
    }
  } else { // addr is a server address
    Socket* s = findServerSocket(addr.id_.node_.index());
    if (s && s->peer_name_.id_.node_.equalsRelaxed(addr.id_.node_)) {
      // server_sockets_ principals should all be empty, this is because
      // the server_sockets_ will always be on the sender side, as in they
      // send the initial HELLO_Message. This means that they will never have
      // receive a HELLO_Message thus never have their principal set.
      ld_check(s->principal_.type == "");
      return &s->principal_;
    }
  }

  return nullptr;
}

int Sender::setPrincipal(const Address& addr, PrincipalIdentity principal) {
  if (addr.isClientAddress()) {
    auto pos = impl_->client_sockets_.find(addr.id_.client_);
    if (pos != impl_->client_sockets_.end()) {
      ld_check(pos->second.peer_name_ == addr);

      // Whenever a HELLO_Message is sent, a new client socket is
      // created on the server side. Meaning that whenever this function is
      // called, the principal should be empty.
      ld_check(pos->second.principal_.type == "");

      // See if this principal requires specialized traffic tagging.
      Worker* w = Worker::onThisThread();
      auto scfg = w->getServerConfig();
      for (auto identity : principal.identities) {
        auto principal_settings = scfg->getPrincipalByName(&identity.second);
        if (principal_settings != nullptr &&
            principal_settings->egress_dscp != 0) {
          pos->second.setDSCP(principal_settings->egress_dscp);
          break;
        }
      }

      pos->second.principal_ = std::move(principal);
      return 0;
    }
  } else { // addr is a server address
    Socket* s = findServerSocket(addr.id_.node_.index());
    if (s && s->peer_name_.id_.node_.equalsRelaxed(addr.id_.node_)) {
      // server_sockets_ should never have setPrincipal called as they
      // should always be the calling side, as in they always send the initial
      // HELLO_Message.
      ld_check(false);
      return 0;
    }
  }

  return -1;
}

const std::string* Sender::getCSID(const Address& addr) {
  if (addr.isClientAddress()) {
    auto pos = impl_->client_sockets_.find(addr.id_.client_);
    if (pos != impl_->client_sockets_.end()) {
      ld_check(pos->second.peer_name_ == addr);
      return &pos->second.csid_;
    }
  } else { // addr is a server address
    Socket* s = findServerSocket(addr.id_.node_.index());
    if (s && s->peer_name_.id_.node_.equalsRelaxed(addr.id_.node_)) {
      // server_sockets_ csid should all be empty, this is because
      // the server_sockets_ will always be on the sender side, as in they
      // send the initial HELLO_Message. This means that they will never have
      // receive a HELLO_Message thus never have their csid set.
      ld_check(s->csid_ == "");
      return &s->csid_;
    }
  }

  return nullptr;
}

int Sender::setCSID(const Address& addr, std::string csid) {
  if (addr.isClientAddress()) {
    auto pos = impl_->client_sockets_.find(addr.id_.client_);
    if (pos != impl_->client_sockets_.end()) {
      ld_check(pos->second.peer_name_ == addr);

      // Whenever a HELLO_Message is sent, a new client socket is
      // created on the server side. Meaning that whenever this function is
      // called, the principal should be empty.
      ld_check(pos->second.csid_ == "");
      pos->second.csid_ = std::move(csid);
      return 0;
    }
  } else { // addr is a server address
    Socket* s = findServerSocket(addr.id_.node_.index());
    if (s && s->peer_name_.id_.node_.equalsRelaxed(addr.id_.node_)) {
      // server_sockets_ should never have setCSID called as they
      // should always be the calling side, as in they always send the initial
      // HELLO_Message.
      ld_check(false);
      return 0;
    }
  }

  return -1;
}

std::string Sender::getClientLocation(const ClientID& cid) {
  Socket* sock = getSocket(cid);
  if (!sock) {
    RATELIMIT_ERROR(std::chrono::seconds(1),
                    1,
                    "Could not find socket for connection: %s",
                    describeConnection(cid).c_str());
    return "";
  }
  return sock->peer_location_;
}

void Sender::setClientLocation(const ClientID& cid,
                               const std::string& location) {
  Socket* sock = getSocket(cid);
  if (!sock) {
    RATELIMIT_ERROR(std::chrono::seconds(1),
                    1,
                    "Could not find socket for connection: %s",
                    describeConnection(cid).c_str());
    return;
  }
  sock->peer_location_ = location;
}

void Sender::setPeerConfigVersion(const Address& addr,
                                  const Message& msg,
                                  config_version_t version) {
  Socket* sock = getSocket(addr, msg);
  if (!sock) {
    ld_check(err == E::SHUTDOWN);
    ld_info("Shutting down. Cannot set peer config version.");
    return;
  }
  sock->setPeerConfigVersion(version);
}

X509* Sender::getPeerCert(const Address& addr) {
  if (addr.isClientAddress()) {
    auto pos = impl_->client_sockets_.find(addr.id_.client_);
    if (pos != impl_->client_sockets_.end()) {
      ld_check(pos->second.peer_name_ == addr);
      if (pos->second.isSSL())
        return pos->second.getPeerCert();
    }
  } else { // addr is a server address
    Socket* s = findServerSocket(addr.id_.node_.index());
    if (s && s->peer_name_.id_.node_.equalsRelaxed(addr.id_.node_)) {
      if (s->isSSL()) {
        X509* cert = s->getPeerCert();

        // Logdevice server nodes are required to send their certificate
        // to the client when creating an SSL socket.
        ld_check(cert);
        return cert;
      }
    }
  }

  return nullptr;
}

Sockaddr Sender::thisThreadSockaddr(const Address& addr) {
  Worker* w = Worker::onThisThread();

  ld_check(w);

  return w->sender().getSockaddr(addr);
}

Sockaddr Sender::sockaddrOrInvalid(const Address& addr) {
  Worker* w = Worker::onThisThread(false);
  if (!w) {
    return Sockaddr();
  }
  return w->sender().getSockaddr(addr);
}

NodeID Sender::getNodeID(const Address& addr) const {
  if (!addr.isClientAddress()) {
    return addr.id_.node_;
  }

  auto it = impl_->client_sockets_.find(addr.id_.client_);
  return it != impl_->client_sockets_.end() ? it->second.peer_node_id_
                                            : NodeID();
}

void Sender::setPeerNodeID(const Address& addr, NodeID node_id) {
  auto it = impl_->client_sockets_.find(addr.id_.client_);
  if (it != impl_->client_sockets_.end()) {
    NodeID& peer_node = it->second.peer_node_id_;
    peer_node = node_id;
  }
}

std::string Sender::describeConnection(const Address& addr) {
  if (!ThreadID::isWorker()) {
    return addr.toString();
  }
  Worker* w = Worker::onThisThread();
  ld_check(w);

  if (w->shuttingDown()) {
    return addr.toString();
  }

  // index of worker to which a connection from or to peer identified by
  // this Address is assigned
  ClientIdxAllocator& alloc = w->processor_->clientIdxAllocator();
  std::pair<WorkerType, worker_id_t> assignee = addr.isClientAddress()
      ? alloc.getWorkerId(addr.id_.client_)
      : std::make_pair(w->worker_type_, w->idx_);

  std::string res;
  res.reserve(64);

  res += Worker::getName(assignee.first, assignee.second);
  res += ":";
  res += addr.toString();
  if (!addr.isClientAddress() ||
      (assignee.first == w->worker_type_ && assignee.second == w->idx_)) {
    const Sockaddr& sa = w->sender().getSockaddr(addr);
    res += " (";
    res += sa.valid() ? sa.toString() : std::string("UNKNOWN");
    res += ")";
  }

  return res;
}

int Sender::noteDisconnectedClient(ClientID client_name) {
  ld_check(client_name.valid());
  ld_check(onMyWorker());

  if (Worker::onThisThread()->shuttingDown()) {
    // This instance might already be (partially) destroyed.
    return 0;
  }

  if (impl_->client_sockets_.count(client_name) == 0) {
    ld_critical("INTERNAL ERROR: the name of a disconnected client socket %s "
                "is not in the client map",
                client_name.toString().c_str());
    ld_check(0);
    err = E::NOTFOUND;
    return -1;
  }

  disconnected_clients_.push_front(client_name);

  return 0;
}

void DisconnectedClientCallback::operator()(Status st, const Address& name) {
  ld_debug("Sender's DisconnectedClientCallback called for socket %s "
           "with status %s",
           Sender::describeConnection(name).c_str(),
           error_name(st));

  ld_check(!active());

  Worker::onThisThread()->sender().noteDisconnectedClient(name.id_.client_);

  delete this;
}

void Sender::initMyLocation() {
  if (Worker::settings().server) {
    std::shared_ptr<ServerConfig> cfg(Worker::getConfig()->serverConfig());
    my_node_index_ = cfg->getMyNodeID().index();
    const Configuration::Node* node_cfg = cfg->getNode(my_node_index_);
    ld_check(node_cfg);
    my_location_ = node_cfg->location;
  } else {
    my_node_index_ = NODE_INDEX_INVALID;
    my_location_ = Worker::settings().client_location;
  }
}

folly::Optional<NodeLocation> Sender::getMyLocation() {
  if (!my_location_.hasValue()) {
    initMyLocation();
  }
  return my_location_;
}

node_index_t Sender::getMyNodeIndex() {
  if (!my_location_.hasValue()) {
    initMyLocation();
  }
  return my_node_index_;
}

void Sender::noteConfigurationChanged() {
  const std::shared_ptr<ServerConfig> cfg(Worker::getConfig()->serverConfig());
  const auto& nodes_cfg = cfg->getNodes();

  initMyLocation();

  for (int i = 0; i < impl_->server_sockets_.size(); i++) {
    std::unique_ptr<Socket>& s = impl_->server_sockets_[i];

    if (!s) {
      continue;
    }

    ld_check(!s->peer_name_.isClientAddress());
    ld_check(s->peer_name_.id_.node_.index() == i);

    auto it = nodes_cfg.find(i);
    if (it != nodes_cfg.end()) {
      const Sockaddr& newaddr =
          it->second.getSockaddr(s->getSockType(), s->getConnType());
      if (s->peer_name_.id_.node_.generation() == it->second.generation &&
          s->peer_sockaddr_ == newaddr) {
        continue;
      } else {
        ld_info("Configuration change detected for node %s. New generation "
                "count is %d. New IP address is %s. Destroying old socket.",
                Sender::describeConnection(Address(s->peer_name_.id_.node_))
                    .c_str(),
                it->second.generation,
                newaddr.toString().c_str());
      }

    } else {
      ld_info(
          "Node %s is no longer in cluster configuration. New cluster "
          "size is %zu. Destroying old socket.",
          Sender::describeConnection(Address(s->peer_name_.id_.node_)).c_str(),
          nodes_cfg.size());
    }

    s->close(E::NOTINCONFIG);
    s.reset();
  }

  impl_->server_sockets_.resize(cfg->getMaxNodeIdx() + 1);
}

bool Sender::bytesPendingLimitReached() {
  return getBytesPending() >
      Worker::settings().outbufs_mb_max_per_thread * 1024 * 1024;
}

void Sender::queueMessageCompletion(std::unique_ptr<Message> msg,
                                    const Address& to,
                                    Status s,
                                    const SteadyTimestamp t) {
  auto mc = std::make_unique<MessageCompletion>(std::move(msg), to, s, t);
  completed_messages_.push_back(*mc.release());
  LD_EV(event_active)(completed_messages_available_, EV_WRITE, 0);
}

std::string Sender::dumpQueuedMessages(Address addr) const {
  std::map<MessageType, int> counts;
  if (addr.valid()) {
    const Socket* socket = nullptr;
    if (addr.isClientAddress()) {
      const auto it = impl_->client_sockets_.find(addr.id_.client_);
      if (it != impl_->client_sockets_.end()) {
        socket = &it->second;
      }
    } else {
      auto idx = addr.id_.node_.index();
      if (idx < impl_->server_sockets_.size()) {
        socket = impl_->server_sockets_[idx].get();
      }
    }
    if (socket == nullptr) {
      // Unexpected but not worth asserting on (crashing the server) since
      // this is debugging code
      return "<socket not found>";
    }
    socket->dumpQueuedMessages(&counts);
  } else {
    for (const auto& entry : impl_->server_sockets_) {
      if (entry != nullptr) {
        entry->dumpQueuedMessages(&counts);
      }
    }
    for (const auto& entry : impl_->client_sockets_) {
      entry.second.dumpQueuedMessages(&counts);
    }
  }
  std::unordered_map<std::string, int> strmap;
  for (const auto& entry : counts) {
    strmap[messageTypeNames[entry.first].c_str()] = entry.second;
  }
  return folly::toJson(folly::toDynamic(strmap));
}

void Sender::forEachSocket(std::function<void(const Socket&)> cb) const {
  for (const auto& entry : impl_->server_sockets_) {
    if (entry) {
      cb(*entry);
    }
  }
  for (const auto& entry : impl_->client_sockets_) {
    cb(entry.second);
  }
}

int Sender::getClientSocketRef(const ClientID cid, WeakRef<Socket>& ref) {
  ld_check(cid.valid());
  auto pos = impl_->client_sockets_.find(cid);
  if (pos == impl_->client_sockets_.end()) {
    return -1;
  }

  ref = pos->second.ref_holder_.ref();
  return 0;
}

void Sender::maybeScheduleRunFlowGroups(FlowGroup& flow_group) {
  if (flow_group.canRunPriorityQ() &&
      flow_groups_run_requested_time_ == SteadyTimestamp()) {
    flow_groups_run_requested_time_ = SteadyTimestamp::now();
    // Activate low priority event which will aggregate demand until the
    // worker becomes idle.
    LD_EV(event_active)(flow_groups_run_requested_, EV_WRITE, 0);
    // Schedule deadline timer as a fail safe in case the Worker is
    // saturated and never becomes idle.
    Worker* w = Worker::onThisThread();
    evtimer_add(flow_groups_run_deadline_exceeded_,
                w->getCommonTimeout(w->settings().flow_groups_run_deadline));
  }
}

void Sender::forAllClientSockets(std::function<void(Socket&)> fn) {
  for (auto& it : impl_->client_sockets_) {
    fn(it.second);
  }
}

}} // namespace facebook::logdevice
