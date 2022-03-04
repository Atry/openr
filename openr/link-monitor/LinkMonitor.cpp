/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/logging/xlog.h>

#include <openr/common/Constants.h>
#include <openr/common/EventLogger.h>
#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Types.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/link-monitor/LinkMonitor.h>

namespace fb303 = facebook::fb303;

namespace {

const std::string kConfigKey{"link-monitor-config"};

/**
 * Transformation function to convert measured rtt (in us) to a metric value
 * to be used. Metric can never be zero.
 */
int32_t
getRttMetric(int64_t rttUs) {
  return std::max((int)(rttUs / 100), (int)1);
}

void
printLinkMonitorState(openr::thrift::LinkMonitorState const& state) {
  XLOG(DBG1) << "LinkMonitor state .... ";
  for (auto const& [area, nodeLabel] : *state.nodeLabelMap_ref()) {
    XLOG(DBG1) << "\tnodeLabel: " << nodeLabel << ", area: " << area;
  }
  XLOG(DBG1) << "\tisOverloaded: "
             << (*state.isOverloaded_ref() ? "true" : "false");
  if (not state.overloadedLinks_ref()->empty()) {
    XLOG(DBG1) << "\toverloadedLinks: "
               << folly::join(",", *state.overloadedLinks_ref());
  }
  if (not state.linkMetricOverrides_ref()->empty()) {
    XLOG(DBG1) << "\tlinkMetricOverrides: ";
    for (auto const& [key, val] : *state.linkMetricOverrides_ref()) {
      XLOG(DBG1) << "\t\t" << key << ": " << val;
    }
  }
}

} // anonymous namespace

namespace openr {

/*
 * NetlinkEventProcessor serves as the general processor struct to parse
 * and understand different types of netlink event LinkMonitor interested in.
 */
struct LinkMonitor::NetlinkEventProcessor {
  LinkMonitor& lm_;
  explicit NetlinkEventProcessor(LinkMonitor& lm) : lm_(lm) {}

  void
  operator()(fbnl::Link&& link) {
    lm_.processLinkEvent(std::move(link));
  }

  void
  operator()(fbnl::IfAddress&& addr) {
    lm_.processAddressEvent(std::move(addr));
  }

  void
  operator()(fbnl::Neighbor&&) {}

  void
  operator()(fbnl::Rule&&) {}
};

//
// LinkMonitor code
//
LinkMonitor::LinkMonitor(
    std::shared_ptr<const Config> config,
    fbnl::NetlinkProtocolSocket* nlSock,
    PersistentStore* configStore,
    messaging::ReplicateQueue<InterfaceDatabase>& interfaceUpdatesQueue,
    messaging::ReplicateQueue<PrefixEvent>& prefixUpdatesQueue,
    messaging::ReplicateQueue<PeerEvent>& peerUpdatesQueue,
    messaging::ReplicateQueue<LogSample>& logSampleQueue,
    messaging::ReplicateQueue<KeyValueRequest>& kvRequestQueue,
    messaging::RQueue<NeighborInitEvent> neighborUpdatesQueue,
    messaging::RQueue<KvStoreSyncEvent> kvStoreEventsQueue,
    messaging::RQueue<fbnl::NetlinkEvent> netlinkEventsQueue,
    bool overrideDrainState)
    : nodeId_(config->getNodeName()),
      enablePerfMeasurement_(
          *config->getLinkMonitorConfig().enable_perf_measurement_ref()),
      enableV4_(config->isV4Enabled()),
      enableSegmentRouting_(config->isSegmentRoutingEnabled()),
      enableNewGRBehavior_(config->isNewGRBehaviorEnabled()),
      prefixForwardingType_(*config->getConfig().prefix_forwarding_type_ref()),
      prefixForwardingAlgorithm_(
          *config->getConfig().prefix_forwarding_algorithm_ref()),
      useRttMetric_(*config->getLinkMonitorConfig().use_rtt_metric_ref()),
      linkflapInitBackoff_(std::chrono::milliseconds(
          *config->getLinkMonitorConfig().linkflap_initial_backoff_ms_ref())),
      linkflapMaxBackoff_(std::chrono::milliseconds(
          *config->getLinkMonitorConfig().linkflap_max_backoff_ms_ref())),
      areas_(config->getAreas()),
      enableOrderedAdjPublication_(
          *config->getConfig().enable_ordered_adj_publication_ref()),
      interfaceUpdatesQueue_(interfaceUpdatesQueue),
      prefixUpdatesQueue_(prefixUpdatesQueue),
      peerUpdatesQueue_(peerUpdatesQueue),
      logSampleQueue_(logSampleQueue),
      kvRequestQueue_(kvRequestQueue),
      expBackoff_(Constants::kInitialBackoff, Constants::kMaxBackoff),
      configStore_(configStore),
      nlSock_(nlSock) {
  // Check non-empty module ptr
  CHECK(configStore_);
  CHECK(nlSock_);

  // Hold time for synchronizing adjacencies in KvStore. We expect all the
  // adjacencies to be fully established within hold time after Open/R starts.
  // TODO: remove this with strict Open/R initialization sequence
  const std::chrono::seconds initialAdjHoldTime{
      *config->getConfig().adj_hold_time_s_ref()};

  // Schedule callback to advertise the initial set of adjacencies and prefixes
  adjHoldTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    XLOG(INFO) << "Hold time expired. Advertising adjacencies and addresses";
    // Advertise adjacencies and addresses after hold-timeout
    advertiseAdjacencies();
    advertiseRedistAddrs();
  });

  // Create throttled adjacency advertiser
  advertiseAdjacenciesThrottled_ = std::make_unique<AsyncThrottle>(
      getEvb(), Constants::kAdjacencyThrottleTimeout, [this]() noexcept {
        // will advertise to all areas but will not trigger a adj key update
        // if nothing changed.
        advertiseAdjacencies();
      });

  // Create throttled interfaces and addresses advertiser
  advertiseIfaceAddrThrottled_ = std::make_unique<AsyncThrottle>(
      getEvb(), Constants::kLinkThrottleTimeout, [this]() noexcept {
        advertiseIfaceAddr();
      });
  // Create timer. Timer is used for immediate or delayed executions.
  advertiseIfaceAddrTimer_ = folly::AsyncTimeout::make(
      *getEvb(), [this]() noexcept { advertiseIfaceAddr(); });

  // Create config-store client
  XLOG(INFO) << "Loading link-monitor state";
  auto state =
      configStore_->loadThriftObj<thrift::LinkMonitorState>(kConfigKey).get();
  // If assumeDrained is set, we will assume drained if no drain state
  // is found in the persitentStore
  auto assumeDrained = config->isAssumeDrained();
  if (state.hasValue()) {
    XLOG(INFO) << "Successfully loaded link-monitor state from disk.";
    state_ = state.value();
    printLinkMonitorState(state_);
  } else {
    XLOG(INFO) << fmt::format(
        "Failed to load link-monitor-state from disk. Setting node as {}",
        assumeDrained ? "DRAINED" : "UNDRAINED");
    state_.isOverloaded_ref() = assumeDrained;
  }

  // overrideDrainState provided, use assumeDrained
  if (overrideDrainState) {
    XLOG(INFO) << fmt::format(
        "Override node as {}", assumeDrained ? "DRAINED" : "UNDRAINED");
    state_.isOverloaded_ref() = assumeDrained;
  }

  if (enableSegmentRouting_) {
    // create range allocator to get unique node labels
    for (auto const& [areaId, areaCfg] : areas_) {
      auto const& srNodeLabelCfg = areaCfg.getNodeSegmentLabelConfig();
      if (not srNodeLabelCfg.has_value()) {
        XLOG(INFO) << fmt::format(
            "Area {} does not have segment rotuing node label config", areaId);
        continue;
      }

      CHECK(
          *srNodeLabelCfg->sr_node_label_type_ref() ==
          thrift::SegmentRoutingNodeLabelType::STATIC)
          << "Unknown segment routing node label allocation type";
      // Use statically configured node segment label as node label
      // state_.nodeLabel_ref() = getStaticNodeSegmentLabel(kv.second);
      auto nodeLbl = getStaticNodeSegmentLabel(areaCfg);
      state_.nodeLabelMap_ref()->insert_or_assign(areaId, nodeLbl);
      XLOG(INFO) << fmt::format(
          "Allocating static node segment label {} inside area {} for {}",
          nodeLbl,
          areaId,
          nodeId_);
    }
  }

  // start initial dump timer
  adjHoldTimer_->scheduleTimeout(initialAdjHoldTime);

  // Add fiber to process the neighbor events
  addFiberTask([q = std::move(neighborUpdatesQueue), this]() mutable noexcept {
    while (true) {
      auto maybeEvent = q.get();
      if (maybeEvent.hasError()) {
        XLOG(INFO) << "Terminating neighbor update processing fiber";
        break;
      }

      folly::variant_match(
          std::move(maybeEvent).value(),
          [this](NeighborEvents&& event) {
            // process different types of event
            processNeighborEvents(std::move(event));
          },
          [](thrift::InitializationEvent&& event) {
            CHECK(
                event == thrift::InitializationEvent::NEIGHBOR_DISCOVERED ||
                event == thrift::InitializationEvent::NEIGHBOR_DISCOVERY_ERROR)
                << fmt::format(
                       "Unexpected initialization event: {}",
                       apache::thrift::util::enumNameSafe(event));
            // TODO: Handle InitializationEvent
          });
    }
  });

  // Add fiber to process the LINK/ADDR events from platform
  addFiberTask([q = std::move(netlinkEventsQueue), this]() mutable noexcept {
    NetlinkEventProcessor visitor(*this);
    while (true) {
      auto maybeEvent = q.get();
      if (maybeEvent.hasError()) {
        XLOG(INFO) << "Terminating netlink events processing fiber";
        break;
      }
      std::visit(visitor, std::move(*maybeEvent));
    }
  });

  // Add fiber to process KvStore Sync events
  // TODO: remove this queue to reduce KvStore dependency
  addFiberTask([q = std::move(kvStoreEventsQueue), this]() mutable noexcept {
    while (true) {
      auto maybeEvent = q.get();
      if (maybeEvent.hasError()) {
        XLOG(INFO) << "Terminating kvstore events processing fiber";
        break;
      }
      // process different types of event
      processKvStoreSyncEvent(std::move(maybeEvent).value());
    }
  });

  // Add fiber to process interfaceDb syning from netlink platform
  addFiberTask([this]() mutable noexcept { syncInterfaceTask(); });

  // Initialize stats keys
  fb303::fbData->addStatExportType("link_monitor.neighbor_up", fb303::SUM);
  fb303::fbData->addStatExportType("link_monitor.neighbor_down", fb303::SUM);
  fb303::fbData->addStatExportType(
      "link_monitor.advertise_adjacencies", fb303::SUM);
  fb303::fbData->addStatExportType("link_monitor.advertise_links", fb303::SUM);
  fb303::fbData->addStatExportType(
      "link_monitor.sync_interface.failure", fb303::SUM);
}

