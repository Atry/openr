/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/zmq/Zmq.h>

#include <openr/common/LsdbUtil.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/OpenrCtrlCppAsyncClient.h>
#include <openr/kvstore/KvStore.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/monitor/LogSample.h>
#include <openr/tests/OpenrThriftServerWrapper.h>

namespace openr {

/**
 * A utility class to wrap and interact with KvStore. It exposes the APIs to
 * send commands to and receive publications from KvStore.
 * Mainly used for testing.
 *
 * Not thread-safe, use from the same thread only.
 */
class KvStoreWrapper {
 public:
  KvStoreWrapper(
      // [TO_BE_DEPRECATED]
      fbzmq::Context& zmqContext,
      // areaId collection
      const std::unordered_set<std::string>& areaIds,
      // KvStoreConfig to drive the instance
      const thrift::KvStoreConfig& kvStoreConfig,
      // Queue for receiving peer updates
      std::optional<messaging::RQueue<PeerEvent>> peerUpdatesQueue =
          std::nullopt,
      // Queue for receiving key-value update requests
      std::optional<messaging::RQueue<KeyValueRequest>> kvRequestQueue =
          std::nullopt);

  ~KvStoreWrapper() {
    stop();
  }

  /**
   * Synchronous APIs to run and stop KvStore. This creates a thread
   * and stop it on destruction.
   *
   * Synchronous => function call with return only after thread is
   *                running/stopped completely.
   */
  void run() noexcept;
  void stop();

  /**
   * Get reader for KvStore updates queue
   */
  messaging::RQueue<KvStorePublication>
  getReader() {
    return kvStoreUpdatesQueue_.getReader();
  }

  /**
   * Get reader for KvStore Initial Sync queue
   */
  messaging::RQueue<KvStoreSyncEvent>
  getInitialSyncEventsReader() {
    return kvStoreSyncEventsQueue_.getReader();
  }

  void
  openQueue() {
    kvStoreUpdatesQueue_.open();
    kvStoreSyncEventsQueue_.open();
  }

  void
  closeQueue() {
    kvStoreUpdatesQueue_.close();
    kvStoreSyncEventsQueue_.close();
  }

  void
  stopThriftServer() {
    thriftServer_->stop();
    thriftServer_.reset();
  }

  /**
   * APIs to set key-value into the KvStore. Returns true on success else
   * returns false.
   */
  bool setKey(
      AreaId const& area,
      std::string key,
      thrift::Value value,
      std::optional<std::vector<std::string>> nodeIds = std::nullopt);

  /**
   * API to retrieve an existing key-value from KvStore. Returns empty if none
   * exists.
   */
  std::optional<thrift::Value> getKey(AreaId const& area, std::string key);

  /**
   * APIs to set key-values into the KvStore. Returns true on success else
   * returns false.
   */
  bool setKeys(
      AreaId const& area,
      const std::vector<std::pair<std::string, thrift::Value>>& keyVals,
      std::optional<std::vector<std::string>> nodeIds = std::nullopt);

  void
  publishKvStoreSynced() {
    kvStoreUpdatesQueue_.push(thrift::InitializationEvent::KVSTORE_SYNCED);
  }

  void pushToKvStoreUpdatesQueue(
      const AreaId& area,
      const std::unordered_map<std::string /* key */, thrift::Value>& keyVals);

  /**
   * API to get dump from KvStore.
   * if we pass a prefix, only return keys that match it
   */
  std::unordered_map<std::string /* key */, thrift::Value> dumpAll(
      AreaId const& area, std::optional<KvStoreFilters> filters = std::nullopt);

  /**
   * API to get dump hashes from KvStore.
   * if we pass a prefix, only return keys that match it
   */
  std::unordered_map<std::string /* key */, thrift::Value> dumpHashes(
      AreaId const& area, std::string const& prefix = "");

  /**
   * API to get dump of self originated key-vals from KvStore.
   */
  SelfOriginatedKeyVals dumpAllSelfOriginated(AreaId const& area);

  /**
   * API to get key vals on which hash differs from provided keyValHashes.
   */
  std::unordered_map<std::string /* key */, thrift::Value> syncKeyVals(
      AreaId const& area, thrift::KeyVals const& keyValHashes);

