/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "Worker.h"

#include <algorithm>
#include <pthread.h>
#include <string>
#include <unistd.h>

#include <folly/Memory.h>
#include <folly/stats/BucketedTimeSeries.h>
#include <sys/resource.h>
#include <sys/time.h>

#include "logdevice/common/AbortAppendersEpochRequest.h"
#include "logdevice/common/AllSequencers.h"
#include "logdevice/common/AppendRequest.h"
#include "logdevice/common/AppendRequestBase.h"
#include "logdevice/common/Appender.h"
#include "logdevice/common/AppenderBuffer.h"
#include "logdevice/common/CheckNodeHealthRequest.h"
#include "logdevice/common/CheckSealRequest.h"
#include "logdevice/common/ClientIdxAllocator.h"
#include "logdevice/common/ClusterState.h"
#include "logdevice/common/CopySetManager.h"
#include "logdevice/common/DataSizeRequest.h"
#include "logdevice/common/EventLoopHandle.h"
#include "logdevice/common/ExponentialBackoffAdaptiveVariable.h"
#include "logdevice/common/FindKeyRequest.h"
#include "logdevice/common/FireAndForgetRequest.h"
#include "logdevice/common/GetClusterStateRequest.h"
#include "logdevice/common/GetEpochRecoveryMetadataRequest.h"
#include "logdevice/common/GetHeadAttributesRequest.h"
#include "logdevice/common/GetLogInfoRequest.h"
#include "logdevice/common/GetTrimPointRequest.h"
#include "logdevice/common/IsLogEmptyRequest.h"
#include "logdevice/common/LogIDUniqueQueue.h"
#include "logdevice/common/LogRecoveryRequest.h"
#include "logdevice/common/LogsConfigApiRequest.h"
#include "logdevice/common/LogsConfigUpdatedRequest.h"
#include "logdevice/common/MetaDataLogWriter.h"
#include "logdevice/common/PermissionChecker.h"
#include "logdevice/common/PrincipalParser.h"
#include "logdevice/common/Processor.h"
#include "logdevice/common/SSLFetcher.h"
#include "logdevice/common/SequencerBackgroundActivator.h"
#include "logdevice/common/ServerConfigUpdatedRequest.h"
#include "logdevice/common/SyncSequencerRequest.h"
#include "logdevice/common/TimeoutMap.h"
#include "logdevice/common/TraceLogger.h"
#include "logdevice/common/TrimRequest.h"
#include "logdevice/common/WriteMetaDataRecord.h"
#include "logdevice/common/ZeroCopiedRecordDisposal.h"
#include "logdevice/common/client_read_stream/AllClientReadStreams.h"
#include "logdevice/common/configuration/ServerConfig.h"
#include "logdevice/common/configuration/UpdateableConfig.h"
#include "logdevice/common/configuration/logs/LogsConfigManager.h"
#include "logdevice/common/event_log/EventLogStateMachine.h"
#include "logdevice/common/protocol/APPENDED_Message.h"
#include "logdevice/common/stats/ServerHistograms.h"
#include "logdevice/common/stats/Stats.h"
#include "logdevice/include/Err.h"

namespace facebook { namespace logdevice {

// the size of the bucket array of activeAppenders_ map
static constexpr size_t N_APPENDER_MAP_BUCKETS = 128 * 1024;

// This pimpl class is a container for all classes that would normally be
// members of Worker but we don't want to have to include them in Worker.h.
class WorkerImpl {
 public:
  WorkerImpl(Worker* w, const std::shared_ptr<UpdateableConfig>& config)
      : sender_(w->getEventBase(),
                config->get()->serverConfig()->getTrafficShapingConfig(),
                config->get()->serverConfig()->getMaxNodeIdx(),
                w->processor_->getWorkerCount(w->worker_type_),
                &w->processor_->clientIdxAllocator()),
        commonTimeouts_(w->getEventBase(), Worker::MAX_FAST_TIMEOUTS),
        activeAppenders_(w->immutable_settings_->server ? N_APPENDER_MAP_BUCKETS
                                                        : 1),
        // AppenderBuffer queue capacity is the system-wide per-log limit
        // divided by the number of Workers
        appenderBuffer_(w->immutable_settings_->appender_buffer_queue_cap /
                        w->immutable_settings_->num_workers),
        // TODO: Make this configurable
        previously_redirected_appends_(1024),

        adaptive_store_delay_(
            w->immutable_settings_->store_timeout.initial_delay,
            w->immutable_settings_->store_timeout.initial_delay,
            w->immutable_settings_->store_timeout.max_delay,
            2,
            1,
            0),

        sslFetcher_(w->immutable_settings_->ssl_cert_path,
                    w->immutable_settings_->ssl_key_path,
                    w->immutable_settings_->ssl_ca_path,
                    w->immutable_settings_->ssl_cert_refresh_interval)

  {
    const bool rv =
        commonTimeouts_.add(std::chrono::microseconds(0), w->zero_timeout_);
    ld_check(rv);
  }

