/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/server/FailureDetector.h"

#include <folly/Memory.h>
#include <folly/Random.h>
#include <folly/small_vector.h>

#include "logdevice/common/ClusterState.h"
#include "logdevice/common/GetClusterStateRequest.h"
#include "logdevice/common/Sender.h"
#include "logdevice/common/Socket.h"
#include "logdevice/common/configuration/Configuration.h"
#include "logdevice/common/stats/ServerHistograms.h"
#include "logdevice/common/stats/Stats.h"
#include "logdevice/common/util.h"
#include "logdevice/server/ServerProcessor.h"

namespace facebook { namespace logdevice {

class FailureDetector::RandomSelector : public FailureDetector::NodeSelector {
  NodeID getNode(FailureDetector* detector) override {
    auto config = detector->getServerConfig();
    auto& nodes = config->getNodes();

    NodeID this_node = config->getMyNodeID();

    // viable candidates are all sequencer/storage nodes we are able to talk to,
    // other than this one
    std::vector<NodeID> candidates;
    for (const auto& it : nodes) {
      node_index_t idx = it.first;
      if (idx != this_node.index() && detector->isValidDestination(idx)) {
        NodeID candidate_id(idx, it.second.generation);
        candidates.push_back(candidate_id);
      }
    }

    if (candidates.size() == 0) {
      // no valid candidates
      return NodeID();
    }

    return candidates[folly::Random::rand32((uint32_t)candidates.size())];
  };
};

class FailureDetector::RoundRobinSelector
    : public FailureDetector::NodeSelector {
 public:
  NodeID getNode(FailureDetector* detector) override {
    auto config = detector->getServerConfig();

    auto& nodes = config->getNodes();
    auto this_node = config->getMyNodeID();

    if (next_node_idx_.hasValue() && !nodes.count(next_node_idx_.value())) {
      // The node was removed from config.
      next_node_idx_ = folly::none;
    }

    if (!next_node_idx_.hasValue() || iters_ >= nodes.size()) {
      // Every `nodes.size()' iterations reset next_node_idx_ to this
      // node's neighbour. This is meant to avoid all nodes gossiping to the
      // same node in each round, even in the presence of sporadic down nodes.

      auto it = nodes.find(this_node.index());
      ld_check(it != nodes.end());
      ++it;
      if (it == nodes.end()) {
        it = nodes.begin();
      }
      next_node_idx_ = it->first;

      iters_ = 0;
    }

    NodeID target;
    for (int attempts = nodes.size(); attempts > 0; --attempts) {
      ld_assert(nodes.count(next_node_idx_.value()));

      target = NodeID(
          next_node_idx_.value(), nodes.at(next_node_idx_.value()).generation);
      ld_check(config->getNode(target) != nullptr);

      auto it = nodes.find(next_node_idx_.value());
      ld_check(it != nodes.end());
      ++it;
      if (it == nodes.end()) {
        it = nodes.begin();
      }

      next_node_idx_ = it->first;

      if (target != this_node && detector->isValidDestination(target.index())) {
        // target found
        break;
      }
    }

    ++iters_;

    target = (target != this_node) ? target : NodeID();
    return target;
  };

 private:
  // index of a next target within the list of all sequencer nodes
  folly::Optional<node_index_t> next_node_idx_;

  // number of calls to getNode()
  size_t iters_{0};
};

// Request used to complete initialization that needs to
// happen on the thread only
class FailureDetector::InitRequest : public Request {
 public:
  explicit InitRequest(FailureDetector* parent)
      : Request(RequestType::FAILURE_DETECTOR_INIT), parent_(parent) {}

  Execution execute() override {
    parent_->startClusterStateTimer();
    return Execution::COMPLETE;
  }

  WorkerType getWorkerTypeAffinity() override {
    return WorkerType::FAILURE_DETECTOR;
  }

