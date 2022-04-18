/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace go openr.KvStore
namespace py openr.KvStore
namespace py3 openr.thrift
namespace lua openr.KvStore
namespace wiki Open_Routing.Thrift_APIs.KvStore

include "fb303/thrift/fb303_core.thrift"

/*
 * Events in OpenR initialization process.
 * Ref: https://openr.readthedocs.io/Protocol_Guide/Initialization_Process.html
 */
enum InitializationEvent {
  /**
   * Open/R initialization process starts.
   */
  INITIALIZING = 0,
  /**
   * Platform agent is ready to accept FIB route programming.
   */
  AGENT_CONFIGURED = 1,
  /**
   * All links configured locally has been discovered via Netllink.
   */
  LINK_DISCOVERED = 2,
  /**
   * All neighbor has been discovered.
   */
  NEIGHBOR_DISCOVERED = 3,
  /**
   * KvStore has completed initial full sync with initial peer set.
   */
  KVSTORE_SYNCED = 4,
  /**
   * Initial RIB computation based on link-state database has completed.
   */
  RIB_COMPUTED = 5,
  /**
   * Initial FIB programming based on the RIB computation has completed.
   */
  FIB_SYNCED = 6,
  /**
   * Initial prefix advertisement and redistribution has completed.
   */
  PREFIX_DB_SYNCED = 7,
  /**
   * Open/R initialization process has completed.
   */
  INITIALIZED = 8,
  /**
   * All peers(not necessarily neighbors) has been discovered and reported.
   * ATTN: multiple neighbors can lead to single peer, aka, parallel
   * adjacencies. PEER_DISCOVERED happens after NEIGHBOR_DISCOVERED.
   */
  PEERS_DISCOVERED = 9,
  /**
   * ErrorCode: failures happen during the peer discovery process.
   */
  PEER_DISCOVERY_ERROR = 11,
  /**
   * ErrorCode: failures happen during initial KvStore sync process.
   */
  KVSTORE_SYNC_ERROR = 12,
}

exception KvStoreError {
  1: string message;
} (message = "message")

/**
 * DUAL message type
 */
enum DualMessageType {
  UPDATE = 1,
  QUERY = 2,
  REPLY = 3,
}

/**
 * A single DUAL message
 */
struct DualMessage {
  /**
   * destination-id
   */
  1: string dstId;

  /**
   * report-distance towards dst-id
   */
  2: i64 distance;

  /**
   * message type
   */
  3: DualMessageType type;
}

/**
 * Container representing multiple dual messages
 */
struct DualMessages {
  /**
   * sender node-id
   */
  1: string srcId;

  /**
   * List of dual-messages
   */
  2: list<DualMessage> messages;
}

/**
 * Number of packets and dual-messages sent/recv for a neighbor
 * one packet may contain multiple messages
 */
struct DualPerNeighborCounters {
  1: i64 pktSent = 0;
  2: i64 pktRecv = 0;
  3: i64 msgSent = 0;
  4: i64 msgRecv = 0;
}

/**
 * Dual exchange message counters for a given root per neighbor
 */
struct DualPerRootCounters {
  1: i64 querySent = 0;
  2: i64 queryRecv = 0;
  3: i64 replySent = 0;
  4: i64 replyRecv = 0;
  5: i64 updateSent = 0;
  6: i64 updateRecv = 0;
  7: i64 totalSent = 0;
  8: i64 totalRecv = 0;
}

/**
 * Map of neighbor-node to neighbor-counters
 */
typedef map<string, DualPerNeighborCounters> (
  cpp.type = "std::unordered_map<std::string, /* neighbor */ openr::thrift::DualPerNeighborCounters>",
) NeighborCounters

/**
 * Map of root-node to root-counters
 */
typedef map<string, map<string, DualPerRootCounters>> (
  cpp.type = "std::unordered_map<std::string, /* root */ std::map<std::string /* neighbor */, openr::thrift::DualPerRootCounters>>",
) RootCounters

/**
 * All DUAL related counters
 */
struct DualCounters {
  1: NeighborCounters neighborCounters;
  2: RootCounters rootCounters;
}

/**
 * `V` of `KV` Store. It encompasses the data that needs to be synchronized
 * along with few attributes that helps ensure eventual consistency.
 */
struct Value {
  /**
   * Current version of this value. Higher version value replaces the lower one.
   * Applications updating the data of an existing KV will always bump up the
   * version.
   *
   * 1st tie breaker - Prefer higher
   */
  1: i64 version;