  ShardAuthoritativeStatusManager shardStatusManager_;
  Sender sender_;
  LogRebuildingMap runningLogRebuildings_;
  FindKeyRequestMap runningFindKey_;
  FireAndForgetRequestMap runningFireAndForgets_;
  TrimRequestMap runningTrimRequests_;
  GetTrimPointRequestMap runningGetTrimPoint_;
  IsLogEmptyRequestMap runningIsLogEmpty_;
  DataSizeRequestMap runningDataSize_;
  GetHeadAttributesRequestMap runningGetHeadAttributes_;
  GetClusterStateRequestMap runningGetClusterState_;
  GetEpochRecoveryMetadataRequestMap runningGetEpochRecoveryMetadata_;
  LogsConfigApiRequestMap runningLogManagementReqs_;
  CheckImpactRequestMap runningCheckImpactReqs_;
  LogsConfigManagerRequestMap runningLogsConfigManagerReqs_;
  LogsConfigManagerReplyMap runningLogsConfigManagerReplies_;
  TimeoutMap commonTimeouts_;
  AppendRequestMap runningAppends_;
  CheckSealRequestMap runningCheckSeals_;
  GetSeqStateRequestMap runningGetSeqState_;
  AppenderMap activeAppenders_;
  GetLogInfoRequestMaps runningGetLogInfo_;
  ClusterStateSubscriptionList clusterStateSubscriptions_;
  LogRecoveryRequestMap runningLogRecoveries_;
  SyncSequencerRequestList runningSyncSequencerRequests_;
  AppenderBuffer appenderBuffer_;
  AppenderBuffer previously_redirected_appends_;
  ChronoExponentialBackoffAdaptiveVariable<std::chrono::milliseconds>
      adaptive_store_delay_;
  LogIDUniqueQueue recoveryQueueDataLog_;
  LogIDUniqueQueue recoveryQueueMetaDataLog_;
  AllClientReadStreams clientReadStreams_;
  WriteMetaDataRecordMap runningWriteMetaDataRecords_;
  AppendRequestEpochMap appendRequestEpochMap_;
  CheckNodeHealthRequestSet pendingHealthChecks_;
  SSLFetcher sslFetcher_;
  std::unique_ptr<SequencerBackgroundActivator> sequencerBackgroundActivator_;
};

static std::string makeThreadName(Processor* processor,
                                  WorkerType type,
                                  worker_id_t idx) {
  const std::string& processor_name = processor->getName();
  std::array<char, 16> name_buf;
  snprintf(name_buf.data(),
           name_buf.size(),
           "%s:%s",
           processor_name.c_str(),
           Worker::getName(type, idx).c_str());
  return name_buf.data();
}

Worker::Worker(Processor* processor,
               worker_id_t idx,
               const std::shared_ptr<UpdateableConfig>& config,
               StatsHolder* stats,
               WorkerType worker_type,
               ThreadID::Type thread_type)
    : EventLoop(makeThreadName(processor, worker_type, idx), thread_type),
      processor_(processor),
      updateable_settings_(processor->updateableSettings()),
      immutable_settings_(processor->updateableSettings().get()),
      idx_(idx),
      worker_type_(worker_type),
      impl_(new WorkerImpl(this, config)),
      config_(config),
      stats_(stats),
      shutting_down_(false),
      accepting_work_(true) {}

size_t Worker::destroyZeroCopiedRecordsInDisposal() {
  return processor_->zeroCopiedRecordDisposal().drainRecords(
      worker_type_, idx_);
}

Worker::~Worker() {
  shutting_down_ = true;
  stopAcceptingWork();

  server_config_update_sub_.unsubscribe();
  logs_config_update_sub_.unsubscribe();
  updateable_settings_subscription_.unsubscribe();

  // BufferedWriter must have cleared this map by now
  ld_check(active_buffered_writers_.empty());

  dispose_metareader_timer_.reset();

  for (auto it = runningWriteMetaDataRecords().map.begin();
       it != runningWriteMetaDataRecords().map.end();) {
    WriteMetaDataRecord* write = it->second;
    it = runningWriteMetaDataRecords().map.erase(it);
    write->getDriver()->onWorkerShutdown();
  }

  // Free all ExponentialBackoffTimerNode objects in timers_
  for (auto it = timers_.begin(); it != timers_.end();) {
    ExponentialBackoffTimerNode& node = *it;
    it = timers_.erase(it);
    delete &node;
  }

  // destroy all Appenders to free up zero copied record that must be
  // disposed of in destroyZeroCopiedRecordsInDisposal() below.
  // Note: on graceful shutdown there should be no active appenders and
  // this is mostly used in situations where full graceful shutdown is not
  // used (e.g., tests)
  activeAppenders().map.clearAndDispose();

  // drain all zero-copied records on all workers before shutting
  // down ZeroCopiedRecordDisposal
  destroyZeroCopiedRecordsInDisposal();

  if (event_handlers_called_.load() != event_handlers_completed_.load()) {
    ld_info("EventHandlers called: %lu, EventHandlers completed: %lu",
            event_handlers_called_.load(),
            event_handlers_completed_.load());
  }
}

std::string Worker::getName(WorkerType type, worker_id_t idx) {
  return std::string("W") + workerTypeChar(type) + std::to_string(idx.val_);
}

std::shared_ptr<Configuration> Worker::getConfiguration() const {
  ld_check((bool)config_);
  return config_->get();
}

std::shared_ptr<ServerConfig> Worker::getServerConfig() const {
  ld_check((bool)config_);
  return config_->getServerConfig();
}

std::shared_ptr<LogsConfig> Worker::getLogsConfig() const {
  ld_check((bool)config_);
  return config_->getLogsConfig();
}

std::shared_ptr<configuration::ZookeeperConfig>
Worker::getZookeeperConfig() const {
  ld_check((bool)config_);
  return config_->getZookeeperConfig();
}

std::shared_ptr<UpdateableConfig> Worker::getUpdateableConfig() {
  ld_check((bool)config_);
  return config_;
}

void Worker::onLogsConfigUpdated() {
  clientReadStreams().noteConfigurationChanged();
  if (rebuilding_coordinator_) {
    rebuilding_coordinator_->noteConfigurationChanged();
  }
}

void Worker::onServerConfigUpdated() {
  ld_check(Worker::onThisThread() == this);
  dbg::thisThreadClusterName() =
      config_->get()->serverConfig()->getClusterName();

  sender().noteConfigurationChanged();
  clientReadStreams().noteConfigurationChanged();
  // propagate the config change to metadata sequencer
  runningWriteMetaDataRecords().noteConfigurationChanged();

  if (event_log_) {
    event_log_->noteConfigurationChanged();
  }

  if (idx_.val_ == 0 && worker_type_ == WorkerType::GENERAL) {
    // running this operation on worker 0 only. the cluster state
    // should be updated/resized only once since it is shared among workers.
    auto cs = getClusterState();
    if (cs) {
      cs->noteConfigurationChanged();
    }
  }
}

namespace {
class SettingsUpdatedRequest : public Request {
 public:
  explicit SettingsUpdatedRequest(worker_id_t worker_id, WorkerType worker_type)
      : Request(RequestType::SETTINGS_UPDATED),
        worker_id_(worker_id),
        worker_type_(worker_type) {}
  Request::Execution execute() override {
    Worker::onThisThread()->onSettingsUpdated();
    return Execution::COMPLETE;
  }
  int getThreadAffinity(int) override {
    return worker_id_.val_;
  }
  WorkerType getWorkerTypeAffinity() override {
    return worker_type_;
  }