 private:
  FailureDetector* parent_;
};

FailureDetector::FailureDetector(UpdateableSettings<GossipSettings> settings,
                                 ServerProcessor* processor,
                                 StatsHolder* stats,
                                 bool attach)
    : settings_(std::move(settings)), stats_(stats), processor_(processor) {
  size_t max_nodes = processor->settings()->max_nodes;

  // Preallocating makes it easier to handle cluster expansion and shrinking.
  auto new_list = new std::atomic<std::chrono::milliseconds>[max_nodes];
  auto current_time_ms = getCurrentTimeInMillis();
  for (int i = 0; i < max_nodes; ++i) {
    new_list[i].store(current_time_ms);
  }
  last_suspected_at_.reset(new_list);

  // All nodes are initially marked as dead.
  gossip_list_.assign(max_nodes, std::numeric_limits<uint32_t>::max());
  gossip_ts_.assign(max_nodes, std::chrono::milliseconds::zero());
  nodes_.assign(max_nodes, Node{NodeState::DEAD, false});

  // none of the nodes requested failover yet
  failover_list_.assign(max_nodes, std::chrono::milliseconds::zero());
  suspect_matrix_.assign(max_nodes, std::vector<uint8_t>(max_nodes, 0));

  switch (settings_->mode) {
    case GossipSettings::SelectionMode::RANDOM:
      selector_.reset(new RandomSelector());
      break;
    case GossipSettings::SelectionMode::ROUND_ROBIN:
      selector_.reset(new RoundRobinSelector());
      break;
    default:
      ld_error("Invalid gossip mode(%d)", (int)settings_->mode);
      ld_check(false);
  }

  start_time_ = std::chrono::steady_clock::now();
  instance_id_ = std::chrono::milliseconds(processor->getServerInstanceId());
  ld_info(
      "Failure Detector starting with instance id: %lu", instance_id_.count());

  auto cs = processor->cluster_state_.get();
  for (size_t i = 0; i < nodes_.size(); ++i) {
    cs->setNodeState(i, ClusterState::NodeState::DEAD);
  }

  if (attach) {
    std::unique_ptr<Request> rq = std::make_unique<InitRequest>(this);
    int rv = processor->postRequest(rq);
    if (rv) {
      ld_warning("Unable to post InitRequest, err=%d", rv);
    }
  }
}

FailureDetector::FailureDetector(UpdateableSettings<GossipSettings> settings,
                                 ServerProcessor* processor,
                                 bool attach)
    : FailureDetector(std::move(settings), processor, nullptr, attach) {}

StatsHolder* FailureDetector::getStats() {
  return stats_;
}

void FailureDetector::startClusterStateTimer() {
  Worker* w = Worker::onThisThread();
  auto cs = Worker::getClusterState();
  if (!cs) {
    ld_info("Invalid get-cluster-state");
    buildInitialState();
  } else {
    cs_timer_.assign([=] {
      if (waiting_for_cluster_state_) {
        ld_info("Timed out waiting for cluster state reply.");
        buildInitialState();
      }
    });
    cs_timer_.activate(settings_->gcs_wait_duration, &w->commonTimeouts());

    ld_info("Sending GET_CLUSTER_STATE to build initial FD cluster view");
    sendGetClusterState();
  }
}

void FailureDetector::sendGetClusterState() {
  Worker* w = Worker::onThisThread();
  auto settings = w->processor_->settings();

  auto cb = [&](Status status,
                const std::vector<uint8_t>& cs_update,
                std::vector<node_index_t> boycotted_nodes) {
    if (status != E::OK) {
      ld_error(
          "Unable to refresh cluster state: %s", error_description(status));
      return;
    }

    std::vector<std::string> dead;
    for (int i = 0; i < cs_update.size(); i++) {
      if (cs_update[i]) {
        dead.push_back("N" + std::to_string(i));
      }
    }

    std::vector<std::string> boycotted_tostring;
    boycotted_tostring.reserve(boycotted_nodes.size());
    for (auto index : boycotted_nodes) {
      boycotted_tostring.emplace_back("N" + std::to_string(index));
    }

    ld_info("Cluster state received with %lu dead nodes (%s) and %lu boycotted "
            "nodes (%s)",
            dead.size(),
            folly::join(',', dead).c_str(),
            boycotted_tostring.size(),
            folly::join(',', boycotted_tostring).c_str());

    buildInitialState(cs_update, std::move(boycotted_nodes));
  };

  std::unique_ptr<Request> req = std::make_unique<GetClusterStateRequest>(
      settings->get_cluster_state_timeout,
      settings->get_cluster_state_wave_timeout,
      std::move(cb));
  auto result = req->execute();
  if (result == Request::Execution::CONTINUE) {
    req.release();
  }
}

void FailureDetector::buildInitialState(
    const std::vector<uint8_t>& cs_update,
    std::vector<node_index_t> boycotted_nodes) {
  if (waiting_for_cluster_state_ == false) {
    return;
  }

  cs_timer_.cancel();
  waiting_for_cluster_state_ = false;
  ld_info("Wait over%s", cs_update.size() ? " (cluster state received)" : "");

  if (cs_update.size()) {
    auto config = getServerConfig();
    node_index_t my_idx = config->getMyNodeID().index();
    // Set the correct state of nodes instead of DEAD
    FailureDetector::NodeState state;

    auto cs = getClusterState();
    for (size_t i = 0; i <= config->getMaxNodeIdx(); ++i) {
      if (i == my_idx)
        continue;

      cs->setNodeState(i, static_cast<ClusterState::NodeState>(cs_update[i]));
      state = cs->isNodeAlive(i) ? NodeState::ALIVE : NodeState::DEAD;
      RATELIMIT_INFO(std::chrono::seconds(1),
                     10,
                     "N%zu transitioned to %s",
                     i,
                     (state == NodeState::ALIVE) ? "ALIVE" : "DEAD");
      nodes_[i].state = state;
    }

    cs->setBoycottedNodes(std::move(boycotted_nodes));
  }

  if (!isolation_checker_) {
    ld_info("Initializing DomainIsolationChecker");
    isolation_checker_ = std::make_unique<DomainIsolationChecker>();
    isolation_checker_->init();
  }

  // Tell others that this node is alive, so that they can
  // start sending gossips.
  broadcastBringup();

  // Start gossiping after we have got a chance to build FD state.
  // If cluster-state reply doesn't come, it is fine, since we will
  // move every potentially ALIVE node from DEAD to SUSPECT on receiving
  // the first regular gossip message after the 'min_gossips_for_stable_state'
  // limit.
  startGossiping();
}

bool FailureDetector::checkSkew(const GOSSIP_Message& msg) {
  const auto millis_now = getCurrentTimeInMillis();
  long skew = labs(millis_now.count() - msg.sent_time_.count());
  HISTOGRAM_ADD(Worker::stats(), gossip_recv_latency, skew * 1000);

  auto threshold = settings_->gossip_time_skew_threshold;
  if (skew >= std::min(std::chrono::milliseconds(1000), threshold).count()) {
    STAT_INCR(getStats(), gossips_delayed_total);
    RATELIMIT_WARNING(std::chrono::seconds(1),
                      5,
                      "A delayed gossip received from %s, delay:%lums"
                      ", sender time:%lu, now:%lu",
                      msg.gossip_node_.toString().c_str(),
                      skew,
                      msg.sent_time_.count(),
                      millis_now.count());
  }

  bool drop_gossip = (skew >= threshold.count());

  if (drop_gossip) {
    RATELIMIT_WARNING(std::chrono::seconds(1),
                      10,
                      "Dropping delayed gossip received from %s, delay:%lums"
                      ", sender time:%lu, now:%lu",
                      msg.gossip_node_.toString().c_str(),
                      skew,
                      msg.sent_time_.count(),
                      millis_now.count());
    STAT_INCR(getStats(), gossips_dropped_total);
  }
  return drop_gossip;
}

bool FailureDetector::isValidInstanceId(std::chrono::milliseconds id,
                                        node_index_t idx) {
  const auto now = getCurrentTimeInMillis();
  if (id > now + settings_->gossip_time_skew_threshold) {
    RATELIMIT_WARNING(std::chrono::seconds(1),
                      1,
                      "Rejecting the instance id:%lu for N%hu as its too far "
                      "in future, now:%lu",
                      id.count(),
                      idx,
                      now.count());
    return false;
  }
  return true;
}

void FailureDetector::noteConfigurationChanged() {
  if (isolation_checker_) {
    isolation_checker_->noteConfigurationChanged();
  }
}

void FailureDetector::gossip() {
  auto config = getServerConfig();
  size_t size = config->getMaxNodeIdx() + 1;

  if (size > gossip_list_.size()) {
    // ignore extra nodes if the size of the cluster exceeds max_nodes
    RATELIMIT_ERROR(std::chrono::minutes(1),
                    1,
                    "Number of nodes in the cluster exceeds the max_nodes "
                    "limit (%zu)",
                    gossip_list_.size());
    size = gossip_list_.size();
  }

  std::lock_guard<std::mutex> lock(mutex_);

  NodeID dest = selector_->getNode(this);

  if (shouldDumpState()) {
    ld_info("FD state before constructing gossip message for %s",
            dest.isNodeID() ? dest.toString().c_str() : "none");
    dumpFDState();
  }

  NodeID this_node = config->getMyNodeID();
  auto now = SteadyTimestamp::now();
  // bump other nodes' entry in gossip list if at least 1 gossip_interval passed
  if (now >= last_gossip_tick_time_ + settings_->gossip_interval) {
    for (size_t i = 0; i < size; ++i) {
      if (i != this_node.index()) {
        // overflow handling
        gossip_list_[i] = std::max(gossip_list_[i], gossip_list_[i] + 1);
      }
    }
    last_gossip_tick_time_ = now;
  }
  // stayin' alive
  gossip_list_[this_node.index()] = 0;
  gossip_ts_[this_node.index()] = instance_id_;
  failover_list_[this_node.index()] =
      failover_.load() ? instance_id_ : std::chrono::milliseconds::zero();

  // Don't trigger other nodes' state transition until we receive min number
  // of gossips. The GCS reply is not same as a regular gossip, and therefore
  // doesn't contain gossip_list_ values. The default values of gossip_list_
  // mean that this node has never heard from other cluster nodes, which
  // translates to DEAD state.
  // It is possible that a GCS reply can move a node to ALIVE
  // and immediately after that detectFailures() will detect the node as DEAD
  // based on gossip_list[], which will again move the node into
  // DEAD->SUSPECT->ALIVE state machine.
  if (num_gossips_received_ >= settings_->min_gossips_for_stable_state) {
    detectFailures(this_node.index(), size);
  } else {
    // In normal scenario, 'this' node will move out of suspect state,
    // either upon expiration of suspect timer, or eventually because of
    // calling detectFailures() above(which calls updateNodeState())
    //
    // But in cases where
    // a) we hit the libevent timer bug, and
    // b) we can't pick a node to send a gossip to, or
    //    there's only 1 node in the cluster
    // we still want to transition out of suspect state when it expires,
    // and change cluster_state_ accordingly.
    updateNodeState(
        this_node.index(), false /*dead*/, true /*self*/, false /*failover*/);
  }

  updateBoycottedNodes();

  if (!dest.isNodeID()) {
    RATELIMIT_WARNING(std::chrono::minutes(1),
                      1,
                      "Unable to find a node to send a gossip message to");
    return;
  }

  // construct a GOSSIP message and send it to dest
  GOSSIP_Message::gossip_list_t gossip_list(
      gossip_list_.begin(), gossip_list_.begin() + size);

  /* TODO: t14673640: remove when no one sends matrices
   * on an unconnected socket */
  GOSSIP_Message::suspect_matrix_t suspect_matrix(0);

  GOSSIP_Message::GOSSIP_flags_t flags = 0;
  GOSSIP_Message::gossip_ts_t gossip_ts_list(
      gossip_ts_.begin(), gossip_ts_.begin() + size);

  // If at least one entry in the failover list is non-zero, include the
  // list in the message.
  GOSSIP_Message::failover_list_t failover_list;
  auto it =
      std::max_element(failover_list_.begin(), failover_list_.begin() + size);
  if (it != failover_list_.end() && *it > std::chrono::milliseconds::zero()) {
    flags |= GOSSIP_Message::HAS_FAILOVER_LIST_FLAG;
    failover_list.assign(failover_list_.begin(), failover_list_.begin() + size);
  }

  const auto boycott_map = getBoycottTracker().getBoycottsForGossip();
  std::vector<Boycott> boycotts;
  boycotts.reserve(boycott_map.size());

  std::transform(boycott_map.cbegin(),
                 boycott_map.cend(),
                 std::back_inserter(boycotts),
                 [](const auto& entry) { return entry.second; });

  // bump the message sequence number
  ++current_msg_id_;
  int rv = sendGossipMessage(
      dest,
      std::make_unique<GOSSIP_Message>(this_node,
                                       std::move(gossip_list),
                                       instance_id_,
                                       getCurrentTimeInMillis(),
                                       std::move(gossip_ts_list),
                                       std::move(failover_list),
                                       std::move(suspect_matrix),
                                       std::move(boycotts),
                                       flags,
                                       current_msg_id_));

  if (rv != 0) {
    RATELIMIT_WARNING(std::chrono::seconds(1),
                      10,
                      "Failed to send GOSSIP to node %s: %s",
                      Sender::describeConnection(Address(dest)).c_str(),
                      error_description(err));
  }

  if (shouldDumpState()) {
    ld_info("FD state after constructing gossip message for %s",
            dest.toString().c_str());
    dumpFDState();
  }
}

namespace {
template <typename T>
bool update_min(T& x, const T val) {
  if (val <= x) {
    x = val;
    return true;
  }
  return false;
}
} // namespace

bool FailureDetector::processFlags(const GOSSIP_Message& msg) {
  std::string msg_type = flagsToString(msg.flags_);
  if (msg_type == "unknown") {
    // no flags or HAS_FAILOVER_LIST_FLAG
    return false;
  }

  RATELIMIT_INFO(std::chrono::seconds(1),
                 5,
                 "Received %s message from %s with instance id:%lu"
                 ", sent_time:%lums",
                 msg_type.c_str(),
                 msg.gossip_node_.toString().c_str(),
                 msg.instance_id_.count(),
                 msg.sent_time_.count());
  if (shouldDumpState()) {
    dumpFDState();
  }

  auto config = getServerConfig();
  node_index_t my_idx = config->getMyNodeID().index();
  node_index_t sender_idx = msg.gossip_node_.index();
  ld_check(my_idx != sender_idx);

  if (sender_idx > gossip_list_.size() - 1) {
    RATELIMIT_ERROR(std::chrono::seconds(1),
                    1,
                    "Sender (%s) is not present in our config, "
                    "max_nodes limit:%zu",
                    msg.gossip_node_.toString().c_str(),
                    gossip_list_.size());
    return true;
  }

  if (!isValidInstanceId(msg.instance_id_, sender_idx)) {
    return true;
  }

  // marking sender alive
  gossip_list_[sender_idx] = 0;
  gossip_ts_[sender_idx] = msg.instance_id_;
  failover_list_[sender_idx] = std::chrono::milliseconds::zero();
  suspect_matrix_[my_idx][sender_idx] = 0;

  if (msg.flags_ & GOSSIP_Message::SUSPECT_STATE_FINISHED) {
    if (nodes_[sender_idx].state != NodeState::ALIVE) {
      updateDependencies(sender_idx, NodeState::ALIVE, false /*failover*/);
      ld_info("N%hu transitioned to ALIVE as a result of receiving "
              "suspect-state-finished message, FD State:"
              "(gossip: %u, instance-id: %lu, failover: %lu)",
              sender_idx,
              gossip_list_[sender_idx],
              gossip_ts_[sender_idx].count(),
              failover_list_[sender_idx].count());
    }
  } else {
    updateNodeState(sender_idx, false, false, false);
  }

  if (shouldDumpState()) {
    ld_info("FD state after receiving %s message from %s",
            msg_type.c_str(),
            msg.gossip_node_.toString().c_str());
    dumpFDState();
  }

  return true;
}

std::string
FailureDetector::flagsToString(GOSSIP_Message::GOSSIP_flags_t flags) {
  if (flags & GOSSIP_Message::SUSPECT_STATE_FINISHED) {
    return "suspect-state-finished";
  } else if (flags & GOSSIP_Message::NODE_BRINGUP_FLAG) {
    return "bringup";
  }

  return "unknown";
}

void FailureDetector::onGossipReceived(const GOSSIP_Message& msg) {
  auto config = getServerConfig();
  node_index_t this_index = config->getMyNodeID().index();

  if (shouldDumpState()) {
    ld_info("Gossip message received from node %s, sent_time:%lums",
            msg.gossip_node_.toString().c_str(),
            msg.sent_time_.count());
    dumpGossipMessage(msg);
  }

  if (checkSkew(msg)) {
    return;
  }

  size_t num_nodes = msg.num_nodes_;
  if (num_nodes > gossip_list_.size()) {
    RATELIMIT_ERROR(std::chrono::minutes(1),
                    1,
                    "Number of nodes in the GOSSIP message (%zu) exceeds the "
                    "max_nodes limit (%zu)",
                    num_nodes,
                    gossip_list_.size());

    num_nodes = gossip_list_.size();
  }

  node_index_t sender_idx = msg.gossip_node_.index();
  std::lock_guard<std::mutex> lock(mutex_);

  if (sender_idx < num_nodes && gossip_ts_[sender_idx] > msg.instance_id_) {
    RATELIMIT_WARNING(std::chrono::seconds(1),
                      5,
                      "Possible time-skew detected on %s, received a lower "
                      "instance id(%lu) from sender than already known(%lu)",
                      msg.gossip_node_.toString().c_str(),
                      msg.instance_id_.count(),
                      gossip_ts_[sender_idx].count());
    STAT_INCR(getStats(), gossips_rejected_instance_id);
    return;
  }

  if (processFlags(msg)) {
    return;
  }

  // Merge the contents of gossip list with those from the message
  // by taking the minimum.
  const bool has_failover_list =
      msg.flags_ & GOSSIP_Message::HAS_FAILOVER_LIST_FLAG;
  folly::small_vector<size_t, 64> to_update;

  for (size_t i = 0; i < num_nodes; ++i) {
    // Don't modify this node's state based on gossip message.
    if (i == this_index)
      continue;

    if (has_failover_list && msg.failover_list_[i] > msg.gossip_ts_[i]) {
      RATELIMIT_CRITICAL(std::chrono::seconds(1),
                         10,
                         "Received invalid combination of Failover(%lu) and"
                         " Instance id(%lu) for N%zu from %s",
                         msg.failover_list_[i].count(),
                         msg.gossip_ts_[i].count(),
                         i,
                         msg.gossip_node_.toString().c_str());
      ld_check(false);
      continue;
    }

    // If the incoming Gossip message knows about an older instance of the
    // process running on Node Ni, then ignore this update.
    if (gossip_ts_[i] > msg.gossip_ts_[i]) {
      ld_spew("Received a stale instance id from %s,"
              " for N%zu, our:%lu, received:%lu",
              msg.gossip_node_.toString().c_str(),
              i,
              gossip_ts_[i].count(),
              msg.gossip_ts_[i].count());
      continue;
    } else if (gossip_ts_[i] < msg.gossip_ts_[i]) {
      // If the incoming Gossip message knows about a valid
      // newer instance of Node Ni, then copy everything
      if (isValidInstanceId(msg.gossip_ts_[i], i)) {
        gossip_list_[i] = msg.gossip_list_[i];
        gossip_ts_[i] = msg.gossip_ts_[i];
        failover_list_[i] = has_failover_list
            ? msg.failover_list_[i]
            : std::chrono::milliseconds::zero();
        to_update.push_back(i);
      }
      continue;
    }

    if (update_min(gossip_list_[i], msg.gossip_list_[i]) ||
        i == msg.gossip_node_.index()) {
      to_update.push_back(i);
    }
    if (has_failover_list) {
      failover_list_[i] = std::max(failover_list_[i], msg.failover_list_[i]);
    }
  }

  // Merge suspect matrices. This is done by copying the row from the matrix
  // included in the message if the other node has more up-to-date
  // information: more precisely, row j of the matrix is copied from node
  // N's suspect matrix only if j-th entry of N's gossip list is no greater
  // than the corresponding entry in our own list (therefore, node N
  // presumably has more recent info about the j-th node).
  if (msg.suspect_matrix_.size() > 0) {
    for (auto& idx : to_update) {
      for (size_t j = 0; j < num_nodes; ++j) {
        suspect_matrix_[idx][j] = msg.suspect_matrix_[idx][j];
      }
    }
  }

  getBoycottTracker().updateReportedBoycotts(msg.boycott_list_);

  num_gossips_received_++;
  if (num_gossips_received_ <= settings_->min_gossips_for_stable_state) {
    ld_debug("Received gossip#%zu", num_gossips_received_);
  }
}

void FailureDetector::startSuspectTimer() {
  ld_info("Starting suspect state timer");

  Worker* w = Worker::onThisThread();
  auto config = getServerConfig();
  node_index_t my_idx = config->getMyNodeID().index();

  gossip_list_[my_idx] = 0;
  updateNodeState(my_idx, false, true, false);

  suspect_timer_.assign([=] {
    ld_info("Suspect timeout expired");
    updateNodeState(my_idx, false, true, false);
  });

  suspect_timer_.activate(settings_->suspect_duration, &w->commonTimeouts());
}

void FailureDetector::startGossiping() {
  ld_info("Start Gossiping.");

  Worker* w = Worker::onThisThread();
  gossip_timer_node_ = w->registerTimer(
      [this](ExponentialBackoffTimerNode* node) {
        gossip();
        node->timer->activate();
      },
      settings_->gossip_interval,
      settings_->gossip_interval);
  ld_check(gossip_timer_node_ != nullptr);
  gossip_timer_node_->timer->activate();
}

std::string
FailureDetector::dumpSuspectMatrix(const GOSSIP_Message::suspect_matrix_t& sm) {
  auto config = getServerConfig();
  size_t n = std::min(sm.size(), config->getMaxNodeIdx() + 1);

  std::string str = "[";
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < n; ++j) {
      str += folly::to<std::string>(sm[i][j]);
    }
    str += (i + 1 < n ? ", " : "");
  }
  str += "]";

