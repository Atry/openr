/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KvStore.h"

#include <fb303/ServiceData.h>
#include <fbzmq/service/logging/LogSample.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/GLog.h>
#include <folly/Random.h>
#include <folly/String.h>

#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/OpenrCtrl_types.h>

using namespace std::chrono;

namespace fb303 = facebook::fb303;

namespace {
std::optional<openr::KvStoreFilters>
getKvStoreFilters(std::shared_ptr<const openr::Config> config) {
  std::optional<openr::KvStoreFilters> kvFilters{std::nullopt};
  // Add key prefixes to allow if set as leaf node
  if (config->getKvStoreConfig().set_leaf_node_ref().value_or(false)) {
    std::vector<std::string> keyPrefixFilters;
    if (auto v = config->getKvStoreConfig().key_prefix_filters_ref()) {
      keyPrefixFilters = *v;
    }
    keyPrefixFilters.push_back(openr::Constants::kPrefixAllocMarker.toString());
    keyPrefixFilters.push_back(
        openr::Constants::kNodeLabelRangePrefix.toString());

    // save nodeIds in the set
    std::set<std::string> originatorIdFilters{};
    for (const auto& id :
         config->getKvStoreConfig().key_originator_id_filters_ref().value_or(
             {})) {
      originatorIdFilters.insert(id);
    }
    originatorIdFilters.insert(config->getNodeName());
    kvFilters = openr::KvStoreFilters(keyPrefixFilters, originatorIdFilters);
  }
  return kvFilters;
}
} // namespace

namespace openr {

KvStoreFilters::KvStoreFilters(
    std::vector<std::string> const& keyPrefix,
    std::set<std::string> const& nodeIds)
    : keyPrefixList_(keyPrefix),
      originatorIds_(nodeIds),
      keyPrefixObjList_(KeyPrefix(keyPrefixList_)) {}

bool
KvStoreFilters::keyMatchAny(
    std::string const& key, thrift::Value const& value) const {
  if (keyPrefixList_.empty() && originatorIds_.empty()) {
    return true;
  }
  if (!keyPrefixList_.empty() && keyPrefixObjList_.keyMatch(key)) {
    return true;
  }
  if (!originatorIds_.empty() && originatorIds_.count(value.originatorId)) {
    return true;
  }
  return false;
}

bool
KvStoreFilters::keyMatch(
    std::string const& key,
    thrift::Value const& value,
    thrift::FilterOperator const& oper) const {
  if (oper == thrift::FilterOperator::OR) {
    return keyMatchAny(key, value);
  }
  return keyMatchAll(key, value);
}

// The function return true if there is a match on all the attributes
// such as key prefix and originator ids.
bool
KvStoreFilters::keyMatchAll(
    std::string const& key, thrift::Value const& value) const {
  if (keyPrefixList_.empty() && originatorIds_.empty()) {
    // No filter and nothing to match against.
    return true;
  }

  if (!keyPrefixList_.empty() && not keyPrefixObjList_.keyMatch(key)) {
    return false;
  }

  if (!originatorIds_.empty() && not originatorIds_.count(value.originatorId)) {
    return false;
  }

  return true;
}

std::vector<std::string>
KvStoreFilters::getKeyPrefixes() const {
  return keyPrefixList_;
}

std::set<std::string>
KvStoreFilters::getOriginatorIdList() const {
  return originatorIds_;
}

std::string
KvStoreFilters::str() const {
  std::string result{};
  result += "\nPrefix filters:\n";
  for (const auto& prefixString : keyPrefixList_) {
    result += folly::sformat("{}, ", prefixString);
  }
  result += "\nOriginator ID filters:\n";
  for (const auto& originatorId : originatorIds_) {
    result += folly::sformat("{}, ", originatorId);
  }
  return result;
}

KvStore::KvStore(
    // initializers for immutable state
    fbzmq::Context& zmqContext,
    messaging::ReplicateQueue<thrift::Publication>& kvStoreUpdatesQueue,
    messaging::RQueue<thrift::PeerUpdateRequest> peerUpdateQueue,
    KvStoreGlobalCmdUrl globalCmdUrl,
    MonitorSubmitUrl monitorSubmitUrl,
    std::shared_ptr<const Config> config,
    std::optional<int> maybeIpTos,
    int zmqHwm,
    bool enableKvStoreThrift,
    bool enablePeriodicSync)
    : kvParams_(
          config->getNodeName(),
          kvStoreUpdatesQueue,
          fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>(
              zmqContext,
              fbzmq::IdentityString{
                  folly::sformat("{}::TCP::CMD", config->getNodeName())},
              folly::none,
              fbzmq::NonblockingFlag{true}),
          zmqHwm,
          enableKvStoreThrift,
          enablePeriodicSync,
          maybeIpTos,
          std::chrono::seconds(config->getKvStoreConfig().sync_interval_s),
          getKvStoreFilters(config),
          config->getKvStoreConfig().flood_rate_ref().to_optional(),
          std::chrono::milliseconds(
              config->getKvStoreConfig().ttl_decrement_ms),
          config->getKvStoreConfig().enable_flood_optimization_ref().value_or(
              false),
          config->getKvStoreConfig().is_flood_root_ref().value_or(false)),
      areas_(config->getAreaIds()) {
  zmqMonitorClient_ =
      std::make_shared<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);
  kvParams_.zmqMonitorClient = zmqMonitorClient_;

  // Schedule periodic timer for counters submission
  counterUpdateTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    for (auto& counter : getGlobalCounters()) {
      fb303::fbData->setCounter(counter.first, counter.second);
    }
    counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);
  });
  counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);

  // Prepare global command socket
  prepareSocket(kvParams_.globalCmdSock, std::string(globalCmdUrl), maybeIpTos);
  addSocket(
      fbzmq::RawZmqSocketPtr{*kvParams_.globalCmdSock},
      ZMQ_POLLIN,
      [this](int) noexcept {
        // Drain all available messages in loop
        while (true) {
          // NOTE: globalCmSock is connected with neighbor's peerSyncSock_.
          // recvMultiple() will get a vector of fbzmq::Message which has:
          //  1) requestIdMsg; 2) delimMsg; 3) kvStoreRequestMsg;
          auto maybeReq = kvParams_.globalCmdSock.recvMultiple();
          if (maybeReq.hasError() and maybeReq.error().errNum == EAGAIN) {
            break;
          }

          if (maybeReq.hasError()) {
            LOG(ERROR) << "failed reading messages from globalCmdSock: "
                       << maybeReq.error();
            continue;
          }

          processCmdSocketRequest(std::move(maybeReq).value());
        } // while
      });

  // Add reader to process peer updates from LinkMonitor
  addFiberTask([q = std::move(peerUpdateQueue), this]() mutable noexcept {
    LOG(INFO) << "Starting peer updates processing fiber";
    while (true) {
      auto maybePeerUpdate = q.get(); // perform read
      VLOG(2) << "Received peer update...";
      if (maybePeerUpdate.hasError()) {
        LOG(INFO) << "Terminating peer updates processing fiber";
        break;
      }
      try {
        processPeerUpdates(std::move(maybePeerUpdate).value());
      } catch (const std::exception& ex) {
        LOG(ERROR) << "Failed to process peer request. Exception: "
                   << ex.what();
      }
    }
  });

  // create KvStoreDb instances
  for (auto const& area : areas_) {
    kvStoreDb_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(area),
        std::forward_as_tuple(
            this,
            kvParams_,
            area,
            fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_CLIENT>(
                zmqContext,
                fbzmq::IdentityString{
                    createPeerSyncId(config->getNodeName(), area)},
                folly::none,
                fbzmq::NonblockingFlag{true}),
            config->getKvStoreConfig().is_flood_root_ref().value_or(false),
            config->getNodeName()));
  }
}

void
KvStore::stop() {
  getEvb()->runImmediatelyOrRunInEventBaseThreadAndWait([this]() {
    // NOTE: destructor of every instance inside `kvStoreDb_` will gracefully
    //       exit and wait for all pending thrift requests to be processed
    //       before eventbase stops.
    kvStoreDb_.clear();
  });

  // Invoke stop method of super class
  OpenrEventBase::stop();
}

// static, public
std::unordered_map<std::string, thrift::Value>
KvStore::mergeKeyValues(
    std::unordered_map<std::string, thrift::Value>& kvStore,
    std::unordered_map<std::string, thrift::Value> const& keyVals,
    std::optional<KvStoreFilters> const& filters) {
  // the publication to build if we update our KV store
  std::unordered_map<std::string, thrift::Value> kvUpdates;

  // Counters for logging
  uint32_t ttlUpdateCnt{0}, valUpdateCnt{0};

  for (const auto& [key, value] : keyVals) {
    if (filters.has_value() && not filters->keyMatch(key, value)) {
      VLOG(4) << "key: " << key << " not adding from " << value.originatorId;
      continue;
    }

    // versions must start at 1; setting this to zero here means
    // we would be beaten by any version supplied by the setter
    int64_t myVersion{0};
    int64_t newVersion = value.version;

    // Check if TTL is valid. It must be infinite or positive number
    // Skip if invalid!
    if (value.ttl != Constants::kTtlInfinity && value.ttl <= 0) {
      continue;
    }

    // if key exist, compare values first
    // if they are the same, no need to propagate changes
    auto kvStoreIt = kvStore.find(key);
    if (kvStoreIt != kvStore.end()) {
      myVersion = kvStoreIt->second.version;
    } else {
      VLOG(4) << "(mergeKeyValues) key: '" << key << "' not found, adding";
    }

    // If we get an old value just skip it
    if (newVersion < myVersion) {
      continue;
    }

    bool updateAllNeeded{false};
    bool updateTtlNeeded{false};

    //
    // Check updateAll and updateTtl
    //
    if (value.value_ref().has_value()) {
      if (newVersion > myVersion) {
        // Version is newer or
        // kvStoreIt is NULL(myVersion is set to 0)
        updateAllNeeded = true;
      } else if (value.originatorId > kvStoreIt->second.originatorId) {
        // versions are the same but originatorId is higher
        updateAllNeeded = true;
      } else if (value.originatorId == kvStoreIt->second.originatorId) {
        // This can occur after kvstore restarts or simply reconnects after
        // disconnection. We let one of the two values win if they
        // differ(higher in this case but can be lower as long as it's
        // deterministic). Otherwise, local store can have new value while
        // other stores have old value and they never sync.
        int rc = (*value.value_ref()).compare(*kvStoreIt->second.value_ref());
        if (rc > 0) {
          // versions and orginatorIds are same but value is higher
          VLOG(3) << "Previous incarnation reflected back for key " << key;
          updateAllNeeded = true;
        } else if (rc == 0) {
          // versions, orginatorIds, value are all same
          // retain higher ttlVersion
          if (value.ttlVersion > kvStoreIt->second.ttlVersion) {
            updateTtlNeeded = true;
          }
        }
      }
    }

    //
    // Check updateTtl
    //
    if (not value.value_ref().has_value() and kvStoreIt != kvStore.end() and
        value.version == kvStoreIt->second.version and
        value.originatorId == kvStoreIt->second.originatorId and
        value.ttlVersion > kvStoreIt->second.ttlVersion) {
      updateTtlNeeded = true;
    }

    if (!updateAllNeeded and !updateTtlNeeded) {
      VLOG(3) << "(mergeKeyValues) no need to update anything for key: '" << key
              << "'";
      continue;
    }

    VLOG(3) << "Updating key: " << key << "\n  Version: " << myVersion << " -> "
            << newVersion << "\n  Originator: "
            << (kvStoreIt != kvStore.end() ? kvStoreIt->second.originatorId
                                           : "null")
            << " -> " << value.originatorId << "\n  TtlVersion: "
            << (kvStoreIt != kvStore.end() ? kvStoreIt->second.ttlVersion : 0)
            << " -> " << value.ttlVersion << "\n  Ttl: "
            << (kvStoreIt != kvStore.end() ? kvStoreIt->second.ttl : 0)
            << " -> " << value.ttl;

    // grab the new value (this will copy, intended)
    thrift::Value newValue = value;

    if (updateAllNeeded) {
      ++valUpdateCnt;
      FB_LOG_EVERY_MS(INFO, 500)
          << "Updating key: " << key << ", Originator: " << value.originatorId
          << ", Version: " << newVersion << ", TtlVersion: " << value.ttlVersion
          << ", Ttl: " << value.ttl;
      //
      // update everything for such key
      //
      CHECK(value.value_ref().has_value());
      if (kvStoreIt == kvStore.end()) {
        // create new entry
        std::tie(kvStoreIt, std::ignore) = kvStore.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(key),
            std::forward_as_tuple(std::move(newValue)));
      } else {
        // update the entry in place, the old value will be destructed
        kvStoreIt->second = std::move(newValue);
      }
      // update hash if it's not there
      if (not kvStoreIt->second.hash_ref().has_value()) {
        kvStoreIt->second.hash_ref() =
            generateHash(value.version, value.originatorId, value.value_ref());
      }
    } else if (updateTtlNeeded) {
      ++ttlUpdateCnt;
      //
      // update ttl,ttlVersion only
      //
      CHECK(kvStoreIt != kvStore.end());

      // update TTL only, nothing else
      kvStoreIt->second.ttl = value.ttl;
      kvStoreIt->second.ttlVersion = value.ttlVersion;
    }

    // announce the update
    kvUpdates.emplace(key, value);
  }

  VLOG(4) << "(mergeKeyValues) updating " << kvUpdates.size()
          << " keyvals. ValueUpdates: " << valUpdateCnt
          << ", TtlUpdates: " << ttlUpdateCnt;
  return kvUpdates;
}

/**
 * Compare two values to find out which value is better
 */
int
KvStore::compareValues(const thrift::Value& v1, const thrift::Value& v2) {
  // compare version
  if (v1.version != v2.version) {
    return v1.version > v2.version ? 1 : -1;
  }

  // compare orginatorId
  if (v1.originatorId != v2.originatorId) {
    return v1.originatorId > v2.originatorId ? 1 : -1;
  }

  // compare value
  if (v1.hash_ref().has_value() and v2.hash_ref().has_value() and
      *v1.hash_ref() == *v2.hash_ref()) {
    // TODO: `ttlVersion` and `ttl` value can be different on neighbor nodes.
    // The ttl-update should never be sent over the full-sync
    // hashes are same => (version, orginatorId, value are same)
    // compare ttl-version
    if (v1.ttlVersion != v2.ttlVersion) {
      return v1.ttlVersion > v2.ttlVersion ? 1 : -1;
    } else {
      return 0;
    }
  }

  // can't use hash, either it's missing or they are different
  // compare values
  if (v1.value_ref().has_value() and v2.value_ref().has_value()) {
    return (*v1.value_ref()).compare(*v2.value_ref());
  } else {
    // some value is missing
    return -2; // unknown
  }
}

