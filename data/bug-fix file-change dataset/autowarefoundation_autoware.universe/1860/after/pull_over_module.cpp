// Copyright 2021 Tier IV, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "behavior_path_planner/scene_module/pull_over/pull_over_module.hpp"

#include "behavior_path_planner/path_utilities.hpp"
#include "behavior_path_planner/scene_module/pull_over/util.hpp"
#include "behavior_path_planner/scene_module/utils/path_shifter.hpp"
#include "behavior_path_planner/util/create_vehicle_footprint.hpp"
#include "behavior_path_planner/utilities.hpp"

#include <lanelet2_extension/utility/utilities.hpp>
#include <motion_utils/motion_utils.hpp>
#include <rclcpp/rclcpp.hpp>
#include <tier4_autoware_utils/tier4_autoware_utils.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using motion_utils::calcLongitudinalOffsetPose;
using motion_utils::calcSignedArcLength;
using nav_msgs::msg::OccupancyGrid;
using tier4_autoware_utils::calcDistance2d;
using tier4_autoware_utils::calcOffsetPose;
using tier4_autoware_utils::createDefaultMarker;
using tier4_autoware_utils::createMarkerColor;
using tier4_autoware_utils::createMarkerScale;
using tier4_autoware_utils::createPoint;
using tier4_autoware_utils::inverseTransformPose;