 private:
  worker_id_t worker_id_;
  WorkerType worker_type_;
};
} // namespace

const Settings& Worker::settings() {
  Worker* w = onThisThread();
  ld_check(w->immutable_settings_);
  return *w->immutable_settings_;
}

void Worker::onSettingsUpdated() {
  // If SettingsUpdatedRequest are posted faster than they're processed,
  // each request will pick up multiple settings updates. This would mean
  // that after the first SettingsUpdatedRequest is processed, we already
  // have an uptodate setting and no further processing is necessary.
  std::shared_ptr<const Settings> new_settings = updateable_settings_.get();
  if (new_settings == immutable_settings_) {
    return;
  }

  immutable_settings_ = new_settings;
  getRequestPump()->setNumRequestsPerIteration(
      immutable_settings_->requests_per_iteration);
  clientReadStreams().noteSettingsUpdated();
  if (logsconfig_manager_) {
    // LogsConfigManager might want to start or stop the underlying RSM if
    // the enable-logsconfig-manager setting is changed.
    logsconfig_manager_->onSettingsUpdated();
  }
}

void Worker::initializeSubscriptions() {
  Processor* processor = processor_;
  worker_id_t idx = idx_;
  WorkerType worker_type = worker_type_;

  auto configUpdateCallback = [processor, idx, worker_type](
                                  bool is_server_config) {
    return [processor, idx, worker_type, is_server_config]() {
      // callback runs on unspecified thread so we need to post a Request
      // through the processor
      std::unique_ptr<Request> req;
      if (is_server_config) {
        req = std::make_unique<ServerConfigUpdatedRequest>(idx, worker_type);
      } else {
        req = std::make_unique<LogsConfigUpdatedRequest>(
            processor->settings()->configuration_update_retry_interval,
            idx,
            worker_type);
      }

      int rv = processor->postWithRetrying(req);

      if (rv != 0) {
        ld_error("error processing %s config update on worker #%d (%s): "
                 "postWithRetrying() failed with status %s",
                 is_server_config ? "server" : "logs",
                 idx.val_,
                 workerTypeStr(worker_type),
                 error_description(err));
      }
    };
  };

  // Subscribe to config updates
  server_config_update_sub_ =
      config_->updateableServerConfig()->subscribeToUpdates(
          configUpdateCallback(true));
  logs_config_update_sub_ = config_->updateableLogsConfig()->subscribeToUpdates(
      configUpdateCallback(false));

  // Pretend we got the config update - to make sure we didn't miss anything
  // before we subscribed
  onServerConfigUpdated();
  onLogsConfigUpdated();

  auto settingsUpdateCallback = [processor, idx, worker_type]() {
    std::unique_ptr<Request> request =
        std::make_unique<SettingsUpdatedRequest>(idx, worker_type);

    int rv = processor->postWithRetrying(request);

    if (rv != 0) {
      ld_error("error processing settings update on worker #%d: "
               "postWithRetrying() failed with status %s",
               idx.val_,
               error_description(err));
    }
  };
  // Subscribe to settings update
  updateable_settings_subscription_ =
      updateable_settings_.raw()->subscribeToUpdates(settingsUpdateCallback);
  // Ensuring that we didn't miss any updates by updating the local copy of
  // settings from the global one
  onSettingsUpdated();
}

void Worker::onThreadStarted() {
  requests_stuck_timer_ = std::make_unique<Timer>(
      std::bind(&Worker::reportOldestRecoveryRequest, this));
  load_timer_ = std::make_unique<Timer>(std::bind(&Worker::reportLoad, this));
  isolation_timer_ = std::make_unique<Timer>(
      std::bind(&Worker::disableSequencersDueIsolationTimeout, this));
  cluster_state_polling_ = std::make_unique<Timer>(
      []() { getClusterState()->refreshClusterStateAsync(); });

  // Now that virtual calls are available (unlike in the constructor),
  // initialise `message_dispatch_'
  message_dispatch_ = createMessageDispatch();

  if (stats_ && worker_type_ == WorkerType::GENERAL) {
    // Imprint the thread-local Stats object with our ID
    stats_->get().worker_id = idx_;
  }

  // Subscribe to config updates and setting updates
  initializeSubscriptions();

  // Initialize load reporting and start timer
  reportLoad();
  reportOldestRecoveryRequest();
}

const std::shared_ptr<TraceLogger> Worker::getTraceLogger() const {
  return processor_->getTraceLogger();
}

// Consider changing value of derived value time_delay_before_force_abort when
// changing value of this poll delay.
static std::chrono::milliseconds PENDING_REQUESTS_POLL_DELAY{50};

void Worker::stopAcceptingWork() {
  accepting_work_ = false;
}

void Worker::finishWorkAndCloseSockets() {
  ld_check(!accepting_work_);

  subclassFinishWork();

  size_t c = runningSyncSequencerRequests().getList().size();
  if (c) {
    runningSyncSequencerRequests().terminateRequests();
    ld_info("Aborted %lu sync sequencer requests", c);
  }

  // Abort all LogRebuilding state machines.
  std::vector<LogRebuildingInterface*> to_abort;
  for (auto& it : runningLogRebuildings().map) {
    to_abort.push_back(it.second.get());
  }
  if (!to_abort.empty()) {
    for (auto l : to_abort) {
      l->abort(false /* notify_complete */);
    }
    ld_info("Aborted %lu log rebuildings", to_abort.size());
  }
  if (rebuilding_coordinator_) {
    rebuilding_coordinator_->shutdown();
  }

  if (event_log_) {
    event_log_->stop();
    event_log_.reset();
  }

  if (logsconfig_manager_) {
    logsconfig_manager_->stop();
    logsconfig_manager_.reset();
  }

  // abort all fire-and-forget requests
  if (!runningFireAndForgets().map.empty()) {
    c = runningFireAndForgets().map.size();
    runningFireAndForgets().map.clear();
    ld_info("Aborted %lu fire-and-forget requests", c);
  }

  // abort get-trim-point requests
  if (!runningGetTrimPoint().map.empty()) {
    c = runningGetTrimPoint().map.size();
    runningGetTrimPoint().map.clear();
    ld_info("Aborted %lu get-trim-point requests", c);
  }

  // Kick off the following async sequence:
  //  1) wait for requestsPending() to become zero
  //  2) tear down state machines such as read streams
  //  3) shut down the communication layer
  //  4) processor_->noteWorkerQuiescent(idx_);

  if (requestsPending() == 0 && !waiting_for_sockets_to_close_) {
    // Destructors for ClientReadStream, ServerReadStream and
    // PerWorkerStorageTaskQueue may involve communication, so take care of
    // that before we tear down the messaging fabric.
    subclassWorkFinished();
    clientReadStreams().clear();
    noteShuttingDownNoPendingRequests();

    ld_info("Shutting down Sender");
    sender().beginShutdown();
    waiting_for_sockets_to_close_ = true;
    // Initialize timer to force close sockets.
    force_close_sockets_counter_ = settings().time_delay_before_force_abort;
  }

  if (requestsPending() == 0 && sender().isClosed()) {
    // already done
    ld_info("Worker finished closing sockets");
    processor_->noteWorkerQuiescent(idx_, worker_type_);

    ld_check(requests_pending_timer_ == nullptr ||
             !requests_pending_timer_->isActive());
  } else {
    // start a timer that'll periodically do the check
    if (requests_pending_timer_ == nullptr) {
      if (requestsPending() > 0) {
        ld_info("Waiting for requests to finish");
      }
      // Initialize the force abort timer.
      force_abort_pending_requests_counter_ =
          settings().time_delay_before_force_abort;
      requests_pending_timer_.reset(new Timer([this] {
        finishWorkAndCloseSockets();
        if (force_abort_pending_requests_counter_ > 0) {
          --force_abort_pending_requests_counter_;
          if (force_abort_pending_requests_counter_ == 0) {
            forceAbortPendingWork();
          }
        }

        if (force_close_sockets_counter_ > 0) {
          --force_close_sockets_counter_;
          if (force_close_sockets_counter_ == 0) {
            forceCloseSockets();
          }
        }
      }));
    }
    requests_pending_timer_->activate(PENDING_REQUESTS_POLL_DELAY);
  }
}

void Worker::forceAbortPendingWork() {
  {
    // Find if the appender map is non-empty. If it is non-empty send
    // AbortAppenderRequest aborting all the appenders on this worker.
    const auto& map = activeAppenders().map;
    ld_info("Is appender map empty %d", map.empty());
    if (!map.empty()) {
      std::unique_ptr<Request> req =
          std::make_unique<AbortAppendersEpochRequest>(idx_);
      processor_->postImportant(req);
    }
  }
}

void Worker::forceCloseSockets() {
  {
    // Close all sockets irrespective of pending work on them.
    auto sockets_closed = sender().closeAllSockets();

    ld_info("Num server sockets closed: %u, Num clients sockets closed: %u",
            sockets_closed.first,
            sockets_closed.second);
  }
}

void Worker::disableSequencersDueIsolationTimeout() {
  Worker::onThisThread(false)
      ->processor_->allSequencers()
      .disableAllSequencersDueToIsolation();
}

void Worker::reportOldestRecoveryRequest() {
  auto now = SteadyTimestamp::now();
  auto min = now;
  LogRecoveryRequest* oldest = nullptr;
  for (const auto& request : impl_->runningLogRecoveries_.map) {
    auto created = request.second->getCreationTimestamp();
    if (created < min) {
      min = created;
      oldest = request.second.get();
    }
  }
  auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - min);
  WORKER_STAT_SET(oldest_recovery_request, diff.count());