void
KvStore::prepareSocket(
    fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& socket,
    std::string const& url,
    std::optional<int> maybeIpTos) {
  std::vector<std::pair<int, int>> socketOptions{
      {ZMQ_SNDHWM, Constants::kHighWaterMark},
      {ZMQ_RCVHWM, Constants::kHighWaterMark},
      {ZMQ_SNDTIMEO, Constants::kReadTimeout.count()},
      {ZMQ_ROUTER_HANDOVER, 1},
      {ZMQ_TCP_KEEPALIVE, Constants::kKeepAliveEnable},
      {ZMQ_TCP_KEEPALIVE_IDLE, Constants::kKeepAliveTime.count()},
      {ZMQ_TCP_KEEPALIVE_CNT, Constants::kKeepAliveCnt},
      {ZMQ_TCP_KEEPALIVE_INTVL, Constants::kKeepAliveIntvl.count()}};

  if (maybeIpTos.has_value()) {
    socketOptions.emplace_back(ZMQ_TOS, maybeIpTos.value());
  }

  for (const auto& [opt, val] : socketOptions) {
    auto rc = socket.setSockOpt(opt, &val, sizeof(val));
    if (rc.hasError()) {
      LOG(FATAL) << "Error setting zmq opt: " << opt << "to " << val
                 << ". Error: " << rc.error();
    }
  }

  auto rc = socket.bind(fbzmq::SocketUrl{url});
  if (rc.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << url << "'. Error: " << rc.error();
  }
}

void
KvStore::processCmdSocketRequest(std::vector<fbzmq::Message>&& req) noexcept {
  if (req.empty()) {
    LOG(ERROR) << "Empty request received";
    return;
  }
  auto maybeReply = processRequestMsg(
      req.front().read<std::string>().value(), std::move(req.back()));
  req.pop_back();

  // All messages of the multipart request except the last are sent back as they
  // are ids or empty delims. Add the response at the end of that list.
  if (maybeReply.hasValue()) {
    req.emplace_back(std::move(maybeReply.value()));
  } else {
    req.emplace_back(
        fbzmq::Message::from(Constants::kErrorResponse.toString()).value());
  }

  if (not req.back().empty()) {
    auto sndRet = kvParams_.globalCmdSock.sendMultiple(req);
    if (sndRet.hasError()) {
      LOG(ERROR) << "Error sending response. " << sndRet.error();
    }
  }
  return;
}

folly::Expected<fbzmq::Message, fbzmq::Error>
KvStore::processRequestMsg(
    const std::string& requestId, fbzmq::Message&& request) {
  fb303::fbData->addStatValue(
      "kvstore.peers.bytes_received", request.size(), fb303::SUM);
  auto maybeThriftReq =
      request.readThriftObj<thrift::KvStoreRequest>(serializer_);

  if (maybeThriftReq.hasError()) {
    LOG(ERROR) << "processRequest: failed reading thrift::processRequestMsg"
               << maybeThriftReq.error();
    return {folly::makeUnexpected(fbzmq::Error())};
  }

  auto& thriftRequest = maybeThriftReq.value();
  // XXX HACK: assume kDefaultArea if thriftRequest.area is empty
  std::string area;
  if (!thriftRequest.area.empty()) {
    area = openr::thrift::KvStore_constants::kDefaultArea();
  } else {
    area = thriftRequest.area; // NOTE: Non constness is intended
  }
  // TODO: migration workaround => if me/peer does is using default area,
  // always honor my config, ignore peer's config.
  if (areas_.size() == 1 and
      (areas_.count(openr::thrift::KvStore_constants::kDefaultArea()) or
       area == openr::thrift::KvStore_constants::kDefaultArea())) {
    area = *areas_.begin();
  }

  VLOG(2) << "Request received for area " << area;
  try {
    auto& kvStoreDb = kvStoreDb_.at(area);
    auto response = kvStoreDb.processRequestMsgHelper(requestId, thriftRequest);
    if (response.hasValue()) {
      fb303::fbData->addStatValue(
          "kvstore.peers.bytes_sent", response->size(), fb303::SUM);
    }
    return response;
  } catch (std::out_of_range const& e) {
    LOG(ERROR) << "std::out_of_range for area " << area;
    return folly::makeUnexpected(
        fbzmq::Error(0, folly::sformat("Invalid area {}", area)));
  }
  return {folly::makeUnexpected(fbzmq::Error())};
}

messaging::RQueue<thrift::Publication>
KvStore::getKvStoreUpdatesReader() {
  return kvParams_.kvStoreUpdatesQueue.getReader();
}

void
KvStore::processPeerUpdates(thrift::PeerUpdateRequest&& req) {
  // Req can contain peerAdd/peerDel simultaneously
  if (req.peerAddParams_ref().has_value()) {
    addUpdateKvStorePeers(req.peerAddParams_ref().value(), req.area).get();
  }
  if (req.peerDelParams_ref().has_value()) {
    deleteKvStorePeers(req.peerDelParams_ref().value(), req.area).get();
  }
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
KvStore::getKvStoreKeyVals(
    thrift::KeyGetParams keyGetParams, std::string area) {
  folly::Promise<std::unique_ptr<thrift::Publication>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        keyGetParams = std::move(keyGetParams),
                        area]() mutable {
    VLOG(3) << "Get key requested for AREA: " << area;

    if (!kvStoreDb_.count(area)) {
      p.setException(
          thrift::OpenrError(folly::sformat("Invalid area: {}", area)));
    } else {
      fb303::fbData->addStatValue("kvstore.cmd_key_get", 1, fb303::COUNT);

      auto& kvStoreDb = kvStoreDb_.at(area);
      auto thriftPub = kvStoreDb.getKeyVals(keyGetParams.keys);
      kvStoreDb.updatePublicationTtl(thriftPub);

      p.setValue(std::make_unique<thrift::Publication>(std::move(thriftPub)));
    }
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
KvStore::dumpKvStoreKeys(
    thrift::KeyDumpParams keyDumpParams, std::string area) {
  folly::Promise<std::unique_ptr<thrift::Publication>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        keyDumpParams = std::move(keyDumpParams),
                        area]() mutable {
    VLOG(3) << "Dump all keys requested for AREA: " << area;

    if (!kvStoreDb_.count(area)) {
      p.setException(
          thrift::OpenrError(folly::sformat("Invalid area: {}", area)));
    } else {
      fb303::fbData->addStatValue("kvstore.cmd_key_dump", 1, fb303::COUNT);

      auto& kvStoreDb = kvStoreDb_.at(area);
      std::vector<std::string> keyPrefixList;
      if (keyDumpParams.keys_ref().has_value()) {
        keyPrefixList = *keyDumpParams.keys_ref();
      } else {
        folly::split(",", *keyDumpParams.prefix_ref(), keyPrefixList, true);
      }
      const auto keyPrefixMatch =
          KvStoreFilters(keyPrefixList, keyDumpParams.originatorIds);

      thrift::FilterOperator oper = thrift::FilterOperator::OR;
      if (keyDumpParams.oper_ref().has_value()) {
        oper = *keyDumpParams.oper_ref();
      }

      auto thriftPub = kvStoreDb.dumpAllWithFilters(keyPrefixMatch, oper);
      if (keyDumpParams.keyValHashes_ref().has_value()) {
        thriftPub = kvStoreDb.dumpDifference(
            thriftPub.keyVals, keyDumpParams.keyValHashes_ref().value());
      }
      kvStoreDb.updatePublicationTtl(thriftPub);
      // I'm the initiator, set flood-root-id
      fromStdOptional(thriftPub.floodRootId_ref(), kvStoreDb.getSptRootId());

      if (keyDumpParams.keyValHashes_ref().has_value() and
          (*keyDumpParams.prefix_ref()).empty() and
          (not keyDumpParams.keys_ref().has_value() or
           (*keyDumpParams.keys_ref()).empty())) {
        // This usually comes from neighbor nodes
        size_t numMissingKeys = 0;
        if (thriftPub.tobeUpdatedKeys_ref().has_value()) {
          numMissingKeys = thriftPub.tobeUpdatedKeys_ref()->size();
        }
        LOG(INFO) << "[Thrift Sync] Processed full-sync request with "
                  << keyDumpParams.keyValHashes_ref().value().size()
                  << " keyValHashes item(s). Sending "
                  << thriftPub.keyVals.size() << " key-vals and "
                  << numMissingKeys << " missing keys";
      }
      p.setValue(std::make_unique<thrift::Publication>(std::move(thriftPub)));
    }
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
KvStore::dumpKvStoreHashes(
    thrift::KeyDumpParams keyDumpParams, std::string area) {
  folly::Promise<std::unique_ptr<thrift::Publication>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        keyDumpParams = std::move(keyDumpParams),
                        area]() mutable {
    VLOG(3) << "Dump all hashes requested for AREA: " << area;

    if (!kvStoreDb_.count(area)) {
      p.setException(
          thrift::OpenrError(folly::sformat("Invalid area: {}", area)));
    } else {
      fb303::fbData->addStatValue("kvstore.cmd_hash_dump", 1, fb303::COUNT);

      auto& kvStoreDb = kvStoreDb_.at(area);
      std::set<std::string> originator{};
      std::vector<std::string> keyPrefixList{};
      if (keyDumpParams.keys_ref().has_value()) {
        keyPrefixList = *keyDumpParams.keys_ref();
      } else {
        folly::split(",", *keyDumpParams.prefix_ref(), keyPrefixList, true);
      }
      KvStoreFilters kvFilters{keyPrefixList, originator};
      auto thriftPub = kvStoreDb.dumpHashWithFilters(kvFilters);
      kvStoreDb.updatePublicationTtl(thriftPub);
      p.setValue(std::make_unique<thrift::Publication>(std::move(thriftPub)));
    }
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
KvStore::setKvStoreKeyVals(
    thrift::KeySetParams keySetParams, std::string area) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        keySetParams = std::move(keySetParams),
                        area]() mutable {
    VLOG(3) << "Set key requested for AREA: " << area;

    if (!kvStoreDb_.count(area)) {
      p.setException(
          thrift::OpenrError(folly::sformat("Invalid area: {}", area)));
    } else {
      // Update statistics
      fb303::fbData->addStatValue("kvstore.cmd_key_set", 1, fb303::COUNT);
      if (keySetParams.timestamp_ms_ref().has_value()) {
        auto floodMs =
            getUnixTimeStampMs() - keySetParams.timestamp_ms_ref().value();
        if (floodMs > 0) {
          fb303::fbData->addStatValue(
              "kvstore.flood_duration_ms", floodMs, fb303::AVG);
        }
      }

      // Update hash for key-values
      auto& kvStoreDb = kvStoreDb_.at(area);
      for (auto& kv : keySetParams.keyVals) {
        auto& value = kv.second;
        if (value.value_ref().has_value()) {
          value.hash_ref() = generateHash(
              value.version, value.originatorId, value.value_ref());
        }
      }

      // Create publication and merge it with local KvStore
      thrift::Publication rcvdPublication;
      rcvdPublication.keyVals = std::move(keySetParams.keyVals);
      rcvdPublication.nodeIds_ref().move_from(keySetParams.nodeIds_ref());
      rcvdPublication.floodRootId_ref().move_from(
          keySetParams.floodRootId_ref());
      kvStoreDb.mergePublication(rcvdPublication);

      // ready to return
      p.setValue();
    }
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::AreasConfig>>
KvStore::getAreasConfig() {
  folly::Promise<std::unique_ptr<thrift::AreasConfig>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable {
    auto areasConfig = thrift::AreasConfig{};
    areasConfig.areas = areas_;
    p.setValue(std::make_unique<thrift::AreasConfig>(std::move(areasConfig)));
  });
  return sf;
}

folly::SemiFuture<std::optional<KvStorePeerState>>
KvStore::getKvStorePeerState(
    std::string const& peerName, std::string const& area) {
  folly::Promise<std::optional<KvStorePeerState>> promise;
  auto sf = promise.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(promise), peerName, area]() mutable {
        if (!kvStoreDb_.count(area)) {
          p.setValue(std::nullopt);
        } else {
          auto& kvStoreDb = kvStoreDb_.at(area);
          auto state = kvStoreDb.getCurrentState(peerName);
          p.setValue(state);
        }
      });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
KvStore::getKvStorePeers(std::string area) {
  folly::Promise<std::unique_ptr<thrift::PeersMap>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), area]() mutable {
    VLOG(2) << "Peer dump requested for AREA: " << area;

    if (!kvStoreDb_.count(area)) {
      p.setException(
          thrift::OpenrError(folly::sformat("Invalid area: {}", area)));
    } else {
      fb303::fbData->addStatValue("kvstore.cmd_peer_dump", 1, fb303::COUNT);
      auto& kvStoreDb = kvStoreDb_.at(area);
      auto peers = kvStoreDb.dumpPeers();
      p.setValue(std::make_unique<thrift::PeersMap>(std::move(peers)));
    }
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
KvStore::addUpdateKvStorePeers(
    thrift::PeerAddParams peerAddParams, std::string area) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        peerAddParams = std::move(peerAddParams),
                        area]() mutable {
    auto peersToAdd = folly::gen::from(*peerAddParams.peers_ref()) |
        folly::gen::get<0>() | folly::gen::as<std::vector<std::string>>();

    LOG(INFO) << "Peer addition for: [" << folly::join(",", peersToAdd)
              << "] in area: " << area;

    if (!kvStoreDb_.count(area)) {
      p.setException(
          thrift::OpenrError(folly::sformat("Invalid area: {}", area)));
    } else if (peerAddParams.peers.empty()) {
      p.setException(thrift::OpenrError(
          "Empty peerNames from peer-add request, ignoring"));
    } else {
      fb303::fbData->addStatValue("kvstore.cmd_peer_add", 1, fb303::COUNT);
      auto& kvStoreDb = kvStoreDb_.at(area);
      kvStoreDb.addPeers(peerAddParams.peers);
      p.setValue();
    }
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
KvStore::deleteKvStorePeers(
    thrift::PeerDelParams peerDelParams, std::string area) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        peerDelParams = std::move(peerDelParams),
                        area]() mutable {
    LOG(INFO) << "Peer deletion for: ["
              << folly::join(",", *peerDelParams.peerNames_ref())
              << "] in area: " << area;

    if (!kvStoreDb_.count(area)) {
      p.setException(
          thrift::OpenrError(folly::sformat("Invalid area: {}", area)));
    } else if (peerDelParams.peerNames.empty()) {
      p.setException(thrift::OpenrError(
          "Empty peerNames from peer-del request, ignoring"));
    } else {
      fb303::fbData->addStatValue("kvstore.cmd_per_del", 1, fb303::COUNT);
      auto& kvStoreDb = kvStoreDb_.at(area);
      kvStoreDb.delPeers(peerDelParams.peerNames);
      p.setValue();
    }
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::SptInfos>>
KvStore::getSpanningTreeInfos(std::string area) {
  folly::Promise<std::unique_ptr<thrift::SptInfos>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), area]() mutable {
    VLOG(3) << "FLOOD_TOPO_GET command requested for AREA: " << area;

    if (!kvStoreDb_.count(area)) {
      p.setException(
          thrift::OpenrError(folly::sformat("Invalid area: {}", area)));
    } else {
      auto& kvStoreDb = kvStoreDb_.at(area);
      auto sptInfos = kvStoreDb.processFloodTopoGet();
      p.setValue(std::make_unique<thrift::SptInfos>(std::move(sptInfos)));
    }
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
KvStore::updateFloodTopologyChild(
    thrift::FloodTopoSetParams floodTopoSetParams, std::string area) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        floodTopoSetParams = std::move(floodTopoSetParams),
                        area]() mutable {
    VLOG(2) << "FLOOD_TOPO_SET command requested for AREA: " << area;

    if (!kvStoreDb_.count(area)) {
      p.setException(
          thrift::OpenrError(folly::sformat("Invalid area: {}", area)));
    } else {
      auto& kvStoreDb = kvStoreDb_.at(area);
      kvStoreDb.processFloodTopoSet(std::move(floodTopoSetParams));
      p.setValue();
    }
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
KvStore::processKvStoreDualMessage(
    thrift::DualMessages dualMessages, std::string area) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        dualMessages = std::move(dualMessages),
                        area]() mutable {
    VLOG(2) << "DUAL messages received for AREA: " << area;

    if (!kvStoreDb_.count(area)) {
      p.setException(
          thrift::OpenrError(folly::sformat("Invalid area: {}", area)));
    } else if (dualMessages.messages.empty()) {
      LOG(ERROR) << "Empty DUAL msg receved";
      p.setValue();
    } else {
      fb303::fbData->addStatValue(
          "kvstore.received_dual_messages", 1, fb303::COUNT);

      auto& kvStoreDb = kvStoreDb_.at(area);
      kvStoreDb.processDualMessages(std::move(dualMessages));
      p.setValue();
    }
  });
  return sf;
}