  return str;
}

std::string FailureDetector::dumpGossipList(std::vector<uint32_t> list) {
  auto config = getServerConfig();
  auto& nodes = config->getNodes();
  size_t n = std::min(gossip_list_.size(), config->getMaxNodeIdx() + 1);
  n = std::min(n, list.size());
  std::string res;

  for (size_t i = 0; i < n; ++i) {
    NodeID node_id(i, nodes.count(i) ? nodes.at(i).generation : 0);
    res += node_id.toString() + " = " + folly::to<std::string>(list[i]) +
        (i < n - 1 ? ", " : "");
  }

  return res;
}

std::string
FailureDetector::dumpInstanceList(std::vector<std::chrono::milliseconds> list) {
  auto config = getServerConfig();
  auto& nodes = config->getNodes();
  size_t n = std::min(gossip_list_.size(), config->getMaxNodeIdx() + 1);
  n = std::min(n, list.size());
  std::string res;

  for (size_t i = 0; i < n; ++i) {
    NodeID node_id(i, nodes.count(i) ? nodes.at(i).generation : 0);
    res += node_id.toString() + " = " +
        folly::to<std::string>(list[i].count()) + (i < n - 1 ? ", " : "");
  }

  return res;
}

void FailureDetector::dumpFDState() {
  auto config = getServerConfig();
  auto& nodes = config->getNodes();
  size_t n = std::min(gossip_list_.size(), config->getMaxNodeIdx() + 1);
  std::string status_str;

  for (size_t i = 0; i < n; ++i) {
    NodeID node_id(i, nodes.count(i) ? nodes.at(i).generation : 0);
    status_str +=
        node_id.toString() + "(" + getNodeStateString(nodes_[i].state) + "), ";
  }

  const dbg::Level level = isTracingOn() ? dbg::Level::INFO : dbg::Level::SPEW;
  ld_log(
      level, "Failure Detector status for all nodes: %s", status_str.c_str());
  ld_log(level,
         "This node's Gossip List: %s",
         dumpGossipList(gossip_list_).c_str());
  ld_log(level,
         "This node's Instance id list: %s",
         dumpInstanceList(gossip_ts_).c_str());
  ld_log(level,
         "This node's Failover List: %s",
         dumpInstanceList(failover_list_).c_str());
}