  /**
   * The node that originate this Value. Higher value replaces the lower one if
   * (version) is same.
   *
   * 2nd tie breaker - Prefer higher
   */
  3: string originatorId;

  /**
   * Application data. This is opaque to KvStore itself. It is upto the
   * applications to define encoding/decoding of data. Within Open/R, we uses
   * thrift structs to avoid burden of encoding/decoding.
   *
   * 3rd tie breaker - Prefer higher
   *
   * KV update with no application data is considered as TTL update. See below
   * for TTL and TTL version.
   */
  2: optional binary value;

  /**
   * TTL in milliseconds associated with this Value. Originator sets the value.
   * An associated timer if fired will purge the value, if there is no ttl
   * update received.
   */
  4: i64 ttl;

  /**
   * Current version of the TTL. KV update with same (version, originator) but
   * higher ttl-version will reset the associated TTL timer to the new TTL value
   * in the update. Should be reset to 0 when the version increments.
   */
  5: i64 ttlVersion = 0;

  /**
   * Hash associated with `tuple<version, originatorId, value>`. Clients
   * should leave it empty and as will be computed by KvStore on `KEY_SET`
   * operation.
   */
  6: optional i64 hash;
} (cpp.minimize_padding)

/**
 * Map of key to value. This is a representation of KvStore data-base. Using
 * `std::unordered_map` in C++ for efficient lookups.
 */
typedef map<string, Value> (
  cpp.type = "std::unordered_map<std::string, openr::thrift::Value>",
) KeyVals

/**
 * Logical operator enum for querying
 */
enum FilterOperator {
  OR = 1,
  AND = 2,
}

/**
 * Request object for setting keys in KvStore.
 */
struct KeySetParams {
  /**
   * Entries, aka list of Key-Value, that are requested to be updated in a
   * KvStore instance.
   */
  2: KeyVals keyVals;

  /**
   * Optional attributes. List of nodes through which this publication has
   * traversed. Client shouldn't worry about this attribute. It is updated and
   * used by KvStore for avoiding flooding loops.
   */
  5: optional list<string> nodeIds;

  /**
   * @deprecated - Optional flood root-id, indicating which SPT this publication
   * should be flooded on; if none, flood to all peers
   */
  6: optional string floodRootId;

  /**
   * Optional attribute to indicate timestamp when request is sent. This is
   * system timestamp in milliseconds since epoch
   */
  7: optional i64 timestamp_ms;

  /**
   * ID representing sender of the request.
   */
  8: optional string senderId;
} (cpp.minimize_padding)

/**
 * Request object for retrieving specific keys from KvStore
 */
struct KeyGetParams {
  1: list<string> keys;
}

/**
 * Request object for retrieving KvStore entries or subscribing KvStore updates.
 * This is more powerful version than KeyGetParams.
 */
struct KeyDumpParams {
  /**
   * This is deprecated in favor of `keys` attribute
   */
  1: string prefix (deprecated);

  /**
   * Set of originator IDs to filter on
   */

  3: set<string> originatorIds;

  /**
   * If set to true (default), ignore TTL updates. This is applicable for
   * subscriptions (aka streaming KvStore updates).
   */
  6: bool ignoreTtl = true;

  /**
   * If set to true, data attribute (`value.value`) will be removed from
   * from response. This would greatly reduces the data that need to be sent to
   * client.
   */
  7: bool doNotPublishValue = false;

  /**
   * Optional attribute to include keyValHashes information from peer.
   * 1) If NOT empty, ONLY respond with keyVals on which hash differs;
   *  2) Otherwise, respond with flooding element to signal DB change;
   */
  2: optional KeyVals keyValHashes;

  /**
   * The default is OR for dumping KV store entries for backward compatibility.
   * The default will be changed to AND later. We can also make `oper`
   * mandatory later. The default for subscription is AND now.
   */
  4: optional FilterOperator oper;

  /**
   * Keys to subscribe to in KV store so that consumers receive only certain
   * kinds of updates. For example, a consumer might be interesred in
   * getting "adj:.*" keys from open/r domain.
   */
  5: optional list<string> keys;

  /**
   * ID representing sender of the request.
   */
  8: optional string senderId;
} (cpp.minimize_padding)

/**
 * Define KvStorePeerState to maintain peer's state transition
 * during peer coming UP/DOWN for initial sync.
 */
enum KvStorePeerState {
  IDLE = 0,
  SYNCING = 1,
  INITIALIZED = 2,
}

