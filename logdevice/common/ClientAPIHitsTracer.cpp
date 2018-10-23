/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/common/ClientAPIHitsTracer.h"

#include "logdevice/common/Worker.h"
#include "logdevice/common/stats/ClientHistograms.h"
#include "logdevice/common/stats/Stats.h"

namespace facebook { namespace logdevice {

ClientAPIHitsTracer::ClientAPIHitsTracer(std::shared_ptr<TraceLogger> logger)
    : SampledTracer(std::move(logger)) {}

inline std::string to_string(const FindKeyAccuracy& accuracy) {
  switch (accuracy) {
    case FindKeyAccuracy::STRICT:
      return "STRICT";
    case FindKeyAccuracy::APPROXIMATE:
      return "APPROXIMATE";
    default:
      ld_check(false);
      return "";
  }
}

#define API_HITS_STATUS_CASE(method, status)    \
  case E::status: {                             \
    WORKER_STAT_INCR(client.method##_##status); \
  } break;

#define API_HITS_DEFAULT(method) \
  default:                       \
    WORKER_STAT_INCR(client.method##_OTHER);

void ClientAPIHitsTracer::traceFindTime(int64_t msec_resp_time,
                                        logid_t in_logid,
                                        std::chrono::milliseconds in_timestamp,
                                        FindKeyAccuracy in_accuracy,
                                        Status out_status,
                                        lsn_t out_lsn) {
  CLIENT_HISTOGRAM_ADD(Worker::stats(), findtime_latency, msec_resp_time);
  switch (out_status) {
    API_HITS_STATUS_CASE(findtime, OK)
    API_HITS_STATUS_CASE(findtime, TIMEDOUT)
    API_HITS_STATUS_CASE(findtime, INVALID_PARAM)
    API_HITS_STATUS_CASE(findtime, ACCESS)
    API_HITS_STATUS_CASE(findtime, PARTIAL)
    API_HITS_STATUS_CASE(findtime, FAILED)
    API_HITS_STATUS_CASE(findtime, SHUTDOWN)
    API_HITS_DEFAULT(findtime)
  }
  auto sample_builder = [=]() -> std::unique_ptr<TraceSample> {
    auto sample = std::make_unique<TraceSample>();
    sample->addNormalValue("method", "findTime");
    sample->addIntValue("response_time", msec_resp_time);
    sample->addNormalValue("input_log_id", std::to_string(in_logid.val()));
    sample->addIntValue("input_timestamp", in_timestamp.count());
    sample->addNormalValue("input_accuracy", to_string(in_accuracy));
    sample->addNormalValue("output_status", error_name(out_status));
    sample->addNormalValue("output_lsn", lsn_to_string(out_lsn));
    return sample;
  };
  publish(API_HITS_TRACER, sample_builder);
}

void ClientAPIHitsTracer::traceFindKey(int64_t msec_resp_time,
                                       logid_t in_logid,
                                       std::string in_key,
                                       FindKeyAccuracy in_accuracy,
                                       Status out_status,
                                       lsn_t out_lsn_lo,
                                       lsn_t out_lsn_hi) {
  CLIENT_HISTOGRAM_ADD(Worker::stats(), findkey_latency, msec_resp_time);
  switch (out_status) {
    API_HITS_STATUS_CASE(findkey, OK)
    API_HITS_STATUS_CASE(findkey, TIMEDOUT)
    API_HITS_STATUS_CASE(findkey, INVALID_PARAM)
    API_HITS_STATUS_CASE(findkey, ACCESS)
    API_HITS_STATUS_CASE(findkey, PARTIAL)
    API_HITS_STATUS_CASE(findkey, FAILED)
    API_HITS_STATUS_CASE(findkey, SHUTDOWN)
    API_HITS_DEFAULT(findkey)
  }
  auto sample_builder = [=]() -> std::unique_ptr<TraceSample> {
    auto sample = std::make_unique<TraceSample>();
    sample->addNormalValue("method", "findKey");
    sample->addIntValue("response_time", msec_resp_time);
    sample->addNormalValue("input_log_id", std::to_string(in_logid.val()));
    sample->addNormalValue("input_key", in_key);
    sample->addNormalValue("input_accuracy", to_string(in_accuracy));
    sample->addNormalValue("output_status", error_name(out_status));
    sample->addNormalValue("output_lsn_lo", lsn_to_string(out_lsn_lo));
    sample->addNormalValue("output_lsn_hi", lsn_to_string(out_lsn_hi));
    return sample;
  };
  publish(API_HITS_TRACER, sample_builder);
}

void ClientAPIHitsTracer::traceGetTailAttributes(
    int64_t msec_resp_time,
    logid_t in_logid,
    Status out_status,
    LogTailAttributes* out_log_tail_attributes) {
  CLIENT_HISTOGRAM_ADD(
      Worker::stats(), get_tail_attributes_latency, msec_resp_time);
  switch (out_status) {
    API_HITS_STATUS_CASE(get_tail_attributes, OK)
    API_HITS_STATUS_CASE(get_tail_attributes, TIMEDOUT)
    API_HITS_STATUS_CASE(get_tail_attributes, CONNFAILED)
    API_HITS_STATUS_CASE(get_tail_attributes, NOSEQUENCER)
    API_HITS_STATUS_CASE(get_tail_attributes, FAILED)
    API_HITS_STATUS_CASE(get_tail_attributes, NOBUFS)
    API_HITS_STATUS_CASE(get_tail_attributes, SHUTDOWN)
    API_HITS_STATUS_CASE(get_tail_attributes, INTERNAL)
    API_HITS_STATUS_CASE(get_tail_attributes, AGAIN)
    API_HITS_DEFAULT(get_tail_attributes)
  }
  auto sample_builder = [msec_resp_time,
                         in_logid,
                         out_status,
                         out_log_tail_attributes = out_log_tail_attributes
                             ? out_log_tail_attributes->toString()
                             : "<nullptr>"]() -> std::unique_ptr<TraceSample> {
    auto sample = std::make_unique<TraceSample>();
    sample->addNormalValue("method", "getTailAttributes");
    sample->addIntValue("response_time", msec_resp_time);
    sample->addNormalValue("input_log_id", std::to_string(in_logid.val()));
    sample->addNormalValue("output_status", error_name(out_status));
    sample->addNormalValue(
        "output_log_tail_attributes", out_log_tail_attributes);
    return sample;
  };
  publish(API_HITS_TRACER, sample_builder);
}

void ClientAPIHitsTracer::traceGetHeadAttributes(
    int64_t msec_resp_time,
    logid_t in_logid,
    Status out_status,
    LogHeadAttributes* out_log_head_attributes) {
  CLIENT_HISTOGRAM_ADD(
      Worker::stats(), get_head_attributes_latency, msec_resp_time);
  switch (out_status) {
    API_HITS_STATUS_CASE(get_head_attributes, OK)
    API_HITS_STATUS_CASE(get_head_attributes, TIMEDOUT)
    API_HITS_STATUS_CASE(get_head_attributes, ACCESS)
    API_HITS_STATUS_CASE(get_head_attributes, INVALID_PARAM)
    API_HITS_STATUS_CASE(get_head_attributes, SHUTDOWN)
    API_HITS_STATUS_CASE(get_head_attributes, FAILED)
    API_HITS_DEFAULT(get_head_attributes)
  }
  auto sample_builder = [msec_resp_time,
                         in_logid,
                         out_status,
                         out_log_head_attributes = out_log_head_attributes
                             ? out_log_head_attributes->toString()
                             : "<nullptr>"]() -> std::unique_ptr<TraceSample> {
    auto sample = std::make_unique<TraceSample>();
    sample->addNormalValue("method", "getHeadAttributes");
    sample->addIntValue("response_time", msec_resp_time);
    sample->addNormalValue("input_log_id", std::to_string(in_logid.val()));
    sample->addNormalValue("output_status", error_name(out_status));
    sample->addNormalValue(
        "output_log_head_attributes", out_log_head_attributes);
    return sample;
  };
  publish(API_HITS_TRACER, sample_builder);
}

void ClientAPIHitsTracer::traceGetTailLSN(int64_t msec_resp_time,
                                          logid_t in_logid,
                                          Status out_status,
                                          lsn_t out_lsn) {
  CLIENT_HISTOGRAM_ADD(Worker::stats(), get_tail_lsn_latency, msec_resp_time);
  switch (out_status) {
    API_HITS_STATUS_CASE(get_tail_lsn, OK)
    API_HITS_STATUS_CASE(get_tail_lsn, TIMEDOUT)
    API_HITS_STATUS_CASE(get_tail_lsn, CONNFAILED)
    API_HITS_STATUS_CASE(get_tail_lsn, NOSEQUENCER)
    API_HITS_STATUS_CASE(get_tail_lsn, FAILED)
    API_HITS_STATUS_CASE(get_tail_lsn, NOBUFS)
    API_HITS_STATUS_CASE(get_tail_lsn, SHUTDOWN)
    API_HITS_STATUS_CASE(get_tail_lsn, INTERNAL)
    API_HITS_DEFAULT(get_tail_lsn)
  }
  auto sample_builder = [=]() -> std::unique_ptr<TraceSample> {
    auto sample = std::make_unique<TraceSample>();
    sample->addNormalValue("method", "getTailLSN");
    sample->addIntValue("response_time", msec_resp_time);
    sample->addNormalValue("input_log_id", std::to_string(in_logid.val()));
    sample->addNormalValue("output_status", error_name(out_status));
    sample->addNormalValue("output_lsn", lsn_to_string(out_lsn));
    return sample;
  };
  publish(API_HITS_TRACER, sample_builder);
}

void ClientAPIHitsTracer::traceIsLogEmpty(int64_t msec_resp_time,
                                          logid_t in_logid,
                                          Status out_status,
                                          bool out_bool) {
  CLIENT_HISTOGRAM_ADD(Worker::stats(), is_log_empty_latency, msec_resp_time);
  switch (out_status) {
    API_HITS_STATUS_CASE(is_log_empty, OK)
    API_HITS_STATUS_CASE(is_log_empty, TIMEDOUT)
    API_HITS_STATUS_CASE(is_log_empty, INVALID_PARAM)
    API_HITS_STATUS_CASE(is_log_empty, PARTIAL)
    API_HITS_STATUS_CASE(is_log_empty, ACCESS)
    API_HITS_STATUS_CASE(is_log_empty, FAILED)
    API_HITS_STATUS_CASE(is_log_empty, NOBUFS)
    API_HITS_STATUS_CASE(is_log_empty, SHUTDOWN)
    API_HITS_STATUS_CASE(is_log_empty, INTERNAL)
    API_HITS_DEFAULT(is_log_empty)
  }
  auto sample_builder = [=]() -> std::unique_ptr<TraceSample> {
    auto sample = std::make_unique<TraceSample>();
    sample->addNormalValue("method", "isLogEmpty");
    sample->addIntValue("response_time", msec_resp_time);
    sample->addNormalValue("input_log_id", std::to_string(in_logid.val()));
    sample->addNormalValue("output_status", error_name(out_status));
    sample->addIntValue("output_bool", out_bool);
    return sample;
  };
  publish(API_HITS_TRACER, sample_builder);
}

void ClientAPIHitsTracer::traceDataSize(int64_t msec_resp_time,
                                        logid_t in_logid,
                                        Status out_status,
                                        size_t out_size) {
  switch (out_status) {
    API_HITS_STATUS_CASE(data_size, OK)
    API_HITS_STATUS_CASE(data_size, TIMEDOUT)
    API_HITS_STATUS_CASE(data_size, INVALID_PARAM)
    API_HITS_STATUS_CASE(data_size, PARTIAL)
    API_HITS_STATUS_CASE(data_size, ACCESS)
    API_HITS_STATUS_CASE(data_size, FAILED)
    API_HITS_STATUS_CASE(data_size, NOBUFS)
    API_HITS_STATUS_CASE(data_size, SHUTDOWN)
    API_HITS_STATUS_CASE(data_size, INTERNAL)
    API_HITS_DEFAULT(data_size)
  }
  auto sample_builder = [=]() -> std::unique_ptr<TraceSample> {
    auto sample = std::make_unique<TraceSample>();
    sample->addNormalValue("method", "dataSize");
    sample->addIntValue("response_time", msec_resp_time);
    sample->addNormalValue("input_log_id", std::to_string(in_logid.val()));
    sample->addNormalValue("output_status", error_name(out_status));
    sample->addIntValue("output_size", out_size);
    return sample;
  };
  publish(API_HITS_TRACER, sample_builder);
}

void ClientAPIHitsTracer::traceTrim(int64_t msec_resp_time,
                                    logid_t in_logid,
                                    lsn_t in_lsn,
                                    Status out_status) {
  CLIENT_HISTOGRAM_ADD(Worker::stats(), trim_latency, msec_resp_time);
  switch (out_status) {
    API_HITS_STATUS_CASE(trim, OK)
    API_HITS_STATUS_CASE(trim, TIMEDOUT)
    API_HITS_STATUS_CASE(trim, INVALID_PARAM)
    API_HITS_STATUS_CASE(trim, FAILED)
    API_HITS_STATUS_CASE(trim, PARTIAL)
    API_HITS_STATUS_CASE(trim, ACCESS)
    API_HITS_STATUS_CASE(trim, NOTFOUND)
    API_HITS_DEFAULT(trim)
  }
  auto sample_builder = [=]() -> std::unique_ptr<TraceSample> {
    auto sample = std::make_unique<TraceSample>();
    sample->addNormalValue("method", "trim");
    sample->addIntValue("response_time", msec_resp_time);
    sample->addNormalValue("input_log_id", std::to_string(in_logid.val()));
    sample->addNormalValue("input_lsn", lsn_to_string(in_lsn));
    sample->addNormalValue("output_status", error_name(out_status));
    return sample;
  };
  publish(API_HITS_TRACER, sample_builder);
}

}} // namespace facebook::logdevice