void FailureDetector::cancelTimers() {
  cs_timer_.cancel();
  suspect_timer_.cancel();
  gossip_timer_node_ = nullptr;
}

void FailureDetector::shutdown() {
  ld_info("Cancelling timers");
  cancelTimers();
}

void FailureDetector::dumpGossipMessage(const GOSSIP_Message& msg) {
  ld_info("Gossip List from %s [%s]",
          msg.gossip_node_.toString().c_str(),
          dumpGossipList(msg.gossip_list_).c_str());
  ld_info("Instance id list from %s [%s]",
          msg.gossip_node_.toString().c_str(),
          dumpInstanceList(msg.gossip_ts_).c_str());
  ld_info("Failover List from %s [%s]",
          msg.gossip_node_.toString().c_str(),
          dumpInstanceList(msg.failover_list_).c_str());
}

void FailureDetector::getClusterDeadNodeStats(size_t* effective_dead_cnt,
                                              size_t* effective_cluster_size) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (effective_dead_cnt != nullptr) {
    *effective_dead_cnt = effective_dead_cnt_;
  }
  if (effective_cluster_size != nullptr) {
    *effective_cluster_size = effective_cluster_size_;
  }
}

void FailureDetector::detectFailures(node_index_t self, size_t n) {
  const int threshold = settings_->gossip_failure_threshold;
  auto config = getServerConfig();

  ld_check(self < n);
  ld_check(n <= gossip_list_.size());

  // update this node's row of the suspect matrix
  for (size_t i = 0; i < n; ++i) {
    suspect_matrix_[self][i] = (gossip_list_[i] > threshold);
  }

  size_t dead_cnt = 0;
  size_t effective_dead_cnt = 0;
  size_t cluster_size = config->getNodes().size();
  size_t effective_cluster_size = cluster_size;
  // Finally, update all the states
  for (size_t j = 0; j < n; ++j) {
    if (j == self) {
      // don't transition yourself to DEAD
      updateNodeState(self, false /*dead*/, true /*self*/, false /*failover*/);
      continue;
    }

    // Node 'j' is likely dead if this node and
    // other nodes haven't heard from it in a long time
    // OR
    // Node 'j' is performing a graceful shutdown.
    // Mark it DEAD so no work ends up sent its way.
    bool failover = (failover_list_[j] > std::chrono::milliseconds::zero());
    bool dead = (gossip_list_[j] > threshold);
    if (dead) {
      // if the node is actually dead, clear the failover boolean, to make sure
      // we don't mistakenly transition from DEAD to FAILING_OVER in the
      // Cluster State.
      failover = false;
    } else {
      // node is not dead but may be failing over. we don't have proper
      // FAILING OVER state in the FD, instead we use this boolean to carry
      // over the fact that the node is shutting down and consider it dead
      // in that case...
      // TODO: revisit Failure Detector to better handle failover
      dead = failover;
    }

    updateNodeState(j, dead, false, failover);

    // re-check node's state as it may be suspect, in which case it is still
    // considered dead
    dead = (nodes_[j].state != NodeState::ALIVE);
    auto node = config->getNode(j);
    if (node != nullptr) {
      if (node->isDisabled()) {
        // It is disabled. Do not count it towards the effective cluster
        // size, as this node doesn't serve anything
        --effective_cluster_size;
      }
      if (dead) {
        ++dead_cnt;
        if (!node->isDisabled()) {
          // only active nodes matter for isolation. see comment below.
          ++effective_dead_cnt;
        }
      }
    }
  }

  effective_dead_cnt_ = effective_dead_cnt;
  effective_cluster_size_ = effective_cluster_size;
  STAT_SET(getStats(), num_nodes, cluster_size);
  STAT_SET(getStats(), num_dead_nodes, dead_cnt);
  STAT_SET(getStats(), effective_num_nodes, effective_cluster_size);
  STAT_SET(getStats(), effective_dead_nodes, effective_dead_cnt);

  // Check whether more than half of the nodes are dead. This may mean that
  // there is a network partitioning and we are in a minority. We record this
  // fact in the isolated_ boolean for sequencers to take action.
  // For the purpose of this check, we consider only the effective numbers. We
  // ignore nodes that are disabled, meaning that they do not participate in
  // cluster activities. This allows the cluster to keep functioning with a
  // subset of nodes, when the others are disabled. The reasoning is: if a node
  // dies while being disabled, it shouldn't affect the cluster, and so
  // shouldn't trigger isolation mode.
  isolated_.store(2 * effective_dead_cnt > effective_cluster_size);

  if (isolation_checker_ != nullptr) {
    isolation_checker_->processIsolationUpdates();
  }
}