folly::SemiFuture<std::map<std::string, int64_t>>
KvStore::getCounters() {
  auto pf = folly::makePromiseContract<std::map<std::string, int64_t>>();
  runInEventBaseThread([this, p = std::move(pf.first)]() mutable {
    p.setValue(getGlobalCounters());
  });
  return std::move(pf.second);
}

std::map<std::string, int64_t>
KvStore::getGlobalCounters() const {
  std::map<std::string, int64_t> flatCounters;
  for (auto& kvDb : kvStoreDb_) {
    auto kvDbCounters = kvDb.second.getCounters();
    // add up counters for same key from all kvStoreDb instances
    flatCounters = std::accumulate(
        kvDbCounters.begin(),
        kvDbCounters.end(),
        flatCounters,
        [](std::map<std::string, int64_t>& flatCounters,
           const std::pair<const std::string, int64_t>& kvDbcounter) {
          flatCounters[kvDbcounter.first] += kvDbcounter.second;
          return flatCounters;
        });
  }
  return flatCounters;
}

//
// This is the state transition matrix for KvStorePeerState. It is a
// sparse-matrix with row representing `KvStorePeerState` and column
// representing `KvStorePeerEvent`. State transition is driven by
// certain event. Invalid state jump will cause fatal error.
//
const std::vector<std::vector<std::optional<KvStorePeerState>>>
    KvStoreDb::peerStateMap_ = {
        /*
         * index 0 - IDLE
         * PEER_ADD => SYNCING
         * SYNC_TIMEOUT => IDLE
         * THRIFT_API_ERROR => IDLE
         */
        {KvStorePeerState::SYNCING,
         std::nullopt,
         std::nullopt,
         KvStorePeerState::IDLE,
         KvStorePeerState::IDLE},
        /*
         * index 1 - SYNCING
         * SYNC_RESP_RCVD => INITIALIZED
         * SYNC_TIMEOUT => IDLE
         * THRIFT_API_ERROR => IDLE
         */
        {std::nullopt,
         std::nullopt,
         KvStorePeerState::INITIALIZED,
         KvStorePeerState::IDLE,
         KvStorePeerState::IDLE},
        /*
         * index 2 - INITIALIZED
         * SYNC_TIMEOUT => IDLE
         * THRIFT_API_ERROR => IDLE
         */
        {std::nullopt,
         std::nullopt,
         KvStorePeerState::INITIALIZED,
         KvStorePeerState::IDLE,
         KvStorePeerState::IDLE}};

// static util function for logging
std::string
KvStoreDb::toStr(KvStorePeerState state) {
  std::string res = "UNDEFINED";
  switch (state) {
  case KvStorePeerState::IDLE:
    return "IDLE";
  case KvStorePeerState::SYNCING:
    return "SYNCING";
  case KvStorePeerState::INITIALIZED:
    return "INITIALIZED";
  default:
    LOG(ERROR) << "Undefined peer state";
  }
  return res;
}

// static util function to fetch peers by state
std::vector<std::string>
KvStoreDb::getPeersByState(KvStorePeerState state) {
  std::vector<std::string> res;
  for (auto const& kv : thriftPeers_) {
    auto const& peer = kv.second;
    if (peer.state == state) {
      res.emplace_back(peer.nodeName);
    }
  }
  return res;
}

// static util function to log state transition
void
KvStoreDb::logStateTransition(
    std::string const& peerName,
    KvStorePeerState oldState,
    KvStorePeerState newState) {
  SYSLOG(INFO) << "State change: [" << toStr(oldState) << "] -> ["
               << toStr(newState) << "] "
               << "for peer: " << peerName;
}

// static util function to fetch current peer state
std::optional<KvStorePeerState>
KvStoreDb::getCurrentState(std::string const& peerName) {
  if (thriftPeers_.count(peerName)) {
    return thriftPeers_.at(peerName).state;
  }
  return std::nullopt;
}

// static util function for state transition
KvStorePeerState
KvStoreDb::getNextState(
    std::optional<KvStorePeerState> const& currState,
    KvStorePeerEvent const& event) {
  CHECK(currState.has_value()) << "Current state is 'UNDEFINED'";

  std::optional<KvStorePeerState> nextState =
      peerStateMap_[static_cast<uint32_t>(currState.value())]
                   [static_cast<uint32_t>(event)];

  CHECK(nextState.has_value()) << "Next state is 'UNDEFINED'";
  return nextState.value();
}

KvStoreDb::KvStorePeer::KvStorePeer(
    const std::string& nodeName,
    const thrift::PeerSpec& peerSpec,
    const ExponentialBackoff<std::chrono::milliseconds>& expBackoff)
    : nodeName(nodeName), peerSpec(peerSpec), expBackoff(expBackoff) {
  CHECK(not this->nodeName.empty());
  CHECK(not this->peerSpec.peerAddr.empty());
  CHECK(
      this->expBackoff.getInitialBackoff() <= this->expBackoff.getMaxBackoff());
}