void
LinkMonitor::stop() {
  // Send stop signal for internal fibers
  syncInterfaceStopSignal_.post();
  XLOG(INFO) << "Successfully posted stop signal for interface-syncing fiber";

  // Invoke stop method of super class
  OpenrEventBase::stop();
  XLOG(INFO) << "EventBase successfully stopped in LinkMonitor";
}

void
LinkMonitor::neighborUpEvent(
    const NeighborEvent& event, bool isGracefulRestart) {
  const auto& neighborAddrV4 = event.neighborAddrV4;
  const auto& neighborAddrV6 = event.neighborAddrV6;
  const auto& localIfName = event.localIfName;
  const auto& remoteIfName = event.remoteIfName;
  const auto& remoteNodeName = event.remoteNodeName;
  const auto& area = event.area;
  const auto kvStoreCmdPort = event.kvStoreCmdPort;
  const auto ctrlThriftPort = event.ctrlThriftPort;
  const auto rttUs = event.rttUs;
  const auto supportFloodOptimization = event.enableFloodOptimization;
  const auto onlyUsedByOtherNode = event.adjOnlyUsedByOtherNode;

  // current unixtime
  auto now = std::chrono::system_clock::now();
  int64_t timestamp =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();

  thrift::Adjacency newAdj = createThriftAdjacency(
      remoteNodeName /* neighbor node name */,
      localIfName /* local ifName neighbor discovered on */,
      toString(neighborAddrV6) /* nextHopV6 */,
      toString(neighborAddrV4) /* nextHopV4 */,
      useRttMetric_ ? getRttMetric(rttUs) : 1 /* metric */,
      0 /* adjacency-label */,
      false /* overload bit */,
      useRttMetric_ ? rttUs : 0 /* rtt */,
      timestamp,
      1 /* weight */,
      remoteIfName);

  SYSLOG(INFO)
      << EventTag() << "Neighbor " << remoteNodeName << " is up on interface "
      << localIfName << ". Remote Interface: " << remoteIfName
      << ", metric: " << *newAdj.metric_ref() << ", rttUs: " << rttUs
      << ", addrV4: " << toString(neighborAddrV4)
      << ", addrV6: " << toString(neighborAddrV6) << ", area: " << area
      << ", supportFloodOptimization: " << std::boolalpha
      << supportFloodOptimization << ", onlyUsedByOtherNode: " << std::boolalpha
      << onlyUsedByOtherNode;
  fb303::fbData->addStatValue("link_monitor.neighbor_up", 1, fb303::SUM);

  std::string repUrl{""};
  std::string peerAddr{""};
  if (!mockMode_) {
    // peer address used for KvStore external sync over ZMQ
    repUrl = fmt::format(
        "tcp://[{}%{}]:{}",
        toString(neighborAddrV6),
        localIfName,
        kvStoreCmdPort);
    // peer address used for KvStore external sync over thrift
    peerAddr = fmt::format("{}%{}", toString(neighborAddrV6), localIfName);
  } else {
    // use inproc address
    repUrl = fmt::format("inproc://{}-kvstore-cmd-global", remoteNodeName);
    // TODO: address value of peerAddr under system test environment
    peerAddr =
        fmt::format("{}%{}", Constants::kPlatformHost.toString(), localIfName);
  }

  CHECK(not repUrl.empty()) << "Got empty repUrl";
  CHECK(not peerAddr.empty()) << "Got empty peerAddr";

  const auto adjId = std::make_pair(remoteNodeName, localIfName);
  // If enableNewGRBehavior_, GR = neighbor restart -> kvstore initial sync.
  // We record GR status of old adj, KvStore Sync event will reset this field.
  // Else: GR = neighbor restart -> spark neighbor establishment.
  // We restart isRestarting flag to False here.
  //
  // TODO: remove `isRestarting` flag once enable_ordered_adj_publication is
  // fully rolled out to PROD.
  bool isRestarting{false};
  if (enableNewGRBehavior_) {
    const auto& areaAdjacencies = adjacencies_.find(area);
    if (areaAdjacencies != adjacencies_.end()) {
      const auto& oldAdj = areaAdjacencies->second.find(adjId);
      if (oldAdj != areaAdjacencies->second.end() and
          oldAdj->second.isRestarting) {
        isRestarting = true;
      }
    }
  }

  // NOTE: for Graceful Restart(GR) case, we don't expect any adjacency
  // information change. Ignore the `onlyUsedByOtherNode` flag for adjacency
  // advertisement.
  adjacencies_[area][adjId] = AdjacencyValue(
      area,
      createPeerSpec(
          repUrl,
          peerAddr,
          ctrlThriftPort,
          thrift::KvStorePeerState::IDLE,
          supportFloodOptimization),
      std::move(newAdj),
      useRttMetric_ ? getRttMetric(rttUs) : 1, // baseMetric
      isRestarting,
      isGracefulRestart ? false : onlyUsedByOtherNode);

  // update kvstore peer
  updateKvStorePeerNeighborUp(area, adjId, adjacencies_[area][adjId]);

  // Advertise new adjancies in a throttled fashion
  advertiseAdjacenciesThrottled_->operator()();
}

void
LinkMonitor::neighborAdjSyncedEvent(const NeighborEvent& event) {
  // DO NOT processing this event if feature is NOT activated
  if (not enableOrderedAdjPublication_) {
    return;
  }

  const auto& area = event.area;
  const auto& localIfName = event.localIfName;
  const auto& remoteNodeName = event.remoteNodeName;

  auto areaAdjIt = adjacencies_.find(area);
  if (areaAdjIt == adjacencies_.end()) {
    LOG(WARNING) << fmt::format(
        "Skip processing neighbor event due to no known adjacencies for area {}",
        area);
    return;
  }

  const auto adjId = std::make_pair(remoteNodeName, localIfName);
  auto adjIt = areaAdjIt->second.find(adjId);
  if (adjIt == areaAdjIt->second.end()) {
    LOG(WARNING) << fmt::format(
        "Skip processing neighbor event due to adjKey: [{}, {}] not found",
        remoteNodeName,
        localIfName);
    return;
  }

  LOG(INFO) << fmt::format(
      "[Initialization] Reset onlyUsedByOtherNode flag for adjKey: [{}, {}]",
      remoteNodeName,
      localIfName);

  // reset flag to indicate adjacency can be used by everyone
  adjIt->second.onlyUsedByOtherNode = false;

  // advertise new adjacencies in a throttled fashion
  advertiseAdjacenciesThrottled_->operator()();
}

