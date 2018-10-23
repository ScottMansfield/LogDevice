/**
 * Copyright (c) 2018-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "SequencerMembership.h"

#include <algorithm>

#include <folly/Format.h>

#include "logdevice/common/debug.h"
#include "logdevice/common/membership/utils.h"
#include "logdevice/common/util.h"
#include "logdevice/include/Err.h"

namespace facebook { namespace logdevice { namespace membership {

using facebook::logdevice::toString;
using namespace MembershipVersion;
using namespace MaintenanceID;

std::string SequencerNodeState::Update::toString() const {
  return folly::sformat("[T:{}, W:{}, M:{}]",
                        membership::toString(transition),
                        membership::toString(weight),
                        membership::toString(maintenance));
}

bool SequencerNodeState::Update::isValid() const {
  if (transition >= SequencerMembershipTransition::Count) {
    RATELIMIT_ERROR(std::chrono::seconds(10),
                    5,
                    "Invalid transition %lu for update %s.",
                    static_cast<size_t>(transition),
                    toString().c_str());
    return false;
  }

  if (weight < 0) {
    RATELIMIT_ERROR(std::chrono::seconds(10),
                    5,
                    "Invalid negative weight %s for update %s.",
                    membership::toString(weight).c_str(),
                    toString().c_str());
    return false;
  }
  return true;
}

std::string SequencerNodeState::toString() const {
  return folly::sformat("[W:{}, M:{}]",
                        membership::toString(weight),
                        membership::toString(active_maintenance));
}

bool SequencerNodeState::isValid() const {
  return weight >= 0;
}

bool SequencerMembership::Update::isValid() const {
  return !node_updates.empty() &&
      std::all_of(node_updates.cbegin(),
                  node_updates.cend(),
                  [](const auto& kv) { return kv.second.isValid(); });
}

std::string SequencerMembership::Update::toString() const {
  std::string node_str;
  bool first = true;
  for (const auto& kv : node_updates) {
    if (!first) {
      node_str += ", ";
    }
    node_str += folly::sformat(
        "{{{}, {}}}", membership::toString(kv.first), kv.second.toString());
    first = false;
  }
  return folly::sformat(
      "[V:{}, {{{}}}]", membership::toString(base_version), node_str);
}

std::pair<bool, SequencerNodeState>
SequencerMembership::getNodeState(node_index_t node) const {
  const auto nit = node_states_.find(node);
  if (nit == node_states_.cend()) {
    return std::make_pair(false, SequencerNodeState{});
  }
  return std::make_pair(true, nit->second);
}

void SequencerMembership::setNodeState(node_index_t node,
                                       SequencerNodeState state) {
  node_states_[node] = std::move(state);
}

bool SequencerMembership::eraseNodeState(node_index_t node) {
  return node_states_.erase(node) > 0;
}

int SequencerMembership::applyUpdate(
    const Membership::Update& membership_update,
    Membership* new_membership_out) const {
  if (membership_update.getType() != MembershipType::SEQUENCER ||
      (new_membership_out != nullptr &&
       new_membership_out->getType() != MembershipType::SEQUENCER)) {
    RATELIMIT_ERROR(
        std::chrono::seconds(10),
        5,
        "Expect update and/or out params of sequencer membership type!");
    err = E::INVALID_PARAM;
    return -1;
  }

  const SequencerMembership::Update& update =
      checked_downcast<const SequencerMembership::Update&>(membership_update);
  SequencerMembership* new_sequencer_membership_out =
      checked_downcast_or_null<SequencerMembership*>(new_membership_out);

  if (!update.isValid()) {
    RATELIMIT_ERROR(std::chrono::seconds(10),
                    5,
                    "Cannnot apply invalid membership update: %s.",
                    update.toString().c_str());
    err = E::INVALID_PARAM;
    return -1;
  }

  if (version_ != update.base_version) {
    RATELIMIT_ERROR(std::chrono::seconds(10),
                    5,
                    "Cannnot apply membership update with wrong base version. "
                    "current version: %s, update: %s.",
                    membership::toString(version_).c_str(),
                    update.toString().c_str());
    err = E::VERSION_MISMATCH;
    return -1;
  }

  SequencerMembership target_membership_state(*this);
  // bump the version in the target state
  target_membership_state.version_ =
      MembershipVersion::Type(version_.val() + 1);

  for (const auto& kv : update.node_updates) {
    const node_index_t node = kv.first;
    const SequencerNodeState::Update& node_update = kv.second;

    bool node_exist;
    SequencerNodeState current_node_state;
    std::tie(node_exist, current_node_state) = getNodeState(node);

    if (!node_exist &&
        node_update.transition != SequencerMembershipTransition::ADD_NODE) {
      RATELIMIT_ERROR(std::chrono::seconds(10),
                      5,
                      "Cannnot apply membership update for node %s as it "
                      "does not exist in membership. current version: %s, "
                      "update: %s.",
                      membership::toString(node).c_str(),
                      membership::toString(version_).c_str(),
                      update.toString().c_str());
      err = E::NOTINCONFIG;
      return -1;
    }

    if (node_exist &&
        node_update.transition == SequencerMembershipTransition::ADD_NODE) {
      RATELIMIT_ERROR(std::chrono::seconds(10),
                      5,
                      "Cannnot apply membership update for node %s as the node "
                      "requested to add already exists in membership. current "
                      "version: %s, update: %s.",
                      membership::toString(node).c_str(),
                      membership::toString(version_).c_str(),
                      update.toString().c_str());
      err = E::EXISTS;
      return -1;
    }

    switch (node_update.transition) {
      case SequencerMembershipTransition::REMOVE_NODE: {
        ld_check(node_exist);
        bool erased = target_membership_state.eraseNodeState(node);
        ld_check(erased);
      } break;
      case SequencerMembershipTransition::SET_WEIGHT:
        ld_check(node_exist);
        FOLLY_FALLTHROUGH;
      case SequencerMembershipTransition::ADD_NODE:
        target_membership_state.setNodeState(
            node, {node_update.weight, node_update.maintenance});
        break;
      case SequencerMembershipTransition::Count:
        ld_check(false);
        err = E::INTERNAL;
        return -1;
    }
  }

  if (new_sequencer_membership_out != nullptr) {
    *new_sequencer_membership_out = target_membership_state;
  }

  dcheckConsistency();
  return 0;
}

bool SequencerMembership::validate() const {
  if (version_ == EMPTY_VERSION && !isEmpty()) {
    RATELIMIT_ERROR(
        std::chrono::seconds(10),
        5,
        "validation failed! Memership is not empty with empty version: %s. "
        "Number of nodes: %lu.",
        membership::toString(version_).c_str(),
        numNodes());
    return false;
  }

  for (const auto& kv : node_states_) {
    if (!kv.second.isValid()) {
      RATELIMIT_ERROR(std::chrono::seconds(10),
                      5,
                      "validation failed! invalid node state for node %s: "
                      "%s. membership version: %s.",
                      membership::toString(kv.first).c_str(),
                      kv.second.toString().c_str(),
                      membership::toString(version_).c_str());
      return false;
    }
  }

  return true;
}

SequencerMembership::SequencerMembership() : Membership(EMPTY_VERSION) {
  dcheckConsistency();
}

std::vector<node_index_t> SequencerMembership::getMembershipNodes() const {
  std::vector<node_index_t> res;
  res.reserve(node_states_.size());
  for (const auto& kv : node_states_) {
    res.push_back(kv.first);
  }
  return res;
}

bool SequencerMembership::operator==(const SequencerMembership& rhs) const {
  return version_ == rhs.getVersion() && node_states_ == rhs.node_states_;
}

}}} // namespace facebook::logdevice::membership