//
// KvStoreDb is the class instance that maintains the KV pairs with internal map
// per AREA. KvStoreDb will sync with peers to maintain eventual consistency.
// It supports external message exchanging through:
//
//  1) ZMQ socket(TO BE DEPRECATED);
//  2) Thrift channel interface;
//
// NOTE Monitoring:
// This module exposes fb303 counters that can be leveraged for monitoring
// KvStoreDb's correctness and performance behevior in production
//
//  kvstore.thrift.num_client_connection_failure: # of client creation failures
//  kvstore.thrift.num_full_sync: # of full-sync performed;
//  kvstore.thrift.num_missing_keys: # of missing keys from syncing with peer;
//  kvstore.thrift.num_full_sync_success: # of successful full-sync performed;
//  kvstore.thrift.num_full_sync_failure: # of failed full-sync performed;
//  kvstore.thrift.full_sync_duration_ms: avg time elapsed for a full-sync req;
//
//  kvstore.thrift.num_flood_pub: # of flooding req issued;
//  kvstore.thrift.num_flood_key_vals: # of keyVals one flooding req contains;
//  kvstore.thrift.num_flood_pub_success: # of successful flooding req
//  performed;
//  kvstore.thrift.num_flood_pub_failure: # of failed flooding req
//  performed;
//  kvstore.thrift.flood_pub_duration_ms: avg time elapsed for a
//  flooding req;
//
//  kvstore.thrift.num_finalized_sync: # of finalized full-sync performed;
//  kvstore.thrift.num_finalized_sync_success: # of successful finalized sync
//  performed;
//  kvstore.thrift.num_finalized_sync_failure: # of failed finalized
//  sync performed;
//  kvstore.thrift.finalized_sync_duration_ms: avg time elapsed
//  for a finalized sync req;
//
KvStoreDb::KvStoreDb(
    OpenrEventBase* evb,
    KvStoreParams& kvParams,
    const std::string& area,
    fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_CLIENT> peersyncSock,
    bool isFloodRoot,
    const std::string& nodeId)
    : DualNode(nodeId, isFloodRoot),
      kvParams_(kvParams),
      area_(area),
      peerSyncSock_(std::move(peersyncSock)),
      evb_(evb) {
  if (kvParams_.floodRate) {
    floodLimiter_ = std::make_unique<folly::BasicTokenBucket<>>(
        kvParams_.floodRate->flood_msg_per_sec,
        kvParams_.floodRate->flood_msg_burst_size);
    pendingPublicationTimer_ =
        folly::AsyncTimeout::make(*evb_->getEvb(), [this]() noexcept {
          if (!floodLimiter_->consume(1)) {
            pendingPublicationTimer_->scheduleTimeout(
                Constants::kFloodPendingPublication);
            return;
          }
          floodBufferedUpdates();
        });
  }

  LOG(INFO) << "Starting kvstore DB instance for node " << nodeId << " area "
            << area;

  // Attach socket callbacks/schedule events
  attachCallbacks();

  // Hook up timer with cleanupTtlCountdownQueue(). The actual scheduling
  // happens within updateTtlCountdownQueue()
  ttlCountdownTimer_ = folly::AsyncTimeout::make(
      *evb_->getEvb(), [this]() noexcept { cleanupTtlCountdownQueue(); });

  // Initialize fb303 counter keys for thrift
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_client_connection_failure", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_full_sync", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_full_sync_success", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_full_sync_failure", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_flood_pub", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_flood_pub_success", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_flood_pub_failure", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_finalized_sync", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_finalized_sync_success", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_finalized_sync_failure", fb303::COUNT);

  fb303::fbData->addStatExportType(
      "kvstore.thrift.full_sync_duration_ms", fb303::AVG);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.flood_pub_duration_ms", fb303::AVG);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.finalized_sync_duration_ms", fb303::AVG);

  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_missing_keys", fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_flood_key_vals", fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_keyvals_update", fb303::SUM);

  // TODO: remove `kvstore.zmq.*` counters once ZMQ socket is deprecated
  fb303::fbData->addStatExportType("kvstore.zmq.num_missing_keys", fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.zmq.num_keyvals_update", fb303::SUM);

  // Initialize stats keys
  fb303::fbData->addStatExportType("kvstore.cmd_hash_dump", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_key_dump", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_key_get", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_key_set", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_peer_add", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_peer_dump", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_per_del", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.expired_key_vals", fb303::SUM);
  fb303::fbData->addStatExportType("kvstore.flood_duration_ms", fb303::AVG);
  fb303::fbData->addStatExportType("kvstore.full_sync_duration_ms", fb303::AVG);
  fb303::fbData->addStatExportType("kvstore.looped_publications", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.peers.bytes_received", fb303::SUM);
  fb303::fbData->addStatExportType("kvstore.peers.bytes_sent", fb303::SUM);
  fb303::fbData->addStatExportType("kvstore.rate_limit_keys", fb303::AVG);
  fb303::fbData->addStatExportType("kvstore.rate_limit_suppress", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.received_dual_messages", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.received_key_vals", fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.received_publications", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.received_redundant_publications", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.sent_key_vals", fb303::SUM);
  fb303::fbData->addStatExportType("kvstore.sent_publications", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.updated_key_vals", fb303::SUM);
}

KvStoreDb::~KvStoreDb() {
  LOG(INFO) << "Terminating KvStoreDb with areaId: " << area_;

  evb_->getEvb()->runImmediatelyOrRunInEventBaseThreadAndWait([this]() {
    // Destroy thrift clients associated with peers, which will
    // fulfill promises with exceptions if any.
    thriftPeers_.clear();

    // Waiting for all pending thrift call to finish
    std::vector<folly::SemiFuture<folly::Unit>> fs;
    for (auto& kv : thriftFs_) {
      fs.emplace_back(std::move(kv.second));
    }
    thriftFs_.clear();
    folly::collectAll(std::move(fs)).get();

    LOG(INFO) << "Done processing all pending thrift reqs in area: " << area_;
  });

  // remove ZMQ socket
  evb_->removeSocket(fbzmq::RawZmqSocketPtr{*peerSyncSock_});
}

void
KvStoreDb::updateTtlCountdownQueue(const thrift::Publication& publication) {
  for (const auto& kv : publication.keyVals) {
    const auto& key = kv.first;
    const auto& value = kv.second;

    if (value.ttl != Constants::kTtlInfinity) {
      TtlCountdownQueueEntry queueEntry;
      queueEntry.expiryTime = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(value.ttl);
      queueEntry.key = key;
      queueEntry.version = value.version;
      queueEntry.ttlVersion = value.ttlVersion;
      queueEntry.originatorId = value.originatorId;

      if ((ttlCountdownQueue_.empty() or
           (queueEntry.expiryTime <= ttlCountdownQueue_.top().expiryTime)) and
          ttlCountdownTimer_) {
        // Reschedule the shorter timeout
        ttlCountdownTimer_->scheduleTimeout(
            std::chrono::milliseconds(value.ttl));
      }

      ttlCountdownQueue_.push(std::move(queueEntry));
    }
  }
}

// build publication out of the requested keys (per request)
// if not keys provided, will return publication with empty keyVals
thrift::Publication
KvStoreDb::getKeyVals(std::vector<std::string> const& keys) {
  thrift::Publication thriftPub;
  thriftPub.area = area_;

  for (auto const& key : keys) {
    // if requested key if found, respond with version and value
    auto it = kvStore_.find(key);
    if (it != kvStore_.end()) {
      // copy here
      thriftPub.keyVals[key] = it->second;
    }
  }
  return thriftPub;
}

// dump the entries of my KV store whose keys match the given prefix
// if prefix is the empty string, the full KV store is dumped
thrift::Publication
KvStoreDb::dumpAllWithFilters(
    KvStoreFilters const& kvFilters, thrift::FilterOperator oper) const {
  thrift::Publication thriftPub;
  thriftPub.area = area_;

  switch (oper) {
  case thrift::FilterOperator::AND:
    for (auto const& kv : kvStore_) {
      if (not kvFilters.keyMatchAll(kv.first, kv.second)) {
        continue;
      }
      thriftPub.keyVals[kv.first] = kv.second;
    }
    break;
  default:
    for (auto const& kv : kvStore_) {
      if (not kvFilters.keyMatch(kv.first, kv.second)) {
        continue;
      }
      thriftPub.keyVals[kv.first] = kv.second;
    }
  }
  return thriftPub;
}

// dump the hashes of my KV store whose keys match the given prefix
// if prefix is the empty string, the full hash store is dumped
thrift::Publication
KvStoreDb::dumpHashWithFilters(KvStoreFilters const& kvFilters) const {
  thrift::Publication thriftPub;
  thriftPub.area = area_;
  for (auto const& kv : kvStore_) {
    if (not kvFilters.keyMatch(kv.first, kv.second)) {
      continue;
    }
    DCHECK(kv.second.hash_ref().has_value());
    auto& value = thriftPub.keyVals[kv.first];
    value.version = kv.second.version;
    value.originatorId = kv.second.originatorId;
    value.hash_ref().copy_from(kv.second.hash_ref());
    value.ttl = kv.second.ttl;
    value.ttlVersion = kv.second.ttlVersion;
  }
  return thriftPub;
}

// dump the keys on which hashes differ from given keyVals
// thriftPub.keyVals: better keys or keys exist only in MY-KEY-VAL
// thriftPub.tobeUpdatedKeys: better keys or keys exist only in REQ-KEY-VAL
// this way, full-sync initiator knows what keys need to send back to finish
// 3-way full-sync
thrift::Publication
KvStoreDb::dumpDifference(
    std::unordered_map<std::string, thrift::Value> const& myKeyVal,
    std::unordered_map<std::string, thrift::Value> const& reqKeyVal) const {
  thrift::Publication thriftPub;
  thriftPub.area = area_;

  thriftPub.tobeUpdatedKeys_ref() = std::vector<std::string>{};
  std::unordered_set<std::string> allKeys;
  for (const auto& kv : myKeyVal) {
    allKeys.insert(kv.first);
  }
  for (const auto& kv : reqKeyVal) {
    allKeys.insert(kv.first);
  }

  for (const auto& key : allKeys) {
    const auto& myKv = myKeyVal.find(key);
    const auto& reqKv = reqKeyVal.find(key);
    if (myKv == myKeyVal.end()) {
      // not exist in myKeyVal
      thriftPub.tobeUpdatedKeys_ref()->emplace_back(key);
      continue;
    }
    if (reqKv == reqKeyVal.end()) {
      // not exist in reqKeyVal
      thriftPub.keyVals.emplace(key, myKv->second);
      continue;
    }
    // common key
    const auto& myVal = myKv->second;
    const auto& reqVal = reqKv->second;
    int rc = KvStore::compareValues(myVal, reqVal);
    if (rc == 1 or rc == -2) {
      // myVal is better or unknown
      thriftPub.keyVals.emplace(key, myVal);
    }
    if (rc == -1 or rc == -2) {
      // reqVal is better or unknown
      thriftPub.tobeUpdatedKeys_ref()->emplace_back(key);
    }
  }

  return thriftPub;
}

// This function serves the purpose of periodically scanning peers in
// IDLE state and promote them to SYNCING state. The initial dump will
// happen in async nature to unblock KvStore to process other requests.
void
KvStoreDb::requestThriftPeerSync() {
  // minimal timeout for next run
  auto timeout = std::chrono::milliseconds(Constants::kMaxBackoff);

  // pre-fetch of peers in "SYNCING" state for later calculation
  uint32_t numThriftPeersInSync =
      getPeersByState(KvStorePeerState::SYNCING).size();

  // Scan over thriftPeers to promote IDLE peers to SYNCING
  for (auto& kv : thriftPeers_) {
    auto& peerName = kv.first; // std::string
    auto& thriftPeer = kv.second; // KvStoreThriftPeer
    auto& peerSpec = thriftPeer.peerSpec; // thrift::PeerSpec

    // ignore peers in state other than IDLE
    if (thriftPeer.state != KvStorePeerState::IDLE) {
      continue;
    }

    // update the global minimum timeout value for next try
    if (not thriftPeer.expBackoff.canTryNow()) {
      timeout =
          std::min(timeout, thriftPeer.expBackoff.getTimeRemainingUntilRetry());
      continue;
    }

    // create thrift client and do backoff if can't go through
    try {
      LOG(INFO) << "[Thrift Sync] Creating kvstore thrift client with addr: "
                << peerSpec.peerAddr << ", port: " << peerSpec.ctrlPort
                << ", peerName: " << peerName;

      // TODO: migrate to secure thrift connection
      auto client = getOpenrCtrlPlainTextClient(
          *(evb_->getEvb()),
          folly::IPAddress(*peerSpec.peerAddr_ref()), /* v6LinkLocal%iface */
          *peerSpec.ctrlPort_ref(), /* port to establish TCP connection */
          Constants::kServiceConnTimeout, /* client connection timeout */
          Constants::kServiceProcTimeout, /* request processing timeout */
          folly::AsyncSocket::anyAddress(), /* bindAddress */
          kvParams_.maybeIpTos /* IP_TOS value for control plane */);
      thriftPeer.client = std::move(client);

      // schedule periodic keepAlive time with 20% jitter variance
      auto period = addJitter<std::chrono::seconds>(
          Constants::kThriftClientKeepAliveInterval, 20.0);
      thriftPeer.keepAliveTimer->scheduleTimeout(period);
    } catch (std::exception const& e) {
      LOG(ERROR) << "[Thrift Sync] Failed to connect to node: " << peerName
                 << "  with addr: " << peerSpec.peerAddr
                 << ". Exception: " << folly::exceptionStr(e);

      // record telemetry for thrift calls
      fb303::fbData->addStatValue(
          "kvstore.thrift.num_client_connection_failure", 1, fb303::COUNT);

      // clean up state for next round of scanning
      thriftPeer.keepAliveTimer->cancelTimeout();
      thriftPeer.client.reset();
      thriftPeer.expBackoff.reportError(); // apply exponential backoff
      timeout =
          std::min(timeout, thriftPeer.expBackoff.getTimeRemainingUntilRetry());
      continue;
    }

    // state transition
    KvStorePeerState oldState = thriftPeer.state;
    thriftPeer.state = getNextState(oldState, KvStorePeerEvent::PEER_ADD);
    logStateTransition(peerName, oldState, thriftPeer.state);

    // mark peer from IDLE -> SYNCING
    numThriftPeersInSync += 1;

    // build KeyDumpParam
    thrift::KeyDumpParams params;
    if (kvParams_.filters.has_value()) {
      std::string keyPrefix =
          folly::join(",", kvParams_.filters.value().getKeyPrefixes());
      /* prefix is for backward compatibility */
      params.prefix = keyPrefix;
      if (not keyPrefix.empty()) {
        params.keys_ref() = kvParams_.filters.value().getKeyPrefixes();
      }
      params.originatorIds_ref() =
          kvParams_.filters.value().getOriginatorIdList();
    }
    KvStoreFilters kvFilters(
        std::vector<std::string>{}, /* keyPrefixList */
        std::set<std::string>{} /* originator */);
    params.keyValHashes_ref() =
        std::move(dumpHashWithFilters(kvFilters).keyVals);

    // record telemetry for initial full-sync
    fb303::fbData->addStatValue(
        "kvstore.thrift.num_full_sync", 1, fb303::COUNT);

    // send request over thrift client and attach callback
    auto startTime = std::chrono::steady_clock::now();
    auto sf = thriftPeer.client->semifuture_getKvStoreKeyValsFilteredArea(
        params, area_);
    auto pendingReqId = ++pendingThriftId_;
    thriftFs_.emplace(
        pendingReqId,
        std::move(sf)
            .via(evb_->getEvb())
            .thenValue([this, peerName, startTime, pendingReqId](
                           thrift::Publication&& pub) {
              // state transition to INITIALIZED
              auto endTime = std::chrono::steady_clock::now();
              auto timeDelta =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      endTime - startTime);
              processThriftSuccess(peerName, std::move(pub), timeDelta);

              // cleanup pendingReqId
              thriftFs_.erase(pendingReqId);
            })
            .thenError([this, peerName, startTime, pendingReqId](
                           const folly::exception_wrapper& ew) {
              // state transition to IDLE
              auto endTime = std::chrono::steady_clock::now();
              auto timeDelta =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      endTime - startTime);
              processThriftFailure(peerName, ew.what(), timeDelta);

              // cleanup pendingReqId
              thriftFs_.erase(pendingReqId);

              // record telemetry for thrift calls
              fb303::fbData->addStatValue(
                  "kvstore.thrift.num_full_sync_failure", 1, fb303::COUNT);
            }));

    // in case pending peer size is over parallelSyncLimit,
    // wait until kMaxBackoff before sending next round of sync
    if (numThriftPeersInSync > parallelSyncLimitOverThrift_) {
      timeout = Constants::kMaxBackoff;
      LOG(INFO) << "[Thrift Sync] " << numThriftPeersInSync
                << " peers are syncing in progress. Over parallel sync limit: "
                << parallelSyncLimitOverThrift_;
      break;
    }
  } // for loop

  // process the rest after min timeout if NOT scheduled
  uint32_t numThriftPeersInIdle =
      getPeersByState(KvStorePeerState::IDLE).size();
  if (numThriftPeersInIdle > 0 or
      numThriftPeersInSync > parallelSyncLimitOverThrift_) {
    LOG_IF(INFO, numThriftPeersInIdle)
        << "[Thrift Sync] " << numThriftPeersInIdle
        << " idle peers require full-sync. Schedule full-sync after: "
        << timeout.count() << "ms.";
    thriftSyncTimer_->scheduleTimeout(std::chrono::milliseconds(timeout));
  }
}

// This function will process the full-dump response from peers:
//  1) Merge peer's publication with local KvStoreDb;
//  2) Send a finalized full-sync to peer for missing keys;
//  3) Exponetially update number of peers to SYNC in parallel;
//  4) Promote KvStorePeerState from SYNCING -> INITIALIZED;
void
KvStoreDb::processThriftSuccess(
    std::string const& peerName,
    thrift::Publication&& pub,
    std::chrono::milliseconds timeDelta) {
  // ATTN: `peerName` is MANDATORY to fulfill the finialized
  //       full-sync with peers.
  const auto kvUpdateCnt = mergePublication(pub, peerName);
  auto numMissingKeys = 0;
  if (pub.tobeUpdatedKeys_ref().has_value()) {
    numMissingKeys = pub.tobeUpdatedKeys_ref()->size();
  }

  // record telemetry for thrift calls
  fb303::fbData->addStatValue(
      "kvstore.thrift.num_full_sync_success", 1, fb303::COUNT);
  fb303::fbData->addStatValue(
      "kvstore.thrift.full_sync_duration_ms", timeDelta.count(), fb303::AVG);
  fb303::fbData->addStatValue(
      "kvstore.thrift.num_missing_keys", numMissingKeys, fb303::SUM);
  fb303::fbData->addStatValue(
      "kvstore.thrift.num_keyvals_update", kvUpdateCnt, fb303::SUM);

  LOG(INFO) << "[Thrift Sync] Full-sync response received from: " << peerName
            << " with " << pub.keyVals.size() << " key-vals and "
            << numMissingKeys << " missing keys. Incured " << kvUpdateCnt
            << " key-value updates."
            << " Processing time: " << timeDelta.count() << "ms.";

  // check if it is valid peer(i.e. peer removed in process of syncing)
  auto peerIt = thriftPeers_.find(peerName);
  if (peerIt == thriftPeers_.end()) {
    LOG(WARNING) << "Received async full-sync response from invalid peer: "
                 << peerName << ". Ignore state transition.";
    return;
  }

  // In case there are duplicate msg for full-sync, state transition
  // will handle it gracefully.
  auto& peer = thriftPeers_.at(peerName);
  CHECK(
      peer.state == KvStorePeerState::SYNCING or
      peer.state == KvStorePeerState::INITIALIZED);
  KvStorePeerState oldState = peer.state;
  peer.state = getNextState(oldState, KvStorePeerEvent::SYNC_RESP_RCVD);
  logStateTransition(peerName, oldState, peer.state);

  // Successfully received full-sync response. Double the parallel
  // sync limit. This is to:
  //  1) accelerate the rest of pending full-syncs if any;
  //  2) assume subsequeny sync diff will be small in traffic amount;
  parallelSyncLimitOverThrift_ = std::min(
      2 * parallelSyncLimitOverThrift_,
      Constants::kMaxFullSyncPendingCountThreshold);

  // Schedule another round of `thriftSyncTimer_` full-sync request if
  // there is still peer in IDLE state. If no IDLE peer, cancel timeout.
  uint32_t numThriftPeersInIdle =
      getPeersByState(KvStorePeerState::IDLE).size();
  if (numThriftPeersInIdle > 0) {
    thriftSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  } else {
    thriftSyncTimer_->cancelTimeout();
  }
}

// This function will process the exception hit during full-dump:
//  1) Change peer state from current state to IDLE due to exception;
//  2) Schedule syncTimer to pick IDLE peer up if NOT scheduled;
void
KvStoreDb::processThriftFailure(
    std::string const& peerName,
    folly::fbstring const& exceptionStr,
    std::chrono::milliseconds timeDelta) {
  LOG(INFO) << "[Thrift Sync] Exception: " << exceptionStr
            << ". Peer name: " << peerName
            << ". Processing time: " << timeDelta.count() << "ms.";

  auto peerIt = thriftPeers_.find(peerName);
  if (peerIt == thriftPeers_.end()) {
    LOG(ERROR) << "Exception happened against invalid peer: " << peerName
               << ". Ignore state transition.";
    return;
  }

  // reset client to reconnect later in next batch of thriftSyncTimer_ scanning
  auto& peer = thriftPeers_.at(peerName);
  peer.keepAliveTimer->cancelTimeout();
  peer.expBackoff.reportError(); // apply exponential backoff
  peer.client.reset();

  // state transition
  KvStorePeerState oldState = peer.state;
  peer.state = getNextState(oldState, KvStorePeerEvent::SYNC_TIMEOUT);
  logStateTransition(peerName, oldState, peer.state);

  // Schedule another round of `thriftSyncTimer_` in case it is
  // NOT scheduled.
  if (not thriftSyncTimer_->isScheduled()) {
    thriftSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  }
}

void
KvStoreDb::addThriftPeers(
    std::unordered_map<std::string, thrift::PeerSpec> const& peers) {
  // kvstore external sync over thrift port of knob enabled
  for (auto const& peerKv : peers) {
    auto const& peerName = peerKv.first;
    auto const& newPeerSpec = peerKv.second;
    auto const& supportFloodOptimization = newPeerSpec.supportFloodOptimization;

    // try to connect with peer
    auto peerIter = thriftPeers_.find(peerName);
    if (peerIter != thriftPeers_.end()) {
      LOG(INFO) << "[Peer Update] " << peerName << " is updated."
                << " peerAddr: " << newPeerSpec.peerAddr
                << " flood-optimization:" << supportFloodOptimization;

      const auto& oldPeerSpec = peerIter->second.peerSpec;
      if (oldPeerSpec.peerAddr != newPeerSpec.peerAddr) {
        // case1: peerSpec updated(i.e. parallel adjacencies can
        //        potentially have peerSpec updated by LM)
        LOG(INFO) << "[Peer Update] peerAddr is updated from: "
                  << oldPeerSpec.peerAddr << " to: " << newPeerSpec.peerAddr;
      }
      logStateTransition(
          peerName, peerIter->second.state, KvStorePeerState::IDLE);

      peerIter->second.peerSpec = newPeerSpec; // update peerSpec
      peerIter->second.state = KvStorePeerState::IDLE; // set IDLE initially
      peerIter->second.keepAliveTimer->cancelTimeout(); // cancel timer
      peerIter->second.client.reset(); // destruct thriftClient
    } else {
      // case 2: found a new peer coming up
      LOG(INFO) << "[Peer Add] " << peerName << " is added."
                << " peerAddr: " << newPeerSpec.peerAddr
                << " flood-optimization:" << supportFloodOptimization;

      KvStorePeer peer(
          peerName,
          newPeerSpec,
          ExponentialBackoff<std::chrono::milliseconds>(
              Constants::kInitialBackoff, Constants::kMaxBackoff));
      // initialize keepAlive timer to make sure thrift client connection
      // will NOT be closed by thrift server due to inactivity
      peer.keepAliveTimer = folly::AsyncTimeout::make(
          *(evb_->getEvb()), [this, peerName]() noexcept {
            auto period = addJitter(Constants::kThriftClientKeepAliveInterval);
            CHECK(thriftPeers_.at(peerName).client)
                << "thrift client is NOT initialized";
            thriftPeers_.at(peerName).client->semifuture_getStatus();
            thriftPeers_.at(peerName).keepAliveTimer->scheduleTimeout(period);
          });
      thriftPeers_.emplace(peerName, std::move(peer));
    }
  } // for loop

  // kick off thriftSyncTimer_ if not yet to asyc process full-sync
  if (not thriftSyncTimer_->isScheduled()) {
    thriftSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  }
}

// add new peers to subscribe to
void
KvStoreDb::addPeers(
    std::unordered_map<std::string, thrift::PeerSpec> const& peers) {
  ++peerAddCounter_;
  std::vector<std::string> dualPeersToAdd;

  // in case thrift communication knob enabled, create a thrift peers
  // to track
  if (kvParams_.enableKvStoreThrift) {
    addThriftPeers(peers);
  }

  for (auto const& kv : peers) {
    auto const& peerName = kv.first;
    auto const& newPeerSpec = kv.second;
    auto const& newPeerCmdId = folly::sformat(
        Constants::kGlobalCmdLocalIdTemplate.toString(),
        peerName,
        peerAddCounter_);
    const auto& supportFloodOptimization = newPeerSpec.supportFloodOptimization;

    try {
      auto it = peers_.find(peerName);
      bool cmdUrlUpdated{false};
      bool isNewPeer{false};

      // add dual peers for both new-peer or update-peer event
      if (supportFloodOptimization) {
        dualPeersToAdd.emplace_back(peerName);
      }

      if (it != peers_.end()) {
        LOG(INFO)
            << "Updating existing peer " << peerName
            << ", support-flood-optimization: " << supportFloodOptimization;

        const auto& peerSpec = it->second.first;

        if (peerSpec.cmdUrl != newPeerSpec.cmdUrl) {
          // case1: peer-spec updated (e.g parallel cases)
          cmdUrlUpdated = true;
          LOG(INFO) << "Disconnecting from " << peerSpec.cmdUrl << " with id "
                    << it->second.second;
          const auto ret =
              peerSyncSock_.disconnect(fbzmq::SocketUrl{peerSpec.cmdUrl});
          if (ret.hasError()) {
            LOG(FATAL) << "Error Disconnecting to URL '" << peerSpec.cmdUrl
                       << "' " << ret.error();
          }
          // Remove any pending expected response for old socket-id
          latestSentPeerSync_.erase(it->second.second);
          it->second.second = newPeerCmdId;
        } else {
          // case2. new peer came up (previsously shut down ungracefully)
          LOG(WARNING) << "new peer " << peerName << ", previously "
                       << "shutdown non-gracefully";
          isNewPeer = true;
        }
        // Update entry with new data
        it->second.first = newPeerSpec;
      } else {
        // case3. new peer came up
        LOG(INFO)
            << "Adding new peer " << peerName
            << ", support-flood-optimization: " << supportFloodOptimization;
        isNewPeer = true;
        cmdUrlUpdated = true;
        std::tie(it, std::ignore) =
            peers_.emplace(peerName, std::make_pair(newPeerSpec, newPeerCmdId));
      }

      if (cmdUrlUpdated) {
        CHECK(newPeerCmdId == it->second.second);
        LOG(INFO) << "Connecting sync channel to " << newPeerSpec.cmdUrl
                  << " with id " << newPeerCmdId;
        auto const optStatus = peerSyncSock_.setSockOpt(
            ZMQ_CONNECT_RID, newPeerCmdId.data(), newPeerCmdId.size());
        if (optStatus.hasError()) {
          LOG(FATAL) << "Error setting ZMQ_CONNECT_RID with value "
                     << newPeerCmdId;
        }
        if (peerSyncSock_.connect(fbzmq::SocketUrl{newPeerSpec.cmdUrl})
                .hasError()) {
          LOG(FATAL) << "Error connecting to URL '" << newPeerSpec.cmdUrl
                     << "'";
        }
      }

      if (isNewPeer) {
        if (supportFloodOptimization) {
          // make sure let peer to unset-child for me for all roots first
          // after that, I'll be fed with proper dual-events and I'll be
          // chosing new nexthop if need.
          unsetChildAll(peerName);
        }
      }

      // ATTN: under thrift connection, initial full-sync will be handled
      //       separately. `peersToSyncWith_` will ONLY handle periodic
      //       random sync.
      if (not kvParams_.enableKvStoreThrift) {
        // Enqueue for full-sync requests
        LOG(INFO) << "Enqueuing full-sync request for peer " << peerName;
        peersToSyncWith_.emplace(
            peerName,
            ExponentialBackoff<std::chrono::milliseconds>(
                Constants::kInitialBackoff, Constants::kMaxBackoff));
      }
    } catch (std::exception const& e) {
      LOG(ERROR) << "Error connecting to: `" << peerName
                 << "` reason: " << folly::exceptionStr(e);
    }
  }
  if (not fullSyncTimer_->isScheduled()) {
    fullSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  }

  // process dual events if any
  if (kvParams_.enableFloodOptimization) {
    for (const auto& peer : dualPeersToAdd) {
      LOG(INFO) << "dual peer up: " << peer;
      DualNode::peerUp(peer, 1 /* link-cost */); // use hop count as metric
    }
  }
}

// Send message via socket
folly::Expected<size_t, fbzmq::Error>
KvStoreDb::sendMessageToPeer(
    const std::string& peerSocketId, const thrift::KvStoreRequest& request) {
  auto msg = fbzmq::Message::fromThriftObj(request, serializer_).value();
  fb303::fbData->addStatValue(
      "kvstore.peers.bytes_sent", msg.size(), fb303::SUM);
  return peerSyncSock_.sendMultiple(
      fbzmq::Message::from(peerSocketId).value(), fbzmq::Message(), msg);
}

std::map<std::string, int64_t>
KvStoreDb::getCounters() const {
  std::map<std::string, int64_t> counters;

  // Add some more flat counters
  counters["kvstore.num_keys"] = kvStore_.size();
  counters["kvstore.num_peers"] = peers_.size();
  // Add up pending and in-flight full sync
  counters["kvstore.pending_full_sync"] =
      peersToSyncWith_.size() + latestSentPeerSync_.size();
  // Record pending unfulfilled thrift request
  counters["kvstore.pending_thrift_request"] = thriftFs_.size();
  return counters;
}

void
KvStoreDb::delThriftPeers(std::vector<std::string> const& peers) {
  for (auto const& peerName : peers) {
    auto peerIter = thriftPeers_.find(peerName);
    if (peerIter == thriftPeers_.end()) {
      LOG(ERROR) << "[Peer Delete] try to delete non-existing peer: "
                 << peerName << ". Skip.";
      continue;
    }
    const auto& peerSpec = peerIter->second.peerSpec;

    LOG(INFO) << "[Peer Delete] " << peerName
              << " is detached from: " << peerSpec.peerAddr
              << ", flood-optimization: " << peerSpec.supportFloodOptimization;

    // destroy peer info
    peerIter->second.keepAliveTimer.reset();
    peerIter->second.client.reset();
    thriftPeers_.erase(peerIter);
  }
}

// delete some peers we are subscribed to
void
KvStoreDb::delPeers(std::vector<std::string> const& peers) {
  std::vector<std::string> dualPeersToRemove;

  if (kvParams_.enableKvStoreThrift) {
    delThriftPeers(peers);
  }

  for (auto const& peerName : peers) {
    // not currently subscribed
    auto it = peers_.find(peerName);
    if (it == peers_.end()) {
      LOG(ERROR) << "Trying to delete non-existing peer '" << peerName << "'";
      continue;
    }

    const auto& peerSpec = it->second.first;
    if (peerSpec.supportFloodOptimization) {
      dualPeersToRemove.emplace_back(peerName);
    }

    LOG(INFO) << "Detaching from: " << peerSpec.cmdUrl
              << ", support-flood-optimization: "
              << peerSpec.supportFloodOptimization;
    auto syncRes = peerSyncSock_.disconnect(fbzmq::SocketUrl{peerSpec.cmdUrl});
    if (syncRes.hasError()) {
      LOG(ERROR) << "Failed to detach. " << syncRes.error();
    }

    peersToSyncWith_.erase(peerName);
    auto const& peerCmdSocketId = it->second.second;
    if (latestSentPeerSync_.count(peerCmdSocketId)) {
      latestSentPeerSync_.erase(peerCmdSocketId);
    }
    peers_.erase(it);
  }

  // remove dual peers if any
  if (kvParams_.enableFloodOptimization) {
    for (const auto& peer : dualPeersToRemove) {
      LOG(INFO) << "dual peer down: " << peer;
      DualNode::peerDown(peer);
    }
  }
}

// Get full KEY_DUMP from peersToSyncWith_
void
KvStoreDb::requestFullSyncFromPeers() {
  // minimal timeout for next run
  auto timeout = std::chrono::milliseconds(Constants::kMaxBackoff);

  // Make requests
  for (auto it = peersToSyncWith_.begin(); it != peersToSyncWith_.end();) {
    auto& peerName = it->first;
    auto& expBackoff = it->second;

    if (not expBackoff.canTryNow()) {
      timeout = std::min(timeout, expBackoff.getTimeRemainingUntilRetry());
      ++it;
      continue;
    }

    // Generate and send router-socket id of peer first. If the kvstore of
    // peer is not connected over the router socket then it will error out
    // exception and we will retry again.
    auto const& peerCmdSocketId = peers_.at(peerName).second;

    // Build request
    thrift::KvStoreRequest dumpRequest;
    thrift::KeyDumpParams params;

    if (kvParams_.filters.has_value()) {
      std::string keyPrefix =
          folly::join(",", kvParams_.filters.value().getKeyPrefixes());
      params.prefix_ref() = keyPrefix;
      params.originatorIds_ref() =
          kvParams_.filters.value().getOriginatorIdList();
    }
    std::set<std::string> originator{};
    std::vector<std::string> keyPrefixList{};
    KvStoreFilters kvFilters{keyPrefixList, originator};
    params.keyValHashes_ref() =
        std::move(dumpHashWithFilters(kvFilters).keyVals);

    dumpRequest.cmd = thrift::Command::KEY_DUMP;
    dumpRequest.keyDumpParams_ref() = params;
    dumpRequest.area = area_;

    VLOG(1) << "Sending full-sync request to peer " << peerName << " using id "
            << peerCmdSocketId;
    auto const ret = sendMessageToPeer(peerCmdSocketId, dumpRequest);

    if (ret.hasError()) {
      // this could be pretty common on initial connection setup
      LOG(ERROR) << "Failed to send full-sync request to peer " << peerName
                 << " using id " << peerCmdSocketId << " (will try again). "
                 << ret.error();
      collectSendFailureStats(ret.error(), peerCmdSocketId);
      expBackoff.reportError(); // Apply exponential backoff
      timeout = std::min(timeout, expBackoff.getTimeRemainingUntilRetry());
      ++it;
    } else {
      latestSentPeerSync_[peerCmdSocketId] = std::chrono::steady_clock::now();

      // Remove the iterator
      it = peersToSyncWith_.erase(it);
    }

    // if pending response is above the limit wait until kMaxBackoff before
    // sending next sync request
    if (latestSentPeerSync_.size() >= parallelSyncLimit_) {
      LOG(INFO) << latestSentPeerSync_.size() << " full-sync in progress which "
                << " is above limit: " << parallelSyncLimit_ << ". Will send "
                << "sync request after max timeout or on receipt of sync "
                << "response";
      timeout = Constants::kMaxBackoff;
      break;
    }
  } // for

  // schedule fullSyncTimer if there are pending peers to sync with or
  // if maximum allowed pending sync count is reached. Adding a new peer
  // will not initiate full sync request if it's already scheduled
  if (not peersToSyncWith_.empty() ||
      latestSentPeerSync_.size() >= parallelSyncLimit_) {
    LOG_IF(INFO, peersToSyncWith_.size())
        << peersToSyncWith_.size() << " peers still require full-sync.";
    LOG(INFO) << "Scheduling full-sync after " << timeout.count() << "ms.";
    // schedule next timeout
    fullSyncTimer_->scheduleTimeout(timeout);
  }
}

// dump all peers we are subscribed to
thrift::PeersMap
KvStoreDb::dumpPeers() {
  thrift::PeersMap peers;

  if (kvParams_.enableKvStoreThrift) {
    for (auto const& kv : thriftPeers_) {
      peers.emplace(kv.first, kv.second.peerSpec);
    }
  } else {
    for (auto const& kv : peers_) {
      peers.emplace(kv.first, kv.second.first);
    }
  }
  return peers;
}

// update TTL with remainng time to expire, TTL version remains
// same so existing keys will not be updated with this TTL
void
KvStoreDb::updatePublicationTtl(
    thrift::Publication& thriftPub, bool removeAboutToExpire) {
  auto timeNow = std::chrono::steady_clock::now();
  for (const auto& qE : ttlCountdownQueue_) {
    // Find key and ensure we are taking time from right entry from queue
    auto kv = thriftPub.keyVals.find(qE.key);
    if (kv == thriftPub.keyVals.end() or kv->second.version != qE.version or
        kv->second.originatorId != qE.originatorId or
        kv->second.ttlVersion != qE.ttlVersion) {
      continue;
    }

    // Compute timeLeft and do sanity check on it
    auto timeLeft = duration_cast<milliseconds>(qE.expiryTime - timeNow);
    if (timeLeft <= kvParams_.ttlDecr) {
      thriftPub.keyVals.erase(kv);
      continue;
    }

    // filter key from publication if time left is below ttl threshold
    if (removeAboutToExpire and timeLeft < Constants::kTtlThreshold) {
      thriftPub.keyVals.erase(kv);
      continue;
    }

    // Set the time-left and decrement it by one so that ttl decrement
    // deterministically whenever it is exchanged between KvStores. This will
    // avoid looping of updates between stores.
    kv->second.ttl = timeLeft.count() - kvParams_.ttlDecr.count();
  }
}

// process a request
folly::Expected<fbzmq::Message, fbzmq::Error>
KvStoreDb::processRequestMsgHelper(
    const std::string& requestId, thrift::KvStoreRequest& thriftReq) {
  VLOG(3)
      << "processRequest: command: `"
      << apache::thrift::TEnumTraits<thrift::Command>::findName(thriftReq.cmd)
      << "` received";

  std::vector<std::string> keys;
  switch (thriftReq.cmd) {
  case thrift::Command::KEY_SET: {
    VLOG(3) << "Set key requested";
    if (not thriftReq.keySetParams_ref().has_value()) {
      LOG(ERROR) << "received none keySetParams";
      return folly::makeUnexpected(fbzmq::Error());
    }

    fb303::fbData->addStatValue("kvstore.cmd_key_set", 1, fb303::COUNT);
    if (thriftReq.keySetParams_ref()->timestamp_ms_ref().has_value()) {
      auto floodMs = getUnixTimeStampMs() -
          thriftReq.keySetParams_ref()->timestamp_ms_ref().value();
      if (floodMs > 0) {
        fb303::fbData->addStatValue(
            "kvstore.flood_duration_ms", floodMs, fb303::AVG);
      }
    }

    auto& ketSetParamsVal = thriftReq.keySetParams_ref().value();
    if (ketSetParamsVal.keyVals.empty()) {
      LOG(ERROR) << "Malformed set request, ignoring";
      return folly::makeUnexpected(fbzmq::Error());
    }

    // Update hash for key-values
    for (auto& kv : ketSetParamsVal.keyVals) {
      auto& value = kv.second;
      if (value.value_ref().has_value()) {
        value.hash_ref() =
            generateHash(value.version, value.originatorId, value.value_ref());
      }
    }

    // Create publication and merge it with local KvStore
    thrift::Publication rcvdPublication;
    rcvdPublication.keyVals = std::move(ketSetParamsVal.keyVals);
    rcvdPublication.nodeIds_ref().move_from(ketSetParamsVal.nodeIds_ref());
    rcvdPublication.floodRootId_ref().move_from(
        ketSetParamsVal.floodRootId_ref());
    mergePublication(rcvdPublication);

    // respond to the client
    if (ketSetParamsVal.solicitResponse) {
      return fbzmq::Message::from(Constants::kSuccessResponse.toString());
    }
    return fbzmq::Message();
  }
  case thrift::Command::KEY_DUMP: {
    VLOG(3) << "Dump all keys requested";
    if (not thriftReq.keyDumpParams_ref().has_value()) {
      LOG(ERROR) << "received none keyDumpParams";
      return folly::makeUnexpected(fbzmq::Error());
    }

    auto& keyDumpParamsVal = thriftReq.keyDumpParams_ref().value();
    fb303::fbData->addStatValue("kvstore.cmd_key_dump", 1, fb303::COUNT);

    std::vector<std::string> keyPrefixList;
    if (keyDumpParamsVal.keys_ref().has_value()) {
      keyPrefixList = *keyDumpParamsVal.keys_ref();
    } else if (keyDumpParamsVal.prefix_ref().has_value()) {
      folly::split(",", *keyDumpParamsVal.prefix_ref(), keyPrefixList, true);
    }

    const auto keyPrefixMatch =
        KvStoreFilters(keyPrefixList, keyDumpParamsVal.originatorIds);
    auto thriftPub = dumpAllWithFilters(keyPrefixMatch);
    if (auto keyValHashes = keyDumpParamsVal.keyValHashes_ref()) {
      thriftPub = dumpDifference(thriftPub.keyVals, *keyValHashes);
    }
    updatePublicationTtl(thriftPub);
    // I'm the initiator, set flood-root-id
    fromStdOptional(thriftPub.floodRootId_ref(), DualNode::getSptRootId());

    if (keyDumpParamsVal.keyValHashes_ref() and
        (not keyDumpParamsVal.prefix_ref().has_value() or
         (*keyDumpParamsVal.prefix_ref()).empty()) and
        (not keyDumpParamsVal.keys_ref().has_value() or
         (*keyDumpParamsVal.keys_ref()).empty())) {
      // This usually comes from neighbor nodes
      size_t numMissingKeys = 0;
      if (thriftPub.tobeUpdatedKeys_ref().has_value()) {
        numMissingKeys = thriftPub.tobeUpdatedKeys_ref()->size();
      }
      LOG(INFO) << "Processed full-sync request from peer " << requestId
                << " with " << (*keyDumpParamsVal.keyValHashes_ref()).size()
                << " keyValHashes item(s). Sending " << thriftPub.keyVals.size()
                << " key-vals and " << numMissingKeys << " missing keys";
    }
    return fbzmq::Message::fromThriftObj(thriftPub, serializer_);
  }
  case thrift::Command::DUAL: {
    VLOG(2) << "DUAL messages received";
    if (not thriftReq.dualMessages_ref().has_value()) {
      LOG(ERROR) << "received none dualMessages";
      return fbzmq::Message(); // ignore it
    }
    if (thriftReq.dualMessages_ref().value().messages.empty()) {
      LOG(ERROR) << "received empty dualMessages";
      return fbzmq::Message(); // ignore it
    }
    fb303::fbData->addStatValue(
        "kvstore.received_dual_messages", 1, fb303::COUNT);
    DualNode::processDualMessages(std::move(*thriftReq.dualMessages_ref()));
    return fbzmq::Message();
  }
  case thrift::Command::FLOOD_TOPO_SET: {
    VLOG(2) << "FLOOD_TOPO_SET command requested";
    if (not thriftReq.floodTopoSetParams_ref().has_value()) {
      LOG(ERROR) << "received none floodTopoSetParams";
      return fbzmq::Message(); // ignore it
    }
    processFloodTopoSet(std::move(*thriftReq.floodTopoSetParams_ref()));
    return fbzmq::Message();
  }
  default: {
    LOG(ERROR) << "Unknown command received";
    return folly::makeUnexpected(fbzmq::Error());
  }
  }
}

thrift::SptInfos
KvStoreDb::processFloodTopoGet() noexcept {
  thrift::SptInfos sptInfos;
  const auto& duals = DualNode::getDuals();

  // set spt-infos
  for (const auto& kv : duals) {
    const auto& rootId = kv.first;
    const auto& info = kv.second.getInfo();
    thrift::SptInfo sptInfo;
    sptInfo.passive = info.sm.state == DualState::PASSIVE;
    sptInfo.cost = info.distance;
    // convert from std::optional to std::optional
    std::optional<std::string> nexthop = std::nullopt;
    if (info.nexthop.has_value()) {
      nexthop = info.nexthop.value();
    }
    fromStdOptional(sptInfo.parent_ref(), nexthop);
    sptInfo.children = kv.second.children();
    sptInfos.infos.emplace(rootId, sptInfo);
  }

  // set counters
  sptInfos.counters = DualNode::getCounters();

  // set flood root-id and peers
  fromStdOptional(sptInfos.floodRootId_ref(), DualNode::getSptRootId());
  std::optional<std::string> floodRootId{std::nullopt};
  if (sptInfos.floodRootId_ref().has_value()) {
    floodRootId = sptInfos.floodRootId_ref().value();
  }
  sptInfos.floodPeers = getFloodPeers(floodRootId);
  return sptInfos;
}

void
KvStoreDb::processFloodTopoSet(
    const thrift::FloodTopoSetParams& setParams) noexcept {
  if (setParams.allRoots_ref().has_value() and *setParams.allRoots_ref() and
      not setParams.setChild) {
    // process unset-child for all-roots command
    auto& duals = DualNode::getDuals();
    for (auto& kv : duals) {
      kv.second.removeChild(setParams.srcId);
    }
    return;
  }

  if (not DualNode::hasDual(setParams.rootId)) {
    LOG(ERROR) << "processFloodTopoSet unknown root-id: " << setParams.rootId;
    return;
  }
  auto& dual = DualNode::getDual(setParams.rootId);
  const auto& child = setParams.srcId;
  if (setParams.setChild) {
    // set child command
    LOG(INFO) << "dual child set: root-id: (" << setParams.rootId
              << ") child: " << setParams.srcId;
    dual.addChild(child);
  } else {
    // unset child command
    LOG(INFO) << "dual child unset: root-id: (" << setParams.rootId
              << ") child: " << setParams.srcId;
    dual.removeChild(child);
  }
}

void
KvStoreDb::sendTopoSetCmd(
    const std::string& rootId,
    const std::string& peerName,
    bool setChild,
    bool allRoots) noexcept {
  const auto& dstCmdSocketId = peers_.at(peerName).second;
  thrift::KvStoreRequest request;
  request.cmd = thrift::Command::FLOOD_TOPO_SET;

  thrift::FloodTopoSetParams setParams;
  setParams.rootId = rootId;
  setParams.srcId = kvParams_.nodeId;
  setParams.setChild = setChild;
  if (allRoots) {
    setParams.allRoots_ref() = allRoots;
  }
  request.floodTopoSetParams_ref() = setParams;
  request.area = area_;

  const auto ret = sendMessageToPeer(dstCmdSocketId, request);
  if (ret.hasError()) {
    LOG(ERROR) << rootId << ": failed to " << (setChild ? "set" : "unset")
               << " spt-parent " << peerName << ", error: " << ret.error();
    collectSendFailureStats(ret.error(), dstCmdSocketId);
  }
}

void
KvStoreDb::setChild(
    const std::string& rootId, const std::string& peerName) noexcept {
  sendTopoSetCmd(rootId, peerName, true, false);
}

void
KvStoreDb::unsetChild(
    const std::string& rootId, const std::string& peerName) noexcept {
  sendTopoSetCmd(rootId, peerName, false, false);
}

void
KvStoreDb::unsetChildAll(const std::string& peerName) noexcept {
  sendTopoSetCmd("" /* root-id is ignored */, peerName, false, true);
}

void
KvStoreDb::processNexthopChange(
    const std::string& rootId,
    const std::optional<std::string>& oldNh,
    const std::optional<std::string>& newNh) noexcept {
  // sanity check
  std::string oldNhStr = oldNh.has_value() ? *oldNh : "none";
  std::string newNhStr = newNh.has_value() ? *newNh : "none";
  CHECK(oldNh != newNh)
      << rootId
      << ": callback invoked while nexthop does not change: " << oldNhStr;
  // root should NEVER change its nexthop (nexthop always equal to myself)
  CHECK_NE(kvParams_.nodeId, rootId);
  LOG(INFO) << "dual nexthop change: root-id (" << rootId << ") " << oldNhStr
            << " -> " << newNhStr;

  // set new parent if any
  if (newNh.has_value()) {
    // peers_ MUST have this new parent
    // if peers_ does not have this peer, that means KvStore already recevied
    // NEIGHBOR-DOWN event (so does dual), but dual still think I should have
    // this neighbor as nexthop, then something is wrong with DUAL
    CHECK(peers_.count(*newNh))
        << rootId << ": trying to set new spt-parent who does not exist "
        << *newNh;
    CHECK_NE(kvParams_.nodeId, *newNh) << "new nexthop is myself";
    setChild(rootId, *newNh);

    // Enqueue new-nexthop for full-sync (insert only if entry doesn't exists)
    // NOTE we have to perform full-sync after we do FLOOD_TOPO_SET, so that
    // we can be sure that I won't be in a disconnected state after we got
    // full synced. (ps: full-sync is 3-way-sync, one direction sync should be
    // good enough)
    LOG(INFO) << "Enqueuing full-sync request for peer " << *newNh;
    peersToSyncWith_.emplace(
        *newNh,
        ExponentialBackoff<std::chrono::milliseconds>(
            Constants::kInitialBackoff, Constants::kMaxBackoff));

    // initial full-sync request if peersToSyncWith_ was empty
    if (not fullSyncTimer_->isScheduled()) {
      fullSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
    }
  }

  // unset old parent if any
  if (oldNh.has_value() and peers_.count(*oldNh)) {
    // valid old parent AND it's still my peer, unset it
    CHECK_NE(kvParams_.nodeId, *oldNh) << "old nexthop was myself";
    // unset it
    unsetChild(rootId, *oldNh);
  }
}

void
KvStoreDb::processSyncResponse(
    const std::string& requestId, fbzmq::Message&& syncPubMsg) noexcept {
  fb303::fbData->addStatValue(
      "kvstore.peers.bytes_received", syncPubMsg.size(), fb303::SUM);

  // syncPubMsg can be of two types
  // 1. ack to SET_KEY ("OK" or "ERR")
  // 2. response of KEY_DUMP (thrift::Publication)
  // We check for first one and then fallback to second one
  if (syncPubMsg.size() < 3) {
    auto syncPubStr = syncPubMsg.read<std::string>().value();
    if (syncPubStr == Constants::kErrorResponse) {
      LOG(ERROR) << "Got error for sent publication from " << requestId;
      return;
    }
    if (syncPubStr == Constants::kSuccessResponse) {
      VLOG(2) << "Got ack for sent publication on " << requestId;
      return;
    }
  }

  // Perform error check
  auto maybeSyncPub =
      syncPubMsg.readThriftObj<thrift::Publication>(serializer_);
  if (maybeSyncPub.hasError()) {
    LOG(ERROR) << "Received bad response on peerSyncSock";
    return;
  }

  const auto& syncPub = maybeSyncPub.value();
  const size_t kvUpdateCnt = mergePublication(syncPub, requestId);
  size_t numMissingKeys = 0;
  if (syncPub.tobeUpdatedKeys_ref().has_value()) {
    numMissingKeys = syncPub.tobeUpdatedKeys_ref()->size();
  }

  LOG(INFO) << "full-sync response received from " << requestId << " with "
            << syncPub.keyVals.size() << " key-vals and " << numMissingKeys
            << " missing keys. Incured " << kvUpdateCnt << " key-value updates";

  fb303::fbData->addStatValue(
      "kvstore.zmq.num_missing_keys", numMissingKeys, fb303::SUM);
  fb303::fbData->addStatValue(
      "kvstore.zmq.num_keyvals_update", kvUpdateCnt, fb303::SUM);

  if (latestSentPeerSync_.count(requestId)) {
    auto syncDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - latestSentPeerSync_.at(requestId));
    fb303::fbData->addStatValue(
        "kvstore.full_sync_duration_ms", syncDuration.count(), fb303::AVG);
    logSyncEvent(requestId, syncDuration);
    VLOG(1) << "It took " << syncDuration.count() << " ms to sync with "
            << requestId;
    latestSentPeerSync_.erase(requestId);
  }

  // We've received a full sync response. Double the parallel sync-request
  // limit. This is under assumption that, subsequent sync request will not
  // incur huge changes.
  parallelSyncLimit_ = std::min(
      2 * parallelSyncLimit_, Constants::kMaxFullSyncPendingCountThreshold);

  // Schedule timeout immediately to resume sending full sync requests. If no
  // outstanding sync is required, then cancel the timeout. Cancelling timeout
  // will let the subsequent sync requests to proceed immediately.
  if (not peersToSyncWith_.empty()) {
    fullSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  } else {
    fullSyncTimer_->cancelTimeout();
  }
}

// send sync request from one neighbor randomly
void
KvStoreDb::requestSync() {
  SCOPE_EXIT {
    auto period =
        addJitter(std::chrono::milliseconds(kvParams_.dbSyncInterval));

    // Schedule next sync with peers
    requestSyncTimer_->scheduleTimeout(period);
  };

  if (peers_.empty()) {
    return;
  }

  // Randomly select one neighbor to request full-dump from
  int randomIndex = folly::Random::rand32() % peers_.size();
  int index{0};
  std::string randomNeighbor;

  for (auto const& kv : peers_) {
    if (index++ == randomIndex) {
      randomNeighbor = kv.first;
      break;
    }
  }

  // Enqueue neighbor for full-sync (insert only if entry doesn't exists)
  LOG(INFO) << "Enqueuing full-sync request for peer " << randomNeighbor;
  peersToSyncWith_.emplace(
      randomNeighbor,
      ExponentialBackoff<std::chrono::milliseconds>(
          Constants::kInitialBackoff, Constants::kMaxBackoff));

  // initial full-sync request if peersToSyncWith_ was empty
  if (not fullSyncTimer_->isScheduled()) {
    fullSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  }
}

// this will poll the sockets listening to the requests
void
KvStoreDb::attachCallbacks() {
  VLOG(2) << "KvStore: Registering events callbacks ...";

  const auto peersSyncSndHwm = peerSyncSock_.setSockOpt(
      ZMQ_SNDHWM, &kvParams_.zmqHwm, sizeof(kvParams_.zmqHwm));
  if (peersSyncSndHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << kvParams_.zmqHwm << " "
               << peersSyncSndHwm.error();
  }
  const auto peerSyncRcvHwm = peerSyncSock_.setSockOpt(
      ZMQ_RCVHWM, &kvParams_.zmqHwm, sizeof(kvParams_.zmqHwm));
  if (peerSyncRcvHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << kvParams_.zmqHwm << " "
               << peerSyncRcvHwm.error();
  }

  // enable handover for inter process router socket
  const int handover = 1;
  const auto peerSyncHandover =
      peerSyncSock_.setSockOpt(ZMQ_ROUTER_HANDOVER, &handover, sizeof(int));
  if (peerSyncHandover.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_ROUTER_HANDOVER to " << handover << " "
               << peerSyncHandover.error();
  }

  // set keep-alive to retire old flows
  const auto peerSyncKeepAlive = peerSyncSock_.setKeepAlive(
      Constants::kKeepAliveEnable,
      Constants::kKeepAliveTime.count(),
      Constants::kKeepAliveCnt,
      Constants::kKeepAliveIntvl.count());
  if (peerSyncKeepAlive.hasError()) {
    LOG(FATAL) << "Error setting KeepAlive " << peerSyncKeepAlive.error();
  }

  if (kvParams_.maybeIpTos.has_value()) {
    const int ipTos = kvParams_.maybeIpTos.value();
    const auto peerSyncTos =
        peerSyncSock_.setSockOpt(ZMQ_TOS, &ipTos, sizeof(int));
    if (peerSyncTos.hasError()) {
      LOG(FATAL) << "Error setting ZMQ_TOS to " << ipTos << " "
                 << peerSyncTos.error();
    }
  }

  evb_->addSocket(
      fbzmq::RawZmqSocketPtr{*peerSyncSock_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(3) << "KvStore: sync response received";
        drainPeerSyncSock();
      });

  // Hacky timer to drain pending messages on peerSyncSock because of
  // notification. This happens ONLY with zmq socket
  drainPeerSyncSockTimer_ =
      folly::AsyncTimeout::make(*evb_->getEvb(), [this]() noexcept {
        drainPeerSyncSock();
        drainPeerSyncSockTimer_->scheduleTimeout(std::chrono::seconds(1));
      });
  drainPeerSyncSockTimer_->scheduleTimeout(std::chrono::seconds(1));

  // Perform full-sync if there are peers to sync with.
  fullSyncTimer_ = folly::AsyncTimeout::make(
      *evb_->getEvb(), [this]() noexcept { requestFullSyncFromPeers(); });

  if (kvParams_.enablePeriodicSync) {
    // Define request sync timer
    requestSyncTimer_ = folly::AsyncTimeout::make(
        *evb_->getEvb(), [this]() noexcept { requestSync(); });

    // Schedule periodic call to re-sync with one of our peer
    requestSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  }

  // Define timer to scan peers in IDLE state to do initial syncing
  if (kvParams_.enableKvStoreThrift) {
    thriftSyncTimer_ = folly::AsyncTimeout::make(
        *evb_->getEvb(), [this]() noexcept { requestThriftPeerSync(); });
  }
}

void
KvStoreDb::drainPeerSyncSock() {
  // Drain all available messages in loop
  while (true) {
    fbzmq::Message requestIdMsg, delimMsg, syncPubMsg;
    auto ret = peerSyncSock_.recvMultiple(requestIdMsg, delimMsg, syncPubMsg);
    if (ret.hasError() and ret.error().errNum == EAGAIN) {
      break;
    }

    // Check for error in receiving messages
    if (ret.hasError()) {
      LOG(ERROR) << "failed reading messages from peerSyncSock_: "
                 << ret.error();
      continue;
    }

    // at this point we received all three parts
    if (not delimMsg.empty()) {
      LOG(ERROR) << "unexpected delimiter from peerSyncSock_: "
                 << delimMsg.read<std::string>().value();
      continue;
    }

    // process the request
    processSyncResponse(
        requestIdMsg.read<std::string>().value(), std::move(syncPubMsg));
  } // while
}

void
KvStoreDb::cleanupTtlCountdownQueue() {
  // record all expired keys
  std::vector<std::string> expiredKeys;
  auto now = std::chrono::steady_clock::now();

  // Iterate through ttlCountdownQueue_ until the top expires in the future
  while (not ttlCountdownQueue_.empty()) {
    auto top = ttlCountdownQueue_.top();
    if (top.expiryTime > now) {
      // Nothing in queue worth evicting
      break;
    }
    auto it = kvStore_.find(top.key);
    if (it != kvStore_.end() and it->second.version == top.version and
        it->second.originatorId == top.originatorId and
        it->second.ttlVersion == top.ttlVersion) {
      expiredKeys.emplace_back(top.key);
      LOG(WARNING)
          << "Delete expired (key, version, originatorId, ttlVersion, ttl, "
          << "node, area) "
          << folly::sformat(
                 "({}, {}, {}, {}, {}, {}, {})",
                 top.key,
                 it->second.version,
                 it->second.originatorId,
                 it->second.ttlVersion,
                 it->second.ttl,
                 kvParams_.nodeId,
                 area_);
      logKvEvent("KEY_EXPIRE", top.key);
      kvStore_.erase(it);
    }
    ttlCountdownQueue_.pop();
  }

  // Reschedule based on most recent timeout
  if (not ttlCountdownQueue_.empty()) {
    ttlCountdownTimer_->scheduleTimeout(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            ttlCountdownQueue_.top().expiryTime - now));
  }

  if (expiredKeys.empty()) {
    // no key expires
    return;
  }
  fb303::fbData->addStatValue(
      "kvstore.expired_key_vals", expiredKeys.size(), fb303::SUM);
  thrift::Publication expiredKeysPub{};
  expiredKeysPub.expiredKeys = std::move(expiredKeys);
  floodPublication(std::move(expiredKeysPub));
}