/**
 * Peer's publication and command socket URLs
 * This is used in peer add requests and in
 * the dump results
 */
struct PeerSpec {
  /**
   * Peer address over thrift for KvStore external sync
   */
  1: string peerAddr;

  /**
   * cmd url for KvStore external sync over ZMQ
   */
  2: string cmdUrl (deprecated);

  /**
   * [TO BE DEPRECATED]
   * support flood optimization or not
   */
  3: bool supportFloodOptimization = 0;

  /**
   * thrift port
   */
  4: i32 ctrlPort = 0;

  /**
   * State of KvStore peering
   */
  5: KvStorePeerState state;
}

/**
 * Unordered map for efficiency for peer to peer-spec
 */
typedef map<string, PeerSpec> (
  cpp.type = "std::unordered_map<std::string, openr::thrift::PeerSpec>",
) PeersMap

/**
 * @deprecated
 */
typedef set<string> (cpp.type = "std::unordered_set<std::string>") PeerNames

/**
 * @deprecated - single spanning tree information
 */
struct SptInfo {
  /**
   * passive state or not
   */
  1: bool passive;

  /**
   * metric cost towards root
   */
  2: i64 cost;

  /**
   * optional parent if any (aka nexthop)
   */
  3: optional string parent;

  /**
   * a set of spt children
   */
  4: PeerNames children;
}

/**
 * @deprecated - map<root-id: SPT-info>
 */
typedef map<string, SptInfo> (
  cpp.type = "std::unordered_map<std::string, openr::thrift::SptInfo>",
) SptInfoMap

/**
 * @deprecated - All spanning tree(s) information
 */
struct SptInfos {
  /**
   * map<root-id: SptInfo>
   */
  1: SptInfoMap infos;

  /**
   * all DUAL related counters
   */
  2: DualCounters counters;

  /**
   * current flood-root-id if any
   */
  3: optional string floodRootId;

  /**
   * current flooding peers
   */
  4: PeerNames floodPeers;
}

/**
 * KvStore Response specification. This is also used to respond to GET requests
 */
struct Publication {
  /**
   * KvStore entries
   */
  2: KeyVals keyVals;

  /**
   * List of expired keys. This is applicable for KvStore subscriptions and
   * flooding.
   * TODO: Expose more detailed information `expiredKeyVals` so that subscribers
   * can act on the values as well. e.g. in Decision/PrefixManager we no longer
   * need to rely on the key name to decode prefix/area/node and can use more
   * compact key formatting.
   */
  3: list<string> expiredKeys;

  /**
   * Optional attributes. List of nodes through which this publication has
   * traversed. Client shouldn't worry about this attribute.
   */
  4: optional list<string> nodeIds;

  /**
   * a list of keys that needs to be updated
   * this is only used for full-sync respone to tell full-sync initiator to
   * send back keyVals that need to be updated
   */
  5: optional list<string> tobeUpdatedKeys;

  /**
   * optional flood root-id, indicating which SPT this publication should be
   * flooded on; if none, flood to all peers
   */
  6: optional string floodRootId (deprecated);

  /**
   * KvStore Area to which this publication belongs
   */
  7: string area;

  /**
   * Optional timestamp when publication is sent. This is system timestamp
   * in milliseconds since epoch
   */
  8: optional i64 timestamp_ms;
} (cpp.minimize_padding)

/**
 * Struct summarizing KvStoreDB for a given area. This is currently used for
 * sending responses to 'breeze kvstore summary'
 */

struct KvStoreAreaSummary {
  /**
   * KvStore area for this summary
   */
  1: string area;

  /**
   * Map of peer Names to peerSpec for all peers in this area
   */
  2: PeersMap peersMap;

  /**
   * Total # of Key Value pairs in KvStoreDB in this area
   */
  3: i32 keyValsCount;

  /**
   * Total size in bytes of KvStoreDB for this area
   */
  4: i32 keyValsBytes;
} (cpp.minimize_padding)

struct KvStoreFloodRate {
  1: i32 flood_msg_per_sec;
  2: i32 flood_msg_burst_size;
}

/**
 * KvStoreConfig is the centralized place to configure
 */
struct KvStoreConfig {
  /**
   * Set the TTL (in ms) of a key in the KvStore. For larger networks where
   * burst of updates can be high having high value makes sense. For smaller
   * networks where burst of updates are low, having low value makes more sense.
   */
  1: i32 key_ttl_ms = 300000;

