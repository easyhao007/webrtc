/*
 *  Copyright 2021 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "pc/jsep_transport_collection.h"

#include <map>

namespace webrtc {

void BundleManager::Update(const cricket::SessionDescription* description) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  bundle_groups_.clear();
  for (const cricket::ContentGroup* new_bundle_group :
       description->GetGroupsByName(cricket::GROUP_TYPE_BUNDLE)) {
    bundle_groups_.push_back(
        std::make_unique<cricket::ContentGroup>(*new_bundle_group));
  }
}

void BundleManager::DeleteMid(const cricket::ContentGroup* bundle_group,
                              const std::string& mid) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  // Remove the rejected content from the |bundle_group|.
  // The const pointer arg is used to identify the group, we verify
  // it before we use it to make a modification.
  auto bundle_group_it = std::find_if(
      bundle_groups_.begin(), bundle_groups_.end(),
      [bundle_group](std::unique_ptr<cricket::ContentGroup>& group) {
        return bundle_group == group.get();
      });
  RTC_DCHECK(bundle_group_it != bundle_groups_.end());
  (*bundle_group_it)->RemoveContentName(mid);
}

void BundleManager::DeleteGroup(const cricket::ContentGroup* bundle_group) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  // Delete the BUNDLE group.
  auto bundle_group_it = std::find_if(
      bundle_groups_.begin(), bundle_groups_.end(),
      [bundle_group](std::unique_ptr<cricket::ContentGroup>& group) {
        return bundle_group == group.get();
      });
  RTC_DCHECK(bundle_group_it != bundle_groups_.end());
  bundle_groups_.erase(bundle_group_it);
}

void JsepTransportCollection::RegisterTransport(
    const std::string& mid,
    std::unique_ptr<cricket::JsepTransport> transport) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  jsep_transports_by_name_[mid] = std::move(transport);
}

std::vector<cricket::JsepTransport*> JsepTransportCollection::Transports() {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  std::vector<cricket::JsepTransport*> result;
  for (auto& kv : jsep_transports_by_name_) {
    result.push_back(kv.second.get());
  }
  return result;
}

void JsepTransportCollection::DestroyAllTransports() {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  for (const auto& jsep_transport : jsep_transports_by_name_) {
    map_change_callback_(jsep_transport.first, nullptr);
  }
  jsep_transports_by_name_.clear();
}

const cricket::JsepTransport* JsepTransportCollection::GetTransportByName(
    const std::string& transport_name) const {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  auto it = jsep_transports_by_name_.find(transport_name);
  return (it == jsep_transports_by_name_.end()) ? nullptr : it->second.get();
}

cricket::JsepTransport* JsepTransportCollection::GetTransportByName(
    const std::string& transport_name) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  auto it = jsep_transports_by_name_.find(transport_name);
  return (it == jsep_transports_by_name_.end()) ? nullptr : it->second.get();
}

cricket::JsepTransport* JsepTransportCollection::GetTransportForMid(
    const std::string& mid) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  auto it = mid_to_transport_.find(mid);
  return it == mid_to_transport_.end() ? nullptr : it->second;
}

const cricket::JsepTransport* JsepTransportCollection::GetTransportForMid(
    const std::string& mid) const {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  auto it = mid_to_transport_.find(mid);
  return it == mid_to_transport_.end() ? nullptr : it->second;
}

bool JsepTransportCollection::SetTransportForMid(
    const std::string& mid,
    cricket::JsepTransport* jsep_transport) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  RTC_DCHECK(jsep_transport);

  auto it = mid_to_transport_.find(mid);
  if (it != mid_to_transport_.end() && it->second == jsep_transport)
    return true;

  pending_mids_.push_back(mid);

  if (it == mid_to_transport_.end()) {
    mid_to_transport_.insert(std::make_pair(mid, jsep_transport));
  } else {
    it->second = jsep_transport;
  }

  return map_change_callback_(mid, jsep_transport);
}

void JsepTransportCollection::RemoveTransportForMid(const std::string& mid) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  bool ret = map_change_callback_(mid, nullptr);
  // Calling OnTransportChanged with nullptr should always succeed, since it is
  // only expected to fail when adding media to a transport (not removing).
  RTC_DCHECK(ret);

  mid_to_transport_.erase(mid);
}

void JsepTransportCollection::RollbackTransports() {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  for (auto&& mid : pending_mids_) {
    RemoveTransportForMid(mid);
  }
  for (auto&& mid : pending_mids_) {
    MaybeDestroyJsepTransport(mid);
  }
  pending_mids_.clear();
}

void JsepTransportCollection::CommitTransports() {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  pending_mids_.clear();
}

bool JsepTransportCollection::TransportInUse(
    cricket::JsepTransport* jsep_transport) const {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  for (const auto& kv : mid_to_transport_) {
    if (kv.second == jsep_transport) {
      return true;
    }
  }
  return false;
}

void JsepTransportCollection::MaybeDestroyJsepTransport(
    const std::string& mid) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  auto it = jsep_transports_by_name_.find(mid);
  if (it == jsep_transports_by_name_.end()) {
    return;
  }

  // Don't destroy the JsepTransport if there are still media sections referring
  // to it.
  if (TransportInUse(it->second.get())) {
    return;
  }

  jsep_transports_by_name_.erase(mid);
  state_change_callback_();
}

}  // namespace webrtc