void
KvStoreDb::bufferPublication(thrift::Publication&& publication) {
  fb303::fbData->addStatValue("kvstore.rate_limit_suppress", 1, fb303::COUNT);
  fb303::fbData->addStatValue(
      "kvstore.rate_limit_keys", publication.keyVals.size(), fb303::AVG);
  std::optional<std::string> floodRootId{std::nullopt};
  if (publication.floodRootId_ref().has_value()) {
    floodRootId = publication.floodRootId_ref().value();
  }
  // update or add keys
  for (auto const& kv : publication.keyVals) {
    publicationBuffer_[floodRootId].emplace(kv.first);
  }
  for (auto const& key : publication.expiredKeys) {
    publicationBuffer_[floodRootId].emplace(key);
  }
}

void
KvStoreDb::floodBufferedUpdates() {
  if (!publicationBuffer_.size()) {
    return;
  }

  // merged-publications to be sent
  std::vector<thrift::Publication> publications;

  // merge publication per root-id
  for (const auto& kv : publicationBuffer_) {
    thrift::Publication publication{};
    // convert from std::optional to std::optional
    std::optional<std::string> floodRootId{std::nullopt};
    if (kv.first.has_value()) {
      floodRootId = kv.first.value();
    }
    fromStdOptional(publication.floodRootId_ref(), floodRootId);
    for (const auto& key : kv.second) {
      auto kvStoreIt = kvStore_.find(key);
      if (kvStoreIt != kvStore_.end()) {
        publication.keyVals.emplace(make_pair(key, kvStoreIt->second));
      } else {
        publication.expiredKeys.emplace_back(key);
      }
    }
    publications.emplace_back(std::move(publication));
  }

  publicationBuffer_.clear();

  for (auto& pub : publications) {
    // when sending out merged publication, we maintain orginal-root-id
    // we act as a forwarder, NOT an initiator. Disable set-flood-root here
    floodPublication(
        std::move(pub), false /* rate-limit */, false /* set-flood-root */);
  }
}