void
LinkMonitor::neighborDownEvent(const NeighborEvent& event) {
  const auto& remoteNodeName = event.remoteNodeName;
  const auto& localIfName = event.localIfName;
  const auto& area = event.area;

  SYSLOG(INFO) << EventTag() << "Neighbor " << remoteNodeName
               << " is down on interface " << localIfName;
  fb303::fbData->addStatValue("link_monitor.neighbor_down", 1, fb303::SUM);

  auto areaAdjIt = adjacencies_.find(area);
  // No corresponding adj, ignore.
  if (areaAdjIt == adjacencies_.end()) {
    return;
  }

  const auto adjId = std::make_pair(remoteNodeName, localIfName);
  auto adjValueIt = areaAdjIt->second.find(adjId);
  // invalid adj, ignore
  if (adjValueIt == areaAdjIt->second.end()) {
    return;
  }

  // update KvStore Peer
  updateKvStorePeerNeighborDown(area, adjId, adjValueIt->second);

  // remove such adjacencies
  adjacencies_[area].erase(adjValueIt);

  // advertise adjacencies
  advertiseAdjacencies(area);
}

void
LinkMonitor::neighborRestartingEvent(const NeighborEvent& event) {
  const auto& remoteNodeName = event.remoteNodeName;
  const auto& localIfName = event.localIfName;
  const auto& area = event.area;

  SYSLOG(INFO) << EventTag() << "Neighbor " << remoteNodeName
               << " is restarting on interface " << localIfName;
  fb303::fbData->addStatValue(
      "link_monitor.neighbor_restarting", 1, fb303::SUM);

  auto areaAdjIt = adjacencies_.find(area);
  // invalid adj, ignore
  if (areaAdjIt == adjacencies_.end()) {
    return;
  }

  const auto adjId = std::make_pair(remoteNodeName, localIfName);
  auto adjValueIt = areaAdjIt->second.find(adjId);
  // invalid adj, ignore
  if (adjValueIt == areaAdjIt->second.end()) {
    return;
  }

  // update adjacencies_ restarting-bit and advertise peers
  adjValueIt->second.isRestarting = true;

  // update KvStore Peer
  updateKvStorePeerNeighborDown(area, adjId, adjValueIt->second);
}

void
LinkMonitor::neighborRttChangeEvent(const NeighborEvent& event) {
  const auto& remoteNodeName = event.remoteNodeName;
  const auto& localIfName = event.localIfName;
  const auto& rttUs = event.rttUs;
  int32_t newRttMetric = getRttMetric(rttUs);
  const auto& area = event.area;

  XLOG(DBG1) << "Metric value changed for neighbor " << remoteNodeName
             << " on interface: " << localIfName << " to " << newRttMetric;

  auto areaAdjIt = adjacencies_.find(area);
  if (areaAdjIt != adjacencies_.end()) {
    auto it = areaAdjIt->second.find({remoteNodeName, localIfName});
    if (it != areaAdjIt->second.end()) {
      auto& adj = it->second.adjacency;
      adj.metric_ref() = newRttMetric;
      adj.rtt_ref() = rttUs;
      advertiseAdjacenciesThrottled_->operator()();
    }
  }
}

void
LinkMonitor::processKvStoreSyncEvent(KvStoreSyncEvent&& event) {
  const auto& nodeName = event.nodeName;
  const auto& area = event.area;

  const auto& areaPeers = peers_.find(area);
  // ignore invalid initial sync events
  if (areaPeers == peers_.end()) {
    return;
  }

  const auto& peerVal = areaPeers->second.find(nodeName);
  // spark neighbor down events erased this peer, nothing to do
  if (peerVal == areaPeers->second.end()) {
    return;
  }

  // parallel link caused KvStore Peer session re-establishment
  // no need to refresh initialSynced state.
  if (peerVal->second.initialSynced) {
    return;
  }

  // set initialSynced = true, promote neighbor's adj up events
  peerVal->second.initialSynced = true;

  SYSLOG(INFO) << "Neighbor " << nodeName << " finished Initial Sync "
               << ", area: " << area << ". Promoting Adjacency UP events.";

  // update adjacency status
  for (const auto& adjId : peerVal->second.establishedSparkNeighbors) {
    auto areaAdjIt = adjacencies_.find(area);
    if (areaAdjIt != adjacencies_.end()) {
      auto it = areaAdjIt->second.find(adjId);
      if (it != areaAdjIt->second.end()) {
        // kvstore sync is done, exit GR mode
        if (it->second.isRestarting) {
          XLOG(INFO) << "Neighbor " << adjId.first << " on interface "
                     << adjId.second << " exiting GR successfully";
          it->second.isRestarting = false;
        }
      }
    }
  }

  advertiseAdjacenciesThrottled_->operator()();
}

void
LinkMonitor::updateKvStorePeerNeighborUp(
    const std::string& area,
    const AdjacencyKey& adjId,
    const AdjacencyValue& adjVal) {
  const auto& remoteNodeName = adjId.first;

  // update kvstore peers
  auto areaPeers = peers_.find(area);
  if (areaPeers == peers_.end()) {
    areaPeers =
        peers_
            .emplace(area, std::unordered_map<std::string, KvStorePeerValue>())
            .first;
  }

  auto peerVal = areaPeers->second.find(remoteNodeName);
  // kvstore peer exists, no need to refresh KvStore session
  if (peerVal != areaPeers->second.end()) {
    // update established adjs
    peerVal->second.establishedSparkNeighbors.emplace(adjId);
    return;
  }

  // if not enableNewGRBehavior_, set initialSynced = true to promote adj up
  // event immediately
  bool initialSynced = enableNewGRBehavior_ ? false : true;

  // create new KvStore Peer struct if it's first adj up
  areaPeers->second.emplace(
      remoteNodeName,
      KvStorePeerValue(adjVal.peerSpec, initialSynced, {adjId}));

  // Do not publish incremental peer event before initial peers are received and
  // published.
  if (not initialNeighborsReceived_) {
    return;
  }

  // Advertise KvStore peers immediately
  thrift::PeersMap peersToAdd;
  peersToAdd.emplace(remoteNodeName, adjVal.peerSpec);
  logPeerEvent("ADD_PEER", remoteNodeName, adjVal.peerSpec);

  PeerEvent event;
  event.emplace(area, AreaPeerEvent(peersToAdd, {} /*peersToDel*/));
  peerUpdatesQueue_.push(std::move(event));
}

void
LinkMonitor::updateKvStorePeerNeighborDown(
    const std::string& area,
    const AdjacencyKey& adjId,
    const AdjacencyValue& adjVal) {
  const auto& remoteNodeName = adjId.first;

  // find kvstore peer for adj
  const auto& areaPeers = peers_.find(area);
  if (areaPeers == peers_.end()) {
    XLOG(WARNING) << "No previous established KvStorePeer found for neighbor "
                  << remoteNodeName
                  << ". Skip updateKvStorePeer for interface down event on "
                  << adjId.second;
    return;
  }
  const auto& peerVal = areaPeers->second.find(remoteNodeName);
  if (peerVal == areaPeers->second.end()) {
    XLOG(WARNING) << "No previous established KvStorePeer found for neighbor "
                  << remoteNodeName
                  << ". Skip updateKvStorePeer for interface down event on "
                  << adjId.second;
    return;
  }

  // get handler of peer to update internal fields
  auto& peer = peerVal->second;

  // remove neighbor from establishedSparkNeighbors list
  peer.establishedSparkNeighbors.erase(adjId);

  // send peer delete request if all spark session is down for this neighbor
  if (peer.establishedSparkNeighbors.empty()) {
    logPeerEvent("DEL_PEER", remoteNodeName, peer.tPeerSpec);

    // send peer del event
    std::vector<std::string> peersToDel{remoteNodeName};

    PeerEvent event;
    event.emplace(area, AreaPeerEvent({} /* peersToAdd */, peersToDel));
    peerUpdatesQueue_.push(std::move(event));

    // remove kvstore peer from internal store.
    areaPeers->second.erase(remoteNodeName);
    return;
  }

  // If current KvStore tPeerSpec != this sparkNeighbor's peerSpec, no need to
  // update peer spec, we are done.
  if (adjVal.peerSpec != peer.tPeerSpec) {
    return;
  }

  // Update tPeerSpec to peerSpec in remaining establishedSparkNeighbors.
  // e.g. adj_1 up -> adj_1 peer spec is used in KvStore Peer
  //      adj_2 up -> peer spec does not change
  //      adj_1 down -> Now adj_2 will be the peer-spec being used to establish
  peer.tPeerSpec = adjacencies_.at(area)
                       .at(*peer.establishedSparkNeighbors.begin())
                       .peerSpec;

  // peer spec change, send peer add event
  logPeerEvent("ADD_PEER", remoteNodeName, peer.tPeerSpec);

  thrift::PeersMap peersToAdd;
  peersToAdd.emplace(remoteNodeName, peer.tPeerSpec);
  PeerEvent event;
  event.emplace(area, AreaPeerEvent(peersToAdd, {} /* peersToDel */));
  peerUpdatesQueue_.push(std::move(event));
}

