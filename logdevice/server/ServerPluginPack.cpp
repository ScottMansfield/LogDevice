/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logdevice/server/ServerPluginPack.h"

#include <dlfcn.h>

namespace facebook { namespace logdevice {

std::unique_ptr<AdminServer>
ServerPluginPack::createAdminServer(Server* /* unused */) {
  ld_info("No AdminAPI server will be created!");
  return nullptr;
}

}} // namespace facebook::logdevice