void
KvStoreDb::finalizeFullSync(
    const std::vector<std::string>& keys, const std::string& senderId) {
  // build keyval to be sent
  thrift::Publication updates;
  for (const auto& key : keys) {
    const auto& it = kvStore_.find(key);
    if (it != kvStore_.end()) {
      updates.keyVals.emplace(key, it->second);
    }
  }

  // Update ttl values to remove expiring keys. Ignore the response if no
  // keys to be sent
  updatePublicationTtl(updates);
  if (not updates.keyVals.size()) {
    return;
  }

  thrift::KvStoreRequest updateRequest;
  thrift::KeySetParams params;

  params.keyVals = std::move(updates.keyVals);
  params.solicitResponse = false;
  // I'm the initiator, set flood-root-id
  fromStdOptional(params.floodRootId_ref(), DualNode::getSptRootId());
  params.timestamp_ms_ref() = getUnixTimeStampMs();

  updateRequest.cmd = thrift::Command::KEY_SET;
  updateRequest.keySetParams_ref() = params;
  updateRequest.area = area_;

  // ATTN: KvStore maintains different mechanism over 3-way full-sync.
  //  1) Over thrift peer connection;
  //  2) Over ZMQ socket;
  if (kvParams_.enableKvStoreThrift) {
    auto peerIt = thriftPeers_.find(senderId);
    if (peerIt == thriftPeers_.end()) {
      LOG(ERROR) << "Invalid peer: " << senderId
                 << " to do finalize sync with. Skip it.";
      return;
    }

    auto& thriftPeer = thriftPeers_.at(senderId);
    if (thriftPeer.state == KvStorePeerState::IDLE or (not thriftPeer.client)) {
      // peer in thriftPeers collection can still be in IDLE state.
      // Skip final full-sync with those peers.
      return;
    }

    LOG(INFO) << "[Thrift Sync] Finalize full-sync back to: " << senderId
              << " with keys: " << folly::join(",", keys);

    // record telemetry for thrift calls
    fb303::fbData->addStatValue(
        "kvstore.thrift.num_finalized_sync", 1, fb303::COUNT);

    auto startTime = std::chrono::steady_clock::now();
    auto sf = thriftPeer.client->semifuture_setKvStoreKeyVals(params, area_);
    auto pendingReqId = ++pendingThriftId_;
    thriftFs_.emplace(
        pendingReqId,
        std::move(sf)
            .via(evb_->getEvb())
            .thenValue([this, senderId, startTime, pendingReqId](
                           folly::Unit&&) {
              VLOG(4) << "Finalize full-sync ack received from peer: "
                      << senderId;
              auto endTime = std::chrono::steady_clock::now();
              auto timeDelta =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      endTime - startTime);

              // cleanup pendingReqId
              thriftFs_.erase(pendingReqId);

              // record telemetry for thrift calls
              fb303::fbData->addStatValue(
                  "kvstore.thrift.num_finalized_sync_success", 1, fb303::COUNT);
              fb303::fbData->addStatValue(
                  "kvstore.thrift.finalized_sync_duration_ms",
                  timeDelta.count(),
                  fb303::AVG);
            })
            .thenError([this, senderId, startTime, pendingReqId](
                           const folly::exception_wrapper& ew) {
              // state transition to IDLE
              auto endTime = std::chrono::steady_clock::now();
              auto timeDelta =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      endTime - startTime);
              processThriftFailure(senderId, ew.what(), timeDelta);

              // cleanup pendingReqId
              thriftFs_.erase(pendingReqId);

              // record telemetry for thrift calls
              fb303::fbData->addStatValue(
                  "kvstore.thrift.num_finalized_sync_failure", 1, fb303::COUNT);
            }));
  } else {
    VLOG(1) << "finalizeFullSync back to: " << senderId
            << " with keys: " << folly::join(",", keys);

    auto const ret = sendMessageToPeer(senderId, updateRequest);
    if (ret.hasError()) {
      // this could fail when senderId goes offline
      LOG(ERROR) << "Failed to send finalizeFullSync to " << senderId
                 << " using id " << senderId << ", error: " << ret.error();
      collectSendFailureStats(ret.error(), senderId);
    }
  }
}

