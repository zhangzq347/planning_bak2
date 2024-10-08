/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file
 **/

#include "modules/planning/traffic_rules/rerouting/rerouting.h"

#include <memory>
#include <string>

#include "modules/common_msgs/basic_msgs/pnc_point.pb.h"
#include "cyber/time/clock.h"
#include "modules/common/vehicle_state/vehicle_state_provider.h"
#include "modules/planning/planning_base/common/planning_context.h"

namespace apollo {
namespace planning {

using apollo::common::Status;
using apollo::cyber::Clock;

bool Rerouting::Init(const std::string& name,
                     const std::shared_ptr<DependencyInjector>& injector) {
  if (!TrafficRule::Init(name, injector)) {
    return false;
  }
  // Load the config this task.
  return TrafficRule::LoadConfig<ReroutingConfig>(&config_);
}

bool Rerouting::ChangeLaneFailRerouting() {
  static constexpr double kRerouteThresholdToEnd = 20.0;
  for (const auto& ref_line_info : frame_->reference_line_info()) {
    if (ref_line_info.ReachedDestination() ||
        ref_line_info.SDistanceToDestination() < kRerouteThresholdToEnd) {
      return true;
    }
  }
  const auto& segments = reference_line_info_->Lanes();
  // 1. If current reference line is drive forward, no rerouting.
  if (segments.NextAction() == routing::FORWARD) {
    // if not current lane, do not check for rerouting
    return true;
  }

  // 2. If vehicle is not on current reference line yet, no rerouting
  if (!segments.IsOnSegment()) {
    return true;
  }

  // 3. If current reference line can connect to next passage, no rerouting
  if (segments.CanExit()) {
    return true;
  }

  // 4. If the end of current passage region not appeared, no rerouting
  const auto& route_end_waypoint = segments.RouteEndWaypoint();
  if (!route_end_waypoint.lane) {
    return true;
  }
  auto point = route_end_waypoint.lane->GetSmoothPoint(route_end_waypoint.s);
  const auto& reference_line = reference_line_info_->reference_line();
  common::SLPoint sl_point;
  if (!reference_line.XYToSL(point, &sl_point)) {
    AERROR << "Failed to project point: " << point.ShortDebugString();
    return false;
  }
  if (!reference_line.IsOnLane(sl_point)) {
    return true;
  }
  // 5. If the end of current passage region is further than kPrepareRoutingTime
  // * speed, no rerouting
  double adc_s = reference_line_info_->AdcSlBoundary().end_s();
  const auto vehicle_state = injector_->vehicle_state();
  double speed = vehicle_state->linear_velocity();
  const double prepare_rerouting_time = config_.prepare_rerouting_time();
  const double prepare_distance = speed * prepare_rerouting_time;
  if (sl_point.s() > adc_s + prepare_distance) {
    ADEBUG << "No need rerouting now because still can drive for time: "
           << prepare_rerouting_time << " seconds";
    return true;
  }
  // 6. Check if we have done rerouting before
  auto* rerouting = injector_->planning_context()
                        ->mutable_planning_status()
                        ->mutable_rerouting();
  if (rerouting == nullptr) {
    AERROR << "rerouting is nullptr.";
    return false;
  }
  double current_time = Clock::NowInSeconds();
  if (rerouting->has_last_rerouting_time() &&
      (current_time - rerouting->last_rerouting_time() <
       config_.cooldown_time())) {
    ADEBUG << "Skip rerouting and wait for previous rerouting result";
    return true;
  }
  if (!frame_->Rerouting(injector_->planning_context())) {
    AERROR << "Failed to send rerouting request";
    return false;
  }

  // store last rerouting time.
  rerouting->set_last_rerouting_time(current_time);
  return true;
}

// @zzq
bool Rerouting::LaneBorrowFailRerouting() {
  auto* rerouting = injector_->planning_context()
                        ->mutable_planning_status()
                        ->mutable_rerouting();
  if (rerouting == nullptr) {
    AERROR << "rerouting is nullptr.";
    return false;
  }

  common::math::Vec2d adc_point(reference_line_info_->vehicle_state().x(),
                                reference_line_info_->vehicle_state().y());
  common::math::Vec2d target_point(423866.1667887506,4438129.189725632);
  double distance = target_point.DistanceTo(adc_point);
  if (distance > 2 || is_rerouting_) {
    return true;
  }
  
  auto *lane_follow_command = rerouting->mutable_lane_follow_command();
  lane_follow_command->clear_way_point();
  auto waypoint = lane_follow_command->add_way_point();
  waypoint->set_x(423856.89459433797);
  waypoint->set_y(4438075.06514868);

  auto *end_pose = lane_follow_command->mutable_end_pose();
  end_pose->set_x(423865.94871507853);
  end_pose->set_y(4437810.73895879);

  rerouting->set_need_rerouting(true);
  AINFO << "LaneBorrowFailRerouting " << rerouting->DebugString();
  is_rerouting_ = true;
  return true;
}
// @zzq

Status Rerouting::ApplyRule(Frame* const frame,
                            ReferenceLineInfo* const reference_line_info) {
  frame_ = frame;
  reference_line_info_ = reference_line_info;
  if (!ChangeLaneFailRerouting()) {
    return Status(common::PLANNING_ERROR,
                  "In un-successful lane change case, rerouting failed");
  }

  // @zzq
  if (!LaneBorrowFailRerouting()) {
    return Status(common::PLANNING_ERROR,
                  "In un-successful lane change case, rerouting failed");
  }
  // @zzq
  
  return Status::OK();
}

}  // namespace planning
}  // namespace apollo
