/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "logdevice/common/SampledTracer.h"
#include "logdevice/common/debug.h"
#include "logdevice/include/Client.h"

namespace facebook { namespace logdevice {

constexpr auto API_HITS_TRACER = "api_hits_tracer";
class ClientAPIHitsTracer : public SampledTracer {
 public:
  explicit ClientAPIHitsTracer(std::shared_ptr<TraceLogger> logger);

  folly::Optional<double> getDefaultSamplePercentage() const override {
    return 0.05;
  }

  void traceFindTime(int64_t msec_resp_time,
                     logid_t in_logid,
                     std::chrono::milliseconds in_timestamp,
                     FindKeyAccuracy in_accuracy,
                     Status out_status,
                     lsn_t out_lsn = LSN_INVALID);

  void traceFindKey(int64_t msec_resp_time,
                    logid_t in_logid,
                    std::string in_key,
                    FindKeyAccuracy in_accuracy,
                    Status out_status,
                    lsn_t out_lsn_lo = LSN_INVALID,
                    lsn_t out_lsn_hi = LSN_INVALID);

  void traceGetTailAttributes(int64_t msec_resp_time,
                              logid_t in_logid,
                              Status out_status,
                              LogTailAttributes* out_log_tail_attributes);

  void traceGetHeadAttributes(int64_t msec_resp_time,
                              logid_t in_logid,
                              Status out_status,
                              LogHeadAttributes* out_log_head_attributes);

  void traceGetTailLSN(int64_t msec_resp_time,
                       logid_t in_logid,
                       Status out_status,
                       lsn_t out_lsn = LSN_INVALID);

  void traceIsLogEmpty(int64_t msec_resp_time,
                       logid_t in_logid,
                       Status out_status,
                       bool out_bool);

  void traceDataSize(int64_t msec_resp_time,
                     logid_t in_logid,
                     Status out_status,
                     size_t out_size);

  void traceTrim(int64_t msec_resp_time,
                 logid_t in_logid,
                 lsn_t in_lsn,
                 Status out_status);
};

}} // namespace facebook::logdevice