void
LinkMonitor::advertiseAdjacencies(const std::string& area) {
  if (adjHoldTimer_->isScheduled()) {
    return;
  }

  // Cancel throttle timeout if scheduled
  if (advertiseAdjacenciesThrottled_->isActive()) {
    advertiseAdjacenciesThrottled_->cancel();
  }

  // Extract information from `adjacencies_`
  auto adjDb = buildAdjacencyDatabase(area);

  XLOG(INFO) << fmt::format(
      "Updating adjacency database in KvStore with {} entries in area: {}",
      adjDb.adjacencies_ref()->size(),
      area);

  // Persist `adj:node_Id` key into KvStore
  const auto keyName = Constants::kAdjDbMarker.toString() + nodeId_;
  std::string adjDbStr = writeThriftObjStr(adjDb, serializer_);
  auto persistAdjacencyKeyVal =
      PersistKeyValueRequest(AreaId{area}, keyName, adjDbStr);
  kvRequestQueue_.push(std::move(persistAdjacencyKeyVal));

  // Config is most likely to have changed. Update it in `ConfigStore`
  configStore_->storeThriftObj(kConfigKey, state_); // not awaiting on result

  // Update some flat counters
  fb303::fbData->addStatValue(
      "link_monitor.advertise_adjacencies", 1, fb303::SUM);
  fb303::fbData->setCounter("link_monitor.adjacencies", getTotalAdjacencies());
  for (const auto& [_, areaAdjacencies] : adjacencies_) {
    for (const auto& [_, adjValue] : areaAdjacencies) {
      auto& adj = adjValue.adjacency;
      fb303::fbData->setCounter(
          "link_monitor.metric." + *adj.otherNodeName_ref(), *adj.metric_ref());
    }
  }
}
void
LinkMonitor::advertiseAdjacencies() {
  // advertise to all areas. Once area configuration per link is implemented
  // then adjacencies can be advertised to a specific area
  for (const auto& [areaId, _] : areas_) {
    // Update KvStore
    advertiseAdjacencies(areaId);
  }
}

void
LinkMonitor::advertiseIfaceAddr() {
  auto retryTime = getRetryTimeOnUnstableInterfaces();

  advertiseInterfaces();
  advertiseRedistAddrs();

  // Cancel throttle timeout if scheduled
  if (advertiseIfaceAddrThrottled_->isActive()) {
    advertiseIfaceAddrThrottled_->cancel();
  }

  // Schedule new timeout if needed to advertise UP but UNSTABLE interfaces
  // once their backoff is clear.
  if (retryTime.count() != 0) {
    advertiseIfaceAddrTimer_->scheduleTimeout(retryTime);
    XLOG(DBG2) << "advertiseIfaceAddr timer scheduled in " << retryTime.count()
               << " ms";
  }
}

void
LinkMonitor::advertiseInterfaces() {
  fb303::fbData->addStatValue("link_monitor.advertise_links", 1, fb303::SUM);

  // Create interface database
  InterfaceDatabase ifDb;
  for (auto& [_, interface] : interfaces_) {
    // Perform regex match
    if (not anyAreaShouldDiscoverOnIface(interface.getIfName())) {
      continue;
    }
    // Transform to `InterfaceInfo` object
    auto interfaceInfo = interface.getInterfaceInfo();

    // Override `UP` status
    interfaceInfo.isUp = interface.isActive();

    // Construct `InterfaceDatabase` object
    ifDb.emplace_back(std::move(interfaceInfo));
  }

  // publish via replicate queue
  interfaceUpdatesQueue_.push(std::move(ifDb));
}

void
LinkMonitor::advertiseRedistAddrs() {
  std::map<folly::CIDRNetwork, std::vector<std::string>> prefixesToAdvertise;
  std::unordered_map<folly::CIDRNetwork, thrift::PrefixEntry> prefixMap;

  // Add redistribute addresses
  for (auto& [_, interface] : interfaces_) {
    // Ignore in-active interfaces
    if (not interface.isActive()) {
      XLOG(DBG1) << "Interface: " << interface.getIfName()
                 << " is NOT active. Skip advertising.";
      continue;
    }

    // Derive list of area to advertise (NOTE: areas are ordered persistently)
    std::vector<std::string> dstAreas;
    for (auto const& [areaId, areaConf] : areas_) {
      if (areaConf.shouldRedistributeIface(interface.getIfName())) {
        dstAreas.emplace_back(areaId);
      }
    }

    // Do not advertise interface addresses if no destination area qualifies
    if (dstAreas.empty()) {
      continue;
    }

    // Add all prefixes of this interface
    for (auto& prefix : interface.getGlobalUnicastNetworks(enableV4_)) {
      // Add prefix in the cache
      prefixesToAdvertise.emplace(prefix, dstAreas);

      // Create prefix entry and populate the
      thrift::PrefixEntry prefixEntry;
      prefixEntry.prefix_ref() = toIpPrefix(prefix);
      prefixEntry.type_ref() = thrift::PrefixType::LOOPBACK;

      // Forwarding information
      prefixEntry.forwardingType_ref() = prefixForwardingType_;
      prefixEntry.forwardingAlgorithm_ref() = prefixForwardingAlgorithm_;

      // Tags
      {
        auto& tags = prefixEntry.tags_ref().value();
        tags.emplace("INTERFACE_SUBNET");
        tags.emplace(fmt::format("{}:{}", nodeId_, interface.getIfName()));
      }
      // Metrics
      {
        auto& metrics = prefixEntry.metrics_ref().value();
        metrics.path_preference_ref() = Constants::kDefaultPathPreference;
        metrics.source_preference_ref() = Constants::kDefaultSourcePreference;
      }

      prefixMap.emplace(prefix, std::move(prefixEntry));
    }
  }

  // Find prefixes to advertise or update
  std::map<std::vector<std::string>, std::vector<thrift::PrefixEntry>>
      toAdvertise;
  for (auto const& [prefix, areas] : prefixesToAdvertise) {
    toAdvertise[areas].emplace_back(std::move(prefixMap.at(prefix)));

    XLOG(DBG1) << fmt::format(
        "Advertise LOOPBACK prefix: {} within areas: [{}]",
        folly::IPAddress::networkToString(prefix),
        folly::join(",", areas));
  }

  // Find prefixes to withdraw
  std::vector<thrift::PrefixEntry> toWithdraw;
  for (auto const& [prefix, areas] : advertisedPrefixes_) {
    if (prefixesToAdvertise.count(prefix)) {
      continue; // Do not mark for withdraw
    }
    thrift::PrefixEntry prefixEntry;
    prefixEntry.prefix_ref() = toIpPrefix(prefix);
    prefixEntry.type_ref() = thrift::PrefixType::LOOPBACK;
    toWithdraw.emplace_back(std::move(prefixEntry));

    XLOG(DBG1) << fmt::format(
        "Withdraw LOOPBACK prefix: {} within areas: [{}]",
        folly::IPAddress::networkToString(prefix),
        folly::join(",", areas));
  }

  // Advertise prefixes (one for each area)
  for (auto& [areas, prefixEntries] : toAdvertise) {
    PrefixEvent event(
        PrefixEventType::ADD_PREFIXES,
        thrift::PrefixType::LOOPBACK,
        std::move(prefixEntries),
        std::unordered_set<std::string>(areas.begin(), areas.end()));
    prefixUpdatesQueue_.push(std::move(event));
  }

  // Withdraw prefixes
  {
    PrefixEvent event(
        PrefixEventType::WITHDRAW_PREFIXES,
        thrift::PrefixType::LOOPBACK,
        std::move(toWithdraw));
    prefixUpdatesQueue_.push(std::move(event));
  }

  // Store advertised prefixes locally
  advertisedPrefixes_.swap(prefixesToAdvertise);
}