void FailureDetector::updateDependencies(node_index_t idx,
                                         FailureDetector::NodeState new_state,
                                         bool failover) {
  nodes_[idx].state = new_state;
  const bool is_dead = (new_state != NodeState::ALIVE);
  auto cs = getClusterState();

  if (isolation_checker_ != nullptr) {
    // may not set in tests
    if (is_dead && cs->isNodeAlive(idx)) {
      isolation_checker_->onNodeDead(idx);
    } else if (!is_dead && !cs->isNodeAlive(idx)) {
      isolation_checker_->onNodeAlive(idx);
    }
  }

  cs->setNodeState(idx,
                   is_dead ? (failover ? ClusterState::NodeState::FAILING_OVER
                                       : ClusterState::NodeState::DEAD)
                           : ClusterState::NodeState::ALIVE);
}

void FailureDetector::updateNodeState(node_index_t idx,
                                      bool dead,
                                      bool self,
                                      bool failover) {
  NodeState current = nodes_[idx].state, next = NodeState::DEAD;
  auto current_time_ms = getCurrentTimeInMillis();
  auto suspect_duration = settings_->suspect_duration;

  if (!dead) {
    next = current;
    switch (current) {
      case DEAD:
        // Transition from DEAD -> SUSPECT requires
        // last_suspect_time to be reset.
        if (settings_->suspect_duration.count() > 0) {
          next = NodeState::SUSPECT;
          last_suspected_at_[idx].store(current_time_ms);
        } else {
          next = NodeState::ALIVE;
        }
        break;
      case SUSPECT:
        ld_check(suspect_duration.count() > 0);
        if (current_time_ms >
            last_suspected_at_[idx].load() + suspect_duration) {
          next = NodeState::ALIVE;
        }
        break;
      case ALIVE:
        // Node stays ALIVE
        break;
    }
  }

  if (current != next) {
    if (next != NodeState::DEAD) {
      // Node's state is no longer DEAD. Reset connection throttling
      // on a server socket to that node to allow subsequent gossip
      // messages to be immediately sent to it.
      Socket* socket = getServerSocket(idx);
      if (socket) {
        socket->resetConnectThrottle();
      }
    } else {
      // This node should transition itself to DEAD.
      if (self) {
        ld_check(false);
      }
    }

    ld_info("N%hu transitioned from %s to %s, FD State:"
            "(gossip: %u, instance-id: %lu, failover: %lu)",
            idx,
            getNodeStateString(current),
            getNodeStateString(next),
            gossip_list_[idx],
            gossip_ts_[idx].count(),
            failover_list_[idx].count());
  }

  updateDependencies(idx, next, failover);
  if (self && current != next && next == NodeState::ALIVE) {
    broadcastSuspectDurationFinished();
  }
}