  /**
   * API to listen for a publication on PUB queue.
   */
  thrift::Publication recvPublication();

  /**
   * API to listen for KvStore sync signal on PUB queue.
   */
  void recvKvStoreSyncedSignal();

  /*
   * Get flooding topology information
   */
  thrift::SptInfos getFloodTopo(AreaId const& area);

  /**
   * APIs to manage (add/remove) KvStore peers. Returns true on success else
   * returns false.
   */
  bool addPeer(AreaId const& area, std::string peerName, thrift::PeerSpec spec);
  bool addPeers(AreaId const& area, thrift::PeersMap& peers);
  bool delPeer(AreaId const& area, std::string peerName);

  std::optional<thrift::KvStorePeerState> getPeerState(
      AreaId const& area, std::string const& peerName);

  /**
   * APIs to get existing peers of a KvStore.
   */
  std::unordered_map<std::string /* peerName */, thrift::PeerSpec> getPeers(
      AreaId const& area);

  /**
   * API to get summary of each KvStore area provided as input.
   */
  std::vector<thrift::KvStoreAreaSummary> getSummary(
      std::set<std::string> selectAreas);

  /**
   * Utility function to get peer-spec for owned KvStore
   */
  thrift::PeerSpec
  getPeerSpec(thrift::KvStorePeerState state = thrift::KvStorePeerState::IDLE) {
    return createPeerSpec(
        globalCmdUrl_, /* cmdUrl for ZMQ */
        Constants::kPlatformHost.toString(), /* peerAddr for thrift */
        getThriftPort(),
        state,
        kvStoreConfig_.enable_flood_optimization_ref().value_or(false));
  }

  /**
   * Get counters from KvStore
   */
  std::map<std::string, int64_t>
  getCounters() {
    return kvStore_->semifuture_getCounters().get();
  }

  KvStore<thrift::OpenrCtrlCppAsyncClient>*
  getKvStore() {
    return kvStore_.get();
  }

  inline std::shared_ptr<OpenrCtrlHandler>&
  getThriftServerCtrlHandler() {
    return thriftServer_->getOpenrCtrlHandler();
  }

  inline uint16_t
  getThriftPort() {
    return thriftServer_->getOpenrCtrlThriftPort();
  }

  const std::string
  getNodeId() {
    return this->nodeId_;
  }

  std::unordered_set<std::string>
  getAreaIds() {
    return areaIds_;
  }

 private:
  const std::string nodeId_;

  // Global URLs could be created outside of kvstore, mainly for testing
  const std::string globalCmdUrl_;

  // AreaId collection to indicate # of KvStoreDb spawn for different areas
  const std::unordered_set<std::string> areaIds_;

  // thrift::KvStoreConfig to feed to the KvStore instance
  const thrift::KvStoreConfig kvStoreConfig_;

  // Queue for streaming KvStore updates
  messaging::ReplicateQueue<KvStorePublication> kvStoreUpdatesQueue_;
  messaging::RQueue<KvStorePublication> kvStoreUpdatesQueueReader_{
      kvStoreUpdatesQueue_.getReader()};

  // Queue to get KvStore Initial Sync Updates
  messaging::ReplicateQueue<KvStoreSyncEvent> kvStoreSyncEventsQueue_;

  // Queue for publishing the event log
  messaging::ReplicateQueue<LogSample> logSampleQueue_;

  // Queue for streaming peer updates from LM
  messaging::ReplicateQueue<PeerEvent> dummyPeerUpdatesQueue_;

  // Emtpy queue for streaming key events from sources which persist keys into
  // KvStore Will be removed once KvStoreClientInternal is deprecated
  messaging::ReplicateQueue<KeyValueRequest> dummyKvRequestQueue_;

  // KvStore owned by this wrapper.
  std::unique_ptr<KvStore<thrift::OpenrCtrlCppAsyncClient>> kvStore_;

  // Thrift Server owned by this warpper;
  std::unique_ptr<OpenrThriftServerWrapper> thriftServer_;

  // Thread in which KvStore will be running.
  std::thread kvStoreThread_;
};

} // namespace openr