std::unordered_set<std::string>
KvStoreDb::getFloodPeers(const std::optional<std::string>& rootId) {
  auto sptPeers = DualNode::getSptPeers(rootId);
  bool floodToAll = false;
  if (not kvParams_.enableFloodOptimization or sptPeers.empty()) {
    // fall back to naive flooding if feature not enabled or can not find
    // valid SPT-peers
    floodToAll = true;
  }

  // flood-peers: SPT-peers + peers-who-does-not-support-dual
  std::unordered_set<std::string> floodPeers;
  for (const auto& kv : peers_) {
    const auto& peer = kv.first;
    const auto& peerSpec = kv.second.first;
    if (floodToAll or sptPeers.count(peer) != 0 or
        not peerSpec.supportFloodOptimization) {
      floodPeers.emplace(peer);
    }
  }
  return floodPeers;
}

void
KvStoreDb::collectSendFailureStats(
    const fbzmq::Error& error, const std::string& dstSockId) {
  fb303::fbData->addStatValue(
      folly::sformat("kvstore.send_failure.{}.{}", dstSockId, error.errNum),
      1,
      fb303::COUNT);
}

void
KvStoreDb::floodPublication(
    thrift::Publication&& publication, bool rateLimit, bool setFloodRoot) {
  // rate limit if configured
  if (floodLimiter_ && rateLimit && !floodLimiter_->consume(1)) {
    bufferPublication(std::move(publication));
    pendingPublicationTimer_->scheduleTimeout(
        Constants::kFloodPendingPublication);
    return;
  }
  // merge with buffered publication and flood
  if (publicationBuffer_.size()) {
    bufferPublication(std::move(publication));
    return floodBufferedUpdates();
  }
  // Update ttl on keys we are trying to advertise. Also remove keys which
  // are about to expire.
  updatePublicationTtl(publication, true);

  // If there are no changes then return
  if (publication.keyVals.empty() && publication.expiredKeys.empty()) {
    return;
  }

  // Find from whom we might have got this publication. Last entry is our ID
  // and hence second last entry is the node from whom we get this publication
  std::optional<std::string> senderId;
  if (publication.nodeIds_ref().has_value() and
      publication.nodeIds_ref()->size()) {
    senderId = publication.nodeIds_ref()->back();
  }
  if (not publication.nodeIds_ref().has_value()) {
    publication.nodeIds_ref() = std::vector<std::string>{};
  }
  publication.nodeIds_ref()->emplace_back(kvParams_.nodeId);

  // Flood publication to internal subscribers
  kvParams_.kvStoreUpdatesQueue.push(publication);
  fb303::fbData->addStatValue("kvstore.num_updates", 1, fb303::COUNT);

  // Flood keyValue ONLY updates to external neighbors
  if (publication.keyVals.empty()) {
    return;
  }

  if (setFloodRoot and not senderId.has_value()) {
    // I'm the initiator, set flood-root-id
    fromStdOptional(publication.floodRootId_ref(), DualNode::getSptRootId());
  }

  // prepare thrift structure for flooding purpose
  thrift::KvStoreRequest floodRequest;
  thrift::KeySetParams params;

  params.keyVals = publication.keyVals;
  // TODO: remove solicit response when all KEY_SET request is over thrift
  params.solicitResponse = false;
  params.nodeIds_ref().copy_from(publication.nodeIds_ref());
  params.floodRootId_ref().copy_from(publication.floodRootId_ref());
  params.timestamp_ms_ref() = getUnixTimeStampMs();

  floodRequest.cmd = thrift::Command::KEY_SET;
  floodRequest.keySetParams_ref() = params;
  floodRequest.area = area_;

  std::optional<std::string> floodRootId{std::nullopt};
  if (params.floodRootId_ref().has_value()) {
    floodRootId = params.floodRootId_ref().value();
  }
  const auto& floodPeers = getFloodPeers(floodRootId);

  // ATTN: KvStore maintains different ways of flooding mechanism.
  //  1) Over thrift peer connection;
  //  2) Over ZMQ socket;
  if (kvParams_.enableKvStoreThrift) {
    for (const auto& peerName : floodPeers) {
      auto peerIt = thriftPeers_.find(peerName);
      if (peerIt == thriftPeers_.end()) {
        LOG(ERROR) << "Invalid flooding peer: " << peerName << ". Skip it.";
        continue;
      }

      if (senderId.has_value() && senderId.value() == peerName) {
        // Do not flood towards senderId from whom we received this publication
        continue;
      }

      auto& thriftPeer = thriftPeers_.at(peerName);
      if (thriftPeer.state == KvStorePeerState::IDLE or
          (not thriftPeer.client)) {
        // peer in thriftPeers collection can still be in IDLE state.
        // Skip flooding to those peers.
        continue;
      }

      // record telemetry for flooding publications
      fb303::fbData->addStatValue(
          "kvstore.thrift.num_flood_pub", 1, fb303::COUNT);
      fb303::fbData->addStatValue(
          "kvstore.thrift.num_flood_key_vals",
          publication.keyVals.size(),
          fb303::SUM);

      auto startTime = std::chrono::steady_clock::now();
      auto sf = thriftPeer.client->semifuture_setKvStoreKeyVals(params, area_);
      auto pendingReqId = ++pendingThriftId_;
      thriftFs_.emplace(
          pendingReqId,
          std::move(sf)
              .via(evb_->getEvb())
              .thenValue([this, peerName, startTime, pendingReqId](
                             folly::Unit&&) {
                VLOG(4) << "Flooding ack received from peer: " << peerName;

                auto endTime = std::chrono::steady_clock::now();
                auto timeDelta =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        endTime - startTime);

                // cleanup pendingReqId
                thriftFs_.erase(pendingReqId);

                // record telemetry for thrift calls
                fb303::fbData->addStatValue(
                    "kvstore.thrift.num_flood_pub_success", 1, fb303::COUNT);
                fb303::fbData->addStatValue(
                    "kvstore.thrift.flood_pub_duration_ms",
                    timeDelta.count(),
                    fb303::AVG);
              })
              .thenError([this, peerName, startTime, pendingReqId](
                             const folly::exception_wrapper& ew) {
                // state transition to IDLE
                auto endTime = std::chrono::steady_clock::now();
                auto timeDelta =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        endTime - startTime);
                processThriftFailure(peerName, ew.what(), timeDelta);

                // cleanup pendingReqId
                thriftFs_.erase(pendingReqId);

                // record telemetry for thrift calls
                fb303::fbData->addStatValue(
                    "kvstore.thrift.num_flood_pub_failure", 1, fb303::COUNT);
              }));
    }
  } else {
    for (const auto& peer : floodPeers) {
      if (senderId.has_value() && senderId.value() == peer) {
        // Do not flood towards senderId from whom we received this publication
        continue;
      }
      VLOG(4) << "Forwarding publication, received from: "
              << (senderId.has_value() ? senderId.value() : "N/A")
              << ", to: " << peer << ", via: " << kvParams_.nodeId;

      fb303::fbData->addStatValue("kvstore.sent_publications", 1, fb303::COUNT);
      fb303::fbData->addStatValue(
          "kvstore.sent_key_vals", publication.keyVals.size(), fb303::SUM);

      // Send flood request
      auto const& peerCmdSocketId = peers_.at(peer).second;
      auto const ret = sendMessageToPeer(peerCmdSocketId, floodRequest);
      if (ret.hasError()) {
        // this could be pretty common on initial connection setup
        LOG(ERROR) << "Failed to flood publication to peer " << peer
                   << " using id " << peerCmdSocketId
                   << ", error: " << ret.error();
        collectSendFailureStats(ret.error(), peerCmdSocketId);
      }
    }
  }
}