std::chrono::milliseconds
LinkMonitor::getRetryTimeOnUnstableInterfaces() {
  std::chrono::milliseconds minRemainMs{0};
  for (auto& [_, interface] : interfaces_) {
    if (interface.isActive()) {
      continue;
    }

    const auto& curRemainMs = interface.getBackoffDuration();
    if (curRemainMs.count() > 0) {
      XLOG(DBG2) << "Interface " << interface.getIfName()
                 << " is in backoff state for " << curRemainMs.count() << "ms";
      minRemainMs = std::min(linkflapMaxBackoff_, curRemainMs);
    }
  }

  return minRemainMs;
}

bool
LinkMonitor::shouldSkipAdjAnnouncement(
    const AdjacencyKey& adjKey, const AdjacencyValue& adjVal) {
  // TODO: once `enable_ordered_adj_publication` is enabled everywhere, the
  // logic to skip adjacency announcement can be removed.
  if (enableOrderedAdjPublication_) {
    return false;
  }

  // ignore adjs that are waiting first KvStore full sync
  bool waitingInitialSync{true};

  const auto& areaPeers = peers_.find(adjVal.area);
  if (areaPeers != peers_.end()) {
    const auto& peerVal = areaPeers->second.find(adjKey.first);
    // set waitingInitialSync false if peer has reached initial sync state
    if (peerVal != areaPeers->second.end() && peerVal->second.initialSynced) {
      waitingInitialSync = false;
    }
  }

  // If adj is not in GR and it's waiting for kvstore sync,
  // skip announcement
  if (not adjVal.isRestarting && waitingInitialSync) {
    return true;
  }
  return false;
}

thrift::AdjacencyDatabase
LinkMonitor::buildAdjacencyDatabase(const std::string& area) {
  // prepare adjacency database
  thrift::AdjacencyDatabase adjDb;

  adjDb.thisNodeName_ref() = nodeId_;
  adjDb.isOverloaded_ref() = *state_.isOverloaded_ref();
  adjDb.area_ref() = area;
  adjDb.nodeLabel_ref() = 0;
  if (enableSegmentRouting_) {
    auto it = state_.nodeLabelMap_ref()->find(area);
    if (it != state_.nodeLabelMap_ref()->end()) {
      adjDb.nodeLabel_ref() = it->second;
    }
  }

  // populate thrift::AdjacencyDatabase.adjacencies based on
  // various condition.
  auto areaAdjIt = adjacencies_.find(area);
  if (areaAdjIt != adjacencies_.end()) {
    for (auto& [adjKey, adjValue] : areaAdjIt->second) {
      if (shouldSkipAdjAnnouncement(adjKey, adjValue)) {
        LOG(INFO) << fmt::format(
            "Skip announcement of adjKey: [{}, {}] without initial sync.",
            adjKey.first,
            adjKey.second);
        continue;
      }

      // NOTE: copy on purpose
      auto adj = folly::copy(adjValue.adjacency);

      // set link overload bit
      adj.isOverloaded_ref() =
          state_.overloadedLinks_ref()->count(*adj.ifName_ref()) > 0;

      // Calculate the adj metric - there are three types of metric, which can
      // be potentially combined:
      // 1. base metric derived from RTT or default hop-count metric.
      //    ATTN: link-metirc/adj-metric override can ONLY override base metric,
      //          and adj-metric override can overrice link-metric override.
      // 2. node-level incremental metric;
      // 3. link-level incremental metric.
      int32_t metric = adjValue.baseMetric;

      // override metric with link metric if it exists
      metric = folly::get_default(
          *state_.linkMetricOverrides_ref(),
          *adj.ifName_ref(),
          adjValue.baseMetric);

      // override metric with adj metric if it exists
      thrift::AdjKey tAdjKey;
      tAdjKey.nodeName_ref() = *adj.otherNodeName_ref();
      tAdjKey.ifName_ref() = *adj.ifName_ref();
      metric =
          folly::get_default(*state_.adjMetricOverrides_ref(), tAdjKey, metric);

      // increment the node-level metric
      metric += *state_.nodeMetricIncrementVal_ref();

      // increment the link-level metric
      if (state_.linkMetiricIncrementMap_ref()->count(*adj.ifName_ref())) {
        metric += state_.linkMetiricIncrementMap_ref()[*adj.ifName_ref()];
      }

      adj.metric_ref() = metric;

      // set flag to indicate if adjacency will ONLY be used by other node
      adj.adjOnlyUsedByOtherNode_ref() = adjValue.onlyUsedByOtherNode;

      adjDb.adjacencies_ref()->emplace_back(std::move(adj));
    }
  }

  // Add perf information if enabled
  if (enablePerfMeasurement_) {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, nodeId_, "ADJ_DB_UPDATED");
    adjDb.perfEvents_ref() = perfEvents;
  } else {
    DCHECK(!adjDb.perfEvents_ref().has_value());
  }

  return adjDb;
}

InterfaceEntry* FOLLY_NULLABLE
LinkMonitor::getOrCreateInterfaceEntry(const std::string& ifName) {
  // Return null if ifName doesn't quality regex match criteria
  if (not anyAreaShouldDiscoverOnIface(ifName) &&
      not anyAreaShouldRedistributeIface(ifName)) {
    return nullptr;
  }

  // Return existing element if any
  auto it = interfaces_.find(ifName);
  if (it != interfaces_.end()) {
    return &(it->second);
  }

  // Create one and return it's reference
  auto res = interfaces_.emplace(
      ifName,
      InterfaceEntry(
          ifName,
          linkflapInitBackoff_,
          linkflapMaxBackoff_,
          *advertiseIfaceAddrThrottled_,
          *advertiseIfaceAddrTimer_));

  return &(res.first->second);
}

void
LinkMonitor::syncInterfaceTask() noexcept {
  XLOG(INFO) << "[Interface Sync] Starting interface syncing fiber task";

  // ATTN: use initial timeoff as the default value to wait for
  // small amount of time when thread starts before syncing
  std::chrono::milliseconds timeout{expBackoff_.getInitialBackoff()};

  while (true) { // Break when stop signal is ready
    // Sleep before next check
    if (syncInterfaceStopSignal_.try_wait_for(timeout)) {
      break; // Baton was posted
    } else {
      syncInterfaceStopSignal_.reset(); // Baton experienced timeout
    }

    auto success = syncInterfaces();
    if (success) {
      expBackoff_.reportSuccess();
      timeout = std::chrono::milliseconds(Constants::kPlatformSyncInterval);

      XLOG(DBG2) << fmt::format(
          "[Interface Sync] Successfully synced interfaceDb. Schedule next sync in {}ms",
          timeout.count());
    } else {
      // Apply exponential backoff and schedule next run
      expBackoff_.reportError();
      timeout = expBackoff_.getTimeRemainingUntilRetry();

      fb303::fbData->addStatValue(
          "link_monitor.sync_interface.failure", 1, fb303::SUM);

      XLOG(ERR) << fmt::format(
          "[Interface Sync] Failed to sync interfaceDb, apply exp backoff and retry in {}ms",
          timeout.count());
    }
  } // while

  XLOG(INFO) << "[Interface Sync] Interface-syncing fiber task got stopped.";
}