bool FailureDetector::isValidDestination(node_index_t node_idx) {
  if (nodes_[node_idx].blacklisted) {
    // exclude blacklisted nodes
    return false;
  }

  Socket* socket = getServerSocket(node_idx);
  if (!socket) {
    // If a connection to the node doesn't exist yet, consider it as a valid
    // destination.
    return true;
  }

  int rv = socket->checkConnection(nullptr);
  if (rv != 0) {
    if (err == E::DISABLED || err == E::NOBUFS) {
      ld_spew("Can't gossip to N%u: %s", node_idx, error_description(err));
      return false;
    }
  }

  return true;
}

bool FailureDetector::isMyDomainIsolated(NodeLocationScope scope) const {
  if (isolation_checker_ == nullptr) {
    // not attached
    return false;
  }
  return isolation_checker_->isMyDomainIsolated(scope);
}

const char* FailureDetector::getNodeStateString(NodeState state) const {
  switch (state) {
    case ALIVE:
      return "ALIVE";
    case SUSPECT:
      return "SUSPECT";
    case DEAD:
      return "DEAD";
  }
  return "UNKNOWN";
}

std::string FailureDetector::getStateString(node_index_t idx) const {
  if (idx >= gossip_list_.size()) {
    return "invalid node index";
  }

  char buf[1024];
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snprintf(buf,
             sizeof(buf),
             "(gossip: %u, instance-id: %lu, failover: %lu, state: %s)",
             gossip_list_[idx],
             gossip_ts_[idx].count(),
             failover_list_[idx].count(),
             getNodeStateString(nodes_[idx].state));
  }
  return std::string(buf);
}

