/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include "folly/Function.h"
#include "logdevice/common/PrincipalIdentity.h"
#include "logdevice/common/SecurityInformation.h"
#include "logdevice/include/types.h"

namespace facebook { namespace logdevice {

/**
 *  Result of permission check. If its permissions are still loading,
 *  and result not yet know, NOTREADY is returned.
 */
enum class PermissionCheckStatus {
  NONE,
  ALLOWED,
  DENIED,
  NOTREADY,
  SYSLIMIT,
  NOTFOUND
};

using callback_func_t = folly::Function<void(PermissionCheckStatus)>;

/**
 * @file an abstract interface used to determine if an action is allowed
 *       to be performed on a log_id/log_group by a client.
 */

class PermissionChecker {
 public:
  virtual ~PermissionChecker(){};

  /**
   * Queries the permission store to determine if the provided Principal can
   * perform the specified ACTION on the logid. Result is supplied to callback
   * function. Check could be performed on calling thread or if is potentially
   * expensive, executed in background thread. In any case callback is executed
   * on the original thread.
   *
   *
   * @param action      The action to be performed by the principal
   * @param principal   The principal containing the identity of the client
   * @param logid       The resource that the client is trying to read or modify
   * @param cb          Result of the check supplied via callback function
   *
   */
  virtual void isAllowed(ACTION action,
                         const PrincipalIdentity& principal,
                         logid_t logid,
                         callback_func_t cb) const = 0;
  /**
   * Returns the PermissionCheckerType that the PermissionChecker is intended to
   * work with.
   */
  virtual PermissionCheckerType getPermissionCheckerType() const = 0;
};

}} // namespace facebook::logdevice