  /**
   * Set node_name attribute to uniquely differentiate KvStore instances.
   *
   * ATTN: the behavior of multiple nodes sharing SAME node_name is NOT defined.
   */
  2: string node_name;

  3: i32 ttl_decrement_ms = 1;

  4: optional KvStoreFloodRate flood_rate;

  /**
   * Sometimes a node maybe a leaf node and have only one path in to network.
   * This node does not require to keep track of the entire topology. In this
   * case, it may be useful to optimize memory by reducing the amount of
   * key/vals tracked by the node. Setting this flag enables key prefix filters
   * defined by key_prefix_filters. A node only tracks keys in kvstore that
   * matches one of the prefixes in key_prefix_filters.
   */
  5: optional bool set_leaf_node;

  /**
   * This comma separated string is used to set the key prefixes when key prefix
   * filter is enabled (See set_leaf_node). It is also set when requesting KEY_DUMP
   * from peer to request keys that match one of these prefixes.
   */
  6: optional list<string> key_prefix_filters;
  7: optional list<string> key_originator_id_filters;

  /**
   * [TO BE DEPRECATED]
   * Set this true to enable flooding-optimization, Open/R will start forming
   * spanning tree and flood updates on formed SPT instead of physical topology.
   * This will greatly reduce kvstore updates traffic, however, based on which
   * node is picked as flood-root, control-plane propagation might increase.
   * Before, propagation is determined by shortest path between two nodes. Now,
   * it will be the path between two nodes in the formed SPT, which is not
   * necessary to be the shortest path. (worst case: 2 x SPT-depth between two
   * leaf nodes). data-plane traffic stays the same.
   */
  8: optional bool enable_flood_optimization;

  /**
   * [TO BE DEPRECATED]
   * Set this true to let this node declare itself as a flood-root. You can set
   * multiple nodes as flood-roots in a network, in steady state, Open/R will
   * pick optimal (smallest node-name) one as the SPT for flooding. If optimal
   * root went away, Open/R will pick 2nd optimal one as SPT-root and so on so
   * forth. If all root nodes went away, Open/R will fall back to naive flooding.
   */
  9: optional bool is_flood_root;

  /**
   * Mark control plane traffic with specified IP-TOS value.
   * Valid range (0, 256) for making.
   * Set this to 0 if you don't want to mark packets.
   */
  10: optional i32 ip_tos;
} (cpp.minimize_padding)

/**
 * Thrift service - exposes RPC APIs for interaction with KvStore module.
 */
service KvStoreService extends fb303_core.BaseService {
  /**
   * Get specific key-values from KvStore. If `filterKeys` is empty then no
   * keys will be returned
   */
  Publication getKvStoreKeyVals(1: list<string> filterKeys) throws (
    1: KvStoreError error,
  );

  /**
   * with area option
   */
  Publication getKvStoreKeyValsArea(
    1: list<string> filterKeys,
    2: string area,
  ) throws (1: KvStoreError error);

  /**
   * Get raw key-values from KvStore with more control over filter
   */
  Publication getKvStoreKeyValsFiltered(1: KeyDumpParams filter) throws (
    1: KvStoreError error,
  );

  /**
   * Get raw key-values from KvStore with more control over filter with 'area'
   * option
   */
  Publication getKvStoreKeyValsFilteredArea(
    1: KeyDumpParams filter,
    2: string area,
  ) throws (1: KvStoreError error);

  /**
   * Get kvstore metadata (no values) with filter
   */
  Publication getKvStoreHashFiltered(1: KeyDumpParams filter) throws (
    1: KvStoreError error,
  );

  /**
   * with area
   */
  Publication getKvStoreHashFilteredArea(
    1: KeyDumpParams filter,
    2: string area,
  ) throws (1: KvStoreError error);

  /**
   * Set/Update key-values in KvStore.
   */
  void setKvStoreKeyVals(1: KeySetParams setParams, 2: string area) throws (
    1: KvStoreError error,
  );

  /**
   * Get KvStore peers
   */
  PeersMap getKvStorePeers() throws (1: KvStoreError error);

  PeersMap getKvStorePeersArea(1: string area) throws (1: KvStoreError error);

  /**
   * Get KvStore Summary for each configured area (provided as the filter set).
   * The resp is a list of Summary structs, one for each area
   */
  list<KvStoreAreaSummary> getKvStoreAreaSummary(
    1: set<string> selectAreas,
  ) throws (1: KvStoreError error);
}