bool
LinkMonitor::syncInterfaces() {
  // Retrieve latest link snapshot from NetlinkProtocolSocket
  folly::Try<InterfaceDatabase> maybeIfDb;
  try {
    maybeIfDb = semifuture_getAllLinks().getTry(Constants::kReadTimeout);
    if (not maybeIfDb.hasValue()) {
      XLOG(ERR) << fmt::format(
          "[Interface Sync] Failed to sync interfaceDb. Exception: {}",
          folly::exceptionStr(maybeIfDb.exception()));

      return false;
    }
  } catch (const folly::FutureTimeout&) {
    XLOG(ERR)
        << "[Interface Sync] Timeout retrieving links. Retry in a moment.";

    return false;
  }

  // ATTN: treat empty link as failure to make sure LinkMonitor can keep
  // retrying to retrieve data from underneath platform.
  InterfaceDatabase ifDb = maybeIfDb.value();
  if (ifDb.empty()) {
    XLOG(ERR) << "[Interface Sync] No interface found. Retry in a moment.";
    return false;
  }

  XLOG(INFO) << fmt::format(
      "[Interface Sync] Successfully retrieved {} links from netlink.",
      ifDb.size());

  // Make updates in InterfaceEntry objects
  for (const auto& info : ifDb) {
    // update cache of ifIndex -> ifName mapping
    //  1) if ifIndex exists, override it with new ifName;
    //  2) if ifIndex does NOT exist, cache the ifName;
    ifIndexToName_[info.ifIndex] = info.ifName;

    // Get interface entry
    auto interfaceEntry = getOrCreateInterfaceEntry(info.ifName);
    if (not interfaceEntry) {
      continue;
    }

    const auto oldNetworks =
        interfaceEntry->getNetworks(); // NOTE: Copy intended
    const auto& newNetworks = info.networks;

    // Update link attributes
    const bool wasUp = interfaceEntry->isUp();
    interfaceEntry->updateAttrs(info.ifIndex, info.isUp);

    // Event logging
    logLinkEvent(
        interfaceEntry->getIfName(),
        wasUp,
        interfaceEntry->isUp(),
        interfaceEntry->getBackoffDuration());

    // Remove old addresses if they are not in new
    for (auto const& oldNetwork : oldNetworks) {
      if (newNetworks.count(oldNetwork) == 0) {
        interfaceEntry->updateAddr(oldNetwork, false);
      }
    }

    // Add new addresses if they are not in old
    for (auto const& newNetwork : newNetworks) {
      if (oldNetworks.count(newNetwork) == 0) {
        interfaceEntry->updateAddr(newNetwork, true);
      }
    }
  }
  return true;
}

void
LinkMonitor::processLinkEvent(fbnl::Link&& link) {
  XLOG(DBG3) << "Received Link Event from NetlinkProtocolSocket...";

  auto ifName = link.getLinkName();
  auto ifIndex = link.getIfIndex();
  auto isUp = link.isUp();

  // Cache interface index name mapping
  // ATTN: will create new ifIndex -> ifName mapping if it is unknown link
  //       `[]` operator is used in purpose
  ifIndexToName_[ifIndex] = ifName;

  auto interfaceEntry = getOrCreateInterfaceEntry(ifName);
  if (interfaceEntry) {
    const bool wasUp = interfaceEntry->isUp();
    interfaceEntry->updateAttrs(ifIndex, isUp);
    logLinkEvent(
        interfaceEntry->getIfName(),
        wasUp,
        interfaceEntry->isUp(),
        interfaceEntry->getBackoffDuration());
  }
}

void
LinkMonitor::processAddressEvent(fbnl::IfAddress&& addr) {
  XLOG(DBG3) << "Received Address Event from NetlinkProtocolSocket...";

  auto ifIndex = addr.getIfIndex();
  auto prefix = addr.getPrefix(); // std::optional<folly::CIDRNetwork>
  auto isValid = addr.isValid();

  // Check for interface name
  auto it = ifIndexToName_.find(ifIndex);
  if (it == ifIndexToName_.end()) {
    XLOG(ERR)
        << fmt::format("Address event for unknown iface index: {}", ifIndex);
    return;
  }

  // Cached ifIndex -> ifName mapping
  auto interfaceEntry = getOrCreateInterfaceEntry(it->second);
  if (interfaceEntry) {
    interfaceEntry->updateAddr(prefix.value(), isValid);
  }
}

void
LinkMonitor::processNeighborEvents(NeighborEvents&& events) {
  for (const auto& event : events) {
    const auto& neighborAddrV4 = event.neighborAddrV4;
    const auto& neighborAddrV6 = event.neighborAddrV6;
    const auto& localIfName = event.localIfName;
    const auto& remoteIfName = event.remoteIfName;
    const auto& remoteNodeName = event.remoteNodeName;
    const auto& area = event.area;

    XLOG(DBG1) << "Received neighbor event for " << remoteNodeName << " from "
               << remoteIfName << " at " << localIfName << " with addrs "
               << toString(neighborAddrV6) << " and "
               << (enableV4_ ? toString(neighborAddrV4) : "")
               << " Area:" << area
               << " Event Type: " << toString(event.eventType);

    switch (event.eventType) {
    case NeighborEventType::NEIGHBOR_UP:
      logNeighborEvent(event);
      neighborUpEvent(event, false);
      break;
    case NeighborEventType::NEIGHBOR_RESTARTED: {
      logNeighborEvent(event);
      neighborUpEvent(event, true);
      break;
    }
    case NeighborEventType::NEIGHBOR_ADJ_SYNCED: {
      logNeighborEvent(event);
      neighborAdjSyncedEvent(event);
      break;
    }
    case NeighborEventType::NEIGHBOR_RESTARTING: {
      CHECK(initialNeighborsReceived_);
      logNeighborEvent(event);
      neighborRestartingEvent(event);
      break;
    }
    case NeighborEventType::NEIGHBOR_DOWN: {
      CHECK(initialNeighborsReceived_);
      logNeighborEvent(event);
      neighborDownEvent(event);
      break;
    }
    case NeighborEventType::NEIGHBOR_RTT_CHANGE: {
      CHECK(initialNeighborsReceived_);
      if (!useRttMetric_) {
        break;
      }
      logNeighborEvent(event);
      neighborRttChangeEvent(event);
      break;
    }
    default:
      XLOG(ERR) << "Unknown event type " << (int32_t)event.eventType;
    }
  } // for

  // Publish all peers to KvStore in OpenR initialization procedure.
  if (not initialNeighborsReceived_) {
    PeerEvent event;
    for (auto& [area, areaPeers] : peers_) {
      // Get added peers in each area.
      thrift::PeersMap peersToAdd;
      for (auto& [remoteNodeName, peerVal] : areaPeers) {
        peersToAdd.emplace(remoteNodeName, peerVal.tPeerSpec);
        logPeerEvent("ADD_PEER", remoteNodeName, peerVal.tPeerSpec);
      }
      event.emplace(area, AreaPeerEvent(peersToAdd, {} /* peersToDel */));
    }
    // Send peers to add in all areas in a batch.
    peerUpdatesQueue_.push(std::move(event));

    initialNeighborsReceived_ = true;
    logInitializationEvent(
        "LinkMonitor", thrift::InitializationEvent::LINK_DISCOVERED);
  }
}