namespace behavior_path_planner
{
PullOverModule::PullOverModule(
  const std::string & name, rclcpp::Node & node, const PullOverParameters & parameters)
: SceneModuleInterface{name, node},
  parameters_{parameters},
  vehicle_info_{vehicle_info_util::VehicleInfoUtil(node).getVehicleInfo()}
{
  rtc_interface_ptr_ = std::make_shared<RTCInterface>(&node, "pull_over");
  goal_pose_pub_ =
    node.create_publisher<PoseStamped>("/planning/scenario_planning/modified_goal", 1);

  lane_departure_checker_ = std::make_unique<LaneDepartureChecker>();
  lane_departure_checker_->setVehicleInfo(
    vehicle_info_util::VehicleInfoUtil(node).getVehicleInfo());

  // for collision check with objects
  vehicle_footprint_ = createVehicleFootprint(vehicle_info_);

  resetStatus();
}

void PullOverModule::resetStatus()
{
  status_.has_decided_path = false;
  status_.path_type = PathType::NONE;
  status_.is_safe = false;
  status_.has_decided_velocity = false;
  status_.has_requested_approval_ = false;
}

// This function is needed for waiting for planner_data_
void PullOverModule::updateOccupancyGrid()
{
  if (!planner_data_->occupancy_grid) {
    RCLCPP_WARN_THROTTLE(getLogger(), *clock_, 5000, "occupancy_grid is not ready");
    return;
  }
  occupancy_grid_map_.setMap(*(planner_data_->occupancy_grid));
}

BehaviorModuleOutput PullOverModule::run()
{
  current_state_ = BT::NodeStatus::RUNNING;
  updateOccupancyGrid();
  return plan();
}

ParallelParkingParameters PullOverModule::getGeometricPullOverParameters() const
{
  ParallelParkingParameters params;

  params.th_arrived_distance = parameters_.th_arrived_distance;
  params.th_stopped_velocity = parameters_.th_stopped_velocity;
  params.after_forward_parking_straight_distance =
    parameters_.after_forward_parking_straight_distance;
  params.after_backward_parking_straight_distance =
    parameters_.after_backward_parking_straight_distance;
  params.forward_parking_velocity = parameters_.forward_parking_velocity;
  params.backward_parking_velocity = parameters_.backward_parking_velocity;
  params.forward_parking_lane_departure_margin = parameters_.forward_parking_lane_departure_margin;
  params.backward_parking_lane_departure_margin =
    parameters_.backward_parking_lane_departure_margin;
  params.arc_path_interval = parameters_.arc_path_interval;
  params.maximum_deceleration = parameters_.maximum_deceleration;
  params.max_steer_angle = parameters_.pull_over_max_steer_angle;

  return params;
}

void PullOverModule::onEntry()
{
  RCLCPP_DEBUG(getLogger(), "PULL_OVER onEntry");
  current_state_ = BT::NodeStatus::SUCCESS;

  // Initialize occupancy grid map
  if (parameters_.use_occupancy_grid) {
    OccupancyGridMapParam occupancy_grid_map_param;
    const double margin = parameters_.occupancy_grid_collision_check_margin;
    occupancy_grid_map_param.vehicle_shape.length =
      planner_data_->parameters.vehicle_length + 2 * margin;
    occupancy_grid_map_param.vehicle_shape.width =
      planner_data_->parameters.vehicle_width + 2 * margin;
    occupancy_grid_map_param.vehicle_shape.base2back =
      planner_data_->parameters.base_link2rear + margin;
    occupancy_grid_map_param.theta_size = parameters_.theta_size;
    occupancy_grid_map_param.obstacle_threshold = parameters_.obstacle_threshold;
    occupancy_grid_map_.setParam(occupancy_grid_map_param);
  }

  // initialize when receiving new route
  if (
    last_received_time_.get() == nullptr ||
    *last_received_time_ != planner_data_->route_handler->getRouteHeader().stamp) {
    // Initialize parallel parking planner status
    parallel_parking_parameters_ = getGeometricPullOverParameters();
    resetStatus();
  }
  last_received_time_ =
    std::make_unique<rclcpp::Time>(planner_data_->route_handler->getRouteHeader().stamp);

  // Use refined goal as modified goal when disabling goal research
  refined_goal_pose_ = calcRefinedGoal();
  if (!parameters_.enable_goal_research) {
    goal_candidates_.clear();
    GoalCandidate goal_candidate{};
    goal_candidate.goal_pose = refined_goal_pose_;
    goal_candidate.distance_from_original_goal = 0.0;
    goal_candidates_.push_back(goal_candidate);
  }
}

void PullOverModule::onExit()
{
  RCLCPP_DEBUG(getLogger(), "PULL_OVER onExit");
  clearWaitingApproval();
  removeRTCStatus();

  // A child node must never return IDLE
  // https://github.com/BehaviorTree/BehaviorTree.CPP/blob/master/include/behaviortree_cpp_v3/basic_types.h#L34
  current_state_ = BT::NodeStatus::SUCCESS;
}

bool PullOverModule::isExecutionRequested() const
{
  if (current_state_ == BT::NodeStatus::RUNNING) {
    return true;
  }
  const auto & current_lanes = util::getCurrentLanes(planner_data_);
  const auto & current_pose = planner_data_->self_pose->pose;
  const auto & goal_pose = planner_data_->route_handler->getGoalPose();

  // check if goal_pose is far
  const bool is_in_goal_route_section =
    planner_data_->route_handler->isInGoalRouteSection(current_lanes.back());
  // current_lanes does not have the goal
  if (!is_in_goal_route_section) {
    return false;
  }
  const double self_to_goal_arc_length =
    util::getSignedDistance(current_pose, goal_pose, current_lanes);
  if (self_to_goal_arc_length > parameters_.request_length) {
    return false;
  }

  // check if goal_pose is in shoulder lane
  bool goal_is_in_shoulder_lane = false;
  lanelet::Lanelet closest_shoulder_lanelet;
  if (lanelet::utils::query::getClosestLanelet(
        planner_data_->route_handler->getShoulderLanelets(), goal_pose,
        &closest_shoulder_lanelet)) {
    // check if goal pose is in shoulder lane
    if (lanelet::utils::isInLanelet(goal_pose, closest_shoulder_lanelet, 0.1)) {
      const auto lane_yaw =
        lanelet::utils::getLaneletAngle(closest_shoulder_lanelet, goal_pose.position);
      const auto goal_yaw = tf2::getYaw(goal_pose.orientation);
      const auto angle_diff = tier4_autoware_utils::normalizeRadian(lane_yaw - goal_yaw);
      constexpr double th_angle = M_PI / 4;
      if (std::abs(angle_diff) < th_angle) {
        goal_is_in_shoulder_lane = true;
      }
    }
  }
  if (!goal_is_in_shoulder_lane) return false;

  // check if self pose is NOT in shoulder lane
  bool self_is_in_shoulder_lane = false;
  const auto self_pose = planner_data_->self_pose->pose;
  if (lanelet::utils::query::getClosestLanelet(
        planner_data_->route_handler->getShoulderLanelets(), self_pose,
        &closest_shoulder_lanelet)) {
    self_is_in_shoulder_lane =
      lanelet::utils::isInLanelet(self_pose, closest_shoulder_lanelet, 0.1);
  }
  if (self_is_in_shoulder_lane) return false;

  return true;
}

bool PullOverModule::isExecutionReady() const { return true; }

Pose PullOverModule::calcRefinedGoal() const
{
  lanelet::ConstLanelet goal_lane;
  Pose goal_pose = planner_data_->route_handler->getGoalPose();

  lanelet::Lanelet closest_shoulder_lanelet;
  lanelet::utils::query::getClosestLanelet(
    planner_data_->route_handler->getShoulderLanelets(), goal_pose, &closest_shoulder_lanelet);

  const Pose center_pose =
    lanelet::utils::getClosestCenterPose(closest_shoulder_lanelet, goal_pose.position);

  const double distance_to_left_bound = util::getDistanceToShoulderBoundary(
    planner_data_->route_handler->getShoulderLanelets(), center_pose);
  const double offset_from_center_line = distance_to_left_bound +
                                         planner_data_->parameters.vehicle_width / 2 +
                                         parameters_.margin_from_boundary;
  const Pose refined_goal_pose = calcOffsetPose(center_pose, 0, -offset_from_center_line, 0);

  return refined_goal_pose;
}

void PullOverModule::researchGoal()
{
  // Find goals in pull over areas.
  goal_candidates_.clear();
  for (double dx = -parameters_.backward_goal_search_length;
       dx <= parameters_.forward_goal_search_length; dx += parameters_.goal_search_interval) {
    const Pose search_pose = calcOffsetPose(refined_goal_pose_, dx, 0, 0);
    if (checkCollisionWithPose(search_pose)) {
      continue;
    }

    const auto objects_in_shoulder_lane =
      util::filterObjectsByLanelets(*(planner_data_->dynamic_object), status_.pull_over_lanes);
    if (checkCollisionWithLongitudinalDistance(search_pose, objects_in_shoulder_lane)) {
      continue;
    }

    GoalCandidate goal_candidate;
    goal_candidate.goal_pose = search_pose;
    goal_candidate.distance_from_original_goal =
      std::abs(inverseTransformPose(search_pose, refined_goal_pose_).position.x);
    goal_candidates_.push_back(goal_candidate);
  }
  // Sort with distance from original goal
  std::sort(goal_candidates_.begin(), goal_candidates_.end());
}

BT::NodeStatus PullOverModule::updateState()
{
  // pull_out module will be run when setting new goal, so not need finishing pull_over module.
  // Finishing it causes wrong lane_following path generation.
  // if (hasFinishedPullOver()) {
  //   current_state_ = BT::NodeStatus::SUCCESS;
  //   return current_state_;
  // }

  return current_state_;
}

bool PullOverModule::isLongEnoughToParkingStart(
  const PathWithLaneId & path, const Pose & parking_start_pose) const
{
  const auto dist_to_parking_start_pose = calcSignedArcLength(
    path.points, planner_data_->self_pose->pose, parking_start_pose.position,
    std::numeric_limits<double>::max(), M_PI_2);
  if (!dist_to_parking_start_pose) return false;

  const double current_vel = planner_data_->self_odometry->twist.twist.linear.x;
  const double current_to_stop_distance =
    std::pow(current_vel, 2) / parameters_.maximum_deceleration / 2;

  // once stopped, it cannot start again if start_pose is close.
  // so need enough distance to restart
  const double eps_vel = 0.01;
  // dist to restart should be less than decide_path_distance.
  // otherwise, the goal would change immediately after departure.
  const double dist_to_restart = parameters_.decide_path_distance / 2;
  if (std::abs(current_vel) < eps_vel && *dist_to_parking_start_pose < dist_to_restart) {
    return false;
  }

  return *dist_to_parking_start_pose > current_to_stop_distance;
}

bool PullOverModule::checkCollisionWithLongitudinalDistance(
  const Pose & ego_pose, const PredictedObjects & dynamic_objects) const
{
  if (parameters_.use_occupancy_grid) {
    bool check_out_of_range = false;
    const double offset = std::max(
      parameters_.goal_to_obstacle_margin - parameters_.occupancy_grid_collision_check_margin, 0.0);

    // check forward collision
    const Pose ego_pose_moved_forward = calcOffsetPose(ego_pose, offset, 0, 0);
    const Pose forward_pose_grid_coords =
      global2local(occupancy_grid_map_.getMap(), ego_pose_moved_forward);
    const auto forward_idx = pose2index(
      occupancy_grid_map_.getMap(), forward_pose_grid_coords,
      occupancy_grid_map_.getParam().theta_size);
    if (occupancy_grid_map_.detectCollision(forward_idx, check_out_of_range)) {
      return true;
    }

    // check backward collision
    const Pose ego_pose_moved_backward = calcOffsetPose(ego_pose, -offset, 0, 0);
    const Pose backward_pose_grid_coords =
      global2local(occupancy_grid_map_.getMap(), ego_pose_moved_backward);
    const auto backward_idx = pose2index(
      occupancy_grid_map_.getMap(), backward_pose_grid_coords,
      occupancy_grid_map_.getParam().theta_size);
    if (occupancy_grid_map_.detectCollision(backward_idx, check_out_of_range)) {
      return true;
    }
  }

  if (parameters_.use_object_recognition) {
    if (
      util::calcLongitudinalDistanceFromEgoToObjects(
        ego_pose, planner_data_->parameters.base_link2front,
        planner_data_->parameters.base_link2rear,
        dynamic_objects) < parameters_.goal_to_obstacle_margin) {
      return true;
    }
  }
  return false;
}

bool PullOverModule::checkCollisionWithPose(const Pose & pose) const
{
  if (parameters_.use_occupancy_grid) {
    const Pose pose_grid_coords = global2local(occupancy_grid_map_.getMap(), pose);
    const auto idx = pose2index(
      occupancy_grid_map_.getMap(), pose_grid_coords, occupancy_grid_map_.getParam().theta_size);
    const bool check_out_of_range = false;
    if (occupancy_grid_map_.detectCollision(idx, check_out_of_range)) {
      return true;
    }
  }

  if (parameters_.use_object_recognition) {
    if (util::checkCollisionBetweenFootprintAndObjects(
          vehicle_footprint_, pose, *(planner_data_->dynamic_object),
          parameters_.object_recognition_collision_check_margin)) {
      return true;
    }
  }
  return false;
}

bool PullOverModule::checkCollisionWithPath(const PathWithLaneId & path) const
{
  if (parameters_.use_occupancy_grid) {
    const bool check_out_of_range = false;
    if (occupancy_grid_map_.hasObstacleOnPath(path, check_out_of_range)) {
      return true;
    }
  }

  if (parameters_.use_object_recognition) {
    if (util::checkCollisionBetweenPathFootprintsAndObjects(
          vehicle_footprint_, path, *(planner_data_->dynamic_object),
          parameters_.object_recognition_collision_check_margin)) {
      return true;
    }
  }
  return false;
}

bool PullOverModule::planWithEfficientPath()
{
  // shift parking path
  if (parameters_.enable_shift_parking) {
    for (const auto goal_candidate : goal_candidates_) {
      if (
        planShiftPath(goal_candidate.goal_pose) &&
        isLongEnoughToParkingStart(
          shift_parking_path_.path, shift_parking_path_.shift_point.start)) {
        // shift parking plan already confirms safety and no lane departure in it's own function.
        modified_goal_pose_ = goal_candidate.goal_pose;
        status_.path = shift_parking_path_.path;
        status_.full_path = shift_parking_path_.path;
        status_.path_type = PathType::SHIFT;
        status_.is_safe = true;
        return true;
      }
    }
  }

  // forward arc path
  if (parameters_.enable_arc_forward_parking) {
    for (const auto goal_candidate : goal_candidates_) {
      parallel_parking_planner_.setData(planner_data_, parallel_parking_parameters_);
      if (
        parallel_parking_planner_.planPullOver(
          goal_candidate.goal_pose, status_.current_lanes, status_.pull_over_lanes, true) &&
        isLongEnoughToParkingStart(
          parallel_parking_planner_.getCurrentPath(), parallel_parking_planner_.getStartPose()) &&
        !checkCollisionWithPath(parallel_parking_planner_.getArcPath()) &&
        !lane_departure_checker_->checkPathWillLeaveLane(
          status_.lanes, parallel_parking_planner_.getArcPath())) {
        modified_goal_pose_ = goal_candidate.goal_pose;
        status_.path = parallel_parking_planner_.getCurrentPath();
        status_.full_path = parallel_parking_planner_.getFullPath();
        status_.path_type = PathType::ARC_FORWARD;
        status_.is_safe = true;
        return true;
      }
    }
  }

  // backward arc path
  if (parameters_.enable_arc_backward_parking) {
    for (const auto goal_candidate : goal_candidates_) {
      parallel_parking_planner_.setData(planner_data_, parallel_parking_parameters_);
      if (
        parallel_parking_planner_.planPullOver(
          goal_candidate.goal_pose, status_.current_lanes, status_.pull_over_lanes, false) &&
        isLongEnoughToParkingStart(
          parallel_parking_planner_.getCurrentPath(), parallel_parking_planner_.getStartPose()) &&
        !checkCollisionWithPath(parallel_parking_planner_.getArcPath()) &&
        !lane_departure_checker_->checkPathWillLeaveLane(
          status_.lanes, parallel_parking_planner_.getArcPath())) {
        modified_goal_pose_ = goal_candidate.goal_pose;
        status_.path = parallel_parking_planner_.getCurrentPath();
        status_.full_path = parallel_parking_planner_.getFullPath();
        status_.path_type = PathType::ARC_BACKWARD;
        status_.is_safe = true;
        return true;
      }
    }
  }

  return false;
}

bool PullOverModule::planWithCloseGoal()
{
  for (const auto goal_candidate : goal_candidates_) {
    if (status_.is_safe) break;
    modified_goal_pose_ = goal_candidate.goal_pose;

    // Generate arc shift path.
    if (
      parameters_.enable_shift_parking && planShiftPath(goal_candidate.goal_pose) &&
      isLongEnoughToParkingStart(shift_parking_path_.path, shift_parking_path_.shift_point.start)) {
      // shift parking plan already confirms safety and no lane departure in it's own function.
      modified_goal_pose_ = goal_candidate.goal_pose;
      status_.path = shift_parking_path_.path;
      status_.full_path = shift_parking_path_.path;
      status_.path_type = PathType::SHIFT;
      status_.is_safe = true;
      return true;
    }

    parallel_parking_planner_.setData(planner_data_, parallel_parking_parameters_);
    // Generate arc forward path.
    if (
      parameters_.enable_arc_forward_parking &&
      parallel_parking_planner_.planPullOver(
        goal_candidate.goal_pose, status_.current_lanes, status_.pull_over_lanes, true) &&
      isLongEnoughToParkingStart(
        parallel_parking_planner_.getCurrentPath(), parallel_parking_planner_.getStartPose()) &&
      !checkCollisionWithPath(parallel_parking_planner_.getArcPath()) &&
      !lane_departure_checker_->checkPathWillLeaveLane(
        status_.lanes, parallel_parking_planner_.getArcPath())) {
      modified_goal_pose_ = goal_candidate.goal_pose;
      status_.path = parallel_parking_planner_.getCurrentPath();
      status_.full_path = parallel_parking_planner_.getFullPath();
      status_.path_type = PathType::ARC_FORWARD;
      status_.is_safe = true;
      return true;
    }

    // Generate arc backward path.
    if (
      parameters_.enable_arc_backward_parking &&
      parallel_parking_planner_.planPullOver(
        goal_candidate.goal_pose, status_.current_lanes, status_.pull_over_lanes, false) &&
      isLongEnoughToParkingStart(
        parallel_parking_planner_.getCurrentPath(), parallel_parking_planner_.getStartPose()) &&
      !checkCollisionWithPath(parallel_parking_planner_.getArcPath()) &&
      !lane_departure_checker_->checkPathWillLeaveLane(
        status_.lanes, parallel_parking_planner_.getArcPath())) {
      modified_goal_pose_ = goal_candidate.goal_pose;
      status_.path = parallel_parking_planner_.getCurrentPath();
      status_.full_path = parallel_parking_planner_.getFullPath();
      status_.path_type = PathType::ARC_BACKWARD;
      status_.is_safe = true;
      return true;
    }
  }

  return false;
}

Pose PullOverModule::getParkingStartPose() const
{
  Pose parking_start_pose;
  if (status_.path_type == PathType::SHIFT) {
    // offset shift start point backward to ensure enough distance
    // for winker operation even when stopped
    const double offset = parameters_.pull_over_minimum_velocity *
                          planner_data_->parameters.turn_light_on_threshold_time;
    const auto offset_pose = calcLongitudinalOffsetPose(
      shift_parking_path_.path.points, shift_parking_path_.shift_point.start.position, -offset);
    parking_start_pose = offset_pose ? *offset_pose : shift_parking_path_.shift_point.start;
  } else if (isArcPath()) {
    parking_start_pose = parallel_parking_planner_.getStartPose();
  }
  return parking_start_pose;
}

BehaviorModuleOutput PullOverModule::plan()
{
  status_.current_lanes = util::getExtendedCurrentLanes(planner_data_);
  status_.pull_over_lanes = pull_over_utils::getPullOverLanes(*(planner_data_->route_handler));
  status_.lanes = lanelet::ConstLanelets{};
  status_.lanes.insert(
    status_.lanes.end(), status_.current_lanes.begin(), status_.current_lanes.end());
  status_.lanes.insert(
    status_.lanes.end(), status_.pull_over_lanes.begin(), status_.pull_over_lanes.end());

  // Check if it needs to decide path
  if (status_.is_safe) {
    const Pose parking_start_pose = getParkingStartPose();
    const auto dist_to_parking_start_pose = calcSignedArcLength(
      status_.path.points, planner_data_->self_pose->pose, parking_start_pose.position,
      std::numeric_limits<double>::max(), M_PI_2);

    if (*dist_to_parking_start_pose < parameters_.decide_path_distance) {
      status_.has_decided_path = true;
    }
  }

  // Use decided path
  if (status_.has_decided_path) {
    if (!status_.has_requested_approval_) {
      // request approval again one the final path is decided
      waitApproval();
      removeRTCStatus();
      uuid_ = generateUUID();
      current_state_ = BT::NodeStatus::SUCCESS;  // for breaking loop
      status_.has_requested_approval_ = true;
    } else if (isActivated() && isWaitingApproval()) {
      clearWaitingApproval();
      last_approved_time_ = std::make_unique<rclcpp::Time>(clock_->now());

      // decide velocity to guarantee turn signal lighting time
      if (!status_.has_decided_velocity) {
        const float vel = static_cast<float>(std::max(
          planner_data_->self_odometry->twist.twist.linear.x,
          parameters_.pull_over_minimum_velocity));
        for (auto & p : status_.path.points) {
          p.point.longitudinal_velocity_mps = std::min(p.point.longitudinal_velocity_mps, vel);
        }
      }
      status_.has_decided_velocity = true;
    }

    if (isActivated() && isArcPath() && last_approved_time_ != nullptr) {
      // if using arc_path and finishing current_path, get next path
      // enough time for turn signal
      const bool has_passed_enough_time = (clock_->now() - *last_approved_time_).seconds() >
                                          planner_data_->parameters.turn_light_on_threshold_time;

      if (isActivated() && hasFinishedCurrentPath() && has_passed_enough_time) {
        parallel_parking_planner_.incrementPathIndex();
        status_.path = parallel_parking_planner_.getCurrentPath();
      }
    }

  } else {  // Replan shift -> arc forward -> arc backward path with each goal candidate.
    // Research goal when enabling research and final path has not been decided
    if (parameters_.enable_goal_research) researchGoal();

    status_.path_type = PathType::NONE;
    status_.is_safe = false;

    bool has_found_safe_path = false;
    if (parameters_.search_priority == "efficient_path") {
      has_found_safe_path = planWithEfficientPath();
    } else if (parameters_.search_priority == "close_goal") {
      has_found_safe_path = planWithCloseGoal();
    } else {
      RCLCPP_ERROR(
        getLogger(), "search_priority should be efficient_path or close_goal, but %s is given.",
        parameters_.search_priority.c_str());
      throw std::domain_error("[pull_over] invalid search_priority");
    }

    // Decelerate before the minimum shift distance from the goal search area.
    if (has_found_safe_path) {
      const auto arc_coordinates =
        lanelet::utils::getArcCoordinates(status_.current_lanes, refined_goal_pose_);
      const Pose search_start_pose = calcOffsetPose(
        refined_goal_pose_, -parameters_.backward_goal_search_length, -arc_coordinates.distance, 0);
      status_.path = util::setDecelerationVelocity(
        status_.path, parameters_.pull_over_velocity, search_start_pose,
        -calcMinimumShiftPathDistance(), parameters_.deceleration_interval);
    }
  }
  // generate drivable area
  {
    const auto p = planner_data_->parameters;
    status_.path.drivable_area = util::generateDrivableArea(
      status_.path, status_.lanes, p.drivable_area_resolution, p.vehicle_length, planner_data_);
  }

  BehaviorModuleOutput output;
  // safe: use pull over path
  if (status_.is_safe) {
    output.path = std::make_shared<PathWithLaneId>(status_.path);
    output.path_candidate = std::make_shared<PathWithLaneId>(status_.full_path);
  } else {
    RCLCPP_WARN_THROTTLE(
      getLogger(), *clock_, 5000, "Not found safe pull_over path. Stop in road lane.");
    // safe -> not_safe or no prev_stop_path: generate new stop_path
    if (status_.prev_is_safe || status_.prev_stop_path == nullptr) {
      output.path = std::make_shared<PathWithLaneId>(generateStopPath());
      status_.prev_stop_path = output.path;
    } else {  // not_safe -> not_safe: use previous stop path
      output.path = status_.prev_stop_path;
    }
  }
  status_.prev_is_safe = status_.is_safe;

  // set hazard and turn signal
  if (status_.has_decided_path) {
    const auto hazard_info = getHazardInfo();
    const auto turn_info = getTurnInfo();

    if (hazard_info.first.command == HazardLightsCommand::ENABLE) {
      output.turn_signal_info.hazard_signal.command = hazard_info.first.command;
      output.turn_signal_info.signal_distance = hazard_info.second;
    } else {
      output.turn_signal_info.turn_signal.command = turn_info.first.command;
      output.turn_signal_info.signal_distance = turn_info.second;
    }
  }

  const auto distance_to_path_change = calcDistanceToPathChange();
  updateRTCStatus(distance_to_path_change.first, distance_to_path_change.second);

  setDebugData();

  // Publish the modified goal only when its path is safe.
  if (status_.is_safe) {
    PoseStamped goal_pose_stamped;
    goal_pose_stamped.header = planner_data_->route_handler->getRouteHeader();
    goal_pose_stamped.pose = modified_goal_pose_;
    goal_pose_pub_->publish(goal_pose_stamped);
  }

  // For evaluations
  if (parameters_.print_debug_info) {
    printParkingPositionError();
  }

  return output;
}

// This const function can not change the menber variables like the goal.
// so implement generating candidate path in planWaitingApproval().
// No specific path for the candidate. It's same to the one generated by plan().
CandidateOutput PullOverModule::planCandidate() const { return CandidateOutput{}; }

BehaviorModuleOutput PullOverModule::planWaitingApproval()
{
  updateOccupancyGrid();
  BehaviorModuleOutput out;
  const auto path = *(plan().path);
  out.path_candidate = std::make_shared<PathWithLaneId>(path);
  out.path = std::make_shared<PathWithLaneId>(getReferencePath());
  if (status_.is_safe && isArcPath()) {
    out.path_candidate = std::make_shared<PathWithLaneId>(parallel_parking_planner_.getFullPath());
  }

  const auto distance_to_path_change = calcDistanceToPathChange();
  updateRTCStatus(distance_to_path_change.first, distance_to_path_change.second);
  waitApproval();

  return out;
}

std::pair<double, double> PullOverModule::calcDistanceToPathChange() const
{
  const Pose parking_start_pose = getParkingStartPose();
  const Pose parking_end_pose = modified_goal_pose_;
  const auto dist_to_parking_start_pose = calcSignedArcLength(
    status_.path.points, planner_data_->self_pose->pose, parking_start_pose.position,
    std::numeric_limits<double>::max(), M_PI_2);
  const double dist_to_parking_finish_pose = calcSignedArcLength(
    status_.path.points, planner_data_->self_pose->pose.position, parking_end_pose.position);
  const double start_distance_to_path_change =
    dist_to_parking_start_pose ? *dist_to_parking_start_pose : std::numeric_limits<double>::max();
  return {start_distance_to_path_change, dist_to_parking_finish_pose};
}

void PullOverModule::setParameters(const PullOverParameters & parameters)
{
  parameters_ = parameters;
}

PathWithLaneId PullOverModule::getReferencePath() const
{
  const auto & route_handler = planner_data_->route_handler;
  const auto & current_pose = planner_data_->self_pose->pose;
  const auto & common_parameters = planner_data_->parameters;

  if (status_.current_lanes.empty()) {
    return PathWithLaneId{};
  }
  const auto arc_coordinates =
    lanelet::utils::getArcCoordinates(status_.current_lanes, refined_goal_pose_);
  const Pose search_start_pose = calcOffsetPose(
    refined_goal_pose_, -parameters_.backward_goal_search_length, -arc_coordinates.distance, 0);
  // if not approved, stop parking start position or goal search start position.
  const Pose stop_pose = status_.is_safe ? getParkingStartPose() : search_start_pose;

  // generate center line path to stop_pose
  const auto arc_position_stop_pose =
    lanelet::utils::getArcCoordinates(status_.current_lanes, stop_pose);
  const double s_forward = arc_position_stop_pose.length;
  const auto arc_position_current_pose =
    lanelet::utils::getArcCoordinates(status_.current_lanes, current_pose);
  const double s_backward =
    std::max(0.0, arc_position_current_pose.length - common_parameters.backward_path_length);

  // stop pose is behind current pose
  if (s_forward < s_backward) {
    return generateStopPath();
  }

  PathWithLaneId reference_path =
    route_handler->getCenterLinePath(status_.current_lanes, s_backward, s_forward, true);
  reference_path.header = route_handler->getRouteHeader();

  // slow down for turn signal, insert stop point to stop_pose
  reference_path = util::setDecelerationVelocityForTurnSignal(
    reference_path, stop_pose, planner_data_->parameters.turn_light_on_threshold_time);

  // slow down before the search area.
  if (stop_pose != search_start_pose) {
    reference_path = util::setDecelerationVelocity(
      reference_path, parameters_.pull_over_velocity, search_start_pose,
      -calcMinimumShiftPathDistance(), parameters_.deceleration_interval);
  }

  reference_path.drivable_area = util::generateDrivableArea(
    reference_path, status_.current_lanes, common_parameters.drivable_area_resolution,
    common_parameters.vehicle_length, planner_data_);

  return reference_path;
}

PathWithLaneId PullOverModule::generateStopPath() const
{
  const auto & route_handler = planner_data_->route_handler;
  const auto & current_pose = planner_data_->self_pose->pose;
  const auto & common_parameters = planner_data_->parameters;

  const double current_vel = planner_data_->self_odometry->twist.twist.linear.x;
  auto stop_path = util::getCenterLinePath(
    *route_handler, status_.current_lanes, current_pose, common_parameters.backward_path_length,
    common_parameters.forward_path_length, common_parameters);

  const double current_to_stop_distance =
    std::pow(current_vel, 2) / parameters_.maximum_deceleration / 2;
  const double arclength_current_pose =
    lanelet::utils::getArcCoordinates(status_.current_lanes, current_pose).length;

  for (auto & point : stop_path.points) {
    const double arclength =
      lanelet::utils::getArcCoordinates(status_.current_lanes, point.point.pose).length;
    const double arclength_current_to_point = arclength - arclength_current_pose;
    if (0.0 < arclength_current_to_point) {
      point.point.longitudinal_velocity_mps = std::clamp(
        static_cast<float>(
          current_vel * (current_to_stop_distance - arclength_current_to_point) /
          current_to_stop_distance),
        0.0f, point.point.longitudinal_velocity_mps);
    } else {
      point.point.longitudinal_velocity_mps =
        std::min(point.point.longitudinal_velocity_mps, static_cast<float>(current_vel));
    }
  }

  stop_path.drivable_area = util::generateDrivableArea(
    stop_path, status_.current_lanes, common_parameters.drivable_area_resolution,
    common_parameters.vehicle_length, planner_data_);

  return stop_path;
}

bool PullOverModule::planShiftPath(const Pose goal_pose)
{
  std::vector<ShiftParkingPath> valid_paths;

  const auto & route_handler = planner_data_->route_handler;
  const auto & current_pose = planner_data_->self_pose->pose;
  const auto & common_parameters = planner_data_->parameters;

  if (!isLongEnough(status_.current_lanes, goal_pose) || status_.pull_over_lanes.empty()) {
    return false;
  }

  // find candidate paths
  const auto pull_over_paths = pull_over_utils::generateShiftParkingPaths(
    *route_handler, status_.current_lanes, status_.pull_over_lanes, current_pose, goal_pose,
    common_parameters, parameters_);
  if (pull_over_paths.empty()) {
    return false;
  }

  // select valid path
  valid_paths = pull_over_utils::selectValidPaths(
    pull_over_paths, status_.current_lanes, status_.pull_over_lanes, current_pose,
    route_handler->isInGoalRouteSection(status_.current_lanes.back()), goal_pose,
    *lane_departure_checker_);
  if (valid_paths.empty()) {
    return false;
  }

  // select safe path
  bool found_safe_path = false;
  for (const auto & path : valid_paths) {
    if (!checkCollisionWithPath(path.shifted_path.path)) {
      shift_parking_path_ = path;
      found_safe_path = true;
      break;
    }
  }

  shift_parking_path_.is_safe = found_safe_path;
  shift_parking_path_.path.header = route_handler->getRouteHeader();

  return found_safe_path;
}

double PullOverModule::calcMinimumShiftPathDistance() const
{
  PathShifter path_shifter;
  const double maximum_jerk = parameters_.maximum_lateral_jerk;
  const double pull_over_velocity = parameters_.pull_over_velocity;
  const auto current_pose = planner_data_->self_pose->pose;
  const double distance_after_pull_over = parameters_.after_pull_over_straight_distance;
  const double distance_before_pull_over = parameters_.before_pull_over_straight_distance;
  const auto & route_handler = planner_data_->route_handler;

  double distance_to_left_bound =
    util::getDistanceToShoulderBoundary(route_handler->getShoulderLanelets(), current_pose);
  double offset_from_center_line = distance_to_left_bound +
                                   planner_data_->parameters.vehicle_width / 2 +
                                   parameters_.margin_from_boundary;

  // calculate minimum pull over distance at pull over velocity, maximum jerk and side offset
  const double pull_over_distance_min = path_shifter.calcLongitudinalDistFromJerk(
    abs(offset_from_center_line), maximum_jerk, pull_over_velocity);
  const double pull_over_total_distance_min =
    distance_after_pull_over + pull_over_distance_min + distance_before_pull_over;

  return pull_over_total_distance_min;
}

bool PullOverModule::isLongEnough(
  const lanelet::ConstLanelets & lanelets, const Pose & goal_pose, const double buffer) const
{
  const auto current_pose = planner_data_->self_pose->pose;
  const double distance_to_goal =
    std::abs(util::getSignedDistance(current_pose, goal_pose, lanelets));

  return distance_to_goal > calcMinimumShiftPathDistance() + buffer;
}

bool PullOverModule::isStopped()
{
  odometry_buffer_.push_back(planner_data_->self_odometry);
  // Delete old data in buffer
  while (rclcpp::ok()) {
    const auto time_diff = rclcpp::Time(odometry_buffer_.back()->header.stamp) -
                           rclcpp::Time(odometry_buffer_.front()->header.stamp);
    if (time_diff.seconds() < parameters_.th_stopped_time) {
      break;
    }
    odometry_buffer_.pop_front();
  }
  bool is_stopped = true;
  for (const auto & odometry : odometry_buffer_) {
    const double ego_vel = util::l2Norm(odometry->twist.twist.linear);
    if (ego_vel > parameters_.th_stopped_velocity) {
      is_stopped = false;
      break;
    }
  }
  return is_stopped;
}

bool PullOverModule::hasFinishedCurrentPath()
{
  const auto current_path_end = status_.path.points.back();
  const auto self_pose = planner_data_->self_pose->pose;
  const bool is_near_target = tier4_autoware_utils::calcDistance2d(current_path_end, self_pose) <
                              parameters_.th_arrived_distance;

  return is_near_target && isStopped();
}

bool PullOverModule::hasFinishedPullOver()
{
  // check ego car is close enough to goal pose
  const auto current_pose = planner_data_->self_pose->pose;
  const bool car_is_on_goal =
    calcDistance2d(current_pose, modified_goal_pose_) < parameters_.th_arrived_distance;

  return car_is_on_goal && isStopped();
}

std::pair<HazardLightsCommand, double> PullOverModule::getHazardInfo() const
{
  HazardLightsCommand hazard_signal;
  const double max_distance = std::numeric_limits<double>::max();

  double distance_to_target_pose;   // distance from current pose to target pose on target lanes
  double distance_to_target_point;  // distance from vehicle front to target point on target lanes.
  {
    const auto arc_position_target_pose =
      lanelet::utils::getArcCoordinates(status_.pull_over_lanes, modified_goal_pose_);
    const auto arc_position_current_pose =
      lanelet::utils::getArcCoordinates(status_.pull_over_lanes, planner_data_->self_pose->pose);
    distance_to_target_pose = arc_position_target_pose.length - arc_position_current_pose.length;
    distance_to_target_point = distance_to_target_pose - planner_data_->parameters.base_link2front;
  }

  const auto velocity = planner_data_->self_odometry->twist.twist.linear.x;
  if (
    (distance_to_target_pose < parameters_.hazard_on_threshold_dis &&
     abs(velocity) < parameters_.hazard_on_threshold_vel) ||
    status_.path_type == PathType::ARC_BACKWARD) {
    hazard_signal.command = HazardLightsCommand::ENABLE;
    return std::make_pair(hazard_signal, distance_to_target_point);
  }

  return std::make_pair(hazard_signal, max_distance);
}

std::pair<TurnIndicatorsCommand, double> PullOverModule::getTurnInfo() const
{
  std::pair<TurnIndicatorsCommand, double> turn_info;

  double distance_from_vehicle_front;
  {
    const auto arc_position_current_pose =
      lanelet::utils::getArcCoordinates(status_.current_lanes, planner_data_->self_pose->pose);

    Pose parking_end_pose;
    {
      if (status_.path_type == PathType::SHIFT) {
        parking_end_pose = shift_parking_path_.shift_point.end;
      } else if (isArcPath()) {
        parking_end_pose = parallel_parking_planner_.getArcEndPose();
      }
    }

    const auto arc_position_end_pose =
      lanelet::utils::getArcCoordinates(status_.current_lanes, parking_end_pose);

    distance_from_vehicle_front = arc_position_end_pose.length - arc_position_current_pose.length -
                                  planner_data_->parameters.base_link2front;
  }

  TurnIndicatorsCommand turn_signal;
  const bool is_before_parking_end = distance_from_vehicle_front >= 0.0;
  turn_signal.command =
    is_before_parking_end ? TurnIndicatorsCommand::ENABLE_LEFT : TurnIndicatorsCommand::NO_COMMAND;
  turn_info.first = turn_signal;
  turn_info.second = distance_from_vehicle_front;
  return turn_info;
}

bool PullOverModule::isArcPath() const
{
  return status_.path_type == PathType::ARC_FORWARD || status_.path_type == PathType::ARC_BACKWARD;
}

void PullOverModule::setDebugData()
{
  debug_marker_.markers.clear();

  using marker_utils::createPathMarkerArray;
  using marker_utils::createPolygonMarkerArray;
  using marker_utils::createPoseMarkerArray;

  const auto add = [this](const MarkerArray & added) {
    tier4_autoware_utils::appendMarkerArray(added, &debug_marker_);
  };

  // Visualize pull over areas
  if (parameters_.enable_goal_research) {
    const Pose start_pose =
      calcOffsetPose(refined_goal_pose_, -parameters_.backward_goal_search_length, 0, 0);
    const Pose end_pose =
      calcOffsetPose(refined_goal_pose_, parameters_.forward_goal_search_length, 0, 0);
    // marker_array.markers.push_back(createParkingAreaMarker(start_pose, end_pose, 0));
    const auto header = planner_data_->route_handler->getRouteHeader();
    const auto color = status_.has_decided_path ? createMarkerColor(1.0, 1.0, 0.0, 0.999)  // yellow
                                                : createMarkerColor(0.0, 1.0, 0.0, 0.999);  // green
    const auto p = planner_data_->parameters;
    debug_marker_.markers.push_back(pull_over_utils::createPullOverAreaMarker(
      start_pose, end_pose, 0, header, p.base_link2front, p.base_link2rear, p.vehicle_width,
      color));
  }

  // Visualize path and related pose
  if (!status_.is_safe) {
    return;
  }
  if (isArcPath()) {
    add(createPoseMarkerArray(parallel_parking_planner_.getCl(), "Cl", 0, 0.9, 0.3, 0.3));
    add(createPoseMarkerArray(parallel_parking_planner_.getCr(), "Cr", 0, 0.3, 0.9, 0.3));
    add(createPoseMarkerArray(
      parallel_parking_planner_.getStartPose(), "pull_over_start_pose", 0, 0.3, 0.3, 0.9));
    add(createPoseMarkerArray(
      parallel_parking_planner_.getArcEndPose(), "pull_over_arc_end_pose", 0, 0.9, 0.9, 0.3));
    add(createPathMarkerArray(
      parallel_parking_planner_.getFullPath(), "full_path", 0, 0.0, 0.5, 0.9));
  } else {
    add(createPathMarkerArray(status_.path, "full_path", 0, 0.0, 0.5, 0.9));
  }
}

void PullOverModule::printParkingPositionError() const
{
  const auto current_pose = planner_data_->self_pose->pose;
  const double real_shoulder_to_map_shoulder = 0.0;

  const Pose goal_to_ego = inverseTransformPose(current_pose, modified_goal_pose_);
  const double dx = goal_to_ego.position.x;
  const double dy = goal_to_ego.position.y;
  const double distance_from_real_shoulder =
    real_shoulder_to_map_shoulder + parameters_.margin_from_boundary - dy;
  RCLCPP_INFO(
    getLogger(), "current pose to goal, dx:%f dy:%f dyaw:%f from_real_shoulder:%f", dx, dy,
    tier4_autoware_utils::rad2deg(
      tf2::getYaw(current_pose.orientation) - tf2::getYaw(modified_goal_pose_.orientation)),
    distance_from_real_shoulder);
}
}  // namespace behavior_path_planner