size_t
KvStoreDb::mergePublication(
    const thrift::Publication& rcvdPublication,
    std::optional<std::string> senderId) {
  // Add counters
  fb303::fbData->addStatValue("kvstore.received_publications", 1, fb303::COUNT);
  fb303::fbData->addStatValue(
      "kvstore.received_key_vals", rcvdPublication.keyVals.size(), fb303::SUM);

  const bool needFinalizeFullSync = senderId.has_value() and
      rcvdPublication.tobeUpdatedKeys_ref().has_value() and
      not rcvdPublication.tobeUpdatedKeys_ref()->empty();

  // This can happen when KvStore is emitting expired-key updates
  if (rcvdPublication.keyVals.empty() and not needFinalizeFullSync) {
    return 0;
  }

  // Check for loop
  const auto nodeIds = rcvdPublication.nodeIds_ref();
  if (nodeIds.has_value() and
      std::find(nodeIds->begin(), nodeIds->end(), kvParams_.nodeId) !=
          nodeIds->end()) {
    fb303::fbData->addStatValue("kvstore.looped_publications", 1, fb303::COUNT);
    return 0;
  }

  // Generate delta with local KvStore
  thrift::Publication deltaPublication;
  deltaPublication.keyVals = KvStore::mergeKeyValues(
      kvStore_, rcvdPublication.keyVals, kvParams_.filters);
  deltaPublication.floodRootId_ref().copy_from(
      rcvdPublication.floodRootId_ref());
  deltaPublication.area = area_;

  const size_t kvUpdateCnt = deltaPublication.keyVals.size();
  fb303::fbData->addStatValue(
      "kvstore.updated_key_vals", kvUpdateCnt, fb303::SUM);

  // Populate nodeIds and our nodeId_ to the end
  if (rcvdPublication.nodeIds_ref().has_value()) {
    deltaPublication.nodeIds_ref().copy_from(rcvdPublication.nodeIds_ref());
  }

  // Update ttl values of keys
  updateTtlCountdownQueue(deltaPublication);

  if (not deltaPublication.keyVals.empty()) {
    // Flood change to all of our neighbors/subscribers
    floodPublication(std::move(deltaPublication));
  } else {
    // Keep track of received publications which din't update any field
    fb303::fbData->addStatValue(
        "kvstore.received_redundant_publications", 1, fb303::COUNT);
  }

  // response to senderId with tobeUpdatedKeys + Vals
  // (last step in 3-way full-sync)
  if (needFinalizeFullSync) {
    finalizeFullSync(*rcvdPublication.tobeUpdatedKeys_ref(), *senderId);
  }

  return kvUpdateCnt;
}

void
KvStoreDb::logSyncEvent(
    const std::string& peerNodeName,
    const std::chrono::milliseconds syncDuration) {
  fbzmq::LogSample sample{};
  sample.addString("event", "KVSTORE_FULL_SYNC");
  sample.addString("node_name", kvParams_.nodeId);
  sample.addString("neighbor", peerNodeName);
  sample.addInt("duration_ms", syncDuration.count());

  fbzmq::thrift::EventLog eventLog;
  eventLog.category = Constants::kEventLogCategory.toString();
  eventLog.samples = {sample.toJson()};
  kvParams_.zmqMonitorClient->addEventLog(std::move(eventLog));
}

void
KvStoreDb::logKvEvent(const std::string& event, const std::string& key) {
  fbzmq::LogSample sample{};

  sample.addString("event", event);
  sample.addString("node_name", kvParams_.nodeId);
  sample.addString("key", key);

  fbzmq::thrift::EventLog eventLog;
  eventLog.category = Constants::kEventLogCategory.toString();
  eventLog.samples = {sample.toJson()};
  kvParams_.zmqMonitorClient->addEventLog(std::move(eventLog));
}

bool
KvStoreDb::sendDualMessages(
    const std::string& neighbor, const thrift::DualMessages& msgs) noexcept {
  if (peers_.count(neighbor) == 0) {
    LOG(ERROR) << "fail to send dual messages to " << neighbor << ", not exist";
    return false;
  }
  const auto& neighborCmdSocketId = peers_.at(neighbor).second;
  thrift::KvStoreRequest dualRequest;
  dualRequest.cmd = thrift::Command::DUAL;
  dualRequest.dualMessages_ref() = msgs;
  dualRequest.area = area_;
  const auto ret = sendMessageToPeer(neighborCmdSocketId, dualRequest);
  // NOTE: we rely on zmq (on top of tcp) to reliably deliver message,
  // if we switch to other protocols, we need to make sure its reliability.
  // Due to zmq async fashion, in case of failure (means the other side
  // is going down), it's ok to lose this pending message since later on,
  // neighor will inform us it's gone. and we will delete it from our dual
  // peers.
  if (ret.hasError()) {
    LOG(ERROR) << "failed to send dual messages to " << neighbor << " using id "
               << neighborCmdSocketId << ", error: " << ret.error();
    collectSendFailureStats(ret.error(), neighborCmdSocketId);
    return false;
  }
  return true;
}

} // namespace openr