std::string FailureDetector::getDomainIsolationString() const {
  if (isolation_checker_) {
    return isolation_checker_->getDebugInfo();
  }
  return "";
}

std::shared_ptr<ServerConfig> FailureDetector::getServerConfig() const {
  return Worker::getConfig()->serverConfig();
}

ClusterState* FailureDetector::getClusterState() const {
  return Worker::getClusterState();
}

void FailureDetector::broadcastWrapper(GOSSIP_Message::GOSSIP_flags_t flags) {
  auto config = getServerConfig();
  auto& nodes = config->getNodes();
  node_index_t my_idx = config->getMyNodeID().index();
  NodeID dest;

  /* mark nodes not in the config as DEAD in suspect matrix */
  for (int j = nodes.size(); j < suspect_matrix_.size(); ++j) {
    for (int i = 0; i < suspect_matrix_.size(); ++i) {
      suspect_matrix_[i][j] = 1;
    }
  }

  std::string msg_type = flagsToString(flags);
  if (msg_type == "unknown") {
    RATELIMIT_ERROR(std::chrono::seconds(1), 1, "Invalid flags=%d", flags);
    ld_check(false);
  }

  ld_info("Broadcasting %s message.", msg_type.c_str());
  gossip_list_[my_idx] = 0;

  for (const auto& it : nodes) {
    node_index_t idx = it.first;
    auto& node = it.second;
    if (idx == my_idx) {
      continue;
    }

    auto gossip_msg = std::make_unique<GOSSIP_Message>();
    gossip_msg->num_nodes_ = 0;
    gossip_msg->gossip_node_ = config->getMyNodeID();
    gossip_msg->flags_ |= flags;
    gossip_msg->instance_id_ = instance_id_;
    gossip_msg->sent_time_ = getCurrentTimeInMillis();

    dest = NodeID(idx, node.generation);
    RATELIMIT_INFO(std::chrono::seconds(1),
                   2,
                   "Sending %s message with instance id:%lu to node %s",
                   msg_type.c_str(),
                   instance_id_.count(),
                   Sender::describeConnection(Address(dest)).c_str());
    int rv = sendGossipMessage(dest, std::move(gossip_msg));
    if (rv != 0) {
      ld_info("Failed to send %s message to node %s: %s",
              msg_type.c_str(),
              Sender::describeConnection(Address(dest)).c_str(),
              error_description(err));
    }
  }
}

