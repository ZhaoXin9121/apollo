/******************************************************************************
 * Copyright 2019 The Apollo Authors. All Rights Reserved.
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

#include "modules/planning/tasks/deciders/path_bounds_decider.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "modules/common/configs/vehicle_config_helper.h"
#include "modules/common/util/string_util.h"
#include "modules/common/util/util.h"
#include "modules/map/hdmap/hdmap_util.h"
#include "modules/planning/common/path_boundary.h"
#include "modules/planning/tasks/deciders/path_decider_obstacle_utils.h"

namespace apollo {
namespace planning {

using apollo::common::ErrorCode;
using apollo::common::Status;
using apollo::common::VehicleConfigHelper;
using apollo::common::util::StrCat;
using apollo::hdmap::HDMapUtil;

namespace {
// PathBoundPoint contains: (s, l_min, l_max).
using PathBoundPoint = std::tuple<double, double, double>;
// PathBound contains a vector of PathBoundPoints.
using PathBound = std::vector<PathBoundPoint>;
// ObstacleEdge contains: (is_start_s, s, l_min, l_max, obstacle_id).
using ObstacleEdge = std::tuple<int, double, double, double, std::string>;

using LaneType = ReferenceLineInfo::LaneType;
}  // namespace

PathBoundsDecider::PathBoundsDecider(const TaskConfig& config)
    : Decider(config) {}

Status PathBoundsDecider::Process(
    Frame* const frame, ReferenceLineInfo* const reference_line_info) {
  // Sanity checks.
  CHECK_NOTNULL(frame);
  CHECK_NOTNULL(reference_line_info);
  std::vector<PathBoundary> candidate_path_boundaries;
  // const TaskConfig& config = Decider::config_;

  // Initialization.
  InitPathBoundsDecider(*frame, *reference_line_info);

  // Generate the fallback path boundary.
  PathBound fallback_path_bound;
  std::string fallback_path_bounds_msg =
      GenerateFallbackPathBound(*reference_line_info, &fallback_path_bound);
  if (fallback_path_bounds_msg != "") {
    ADEBUG << "Cannot generate a fallback path bound.";
    return Status(ErrorCode::PLANNING_ERROR, fallback_path_bounds_msg);
  }
  if (fallback_path_bound.empty()) {
    const std::string msg = "Failed to get a valid fallback path boundary";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }
  if (!fallback_path_bound.empty()) {
    CHECK_LE(adc_frenet_l_, std::get<2>(fallback_path_bound[0]));
    CHECK_GE(adc_frenet_l_, std::get<1>(fallback_path_bound[0]));
  }
  // Update the fallback path boundary into the reference_line_info.
  std::vector<std::pair<double, double>> fallback_path_bound_pair;
  for (size_t i = 0; i < fallback_path_bound.size(); ++i) {
    fallback_path_bound_pair.emplace_back(std::get<1>(fallback_path_bound[i]),
                                          std::get<2>(fallback_path_bound[i]));
  }
  candidate_path_boundaries.emplace_back(std::get<0>(fallback_path_bound[0]),
                                         kPathBoundsDeciderResolution,
                                         fallback_path_bound_pair);
  candidate_path_boundaries.back().set_label("fallback");

  // Generate regular path boundaries.
  std::vector<LaneBorrowInfo> lane_borrow_info_list;
  if (reference_line_info->is_path_lane_borrow()) {
    // Try borrowing from left and from right neighbor lane.
    lane_borrow_info_list = {LaneBorrowInfo::LEFT_BORROW,
                             LaneBorrowInfo::RIGHT_BORROW,
                             LaneBorrowInfo::NO_BORROW};
  } else {
    // Only use self-lane with no lane borrowing
    lane_borrow_info_list = {LaneBorrowInfo::NO_BORROW};
  }
  // Try every possible lane-borrow option:
  for (const auto& lane_borrow_info : lane_borrow_info_list) {
    PathBound regular_path_bound;
    std::string blocking_obstacle_id = "";
    std::string borrow_lane_type = "";
    std::string path_bounds_msg = GenerateRegularPathBound(
        *reference_line_info, lane_borrow_info, &regular_path_bound,
        &blocking_obstacle_id, &borrow_lane_type);
    if (path_bounds_msg != "") {
      continue;
    }
    if (regular_path_bound.empty()) {
      continue;
    }
    CHECK_LE(adc_frenet_l_, std::get<2>(regular_path_bound[0]));
    CHECK_GE(adc_frenet_l_, std::get<1>(regular_path_bound[0]));
    // Update the path boundary into the reference_line_info.
    std::vector<std::pair<double, double>> regular_path_bound_pair;
    for (size_t i = 0; i < regular_path_bound.size(); ++i) {
      regular_path_bound_pair.emplace_back(std::get<1>(regular_path_bound[i]),
                                           std::get<2>(regular_path_bound[i]));
    }
    candidate_path_boundaries.emplace_back(std::get<0>(regular_path_bound[0]),
                                           kPathBoundsDeciderResolution,
                                           regular_path_bound_pair);
    std::string path_label = "";
    switch (lane_borrow_info) {
      case LaneBorrowInfo::LEFT_BORROW:
        path_label = "left";
        break;
      case LaneBorrowInfo::RIGHT_BORROW:
        path_label = "right";
        break;
      default:
        path_label = "self";
        break;
    }
    candidate_path_boundaries.back().set_label(
        StrCat("regular/", path_label, "/", borrow_lane_type));
    candidate_path_boundaries.back().set_blocking_obstacle_id(
        blocking_obstacle_id);
  }

  // Remove redundant boundaries
  RemoveRedundantPathBoundaries(&candidate_path_boundaries);

  // Success
  reference_line_info->SetCandidatePathBoundaries(candidate_path_boundaries);
  ADEBUG << "Completed regular and fallback path boundaries generation.";
  return Status::OK();
}

void PathBoundsDecider::InitPathBoundsDecider(
    const Frame& frame, const ReferenceLineInfo& reference_line_info) {
  const ReferenceLine& reference_line = reference_line_info.reference_line();
  const common::TrajectoryPoint& planning_start_point =
      frame.PlanningStartPoint();
  // Reset variables.
  adc_frenet_s_ = 0.0;
  adc_frenet_l_ = 0.0;
  adc_l_to_lane_center_ = 0.0;
  adc_lane_width_ = 0.0;

  // Initialize some private variables.
  // ADC s/l info.

  // TODO(jiacheng): using ToFrenetFrame only.
  auto adc_frenet_position =
      reference_line.GetFrenetPoint(planning_start_point.path_point());
  adc_frenet_s_ = adc_frenet_position.s();
  adc_frenet_l_ = adc_frenet_position.l();
  double offset_to_map = 0.0;
  reference_line.GetOffsetToMap(adc_frenet_s_, &offset_to_map);
  adc_l_to_lane_center_ = adc_frenet_l_ + offset_to_map;
  auto adc_sl_info = reference_line.ToFrenetFrame(planning_start_point);
  adc_frenet_sd_ = adc_sl_info.first[1];
  adc_frenet_ld_ = adc_sl_info.second[1] * adc_frenet_sd_;

  // ADC's lane width.
  double lane_left_width = 0.0;
  double lane_right_width = 0.0;
  if (!reference_line.GetLaneWidth(adc_frenet_s_, &lane_left_width,
                                   &lane_right_width)) {
    AWARN << "Failed to get lane width at planning start point.";
    adc_lane_width_ = kDefaultLaneWidth;
  } else {
    adc_lane_width_ = lane_left_width + lane_right_width;
  }
}

std::string PathBoundsDecider::GenerateRegularPathBound(
    const ReferenceLineInfo& reference_line_info,
    const LaneBorrowInfo lane_borrow_info, PathBound* const path_bound,
    std::string* const blocking_obstacle_id,
    std::string* const borrow_lane_type) {
  // 1. Initialize the path boundaries to be an indefinitely large area.
  if (!InitPathBoundary(reference_line_info.reference_line(), path_bound)) {
    const std::string msg = "Failed to initialize path boundaries.";
    AERROR << msg;
    return msg;
  }
  // PathBoundsDebugString(*path_bound);

  // 2. Decide a rough boundary based on lane info and ADC's position
  if (!GetBoundaryFromLanesAndADC(reference_line_info, lane_borrow_info, 0.1,
                                  path_bound, borrow_lane_type)) {
    const std::string msg =
        "Failed to decide a rough boundary based on "
        "road information.";
    AERROR << msg;
    return msg;
  }
  // PathBoundsDebugString(*path_bound);

  // 3. Fine-tune the boundary based on static obstacles
  PathBound temp_path_bound = *path_bound;
  if (!GetBoundaryFromStaticObstacles(reference_line_info.path_decision(),
                                      path_bound, blocking_obstacle_id)) {
    const std::string msg =
        "Failed to decide fine tune the boundaries after "
        "taking into consideration all static obstacles.";
    AERROR << msg;
    return msg;
  }
  // Append some extra path bound points to avoid zero-length path data.
  int counter = 0;
  while (!blocking_obstacle_id->empty() &&
         path_bound->size() < temp_path_bound.size() &&
         counter < kNumExtraTailBoundPoint) {
    path_bound->push_back(temp_path_bound[path_bound->size()]);
    counter++;
  }
  // PathBoundsDebugString(*path_bound);

  // 4. Adjust the boundary considering dynamic obstacles
  // TODO(all): may need to implement this in the future.

  ADEBUG << "Completed generating path boundaries.";
  return "";
}

std::string PathBoundsDecider::GenerateFallbackPathBound(
    const ReferenceLineInfo& reference_line_info, PathBound* const path_bound) {
  // 1. Initialize the path boundaries to be an indefinitely large area.
  if (!InitPathBoundary(reference_line_info.reference_line(), path_bound)) {
    const std::string msg = "Failed to initialize fallback path boundaries.";
    AERROR << msg;
    return msg;
  }
  // PathBoundsDebugString(*path_bound);

  // 2. Decide a rough boundary based on lane info and ADC's position
  std::string dummy_borrow_lane_type;
  if (!GetBoundaryFromLanesAndADC(reference_line_info,
                                  LaneBorrowInfo::NO_BORROW, 0.5, path_bound,
                                  &dummy_borrow_lane_type)) {
    const std::string msg =
        "Failed to decide a rough fallback boundary based on "
        "road information.";
    AERROR << msg;
    return msg;
  }
  // PathBoundsDebugString(*path_bound);

  ADEBUG << "Completed generating fallback path boundaries.";
  return "";
}

void PathBoundsDecider::RemoveRedundantPathBoundaries(
    std::vector<PathBoundary>* const candidate_path_boundaries) {
  // 1. Check to see if both "left" and "right" exist.
  bool is_left_exist = false;
  std::vector<std::pair<double, double>> left_boundary;
  bool is_right_exist = false;
  std::vector<std::pair<double, double>> right_boundary;
  for (const auto& path_boundary : *candidate_path_boundaries) {
    if (path_boundary.label().find("left") != std::string::npos) {
      is_left_exist = true;
      left_boundary = path_boundary.boundary();
    }
    if (path_boundary.label().find("right") != std::string::npos) {
      is_right_exist = true;
      right_boundary = path_boundary.boundary();
    }
  }
  // 2. Check if "left" is contained by "right", and vice versa.
  if (!is_left_exist || !is_right_exist) {
    return;
  }
  bool is_left_redundant = false;
  bool is_right_redundant = false;
  if (IsContained(left_boundary, right_boundary)) {
    is_left_redundant = true;
  }
  if (IsContained(right_boundary, left_boundary)) {
    is_right_redundant = true;
  }

  // 3. If one contains the other, then remove the redundant one.
  for (size_t i = 0; i < candidate_path_boundaries->size(); ++i) {
    const auto& path_boundary = (*candidate_path_boundaries)[i];
    if (path_boundary.label().find("right") != std::string::npos &&
        is_right_redundant) {
      (*candidate_path_boundaries)[i] = candidate_path_boundaries->back();
      candidate_path_boundaries->pop_back();
      break;
    }
    if (path_boundary.label().find("left") != std::string::npos &&
        is_left_redundant) {
      (*candidate_path_boundaries)[i] = candidate_path_boundaries->back();
      candidate_path_boundaries->pop_back();
      break;
    }
  }
  return;
}

bool PathBoundsDecider::IsContained(
    const std::vector<std::pair<double, double>>& lhs,
    const std::vector<std::pair<double, double>>& rhs) {
  if (lhs.size() > rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i].first < rhs[i].first) {
      return false;
    }
    if (lhs[i].second > rhs[i].second) {
      return false;
    }
  }
  return true;
}

bool PathBoundsDecider::InitPathBoundary(const ReferenceLine& reference_line,
                                         PathBound* const path_bound) {
  // Sanity checks.
  CHECK_NOTNULL(path_bound);
  path_bound->clear();

  // Starting from ADC's current position, increment until the horizon, and
  // set lateral bounds to be infinite at every spot.
  for (double curr_s = adc_frenet_s_;
       curr_s < std::min(adc_frenet_s_ + kPathBoundsDeciderHorizon,
                         reference_line.Length());
       curr_s += kPathBoundsDeciderResolution) {
    path_bound->emplace_back(curr_s, std::numeric_limits<double>::lowest(),
                             std::numeric_limits<double>::max());
  }

  // return.
  if (path_bound->empty()) {
    ADEBUG << "Empty path boundary in InitPathBoundary";
    return false;
  }
  return true;
}

bool PathBoundsDecider::GetBoundaryFromLanesAndADC(
    const ReferenceLineInfo& reference_line_info,
    const LaneBorrowInfo lane_borrow_info, double ADC_buffer,
    PathBound* const path_bound, std::string* const borrow_lane_type) {
  // Sanity checks.
  CHECK_NOTNULL(path_bound);
  CHECK(!path_bound->empty());
  const ReferenceLine& reference_line = reference_line_info.reference_line();

  // Go through every point, update the boundary based on lane info and
  // ADC's position.
  double past_lane_left_width = adc_lane_width_ / 2.0;
  double past_lane_right_width = adc_lane_width_ / 2.0;
  // double past_neighbor_lane_width = 0.0;
  int path_blocked_idx = -1;
  bool borrowing_reverse_lane = false;
  for (size_t i = 0; i < path_bound->size(); ++i) {
    double curr_s = std::get<0>((*path_bound)[i]);
    // 1. Get the current lane width at current point.
    double curr_lane_left_width = 0.0;
    double curr_lane_right_width = 0.0;
    if (!reference_line.GetLaneWidth(curr_s, &curr_lane_left_width,
                                     &curr_lane_right_width)) {
      AWARN << "Failed to get lane width at s = " << curr_s;
      curr_lane_left_width = past_lane_left_width;
      curr_lane_right_width = past_lane_right_width;
    } else {
      past_lane_left_width = curr_lane_left_width;
      past_lane_right_width = curr_lane_right_width;
    }

    // 2. Get the neighbor lane widths at the current point.
    double curr_neighbor_lane_width = 0.0;
    hdmap::Id neighbor_lane_id;
    if (lane_borrow_info == LaneBorrowInfo::LEFT_BORROW) {
      // Borrowing left neighbor lane.
      if (reference_line_info.GetNeighborLaneInfo(LaneType::LeftForward, curr_s,
                                                  &neighbor_lane_id,
                                                  &curr_neighbor_lane_width)) {
        ADEBUG << "Borrowing left forward neighbor lane.";
      } else if (reference_line_info.GetNeighborLaneInfo(
                     LaneType::LeftReverse, curr_s, &neighbor_lane_id,
                     &curr_neighbor_lane_width)) {
        borrowing_reverse_lane = true;
        ADEBUG << "Borrowing left reverse neighbor lane.";
      } else {
        ADEBUG << "There is no left neighbor lane.";
      }
    } else if (lane_borrow_info == LaneBorrowInfo::RIGHT_BORROW) {
      // Borrowing right neighbor lane.
      if (reference_line_info.GetNeighborLaneInfo(LaneType::RightForward,
                                                  curr_s, &neighbor_lane_id,
                                                  &curr_neighbor_lane_width)) {
        ADEBUG << "Borrowing right forward neighbor lane.";
      } else if (reference_line_info.GetNeighborLaneInfo(
                     LaneType::RightReverse, curr_s, &neighbor_lane_id,
                     &curr_neighbor_lane_width)) {
        borrowing_reverse_lane = true;
        ADEBUG << "Borrowing right reverse neighbor lane.";
      } else {
        ADEBUG << "There is no right neighbor lane.";
      }
    }

    // 3. Calculate the proper boundary based on lane-width, ADC's position,
    //    and ADC's velocity.
    constexpr double kMaxLateralAccelerations = 1.0;
    double offset_to_map = 0.0;
    reference_line.GetOffsetToMap(curr_s, &offset_to_map);

    double ADC_speed_buffer = (adc_frenet_ld_ > 0 ? 1.0 : -1.0) *
                              adc_frenet_ld_ * adc_frenet_ld_ /
                              kMaxLateralAccelerations / 2.0;

    double curr_left_bound_lane =
        curr_lane_left_width + (lane_borrow_info == LaneBorrowInfo::LEFT_BORROW
                                    ? curr_neighbor_lane_width
                                    : 0.0);
    double curr_left_bound_adc =
        std::fmax(adc_l_to_lane_center_,
                  adc_l_to_lane_center_ + ADC_speed_buffer) +
        GetBufferBetweenADCCenterAndEdge() + ADC_buffer;
    double curr_left_bound =
        std::fmax(curr_left_bound_lane, curr_left_bound_adc) - offset_to_map;

    double curr_right_bound_lane =
        -curr_lane_right_width -
        (lane_borrow_info == LaneBorrowInfo::RIGHT_BORROW
             ? curr_neighbor_lane_width
             : 0.0);
    double curr_right_bound_adc =
        std::fmin(adc_l_to_lane_center_,
                  adc_l_to_lane_center_ + ADC_speed_buffer) -
        GetBufferBetweenADCCenterAndEdge() - ADC_buffer;
    double curr_right_bound =
        std::fmin(curr_right_bound_lane, curr_right_bound_adc) - offset_to_map;

    ADEBUG << "At s = " << curr_s
           << ", left_lane_bound = " << curr_lane_left_width
           << ", right_lane_bound = " << curr_lane_right_width
           << ", offset = " << offset_to_map;

    // 4. Update the boundary.
    double dummy = 0.0;
    if (!UpdatePathBoundaryAndCenterLine(i, curr_left_bound, curr_right_bound,
                                         path_bound, &dummy)) {
      path_blocked_idx = static_cast<int>(i);
    }
    if (path_blocked_idx != -1) {
      break;
    }
  }

  TrimPathBounds(path_blocked_idx, path_bound);

  if (lane_borrow_info == LaneBorrowInfo::NO_BORROW) {
    *borrow_lane_type = "";
  } else {
    *borrow_lane_type = borrowing_reverse_lane ? "reverse" : "forward";
  }

  return true;
}

bool PathBoundsDecider::GetLaneInfoFromPoint(
    double point_x, double point_y, double point_z, double point_theta,
    hdmap::LaneInfoConstPtr* const lane) {
  constexpr double kLaneSearchRadius = 1.0;
  constexpr double kLaneSearchMaxThetaDiff = M_PI / 3.0;
  double s = 0.0;
  double l = 0.0;
  if (HDMapUtil::BaseMapPtr()->GetNearestLaneWithHeading(
          common::util::MakePointENU(point_x, point_y, point_z),
          kLaneSearchRadius, point_theta, kLaneSearchMaxThetaDiff, lane, &s,
          &l) != 0) {
    AWARN << "Failed to find nearest lane from map at position: "
          << "(x, y, z) = (" << point_x << ", " << point_y << ", " << point_z
          << ")"
          << ", heading = " << point_theta;
    return false;
  }
  return true;
}

bool PathBoundsDecider::GetBoundaryFromRefLineOffset(
    const ReferenceLine& reference_line, PathBound* const path_bound) {
  // Sanity checks.
  CHECK_NOTNULL(path_bound);
  CHECK(!path_bound->empty());

  // Go through every point on reference_line, and take care those that
  // deviate from lane-center line considerably.
  //  - Expand lane boundary on one side. (already done previously)
  //  - Treat the other side as if there is an obstacle.
  // for (size_t i = 1; i < path_bound->size(); ++i) {
  //   double curr_s = std::get<0>((*path_bound)[i]);
  //   double offset_to_map = 0.0;
  //   reference_line.GetOffsetToMap(curr_s, &offset_to_map);

  // }
  return true;
}

// Currently, it processes each obstacle based on its frenet-frame
// projection. Therefore, it might be overly conservative when processing
// obstacles whose headings differ from road-headings a lot.
// TODO(all): (future work) this can be improved in the future.
bool PathBoundsDecider::GetBoundaryFromStaticObstacles(
    const PathDecision& path_decision, PathBound* const path_boundaries,
    std::string* const blocking_obstacle_id) {
  // Preprocessing.
  auto indexed_obstacles = path_decision.obstacles();
  auto sorted_obstacles = SortObstaclesForSweepLine(indexed_obstacles);
  double center_line = adc_frenet_l_;
  size_t obs_idx = 0;
  int path_blocked_idx = -1;
  std::multiset<double, std::greater<double>> right_bounds;
  right_bounds.insert(std::numeric_limits<double>::lowest());
  std::multiset<double> left_bounds;
  left_bounds.insert(std::numeric_limits<double>::max());
  // Maps obstacle ID's to the decided ADC pass direction, if ADC should
  // pass from left, then true; otherwise, false.
  std::unordered_map<std::string, bool> obs_id_to_direction;
  // Maps obstacle ID's to the decision of whether side-pass on this obstacle
  // is allowed. If allowed, then true; otherwise, false.
  std::unordered_map<std::string, bool> obs_id_to_sidepass_decision;

  // Step through every path point.
  for (size_t i = 1; i < path_boundaries->size(); ++i) {
    double curr_s = std::get<0>((*path_boundaries)[i]);
    // Check and see if there is any obstacle change:
    if (obs_idx < sorted_obstacles.size() &&
        std::get<1>(sorted_obstacles[obs_idx]) < curr_s) {
      while (obs_idx < sorted_obstacles.size() &&
             std::get<1>(sorted_obstacles[obs_idx]) < curr_s) {
        const auto& curr_obstacle = sorted_obstacles[obs_idx];
        double curr_obstacle_l_min = std::get<2>(curr_obstacle);
        double curr_obstacle_l_max = std::get<3>(curr_obstacle);
        std::string curr_obstacle_id = std::get<4>(curr_obstacle);
        if (std::get<0>(curr_obstacle) == 1) {
          // A new obstacle enters into our scope:
          //   - Decide which direction for the ADC to pass.
          //   - Update the left/right bound accordingly.
          //   - If boundaries blocked, then decide whether can side-pass.
          //   - If yes, then borrow neighbor lane to side-pass.
          if (curr_obstacle_l_min + curr_obstacle_l_max < center_line * 2) {
            // Obstacle is to the right of center-line, should pass from left.
            obs_id_to_direction[curr_obstacle_id] = true;
            right_bounds.insert(curr_obstacle_l_max);
            if (!UpdatePathBoundaryAndCenterLine(
                    i, *left_bounds.begin(), *right_bounds.begin(),
                    path_boundaries, &center_line)) {
              path_blocked_idx = static_cast<int>(i);
              *blocking_obstacle_id = curr_obstacle_id;
              break;
            }
          } else {
            // Obstacle is to the left of center-line, should pass from right.
            obs_id_to_direction[curr_obstacle_id] = false;
            left_bounds.insert(curr_obstacle_l_min);
            if (!UpdatePathBoundaryAndCenterLine(
                    i, *left_bounds.begin(), *right_bounds.begin(),
                    path_boundaries, &center_line)) {
              path_blocked_idx = static_cast<int>(i);
              *blocking_obstacle_id = curr_obstacle_id;
              break;
            }
          }
        } else {
          // An existing obstacle exits our scope.
          if (obs_id_to_direction[curr_obstacle_id]) {
            right_bounds.erase(right_bounds.find(curr_obstacle_l_max));
          } else {
            left_bounds.erase(left_bounds.find(curr_obstacle_l_min));
          }
          obs_id_to_direction.erase(curr_obstacle_id);
        }
        // Update the bounds and center_line.
        std::get<1>((*path_boundaries)[i]) = std::fmax(
            std::get<1>((*path_boundaries)[i]),
            *right_bounds.begin() + GetBufferBetweenADCCenterAndEdge());
        std::get<2>((*path_boundaries)[i]) = std::fmin(
            std::get<2>((*path_boundaries)[i]),
            *left_bounds.begin() - GetBufferBetweenADCCenterAndEdge());
        if (std::get<1>((*path_boundaries)[i]) >
            std::get<2>((*path_boundaries)[i])) {
          ADEBUG << "Path is blocked at s = " << curr_s;
          path_blocked_idx = static_cast<int>(i);
          break;
        } else {
          center_line = (std::get<1>((*path_boundaries)[i]) +
                         std::get<2>((*path_boundaries)[i])) /
                        2.0;
        }

        ++obs_idx;
      }
    } else {
      // If no obstacle change, update the bounds and center_line.
      std::get<1>((*path_boundaries)[i]) =
          std::fmax(std::get<1>((*path_boundaries)[i]),
                    *right_bounds.begin() + GetBufferBetweenADCCenterAndEdge());
      std::get<2>((*path_boundaries)[i]) =
          std::fmin(std::get<2>((*path_boundaries)[i]),
                    *left_bounds.begin() - GetBufferBetweenADCCenterAndEdge());
      if (std::get<1>((*path_boundaries)[i]) >
          std::get<2>((*path_boundaries)[i])) {
        ADEBUG << "Path is blocked at s = " << curr_s;
        path_blocked_idx = static_cast<int>(i);
        if (!obs_id_to_direction.empty()) {
          *blocking_obstacle_id = obs_id_to_direction.begin()->first;
        }
      } else {
        center_line = (std::get<1>((*path_boundaries)[i]) +
                       std::get<2>((*path_boundaries)[i])) /
                      2.0;
      }
    }

    // Early exit if path is blocked.
    if (path_blocked_idx != -1) {
      break;
    }
  }

  TrimPathBounds(path_blocked_idx, path_boundaries);

  return true;
}

// The tuple contains (is_start_s, s, l_min, l_max, obstacle_id)
std::vector<ObstacleEdge> PathBoundsDecider::SortObstaclesForSweepLine(
    const IndexedList<std::string, Obstacle>& indexed_obstacles) {
  std::vector<ObstacleEdge> sorted_obstacles;

  // Go through every obstacle and preprocess it.
  for (const auto* obstacle : indexed_obstacles.Items()) {
    // Only focus on those within-scope obstacles.
    if (!IsWithinPathDeciderScopeObstacle(*obstacle)) {
      continue;
    }
    // Only focus on obstacles that are ahead of ADC.
    if (obstacle->PerceptionSLBoundary().end_s() < adc_frenet_s_) {
      continue;
    }
    // Decompose each obstacle's rectangle into two edges: one at
    // start_s; the other at end_s.
    const auto obstacle_sl = obstacle->PerceptionSLBoundary();
    sorted_obstacles.emplace_back(
        1, obstacle_sl.start_s() - kObstacleStartSBuffer,
        obstacle_sl.start_l() - kObstacleLBuffer,
        obstacle_sl.end_l() + kObstacleLBuffer, obstacle->Id());
    sorted_obstacles.emplace_back(0, obstacle_sl.end_s() + kObstacleEndSBuffer,
                                  obstacle_sl.start_l() - kObstacleLBuffer,
                                  obstacle_sl.end_l() + kObstacleLBuffer,
                                  obstacle->Id());
  }

  // Sort.
  std::sort(sorted_obstacles.begin(), sorted_obstacles.end(),
            [](const ObstacleEdge& lhs, const ObstacleEdge& rhs) {
              if (std::get<1>(lhs) != std::get<1>(rhs)) {
                return std::get<1>(lhs) < std::get<1>(rhs);
              } else {
                return std::get<0>(lhs) > std::get<0>(rhs);
              }
            });

  return sorted_obstacles;
}

std::vector<PathBound> PathBoundsDecider::ConstructSubsequentPathBounds(
    const std::vector<ObstacleEdge>& sorted_obstacles, size_t path_idx,
    size_t obs_idx,
    std::unordered_map<std::string, std::tuple<bool, double>>* const
        obs_id_to_details,
    PathBound* const curr_path_bounds) {
  double left_bounds_from_obstacles = std::numeric_limits<double>::max();
  double right_bounds_from_obstacles = std::numeric_limits<double>::lowest();
  double curr_s = std::get<0>((*curr_path_bounds)[path_idx]);
  //==============================================================
  // If searched through all available s and found a path, return.
  if (path_idx >= curr_path_bounds->size()) {
    ADEBUG << "Completed path bounds search ending at path_idx = " << path_idx;
    return {*curr_path_bounds};
  }

  //==============================================================
  // If there is no obstacle updates at this path_idx.
  if (obs_idx >= sorted_obstacles.size() ||
      std::get<1>(sorted_obstacles[obs_idx]) > curr_s) {
    // 0. Backup the old memory.
    double dummy = 0.0;
    auto old_path_boundary = *curr_path_bounds;
    // 1. Get the boundary from obstacles.
    for (auto it = obs_id_to_details->begin(); it != obs_id_to_details->end();
         ++it) {
      if (std::get<0>(it->second)) {
        // Pass from left.
        right_bounds_from_obstacles =
            std::max(right_bounds_from_obstacles, std::get<1>(it->second));
      } else {
        // Pass from right.
        left_bounds_from_obstacles =
            std::min(left_bounds_from_obstacles, std::get<1>(it->second));
      }
    }
    // 2. Update the path boundary
    bool is_able_to_update = UpdatePathBoundaryAndCenterLine(
        path_idx, left_bounds_from_obstacles, right_bounds_from_obstacles,
        curr_path_bounds, &dummy);
    // 3. Return proper values.
    std::vector<PathBound> ret;
    if (is_able_to_update) {
      ret =
          ConstructSubsequentPathBounds(sorted_obstacles, path_idx + 1, obs_idx,
                                        obs_id_to_details, curr_path_bounds);
    } else {
      TrimPathBounds(static_cast<int>(path_idx), curr_path_bounds);
      ret.push_back(*curr_path_bounds);
    }
    *curr_path_bounds = old_path_boundary;
    return ret;
  }

  //==============================================================
  // If there are obstacle changes
  // 0. Backup the old memory.
  std::unordered_map<std::string, std::tuple<bool, double>>
      old_obs_id_to_details = *obs_id_to_details;
  auto old_path_boundary = *curr_path_bounds;
  double dummy = 0.0;
  // 1. Go through all obstacle changes.
  //    - For exiting obstacle, remove from our memory.
  //    - For entering obstacle, save it to a vector.
  std::vector<ObstacleEdge> new_entering_obstacles;
  size_t new_obs_idx = obs_idx;
  while (new_obs_idx < sorted_obstacles.size() &&
         std::get<1>(sorted_obstacles[new_obs_idx]) <= curr_s) {
    if (!std::get<0>(sorted_obstacles[new_obs_idx])) {
      // For exiting obstacle.
      obs_id_to_details->erase(std::get<4>(sorted_obstacles[new_obs_idx]));
    } else {
      // For entering obstacle.
      new_entering_obstacles.push_back(sorted_obstacles[new_obs_idx]);
    }
    ++new_obs_idx;
  }
  // 2. For new entering obstacles, decide possible pass directions.
  //    (ranked in terms of optimality)
  auto pass_direction_decisions =
      DecidePassDirections(0.0, 0.0, new_entering_obstacles);
  // 3. Try constructing subsequent path-bounds for all possible directions.
  std::vector<PathBound> ret;
  for (size_t i = 0; i < pass_direction_decisions.size(); ++i) {
    // For each possible direction:
    // a. Update the obs_id_to_details
    for (size_t j = 0; j < pass_direction_decisions[i].size(); ++j) {
      if (pass_direction_decisions[i][j]) {
        // Pass from left.
        (*obs_id_to_details)[std::get<4>(new_entering_obstacles[j])] =
            std::make_tuple(true, std::get<3>(new_entering_obstacles[j]));
      } else {
        // Pass from right.
        (*obs_id_to_details)[std::get<4>(new_entering_obstacles[j])] =
            std::make_tuple(false, std::get<2>(new_entering_obstacles[j]));
      }
    }
    // b. Figure out left/right bounds after the updates.
    for (auto it = obs_id_to_details->begin(); it != obs_id_to_details->end();
         ++it) {
      if (std::get<0>(it->second)) {
        // Pass from left.
        right_bounds_from_obstacles =
            std::max(right_bounds_from_obstacles, std::get<1>(it->second));
      } else {
        // Pass from right.
        left_bounds_from_obstacles =
            std::min(left_bounds_from_obstacles, std::get<1>(it->second));
      }
    }
    // c. Update for this path_idx, and construct the subsequent path bounds.
    std::vector<PathBound> curr_dir_path_boundaries;
    bool is_able_to_update = UpdatePathBoundaryAndCenterLine(
        path_idx, left_bounds_from_obstacles, right_bounds_from_obstacles,
        curr_path_bounds, &dummy);
    if (is_able_to_update) {
      curr_dir_path_boundaries = ConstructSubsequentPathBounds(
          sorted_obstacles, path_idx + 1, new_obs_idx, obs_id_to_details,
          curr_path_bounds);
    } else {
      TrimPathBounds(static_cast<int>(path_idx), curr_path_bounds);
      curr_dir_path_boundaries.push_back(*curr_path_bounds);
    }
    // d. Update the path_bounds into the vector, and revert changes
    //    to curr_path_bounds for next cycle.
    ret.insert(ret.end(), curr_dir_path_boundaries.begin(),
               curr_dir_path_boundaries.end());
    *curr_path_bounds = old_path_boundary;
  }
  // 4. Select the best path_bounds in ret.
  *obs_id_to_details = old_obs_id_to_details;
  *curr_path_bounds = old_path_boundary;
  std::sort(ret.begin(), ret.end(),
            [](const PathBound& lhs, const PathBound& rhs) {
              return lhs.size() > rhs.size();
            });
  while (ret.size() > 3) {
    ret.pop_back();
  }
  return ret;
}

std::vector<std::vector<bool>> PathBoundsDecider::DecidePassDirections(
    double l_min, double l_max,
    const std::vector<ObstacleEdge>& new_entering_obstacles) {
  std::vector<std::vector<bool>> decisions;

  // Convert into lateral edges.
  std::vector<ObstacleEdge> lateral_edges;
  lateral_edges.emplace_back(1, std::numeric_limits<double>::lowest(), 0.0, 0.0,
                             "l_min");
  lateral_edges.emplace_back(0, l_min, 0.0, 0.0, "l_min");
  lateral_edges.emplace_back(1, l_max, 0.0, 0.0, "l_max");
  lateral_edges.emplace_back(0, std::numeric_limits<double>::max(), 0.0, 0.0,
                             "l_max");
  for (size_t i = 0; i < new_entering_obstacles.size(); ++i) {
    if (std::get<3>(new_entering_obstacles[i]) < l_min ||
        std::get<2>(new_entering_obstacles[i]) > l_max) {
      continue;
    }
    lateral_edges.emplace_back(1, std::get<2>(new_entering_obstacles[i]), 0.0,
                               0.0, std::get<4>(new_entering_obstacles[i]));
    lateral_edges.emplace_back(0, std::get<3>(new_entering_obstacles[i]), 0.0,
                               0.0, std::get<4>(new_entering_obstacles[i]));
  }
  // Sort the lateral edges for lateral sweep-line algorithm.
  std::sort(lateral_edges.begin(), lateral_edges.end(),
            [](const ObstacleEdge& lhs, const ObstacleEdge& rhs) {
              if (std::get<1>(lhs) != std::get<1>(rhs)) {
                return std::get<1>(lhs) < std::get<1>(rhs);
              } else {
                return std::get<0>(lhs) > std::get<0>(rhs);
              }
            });

  // Go through the lateral edges and find any possible slot.
  std::vector<double> empty_slot;
  int num_obs = 0;
  for (size_t i = 0; i < lateral_edges.size(); ++i) {
    // Update obstacle overlapping info.
    if (std::get<0>(lateral_edges[i])) {
      ++num_obs;
    } else {
      --num_obs;
    }
    // If there is an empty slot within lane boundary.
    if (num_obs == 0 && i != lateral_edges.size() - 1) {
      empty_slot.push_back(
          (std::get<1>(lateral_edges[i]) + std::get<1>(lateral_edges[i + 1])) /
          2.0);
    }
  }
  // For each empty slot, update a corresponding pass direction
  for (size_t i = 0; i < empty_slot.size(); ++i) {
    double pass_position = empty_slot[i];
    std::vector<bool> pass_direction;
    for (size_t j = 0; j < new_entering_obstacles.size(); ++j) {
      if (std::get<2>(new_entering_obstacles[j]) > pass_position) {
        pass_direction.push_back(false);
      } else {
        pass_direction.push_back(true);
      }
    }
    decisions.push_back(pass_direction);
  }
  // TODO(jiacheng): sort the decisions based on the feasibility.

  return decisions;
}

double PathBoundsDecider::GetBufferBetweenADCCenterAndEdge() {
  double adc_half_width =
      VehicleConfigHelper::GetConfig().vehicle_param().width() / 2.0;
  // TODO(all): currently it's a fixed number. But it can take into account many
  // factors such as: ADC length, possible turning angle, speed, etc.
  constexpr double kAdcEdgeBuffer = 0.0;

  return (adc_half_width + kAdcEdgeBuffer);
}

bool PathBoundsDecider::UpdatePathBoundaryAndCenterLine(
    size_t idx, double left_bound, double right_bound,
    PathBound* const path_boundaries, double* const center_line) {
  // Update the right bound (l_min):
  double new_l_min =
      std::fmax(std::get<1>((*path_boundaries)[idx]),
                right_bound + GetBufferBetweenADCCenterAndEdge());
  // Update the left bound (l_max):
  double new_l_max = std::fmin(std::get<2>((*path_boundaries)[idx]),
                               left_bound - GetBufferBetweenADCCenterAndEdge());

  // Check if ADC is blocked.
  // If blocked, don't update anything, return false.
  if (new_l_min > new_l_max) {
    ADEBUG << "Path is blocked at idx = " << idx;
    return false;
  }
  // Otherwise, update path_boundaries and center_line; then return true.
  std::get<1>((*path_boundaries)[idx]) = new_l_min;
  std::get<2>((*path_boundaries)[idx]) = new_l_max;
  *center_line = (std::get<1>((*path_boundaries)[idx]) +
                  std::get<2>((*path_boundaries)[idx])) /
                 2.0;
  return true;
}

void PathBoundsDecider::TrimPathBounds(const int path_blocked_idx,
                                       PathBound* const path_boundaries) {
  if (path_blocked_idx != -1) {
    if (path_blocked_idx == 0) {
      ADEBUG << "Completely blocked. Cannot move at all.";
    }
    int range = static_cast<int>(path_boundaries->size()) - path_blocked_idx;
    for (int i = 0; i < range; ++i) {
      path_boundaries->pop_back();
    }
  }
}

void PathBoundsDecider::PathBoundsDebugString(
    const PathBound& path_boundaries) {
  for (size_t i = 0; i < path_boundaries.size(); ++i) {
    AERROR << "idx " << i << "; s = " << std::get<0>(path_boundaries[i])
           << "; l_min = " << std::get<1>(path_boundaries[i])
           << "; l_max = " << std::get<2>(path_boundaries[i]);
  }
}

}  // namespace planning
}  // namespace apollo