  if (diff > std::chrono::minutes(10) && oldest != nullptr) {
    auto describe_oldest_recovery = [&] {
      InfoRecoveriesTable table(getInfoRecoveriesTableColumns(), true);
      oldest->getDebugInfo(table);
      return table.toString();
    };
    RATELIMIT_WARNING(
        std::chrono::minutes(10),
        2,
        "Oldest log recovery is %ld seconds old:\n%s",
        std::chrono::duration_cast<std::chrono::seconds>(diff).count(),
        describe_oldest_recovery().c_str());
  }

  requests_stuck_timer_->activate(std::chrono::minutes(1));
}

EpochRecovery* Worker::findActiveEpochRecovery(logid_t logid) const {
  ld_check(logid != LOGID_INVALID);

  auto it = runningLogRecoveries().map.find(logid);
  if (it == runningLogRecoveries().map.end()) {
    err = E::NOTFOUND;
    return nullptr;
  }

  LogRecoveryRequest* log_recovery = it->second.get();

  ld_check(log_recovery);

  EpochRecovery* erm = log_recovery->getActiveEpochRecovery();

  if (!erm) {
    err = E::NOTFOUND;
  }

  return erm;
}

bool Worker::requestsPending() const {
  std::vector<std::string> counts;
#define PROCESS(x, name)                                     \
  if (!x.empty()) {                                          \
    counts.push_back(std::to_string(x.size()) + " " + name); \
  }

  PROCESS(activeAppenders().map, "appenders");
  PROCESS(runningFindKey().map, "findkeys");
  PROCESS(runningFireAndForgets().map, "fire and forgets");
  PROCESS(runningGetLogInfo().gli_map, "get log infos");
  PROCESS(runningGetLogInfo().per_node_map, "per-node get log infos");
  PROCESS(runningGetTrimPoint().map, "get log trim points");
  PROCESS(runningTrimRequests().map, "trim requests");
  PROCESS(runningLogRebuildings().map, "log rebuildings");
  PROCESS(runningSyncSequencerRequests().getList(), "sync sequencer requests");
#undef PROCESS

  if (counts.empty()) {
    return false;
  }
  RATELIMIT_INFO(std::chrono::seconds(5),
                 5,
                 "Pending requests: %s",
                 folly::join(", ", counts).c_str());
  return true;
}

void Worker::popRecoveryRequest() {
  auto& metadataqueue_index =
      recoveryQueueMetaDataLog().q.get<LogIDUniqueQueue::FIFOIndex>();
  auto& dataqueue_index =
      recoveryQueueDataLog().q.get<LogIDUniqueQueue::FIFOIndex>();

  while (true) {
    auto& index =
        metadataqueue_index.size() > 0 ? metadataqueue_index : dataqueue_index;

    if (index.size() == 0) {
      return;
    }

    ld_check(index.size() > 0);
    auto& seqmap = processor_->allSequencers();

    auto it = index.begin();
    std::shared_ptr<Sequencer> seq = seqmap.findSequencer(*it);

    if (!seq) {
      // Sequencer went away.
      if (ld_catch(MetaDataLog::isMetaDataLog(*it) && err == E::NOSEQUENCER,
                   "INTERNAL ERROR: couldn't find sequencer for queued "
                   "recovery for log %lu: %s",
                   it->val_,
                   error_name(err))) {
        ld_info("No sequencer for queued recovery for log %lu", it->val_);
      }
      WORKER_STAT_INCR(recovery_completed);
    } else if (seq->startRecovery() != 0) {
      ld_error("Failed to start recovery for log %lu: %s",
               it->val_,
               error_description(err));
      return;
    }

    WORKER_STAT_DECR(recovery_enqueued);
    index.erase(it);
    return;
  }
}

ExponentialBackoffTimerNode* Worker::registerTimer(
    std::function<void(ExponentialBackoffTimerNode*)> callback,
    const chrono_expbackoff_t<ExponentialBackoffTimer::Duration>& settings) {
  auto timer = std::make_unique<ExponentialBackoffTimer>(
      std::function<void()>(), settings);
  ExponentialBackoffTimerNode* node =
      new ExponentialBackoffTimerNode(std::move(timer));

  node->timer->setCallback([node, cb = std::move(callback)]() { cb(node); });

  timers_.push_back(*node);
  // The node (and timer) is semi-owned by the Worker now.  It can get deleted
  // by the caller; if not, it will get destroyed in our destructor.
  return node;
}

void Worker::disposeOfMetaReader(std::unique_ptr<MetaDataLogReader> reader) {
  ld_check(reader);
  if (shutting_down_) {
    // we are in the destructor of the Worker and members of the Worker may
    // have already gotten destroyed. destroy the reader directly and return.
    return;
  }

  // create the timer if not exist
  if (accepting_work_ && dispose_metareader_timer_ == nullptr) {
    dispose_metareader_timer_.reset(new Timer([this] {
      while (!finished_meta_readers_.empty()) {
        finished_meta_readers_.pop();
      }
    }));
  }

  reader->finalize();
  finished_meta_readers_.push(std::move(reader));
  if (accepting_work_ && dispose_metareader_timer_) {
    dispose_metareader_timer_->activate(
        std::chrono::milliseconds::zero(), &commonTimeouts());
  }
}

void Worker::reportLoad() {
  if (worker_type_ != WorkerType::GENERAL) {
    // specialised workers are not meant to take normal worker load,
    // therefore skipping it from load balancing calculations
    return;
  }

  using namespace std::chrono;
  auto now = steady_clock::now();

  struct rusage usage;
  int rv = getrusage(RUSAGE_THREAD, &usage);
  ld_check(rv == 0);

  int64_t now_load = (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000 +
      (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);

  if (last_load_ >= 0) {
    // We'll report load in CPU microseconds per wall clock second.  This
    // should be a number on the order of millions, which should be
    // comfortable for subsequent handling.
    int64_t load_delta = (now_load - last_load_) /
        duration_cast<duration<double>>(now - last_load_time_).count();

    PER_WORKER_STAT_ADD_SAMPLE(Worker::stats(), idx_, load_delta);

    ld_spew("%s reporting load %ld", getName().c_str(), load_delta);
    processor_->reportLoad(idx_, load_delta, worker_type_);
  }

  last_load_ = now_load;
  last_load_time_ = now;
  load_timer_->activate(seconds(10));
}

EventLogStateMachine* Worker::getEventLogStateMachine() {
  return event_log_.get();
}

void Worker::setEventLogStateMachine(
    std::unique_ptr<EventLogStateMachine> event_log) {
  ld_check(event_log_ == nullptr);
  event_log->setWorkerId(idx_);
  event_log_ = std::move(event_log);
}

void Worker::setLogsConfigManager(std::unique_ptr<LogsConfigManager> manager) {
  ld_check(logsconfig_manager_ == nullptr);
  logsconfig_manager_ = std::move(manager);
}

LogsConfigManager* Worker::getLogsConfigManager() {
  return logsconfig_manager_.get();
}

void Worker::setRebuildingCoordinator(
    RebuildingCoordinatorInterface* rebuilding_coordinator) {
  rebuilding_coordinator_ = rebuilding_coordinator;
}

ClusterState* Worker::getClusterState() {
  return Worker::onThisThread()->processor_->cluster_state_.get();
}

void Worker::onStoppedRunning(RunState prev_state) {
  Worker* w = Worker::onThisThread();
  ld_check(w);
  std::chrono::steady_clock::time_point start_time;
  start_time = w->currentlyRunningStart_;

  setCurrentlyRunningState(RunState(), prev_state);

  auto end_time = w->currentlyRunningStart_;
  // Bumping the counters
  if (end_time - start_time >= settings().request_execution_delay_threshold) {
    RATELIMIT_WARNING(std::chrono::seconds(1),
                      2,
                      "Slow request/timer callback: %.3fs, source: %s",
                      std::chrono::duration_cast<std::chrono::duration<double>>(
                          end_time - start_time)
                          .count(),
                      prev_state.describe().c_str());
    WORKER_STAT_INCR(worker_slow_requests);
  }
  auto usec = std::chrono::duration_cast<std::chrono::microseconds>(end_time -
                                                                    start_time)
                  .count();
  switch (prev_state.type_) {
    case RunState::MESSAGE: {
      auto msg_type = static_cast<int>(prev_state.subtype_.message);
      ld_check(msg_type < static_cast<int>(MessageType::MAX));
      MESSAGE_TYPE_STAT_ADD(
          Worker::stats(), msg_type, message_worker_usec, usec);
      HISTOGRAM_ADD(Worker::stats(), message_callback_duration[msg_type], usec);
      break;
    }
    case RunState::REQUEST: {
      auto rqtype = static_cast<int>(prev_state.subtype_.request);
      ld_check(rqtype < static_cast<int>(RequestType::MAX));
      REQUEST_TYPE_STAT_ADD(Worker::stats(), rqtype, request_worker_usec, usec);
      HISTOGRAM_ADD(Worker::stats(), request_execution_duration[rqtype], usec);
      break;
    }
    case RunState::NONE: {
      REQUEST_TYPE_STAT_ADD(
          Worker::stats(), RequestType::INVALID, request_worker_usec, usec);
      HISTOGRAM_ADD(
          Worker::stats(),
          request_execution_duration[static_cast<int>(RequestType::INVALID)],
          usec);
      break;
    }
  }
}

void Worker::onStartedRunning(RunState new_state) {
  setCurrentlyRunningState(new_state, RunState());
}

void Worker::activateIsolationTimer() {
  isolation_timer_->activate(immutable_settings_->isolated_sequencer_ttl);
}

void Worker::activateClusterStatePolling() {
  cluster_state_polling_->activate(std::chrono::seconds(600));
}

void Worker::deactivateIsolationTimer() {
  isolation_timer_->cancel();
}

void Worker::setCurrentlyRunningState(RunState new_state, RunState prev_state) {
#ifndef NDEBUG
  if ((ThreadID::isWorker()) &&
      !dynamic_cast<Worker*>(EventLoop::onThisThread())) {
    RATELIMIT_ERROR(std::chrono::seconds(10),
                    10,
                    "Attempting to set worker state on a worker being "
                    "destroyed to: %s, expected state: %s.",
                    new_state.describe().c_str(),
                    prev_state.describe().c_str());
    ld_check(false);
    return;
  }
#endif

  Worker* w = Worker::onThisThread(false);
  if (!w) {
    RATELIMIT_ERROR(std::chrono::seconds(10),
                    10,
                    "Attempting to set worker state while not on a worker. New "
                    "state: %s, expected state: %s.",
                    new_state.describe().c_str(),
                    prev_state.describe().c_str());
    ld_check(false);
    return;
  }
  ld_check(w->currentlyRunning_ == prev_state);
  w->currentlyRunning_ = new_state;
  w->currentlyRunningStart_ = std::chrono::steady_clock::now();
}

std::unique_ptr<MessageDispatch> Worker::createMessageDispatch() {
  // We create a vanilla MessageDispatch instance; ServerWorker and
  // ClientWorker create more specific ones
  return std::make_unique<MessageDispatch>();
}

// Stashes current RunState and pauses its timer. Returns everything needed to
// restore it. Use it for nesting RunStates.
std::tuple<RunState, std::chrono::steady_clock::duration>
Worker::packRunState() {
#ifndef NDEBUG
  if ((ThreadID::isWorker()) &&
      !dynamic_cast<Worker*>(EventLoop::onThisThread())) {
    RATELIMIT_ERROR(std::chrono::seconds(10),
                    10,
                    "Attempting to pack worker state on a worker being "
                    "destroyed.");
    ld_check(false);
    return std::make_tuple(RunState(), std::chrono::steady_clock::duration(0));
  }
#endif

  Worker* w = Worker::onThisThread(false);
  if (!w) {
    RATELIMIT_ERROR(std::chrono::seconds(10),
                    10,
                    "Attempting to pack worker state while not on a worker.");
    ld_check(false);
    return std::make_tuple(RunState(), std::chrono::steady_clock::duration(0));
  }
  auto res = std::make_tuple(
      w->currentlyRunning_,
      std::chrono::steady_clock::now() - w->currentlyRunningStart_);
  w->currentlyRunning_ = RunState();
  w->currentlyRunningStart_ = std::chrono::steady_clock::now();
  return res;
}

void Worker::unpackRunState(
    std::tuple<RunState, std::chrono::steady_clock::duration> s) {
#ifndef NDEBUG
  if ((ThreadID::isWorker()) &&
      !dynamic_cast<Worker*>(EventLoop::onThisThread())) {
    RATELIMIT_ERROR(std::chrono::seconds(10),
                    10,
                    "Attempting to unpack worker state on a worker being "
                    "destroyed.");
    ld_check(false);
    return;
  }
#endif

  Worker* w = Worker::onThisThread(false);
  if (!w) {
    RATELIMIT_ERROR(std::chrono::seconds(10),
                    10,
                    "Attempting to pack worker state while not on a worker.");
    ld_check(false);
    return;
  }
  ld_check(w->currentlyRunning_.type_ == RunState::Type::NONE);
  w->currentlyRunning_ = std::get<0>(s);
  w->currentlyRunningStart_ = std::chrono::steady_clock::now() - std::get<1>(s);
}

//
// Pimpl getters
//

Sender& Worker::sender() const {
  return impl_->sender_;
}

LogRebuildingMap& Worker::runningLogRebuildings() const {
  return impl_->runningLogRebuildings_;
}

FindKeyRequestMap& Worker::runningFindKey() const {
  return impl_->runningFindKey_;
}

FireAndForgetRequestMap& Worker::runningFireAndForgets() const {
  return impl_->runningFireAndForgets_;
}

TrimRequestMap& Worker::runningTrimRequests() const {
  return impl_->runningTrimRequests_;
}

GetTrimPointRequestMap& Worker::runningGetTrimPoint() const {
  return impl_->runningGetTrimPoint_;
}

IsLogEmptyRequestMap& Worker::runningIsLogEmpty() const {
  return impl_->runningIsLogEmpty_;
}

DataSizeRequestMap& Worker::runningDataSize() const {
  return impl_->runningDataSize_;
}

GetHeadAttributesRequestMap& Worker::runningGetHeadAttributes() const {
  return impl_->runningGetHeadAttributes_;
}

GetClusterStateRequestMap& Worker::runningGetClusterState() const {
  return impl_->runningGetClusterState_;
}

LogsConfigApiRequestMap& Worker::runningLogsConfigApiRequests() const {
  return impl_->runningLogManagementReqs_;
}

CheckImpactRequestMap& Worker::runningCheckImpactRequests() const {
  return impl_->runningCheckImpactReqs_;
}

LogsConfigManagerRequestMap& Worker::runningLogsConfigManagerRequests() const {
  return impl_->runningLogsConfigManagerReqs_;
}

LogsConfigManagerReplyMap& Worker::runningLogsConfigManagerReplies() const {
  return impl_->runningLogsConfigManagerReplies_;
}

TimeoutMap& Worker::commonTimeouts() const {
  return impl_->commonTimeouts_;
}

AppenderMap& Worker::activeAppenders() const {
  return impl_->activeAppenders_;
}

AppendRequestMap& Worker::runningAppends() const {
  return impl_->runningAppends_;
}

CheckSealRequestMap& Worker::runningCheckSeals() const {
  return impl_->runningCheckSeals_;
}

GetSeqStateRequestMap& Worker::runningGetSeqState() const {
  return impl_->runningGetSeqState_;
}

GetLogInfoRequestMaps& Worker::runningGetLogInfo() const {
  return impl_->runningGetLogInfo_;
}

ClusterStateSubscriptionList& Worker::clusterStateSubscriptions() const {
  return impl_->clusterStateSubscriptions_;
}

AppenderBuffer& Worker::appenderBuffer() const {
  return impl_->appenderBuffer_;
}

AppenderBuffer& Worker::previouslyRedirectedAppends() const {
  return impl_->previously_redirected_appends_;
}

ChronoExponentialBackoffAdaptiveVariable<std::chrono::milliseconds>&
Worker::adaptiveStoreDelay() {
  return impl_->adaptive_store_delay_;
}

LogRecoveryRequestMap& Worker::runningLogRecoveries() const {
  return impl_->runningLogRecoveries_;
}

SyncSequencerRequestList& Worker::runningSyncSequencerRequests() const {
  return impl_->runningSyncSequencerRequests_;
}

LogIDUniqueQueue& Worker::recoveryQueueDataLog() const {
  return impl_->recoveryQueueDataLog_;
}

LogIDUniqueQueue& Worker::recoveryQueueMetaDataLog() const {
  return impl_->recoveryQueueMetaDataLog_;
}

ShardAuthoritativeStatusManager& Worker::shardStatusManager() const {
  return impl_->shardStatusManager_;
}

AllClientReadStreams& Worker::clientReadStreams() const {
  return impl_->clientReadStreams_;
}

WriteMetaDataRecordMap& Worker::runningWriteMetaDataRecords() const {
  return impl_->runningWriteMetaDataRecords_;
}

AppendRequestEpochMap& Worker::appendRequestEpochMap() const {
  return impl_->appendRequestEpochMap_;
}

CheckNodeHealthRequestSet& Worker::pendingHealthChecks() const {
  return impl_->pendingHealthChecks_;
}

GetEpochRecoveryMetadataRequestMap&
Worker::runningGetEpochRecoveryMetadata() const {
  return impl_->runningGetEpochRecoveryMetadata_;
}

SSLFetcher& Worker::sslFetcher() const {
  return impl_->sslFetcher_;
}

std::unique_ptr<SequencerBackgroundActivator>&
Worker::sequencerBackgroundActivator() const {
  return impl_->sequencerBackgroundActivator_;
}

std::string Worker::describeMyNode() {
  auto worker = Worker::onThisThread(false);
  if (!worker) {
    return "Can't find Processor";
  }
  return worker->processor_->describeMyNode();
}
}} // namespace facebook::logdevice
