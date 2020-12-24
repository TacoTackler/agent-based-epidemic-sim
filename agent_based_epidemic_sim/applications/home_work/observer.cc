// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "agent_based_epidemic_sim/applications/home_work/observer.h"

#include <initializer_list>

#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "agent_based_epidemic_sim/agent_synthesis/population_profile.pb.h"
#include "agent_based_epidemic_sim/core/agent.h"
#include "agent_based_epidemic_sim/core/enum_indexed_array.h"
#include "agent_based_epidemic_sim/port/proto_enum_utils.h"
#include "agent_based_epidemic_sim/util/histogram.h"

namespace abesim {

namespace {

const int kDurationBuckets = 6;
const int kContactBuckets = 10;

}  // namespace

HomeWorkSimulationObserver::HomeWorkSimulationObserver(
    LocationTypeFn location_type)
    : location_type_(std::move(location_type)) {
  health_state_counts_.fill(0);
}

void HomeWorkSimulationObserver::Observe(
    const Agent& agent, absl::Span<const InfectionOutcome> outcomes) {
  health_state_counts_[agent.CurrentHealthState()]++;
  auto& visitor_contacts = contacts_[agent.uuid()];
  for (const InfectionOutcome& outcome : outcomes) {
    if (outcome.exposure_type == InfectionOutcomeProto::CONTACT) {
      visitor_contacts.insert(outcome.source_uuid);
    }
  }
}
void HomeWorkSimulationObserver::Observe(const Location& location,
                                         const absl::Span<const Visit> visits) {
  for (const Visit& visit : visits) {
    agent_location_type_durations_[visit.agent_uuid]
                                  [location_type_(visit.location_uuid)] +=
        visit.end_time - visit.start_time;
  }
}

HomeWorkSimulationObserverFactory::HomeWorkSimulationObserverFactory(
    file::FileWriter* const output, LocationTypeFn location_type,
    const std::vector<std::pair<std::string, std::string>>& pass_through_fields)
    : output_(output), location_type_(std::move(location_type)) {
  std::string headers;
  if (!pass_through_fields.empty()) {
    for (const auto& field : pass_through_fields) {
      headers += field.first + ",";
      data_prefix_ += field.second + ",";
    }
  }
  headers += "timestep_end,agents";
  for (HealthState::State state : EnumerateEnumValues<HealthState::State>()) {
    headers += absl::StrCat(",", HealthState::State_Name(state));
  }
  for (const char* location_type : {"home", "work"}) {
    for (int i = 0; i < kDurationBuckets; ++i) {
      absl::StrAppendFormat(
          &headers, ",%s_%s", location_type,
          FormatDuration(absl::Hours(i == 0 ? 0 : 1 << (i - 1))));
    }
  }
  for (int i = 0; i < kContactBuckets; ++i) {
    absl::StrAppendFormat(&headers, ",contact_%d", 1 << i);
  }
  headers += "\n";
  status_.Update(output_->WriteString(headers));
}

void HomeWorkSimulationObserverFactory::Aggregate(
    const Timestep& timestep,
    absl::Span<std::unique_ptr<HomeWorkSimulationObserver> const> observers) {
  health_state_counts_.fill(0);
  agent_location_type_durations_.clear();
  contacts_.clear();
  int agents = 0;

  for (auto& observer : observers) {
    for (HealthState::State state : EnumerateEnumValues<HealthState::State>()) {
      int n = observer->health_state_counts_[state];
      health_state_counts_[state] += n;
      agents += n;
    }
    for (auto iter : observer->agent_location_type_durations_) {
      for (LocationReference::Type location_type :
           EnumerateEnumValues<LocationReference::Type>()) {
        agent_location_type_durations_[iter.first][location_type] +=
            iter.second[location_type];
      }
    }
    for (auto iter : observer->contacts_) {
      contacts_[iter.first].insert(iter.second.begin(), iter.second.end());
    }
  }

  std::string line = data_prefix_;
  absl::StrAppendFormat(&line, "%d,%d",
                        absl::ToUnixSeconds(timestep.end_time()), agents);
  for (HealthState::State state : EnumerateEnumValues<HealthState::State>()) {
    absl::StrAppendFormat(&line, ",%d", health_state_counts_[state]);
  }

  LocationArray<Log2Histogram<absl::Duration, kDurationBuckets>>
      location_histogram;
  for (const auto& iter : agent_location_type_durations_) {
    for (LocationReference::Type i :
         EnumerateEnumValues<LocationReference::Type>()) {
      if (iter.second[i] == absl::ZeroDuration()) continue;
      location_histogram[i].Add(iter.second[i], absl::Hours(1));
    }
  }
  for (LocationReference::Type i :
       EnumerateEnumValues<LocationReference::Type>()) {
    // In the home_work simulation we only care about household and business
    // locations.
    if (i != LocationReference::HOUSEHOLD && i != LocationReference::BUSINESS) {
      continue;
    }
    location_histogram[i].AppendValuesToString(&line);
  }

  Log2Histogram<size_t, kContactBuckets> contact_histogram;
  for (const auto& iter : contacts_) {
    if (iter.second.empty()) continue;
    contact_histogram.Add(iter.second.size() - 1, 1);
  }
  contact_histogram.AppendValuesToString(&line);

  line += "\n";
  status_.Update(output_->WriteString(line));
}

std::unique_ptr<HomeWorkSimulationObserver>
HomeWorkSimulationObserverFactory::MakeObserver(
    const Timestep& timestep) const {
  return absl::make_unique<HomeWorkSimulationObserver>(location_type_);
}

}  // namespace abesim