int FailureDetector::sendGossipMessage(NodeID node,
                                       std::unique_ptr<GOSSIP_Message> gossip) {
  ld_spew("Sending Gossip message to %s", node.toString().c_str());

  return Worker::onThisThread()->sender().sendMessage(std::move(gossip), node);
}

void FailureDetector::onGossipMessageSent(Status st,
                                          const Address& to,
                                          uint64_t msg_id) {
  if (st != E::OK) {
    // keep track of the number of errors
    STAT_INCR(getStats(), gossips_failed_to_send);
  }

  if (current_msg_id_ != msg_id || msg_id == 0) {
    // ignore this callback as it was for an older message
    return;
  }

  // set the limit of retries to the number of nodes in the cluster.
  // this is arbitrary to try best to send a message to any node.
  const size_t max_attempts = getServerConfig()->getNodes().size();
  if (st == E::OK) {
    // message was sent successfully, reset the counter.
    num_gossip_attempts_failed_ = 0;
  } else if (num_gossip_attempts_failed_ == max_attempts) {
    // failed to send message, but we reached the maximum retries.
    // reset the counter and log a message
    RATELIMIT_WARNING(std::chrono::seconds(1),
                      1,
                      "Could not send gossip to %s: %s. "
                      "Consecutively failed to send %lu gossips.",
                      Sender::describeConnection(Address(to)).c_str(),
                      error_description(st),
                      num_gossip_attempts_failed_);
    num_gossip_attempts_failed_ = 0;
  } else {
    // failed to send message, let's try another node.
    // bump the counter and retry
    ++num_gossip_attempts_failed_;
    if (gossip_timer_node_) {
      RATELIMIT_INFO(std::chrono::seconds(1),
                     1,
                     "Could not send gossip to %s: %s. "
                     "Trying another node.",
                     Sender::describeConnection(Address(to)).c_str(),
                     error_description(st));
      // here we do not directly send another gossip but rather schedule the
      // gossip timer for immediate execution.
      gossip_timer_node_->timer->fire();
    }
  }
}

void FailureDetector::updateBoycottedNodes() {
  getClusterState()->setBoycottedNodes(
      getBoycottTracker().getBoycottedNodes(SystemTimestamp::now()));
}

void FailureDetector::setOutliers(std::vector<NodeID> outliers) {
  getBoycottTracker().setLocalOutliers(std::move(outliers));
}

void FailureDetector::resetBoycottedNode(node_index_t node_index) {
  getBoycottTracker().resetBoycott(node_index);
}

bool FailureDetector::isOutlier(node_index_t node_index) {
  return getBoycottTracker().isBoycotted(node_index);
}

Socket* FailureDetector::getServerSocket(node_index_t idx) {
  return Worker::onThisThread()->sender().findServerSocket(idx);
}

void FailureDetector::setBlacklisted(node_index_t idx, bool blacklisted) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (idx < nodes_.size()) {
    nodes_[idx].blacklisted = blacklisted;
  }
}

bool FailureDetector::isBlacklisted(node_index_t idx) const {
  bool blacklisted = false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (idx < nodes_.size()) {
    blacklisted = nodes_[idx].blacklisted;
  }
  return blacklisted;
}

bool FailureDetector::isAlive(node_index_t idx) const {
  /* common case */
  bool alive = getClusterState()->isNodeAlive(idx);
  if (alive) {
    return true;
  }

  /* We'll check whether suspect duration has already passed
   * or not, only for this node.
   */
  auto config = getServerConfig();
  node_index_t my_idx = config->getMyNodeID().index();
  if (my_idx != idx) {
    return false;
  }

  /* If the current node's SUSPECT state has elapsed,
   * return ALIVE instead of DEAD.
   * FD Thread will soon transition us(this node) to ALIVE.
   */
  auto current_time_ms = getCurrentTimeInMillis();
  auto suspect_duration = settings_->suspect_duration;
  if (suspect_duration.count() > 0) {
    if (current_time_ms > last_suspected_at_[idx].load() + suspect_duration) {
      RATELIMIT_INFO(
          std::chrono::seconds(1),
          1,
          "Suspect duration for this node(N%hu) expired, "
          "but FD hasn't yet made the transition, treating the node as ALIVE",
          idx);
      return true;
    }
  }

  return false;
}

bool FailureDetector::isIsolated() const {
  if (settings_->ignore_isolation) {
    return false;
  }
  return isolated_.load();
}

}} // namespace facebook::logdevice
