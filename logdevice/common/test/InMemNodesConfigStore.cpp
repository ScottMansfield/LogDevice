/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logdevice/common/test/InMemNodesConfigStore.h"

#include "logdevice/common/util.h"

namespace facebook { namespace logdevice { namespace configuration {

//////// InMemNodesConfigStore ////////

int InMemNodesConfigStore::getConfig(std::string key,
                                     value_callback_t cb) const {
  std::string value{};
  Status status = getConfigSync(std::move(key), &value);

  if (status == Status::OK || status == Status::NOTFOUND ||
      status == Status::ACCESS || status == Status::AGAIN) {
    cb(status, value);
    return 0;
  }

  err = status;
  return -1;
}

Status InMemNodesConfigStore::getConfigSync(std::string key,
                                            std::string* value_out) const {
  {
    auto lockedConfigs = configs_.rlock();
    auto it = lockedConfigs->find(key);
    if (it == lockedConfigs->end()) {
      return Status::NOTFOUND;
    }
    if (value_out) {
      *value_out = it->second;
    }
  }
  return Status::OK;
}

int InMemNodesConfigStore::updateConfig(std::string key,
                                        std::string value,
                                        folly::Optional<version_t> base_version,
                                        write_callback_t cb) {
  version_t version;
  std::string value_out;
  Status status = updateConfigSync(
      std::move(key), std::move(value), base_version, &version, &value_out);

  if (status == Status::OK || status == Status::NOTFOUND ||
      status == Status::VERSION_MISMATCH || status == Status::ACCESS ||
      status == Status::AGAIN) {
    cb(status, version, std::move(value_out));
    return 0;
  }

  err = status;
  return -1;
}

Status
InMemNodesConfigStore::updateConfigSync(std::string key,
                                        std::string value,
                                        folly::Optional<version_t> base_version,
                                        version_t* version_out,
                                        std::string* value_out) {
  auto opt = extract_fn_(value);
  if (!opt) {
    return Status::INVALID_PARAM;
  }
  version_t value_version = opt.value();
  {
    auto lockedConfigs = configs_.wlock();
    auto it = lockedConfigs->find(key);
    if (it == lockedConfigs->end()) {
      if (base_version) {
        return Status::NOTFOUND;
      }
      set_if_not_null(version_out, value_version);
      auto res = lockedConfigs->emplace(std::move(key), std::move(value));
      ld_assert(res.second); // inserted
      return Status::OK;
    }

    auto curr_version_opt = extract_fn_(it->second);
    if (!opt) {
      return Status::INVALID_PARAM;
    }
    version_t curr_version = curr_version_opt.value();
    if (base_version && curr_version != base_version) {
      // conditional update version mismatch
      // TODO: set err accordingly
      set_if_not_null(version_out, curr_version);
      set_if_not_null(value_out, it->second);
      return Status::VERSION_MISMATCH;
    }
    set_if_not_null(version_out, value_version);
    it->second = std::move(value);
  }
  return Status::OK;
}
}}} // namespace facebook::logdevice::configuration