// NOTE: add commands which set/unset overload bit or metric values will
// immediately advertise new adjacencies into the KvStore.
folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_setNodeOverload(bool isOverloaded) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), isOverloaded]() mutable {
    std::string cmd =
        isOverloaded ? "SET_NODE_OVERLOAD" : "UNSET_NODE_OVERLOAD";
    if (*state_.isOverloaded_ref() == isOverloaded) {
      XLOG(INFO) << "Skip cmd: [" << cmd << "]. Node already in target state: ["
                 << (isOverloaded ? "OVERLOADED" : "NOT OVERLOADED") << "]";
    } else {
      state_.isOverloaded_ref() = isOverloaded;
      SYSLOG(INFO) << EventTag() << (isOverloaded ? "Setting" : "Unsetting")
                   << " overload bit for node";
      advertiseAdjacencies();
    }
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_setInterfaceOverload(
    std::string interfaceName, bool isOverloaded) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        interfaceName,
                        isOverloaded]() mutable {
    std::string cmd =
        isOverloaded ? "SET_LINK_OVERLOAD" : "UNSET_LINK_OVERLOAD";
    if (0 == interfaces_.count(interfaceName)) {
      XLOG(ERR) << "Skip cmd: [" << cmd
                << "] due to unknown interface: " << interfaceName;
      p.setValue();
      return;
    }

    if (isOverloaded && state_.overloadedLinks_ref()->count(interfaceName)) {
      XLOG(INFO) << "Skip cmd: [" << cmd << "]. Interface: " << interfaceName
                 << " is already overloaded";
      p.setValue();
      return;
    }

    if (!isOverloaded && !state_.overloadedLinks_ref()->count(interfaceName)) {
      XLOG(INFO) << "Skip cmd: [" << cmd << "]. Interface: " << interfaceName
                 << " is currently NOT overloaded";
      p.setValue();
      return;
    }

    if (isOverloaded) {
      state_.overloadedLinks_ref()->insert(interfaceName);
      SYSLOG(INFO) << EventTag() << "Setting overload bit for interface "
                   << interfaceName;
    } else {
      state_.overloadedLinks_ref()->erase(interfaceName);
      SYSLOG(INFO) << EventTag() << "Unsetting overload bit for interface "
                   << interfaceName;
    }
    advertiseAdjacenciesThrottled_->operator()();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_setLinkMetric(
    std::string interfaceName, std::optional<int32_t> overrideMetric) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), interfaceName, overrideMetric]() mutable {
        std::string cmd = overrideMetric.has_value() ? "SET_LINK_METRIC"
                                                     : "UNSET_LINK_METRIC";
        if (0 == interfaces_.count(interfaceName)) {
          XLOG(ERR) << "Skip cmd: [" << cmd
                    << "] due to unknown interface: " << interfaceName;
          p.setValue();
          return;
        }

        if (overrideMetric.has_value() &&
            state_.linkMetricOverrides_ref()->count(interfaceName) &&
            state_.linkMetricOverrides_ref()[interfaceName] ==
                overrideMetric.value()) {
          XLOG(INFO) << "Skip cmd: " << cmd
                     << ". Overridden metric: " << overrideMetric.value()
                     << " already set for interface: " << interfaceName;
          p.setValue();
          return;
        }

        if (!overrideMetric.has_value() &&
            !state_.linkMetricOverrides_ref()->count(interfaceName)) {
          XLOG(INFO) << "Skip cmd: " << cmd
                     << ". No overridden metric found for interface: "
                     << interfaceName;
          p.setValue();
          return;
        }

        if (overrideMetric.has_value()) {
          state_.linkMetricOverrides_ref()[interfaceName] =
              overrideMetric.value();
          SYSLOG(INFO) << "Overriding metric for interface " << interfaceName
                       << " to " << overrideMetric.value();
        } else {
          state_.linkMetricOverrides_ref()->erase(interfaceName);
          SYSLOG(INFO) << "Removing metric override for interface "
                       << interfaceName;
        }
        advertiseAdjacenciesThrottled_->operator()();
        p.setValue();
      });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_setAdjacencyMetric(
    std::string interfaceName,
    std::string adjNodeName,
    std::optional<int32_t> overrideMetric) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        interfaceName,
                        adjNodeName,
                        overrideMetric]() mutable {
    std::string cmd = overrideMetric.has_value() ? "SET_ADJACENCY_METRIC"
                                                 : "UNSET_ADJACENCY_METRIC";
    thrift::AdjKey adjKey;
    *adjKey.ifName_ref() = interfaceName;
    *adjKey.nodeName_ref() = adjNodeName;

    auto adjacencyKey = std::make_pair(adjNodeName, interfaceName);
    bool unknownAdj{true};
    for (const auto& [_, areaAdjacencies] : adjacencies_) {
      if (areaAdjacencies.count(adjacencyKey)) {
        unknownAdj = false;
        // Found it.
        break;
      }
    }
    // Invalid adj encountered, ignoring.
    if (unknownAdj) {
      XLOG(ERR) << "Skip cmd: [" << cmd << "] due to unknown adj: ["
                << adjNodeName << ":" << interfaceName << "]";
      p.setValue();
      return;
    }

    if (overrideMetric.has_value() &&
        state_.adjMetricOverrides_ref()->count(adjKey) &&
        state_.adjMetricOverrides_ref()[adjKey] == overrideMetric.value()) {
      XLOG(INFO) << "Skip cmd: " << cmd
                 << ". Overridden metric: " << overrideMetric.value()
                 << " already set for: [" << adjNodeName << ":" << interfaceName
                 << "]";
      p.setValue();
      return;
    }

    if (!overrideMetric.has_value() &&
        !state_.adjMetricOverrides_ref()->count(adjKey)) {
      XLOG(INFO) << "Skip cmd: " << cmd << ". No overridden metric found for: ["
                 << adjNodeName << ":" << interfaceName << "]";
      p.setValue();
      return;
    }

    if (overrideMetric.has_value()) {
      state_.adjMetricOverrides_ref()[adjKey] = overrideMetric.value();
      SYSLOG(INFO) << "Overriding metric for adjacency: [" << adjNodeName << ":"
                   << interfaceName << "] to " << overrideMetric.value();
    } else {
      state_.adjMetricOverrides_ref()->erase(adjKey);
      SYSLOG(INFO) << "Removing metric override for adjacency: [" << adjNodeName
                   << ":" << interfaceName << "]";
    }
    advertiseAdjacenciesThrottled_->operator()();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_unsetNodeInterfaceMetricIncrement() {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable {
    if (0 == state_.nodeMetricIncrementVal()) {
      // the increment value already applied
      XLOG(INFO) << "Skip cmd: unsetNodeInterfaceMetricIncrement."
                 << "\n  Already set this node-level metric increment to 0";
      p.setValue();
      return;
    }
    // reset the increment to 0
    state_.nodeMetricIncrementVal() = 0;

    advertiseAdjacenciesThrottled_->operator()();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_setNodeInterfaceMetricIncrement(
    int32_t metricIncrementVal) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), metricIncrementVal]() mutable {
    // invalid increment input
    if (metricIncrementVal <= 0) {
      XLOG(ERR)
          << "Skip cmd: setNodeInterfaceMetricIncrement."
          << "\n  Parameter `metricIncrementVal` should be a positive integer.";
      p.setValue();
      return;
    }

    if (metricIncrementVal == *state_.nodeMetricIncrementVal_ref()) {
      // the increment value already applied
      XLOG(INFO) << "Skip cmd: setNodeInterfaceMetricIncrement"
                 << "\n  Already set this node-level metric increment value: "
                 << metricIncrementVal;
      p.setValue();
      return;
    }

    XLOG(INFO)
        << "Set the node-level static metric increment value:"
        << "\n  Old increment value: " << *state_.nodeMetricIncrementVal_ref()
        << "\n  Setting new increment value: " << metricIncrementVal;

    // set the state
    state_.nodeMetricIncrementVal_ref() = metricIncrementVal;

    advertiseAdjacenciesThrottled_->operator()();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_setInterfaceMetricIncrement(
    std::string interfaceName, int32_t metricIncrementVal) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        interfaceName,
                        metricIncrementVal]() mutable {
    // invalid increment input
    if (metricIncrementVal <= 0) {
      XLOG(ERR)
          << "Skip cmd: setInterfaceMetricIncrement."
          << "\n   Parameter `metricIncrementVal` should be a positive integer.";
      p.setValue();
      return;
    }

    if (0 == interfaces_.count(interfaceName)) {
      XLOG(ERR) << "Skip cmd: setInterfaceMetricIncrement."
                << "due to unknown interface: " << interfaceName;
      p.setValue();
      return;
    }

    if (state_.linkMetiricIncrementMap_ref()->count(interfaceName) &&
        state_.linkMetiricIncrementMap_ref()[interfaceName] ==
            metricIncrementVal) {
      XLOG(INFO) << "Skip cmd: setInterfaceMetricIncrement."
                 << "\n  Increment metric: " << metricIncrementVal
                 << " already set for interface: " << interfaceName;
      p.setValue();
      return;
    }

    // set the link-level metric increment
    auto oldMetric = folly::get_default(
        *state_.linkMetiricIncrementMap_ref(), interfaceName, 0);
    SYSLOG(INFO) << "Increment metric for interface " << interfaceName
                 << "\n  Old increment value: " << oldMetric
                 << "\n  Setting new increment value: " << metricIncrementVal;

    state_.linkMetiricIncrementMap_ref()[interfaceName] = metricIncrementVal;

    advertiseAdjacenciesThrottled_->operator()();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::semifuture_unsetInterfaceMetricIncrement(
    std::string interfaceName) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), interfaceName]() mutable {
    if (0 == interfaces_.count(interfaceName)) {
      XLOG(ERR) << "Skip cmd: [unsetInterfaceMetricIncrement]."
                << "due to unknown interface: " << interfaceName;
      p.setValue();
      return;
    }

    if (not state_.linkMetiricIncrementMap_ref()->count(interfaceName)) {
      XLOG(INFO) << "Skip cmd: [unsetInterfaceMetricIncrement]."
                 << "due the interface " << interfaceName
                 << "didn't set the link-level metric increment before.";
      p.setValue();
      return;
    }

    SYSLOG(INFO) << "Removing link-level metric increment for interface: "
                 << interfaceName;
    state_.linkMetiricIncrementMap_ref()->erase(interfaceName);

    advertiseAdjacenciesThrottled_->operator()();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::DumpLinksReply>>
LinkMonitor::semifuture_getInterfaces() {
  XLOG(DBG2) << "Dump Links requested, replying withV " << interfaces_.size()
             << " links";

  folly::Promise<std::unique_ptr<thrift::DumpLinksReply>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable {
    // reply with the dump of known interfaces and their states
    thrift::DumpLinksReply reply;
    *reply.thisNodeName_ref() = nodeId_;
    reply.isOverloaded_ref() = *state_.isOverloaded_ref();

    // Fill interface details
    for (auto& [_, interface] : interfaces_) {
      const auto& ifName = interface.getIfName();

      thrift::InterfaceDetails ifDetails;
      ifDetails.info_ref() = interface.getInterfaceInfo().toThrift();
      ifDetails.isOverloaded_ref() =
          state_.overloadedLinks_ref()->count(ifName) > 0;

      // Add metric override if any
      if (state_.linkMetricOverrides_ref()->count(ifName) > 0) {
        ifDetails.metricOverride_ref() =
            state_.linkMetricOverrides_ref()->at(ifName);
      }

      // Add link-backoff
      auto backoffMs = interface.getBackoffDuration();
      if (backoffMs.count() != 0) {
        ifDetails.linkFlapBackOffMs_ref() = backoffMs.count();
      } else {
        ifDetails.linkFlapBackOffMs_ref().reset();
      }

      reply.interfaceDetails_ref()->emplace(ifName, std::move(ifDetails));
    }
    p.setValue(std::make_unique<thrift::DumpLinksReply>(std::move(reply)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>>
LinkMonitor::semifuture_getAdjacencies(thrift::AdjacenciesFilter filter) {
  XLOG(DBG2) << "Dump adj requested, reply with " << getTotalAdjacencies()
             << " adjs";

  folly::Promise<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), filter = std::move(filter)]() mutable {
        auto res = std::make_unique<std::vector<thrift::AdjacencyDatabase>>();
        if (filter.selectAreas_ref()->empty()) {
          for (auto const& [areaId, _] : areas_) {
            res->push_back(buildAdjacencyDatabase(areaId));
          }
        } else {
          for (auto const& areaId : *filter.selectAreas_ref()) {
            res->push_back(buildAdjacencyDatabase(areaId));
          }
        }
        p.setValue(std::move(res));
      });
  return sf;
}

folly::SemiFuture<std::unique_ptr<
    std::map<std::string, std::vector<thrift::AdjacencyDatabase>>>>
LinkMonitor::semifuture_getAreaAdjacencies(thrift::AdjacenciesFilter filter) {
  XLOG(DBG2) << "Dump adj requested, reply with " << getTotalAdjacencies()
             << " adjs";

  folly::Promise<std::unique_ptr<
      std::map<std::string, std::vector<thrift::AdjacencyDatabase>>>>
      p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), filter = std::move(filter)]() mutable {
        auto res = std::make_unique<
            std::map<std::string, std::vector<thrift::AdjacencyDatabase>>>();
        if (filter.selectAreas_ref()->empty()) {
          for (auto const& [areaId, _] : areas_) {
            res->operator[](areaId).push_back(buildAdjacencyDatabase(areaId));
          }
        } else {
          for (auto const& areaId : *filter.selectAreas_ref()) {
            res->operator[](areaId).push_back(buildAdjacencyDatabase(areaId));
          }
        }
        p.setValue(std::move(res));
      });
  return sf;
}

