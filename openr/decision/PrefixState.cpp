/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/logging/xlog.h>

#include <openr/common/LsdbUtil.h>
#include <openr/decision/PrefixState.h>

namespace openr {

std::unordered_set<folly::CIDRNetwork>
PrefixState::updatePrefix(
    PrefixKey const& key, thrift::PrefixEntry const& entry) {
  std::unordered_set<folly::CIDRNetwork> changed;

  auto [it, inserted] = prefixes_[key.getCIDRNetwork()].emplace(
      key.getNodeAndArea(), std::make_shared<thrift::PrefixEntry>(entry));

  // Skip rest of code, if prefix exists and has no change
  if (not inserted && *it->second == entry) {
    return changed;
  }
  // Update prefix
  if (not inserted) {
    it->second = std::make_shared<thrift::PrefixEntry>(entry);
  }
  changed.insert(key.getCIDRNetwork());

  XLOG(DBG1) << "[ROUTE ADVERTISEMENT] "
             << "Area: " << key.getPrefixArea()
             << ", Node: " << key.getNodeName() << ", "
             << toString(entry, VLOG_IS_ON(1));
  return changed;
}

std::unordered_set<folly::CIDRNetwork>
PrefixState::deletePrefix(PrefixKey const& key) {
  std::unordered_set<folly::CIDRNetwork> changed;

  auto search = prefixes_.find(key.getCIDRNetwork());
  if (search != prefixes_.end() and
      search->second.erase(key.getNodeAndArea())) {
    changed.insert(key.getCIDRNetwork());
    XLOG(DBG1) << "[ROUTE WITHDRAW] "
               << "Area: " << key.getPrefixArea()
               << ", Node: " << key.getNodeName() << ", "
               << folly::IPAddress::networkToString(key.getCIDRNetwork());
    // clean up data structures
    if (search->second.empty()) {
      prefixes_.erase(search);
    }
  }
  return changed;
}

std::vector<thrift::ReceivedRouteDetail>
PrefixState::getReceivedRoutesFiltered(
    thrift::ReceivedRouteFilter const& filter) const {
  std::vector<thrift::ReceivedRouteDetail> routes;
  if (filter.prefixes_ref()) {
    for (auto& prefix : filter.prefixes_ref().value()) {
      auto it = prefixes_.find(toIPNetwork(prefix));
      if (it == prefixes_.end()) {
        continue;
      }
      filterAndAddReceivedRoute(
          routes,
          filter.nodeName_ref(),
          filter.areaName_ref(),
          it->first,
          it->second);
    }
  } else {
    for (auto& [prefix, prefixEntries] : prefixes_) {
      filterAndAddReceivedRoute(
          routes,
          filter.nodeName_ref(),
          filter.areaName_ref(),
          prefix,
          prefixEntries);
    }
  }
  return routes;
}

void
PrefixState::filterAndAddReceivedRoute(
    std::vector<thrift::ReceivedRouteDetail>& routes,
    apache::thrift::optional_field_ref<const std::string&> const& nodeFilter,
    apache::thrift::optional_field_ref<const std::string&> const& areaFilter,
    folly::CIDRNetwork const& prefix,
    PrefixEntries const& prefixEntries) {
  // Return immediately if no prefix-entry
  if (prefixEntries.empty()) {
    return;
  }

  thrift::ReceivedRouteDetail routeDetail;
  routeDetail.prefix_ref() = toIpPrefix(prefix);

  // Add prefix entries and honor the filter
  for (auto& [nodeAndArea, prefixEntry] : prefixEntries) {
    if (nodeFilter && *nodeFilter != nodeAndArea.first) {
      continue;
    }
    if (areaFilter && *areaFilter != nodeAndArea.second) {
      continue;
    }
    routeDetail.routes_ref()->emplace_back();
    auto& route = routeDetail.routes_ref()->back();
    route.key_ref()->node_ref() = nodeAndArea.first;
    route.key_ref()->area_ref() = nodeAndArea.second;
    route.route_ref() = *prefixEntry;
  }

  // Add detail if there are entries to return
  if (routeDetail.routes_ref()->size()) {
    routes.emplace_back(std::move(routeDetail));
  }
}

bool
PrefixState::hasConflictingForwardingInfo(const PrefixEntries& prefixEntries) {
  // Empty prefix entries doesn't indicate conflicting information
  if (prefixEntries.empty()) {
    return false;
  }

  // There is at-least one entry, get its reference
  const auto& firstEntry = prefixEntries.begin()->second;

  // Iterate over all entries and make sure the forwarding information agrees
  for (auto& [_, entry] : prefixEntries) {
    if (firstEntry->forwardingAlgorithm_ref() !=
        entry->forwardingAlgorithm_ref()) {
      return true;
    }
    if (firstEntry->forwardingType_ref() != entry->forwardingType_ref()) {
      return true;
    }
  }

  // No conflicting information
  return false;
}

} // namespace openr