folly::SemiFuture<InterfaceDatabase>
LinkMonitor::semifuture_getAllLinks() {
  XLOG(DBG2) << "Querying all links and their addresses from system";
  return collectAll(nlSock_->getAllLinks(), nlSock_->getAllIfAddresses())
      .deferValue(
          [](std::tuple<
              folly::Try<folly::Expected<std::vector<fbnl::Link>, int>>,
              folly::Try<folly::Expected<std::vector<fbnl::IfAddress>, int>>>&&
                 res) {
            std::unordered_map<int64_t, InterfaceInfo> links;
            // Create links
            auto nlLinks = std::get<0>(res).value();
            if (nlLinks.hasError()) {
              throw fbnl::NlException("Failed fetching links", nlLinks.error());
            }
            for (auto& nlLink : nlLinks.value()) {
              // explicitly constuct linkEntry with EMPTY addresses
              InterfaceInfo link(
                  nlLink.getLinkName(), nlLink.isUp(), nlLink.getIfIndex(), {});
              links.emplace(nlLink.getIfIndex(), std::move(link));
            }

            // Add addresses
            auto nlAddrs = std::get<1>(res).value();
            if (nlAddrs.hasError()) {
              throw fbnl::NlException("Failed fetching addrs", nlAddrs.error());
            }
            for (auto& nlAddr : nlAddrs.value()) {
              auto& link = links.at(nlAddr.getIfIndex());
              link.networks.emplace(nlAddr.getPrefix().value());
            }

            // Convert to list and return
            InterfaceDatabase result{};
            for (auto& [_, link] : links) {
              result.emplace_back(std::move(link));
            }
            return result;
          });
}

void
LinkMonitor::logNeighborEvent(NeighborEvent const& event) {
  LogSample sample{};
  sample.addString("event", toString(event.eventType));
  sample.addString("neighbor", event.remoteNodeName);
  sample.addString("interface", event.localIfName);
  sample.addString("remote_interface", event.remoteIfName);
  sample.addString("area", event.area);
  sample.addInt("rtt_us", event.rttUs);

  logSampleQueue_.push(std::move(sample));
}

void
LinkMonitor::logLinkEvent(
    const std::string& iface,
    bool wasUp,
    bool isUp,
    std::chrono::milliseconds backoffTime) {
  // Do not log if no state transition
  if (wasUp == isUp) {
    return;
  }

  LogSample sample{};
  const std::string event = isUp ? "UP" : "DOWN";
  sample.addString("event", fmt::format("IFACE_{}", event));
  sample.addString("interface", iface);
  sample.addInt("backoff_ms", backoffTime.count());

  logSampleQueue_.push(sample);

  SYSLOG(INFO) << "Interface " << iface << " is " << event
               << " and has backoff of " << backoffTime.count() << "ms";
}

void
LinkMonitor::logPeerEvent(
    const std::string& event,
    const std::string& peerName,
    const thrift::PeerSpec& peerSpec) {
  LogSample sample{};
  const auto& peerAddr = *peerSpec.peerAddr_ref();
  const auto& ctrlPort = *peerSpec.ctrlPort_ref();
  sample.addString("event", event);
  sample.addString("node_name", nodeId_);
  sample.addString("peer_name", peerName);
  sample.addString("peer_addr", peerAddr);
  sample.addInt("ctrl_port", ctrlPort);

  logSampleQueue_.push(sample);

  SYSLOG(INFO) << "[" << event << "] for " << peerName
               << " with address: " << peerAddr
               << ", port: " << std::to_string(ctrlPort);
}

bool
LinkMonitor::anyAreaShouldDiscoverOnIface(std::string const& iface) const {
  bool anyMatch = false;
  for (auto const& [_, areaConf] : areas_) {
    anyMatch |= areaConf.shouldDiscoverOnIface(iface);
  }
  return anyMatch;
}

bool
LinkMonitor::anyAreaShouldRedistributeIface(std::string const& iface) const {
  bool anyMatch = false;
  for (auto const& [_, areaConf] : areas_) {
    anyMatch |= areaConf.shouldRedistributeIface(iface);
  }
  return anyMatch;
}

const std::pair<int32_t, int32_t>
LinkMonitor::getNodeSegmentLabelRange(
    AreaConfiguration const& areaConfig) const {
  CHECK(areaConfig.getNodeSegmentLabelConfig().has_value());
  std::pair<int32_t, int32_t> labelRange{
      *areaConfig.getNodeSegmentLabelConfig()
           ->node_segment_label_range_ref()
           ->start_label_ref(),
      *areaConfig.getNodeSegmentLabelConfig()
           ->node_segment_label_range_ref()
           ->end_label_ref()};
  return labelRange;
}

int32_t
LinkMonitor::getStaticNodeSegmentLabel(
    AreaConfiguration const& areaConfig) const {
  CHECK(areaConfig.getNodeSegmentLabelConfig().has_value());
  if (areaConfig.getNodeSegmentLabelConfig()
          ->node_segment_label_ref()
          .has_value()) {
    return *areaConfig.getNodeSegmentLabelConfig()->node_segment_label_ref();
  }
  return 0;
}

/// Total # of adjacencies stored across all areas.
size_t
LinkMonitor::getTotalAdjacencies() {
  size_t numAdjacencies{0};
  for (const auto& [_, areaAdjacencies] : adjacencies_) {
    numAdjacencies += areaAdjacencies.size();
  }
  return numAdjacencies;
}

} // namespace openr
