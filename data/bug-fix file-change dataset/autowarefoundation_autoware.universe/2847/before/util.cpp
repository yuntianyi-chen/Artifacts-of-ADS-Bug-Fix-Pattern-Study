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

#include "behavior_path_planner/scene_module/lane_change/util.hpp"

#include "behavior_path_planner/parameters.hpp"
#include "behavior_path_planner/path_utilities.hpp"
#include "behavior_path_planner/scene_module/lane_change/lane_change_module_data.hpp"
#include "behavior_path_planner/scene_module/lane_change/lane_change_path.hpp"
#include "behavior_path_planner/scene_module/utils/path_shifter.hpp"
#include "behavior_path_planner/utilities.hpp"

#include <lanelet2_extension/utility/query.hpp>
#include <lanelet2_extension/utility/utilities.hpp>
#include <rclcpp/rclcpp.hpp>

#include <lanelet2_core/LaneletMap.h>
#include <tf2/utils.h>
#include <tf2_ros/transform_listener.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{

using autoware_auto_perception_msgs::msg::ObjectClassification;
using autoware_auto_perception_msgs::msg::PredictedObjects;
using autoware_auto_planning_msgs::msg::PathWithLaneId;
using behavior_path_planner::util::calcObjectPolygon;
using behavior_path_planner::util::getHighestProbLabel;
using geometry_msgs::msg::Pose;
using tier4_autoware_utils::LineString2d;
using tier4_autoware_utils::Point2d;
using tier4_autoware_utils::Polygon2d;

void filterObjectIndices(
  const PredictedObjects & objects, const lanelet::ConstLanelets & current_lanes,
  const lanelet::ConstLanelets & target_lanes, const PathWithLaneId & ego_path,
  const Pose & current_pose, const double forward_path_length, const double filter_width,
  std::vector<size_t> & current_lane_obj_indices, std::vector<size_t> & target_lane_obj_indices,
  std::vector<size_t> & others_obj_indices, const bool ignore_unknown_obj = false)
{
  // Reserve maximum amount possible
  current_lane_obj_indices.reserve(objects.objects.size());
  target_lane_obj_indices.reserve(objects.objects.size());
  others_obj_indices.reserve(objects.objects.size());

  const auto get_basic_polygon =
    [](const lanelet::ConstLanelets & lanes, const double start_dist, const double end_dist) {
      const auto polygon_3d = lanelet::utils::getPolygonFromArcLength(lanes, start_dist, end_dist);
      return lanelet::utils::to2D(polygon_3d).basicPolygon();
    };
  const auto arc = lanelet::utils::getArcCoordinates(current_lanes, current_pose);
  const auto current_polygon =
    get_basic_polygon(current_lanes, arc.length, arc.length + forward_path_length);
  const auto target_polygon =
    get_basic_polygon(target_lanes, 0.0, std::numeric_limits<double>::max());
  LineString2d ego_path_linestring;
  ego_path_linestring.reserve(ego_path.points.size());
  for (const auto & pt : ego_path.points) {
    const auto & position = pt.point.pose.position;
    boost::geometry::append(ego_path_linestring, Point2d(position.x, position.y));
  }

  for (size_t i = 0; i < objects.objects.size(); ++i) {
    const auto & obj = objects.objects.at(i);

    if (ignore_unknown_obj) {
      const auto t = getHighestProbLabel(obj.classification);
      if (t == ObjectClassification::UNKNOWN) {
        continue;
      }
    }

    Polygon2d obj_polygon;
    if (!calcObjectPolygon(obj, &obj_polygon)) {
      RCLCPP_ERROR_STREAM(
        rclcpp::get_logger("behavior_path_planner").get_child("lane_change"),
        "Failed to calcObjectPolygon...!!!");
      continue;
    }

    if (boost::geometry::intersects(current_polygon, obj_polygon)) {
      const double distance = boost::geometry::distance(obj_polygon, ego_path_linestring);

      if (distance < filter_width) {
        current_lane_obj_indices.push_back(i);
        continue;
      }
    }

    const bool is_intersect_with_target = boost::geometry::intersects(target_polygon, obj_polygon);
    if (is_intersect_with_target) {
      target_lane_obj_indices.push_back(i);
    } else {
      others_obj_indices.push_back(i);
    }
  }
}
}  // namespace

namespace behavior_path_planner::lane_change_utils
{
using autoware_auto_planning_msgs::msg::PathPointWithLaneId;
using lanelet::ArcCoordinates;

inline double calcLaneChangeResamplingInterval(
  const double lane_changing_distance, const double lane_changing_speed)
{
  constexpr auto min_resampling_points{30.0};
  constexpr auto resampling_dt{0.2};
  return std::max(
    lane_changing_distance / min_resampling_points, lane_changing_speed * resampling_dt);
}

PathWithLaneId combineReferencePath(const PathWithLaneId & path1, const PathWithLaneId & path2)
{
  PathWithLaneId path;
  path.points.insert(path.points.end(), path1.points.begin(), path1.points.end());

  // skip overlapping point
  path.points.insert(path.points.end(), next(path2.points.begin()), path2.points.end());

  return path;
}

bool isPathInLanelets(
  const PathWithLaneId & path, const lanelet::ConstLanelets & original_lanelets,
  const lanelet::ConstLanelets & target_lanelets)
{
  const auto current_lane_poly = lanelet::utils::getPolygonFromArcLength(
    original_lanelets, 0, std::numeric_limits<double>::max());
  const auto target_lane_poly =
    lanelet::utils::getPolygonFromArcLength(target_lanelets, 0, std::numeric_limits<double>::max());
  const auto current_lane_poly_2d = lanelet::utils::to2D(current_lane_poly).basicPolygon();
  const auto target_lane_poly_2d = lanelet::utils::to2D(target_lane_poly).basicPolygon();
  for (const auto & pt : path.points) {
    const lanelet::BasicPoint2d ll_pt(pt.point.pose.position.x, pt.point.pose.position.y);
    const auto is_in_current = boost::geometry::covered_by(ll_pt, current_lane_poly_2d);
    if (is_in_current) {
      continue;
    }
    const auto is_in_target = boost::geometry::covered_by(ll_pt, target_lane_poly_2d);
    if (!is_in_target) {
      return false;
    }
  }
  return true;
}

double getExpectedVelocityWhenDecelerate(
  const double & velocity, const double & expected_acceleration, const double & duration)
{
  return velocity + expected_acceleration * duration;
}

double getDistanceWhenDecelerate(
  const double & velocity, const double & expected_acceleration, const double & duration,
  const double & minimum_distance)
{
  const auto distance = velocity * duration + 0.5 * expected_acceleration * std::pow(duration, 2);
  return std::max(distance, minimum_distance);
}

std::optional<LaneChangePath> constructCandidatePath(
  const PathWithLaneId & prepare_segment, const PathWithLaneId & lane_changing_segment,
  const PathWithLaneId & target_lane_reference_path, const ShiftLine & shift_line,
  const lanelet::ConstLanelets & original_lanelets, const lanelet::ConstLanelets & target_lanelets,
  const double acceleration, const LaneChangePhaseInfo distance, const LaneChangePhaseInfo speed,
  const LaneChangeParameters & lane_change_param)
{
  PathShifter path_shifter;
  path_shifter.setPath(target_lane_reference_path);
  path_shifter.addShiftLine(shift_line);
  ShiftedPath shifted_path;

  // offset front side
  bool offset_back = false;

  const auto lane_changing_speed = speed.lane_changing;

  path_shifter.setVelocity(lane_changing_speed);
  path_shifter.setLateralAccelerationLimit(std::abs(lane_change_param.lane_changing_lateral_acc));

  if (!path_shifter.generate(&shifted_path, offset_back)) {
    RCLCPP_DEBUG(
      rclcpp::get_logger("behavior_path_planner").get_child("lane_change").get_child("util"),
      "failed to generate shifted path.");
  }

  const auto prepare_distance = distance.prepare;
  const auto lane_change_distance = distance.lane_changing;

  LaneChangePath candidate_path;
  candidate_path.acceleration = acceleration;
  candidate_path.length.prepare = prepare_distance;
  candidate_path.length.lane_changing = lane_change_distance;
  candidate_path.duration.prepare = std::invoke([&]() {
    const auto duration =
      prepare_distance / std::max(lane_change_param.minimum_lane_change_velocity, speed.prepare);
    return std::min(duration, lane_change_param.lane_change_prepare_duration);
  });
  candidate_path.duration.lane_changing = std::invoke([&]() {
    const auto rounding_multiplier = 1.0 / lane_change_param.prediction_time_resolution;
    return std::ceil((lane_change_distance / lane_changing_speed) * rounding_multiplier) /
           rounding_multiplier;
  });
  candidate_path.shift_line = shift_line;
  candidate_path.reference_lanelets = original_lanelets;
  candidate_path.target_lanelets = target_lanelets;

  RCLCPP_DEBUG(
    rclcpp::get_logger("behavior_path_planner")
      .get_child("lane_change")
      .get_child("util")
      .get_child("constructCandidatePath"),
    "prepare_distance: %f, lane_change: %f", prepare_distance, lane_change_distance);

  const PathPointWithLaneId & lane_changing_start_point = prepare_segment.points.back();
  const PathPointWithLaneId & lane_changing_end_point = lane_changing_segment.points.front();
  const Pose & lane_changing_end_pose = lane_changing_end_point.point.pose;
  const auto lanechange_end_idx =
    motion_utils::findNearestIndex(shifted_path.path.points, lane_changing_end_pose);
  const auto insertLaneIDs = [](auto & target, const auto src) {
    target.lane_ids.insert(target.lane_ids.end(), src.lane_ids.begin(), src.lane_ids.end());
  };
  if (lanechange_end_idx) {
    for (size_t i = 0; i < shifted_path.path.points.size(); ++i) {
      auto & point = shifted_path.path.points.at(i);
      if (i < *lanechange_end_idx) {
        insertLaneIDs(point, lane_changing_start_point);
        insertLaneIDs(point, lane_changing_end_point);
        point.point.longitudinal_velocity_mps = std::min(
          point.point.longitudinal_velocity_mps,
          lane_changing_start_point.point.longitudinal_velocity_mps);
        continue;
      }
      point.point.longitudinal_velocity_mps =
        std::min(point.point.longitudinal_velocity_mps, static_cast<float>(lane_changing_speed));
      const auto nearest_idx =
        motion_utils::findNearestIndex(lane_changing_segment.points, point.point.pose);
      point.lane_ids = lane_changing_segment.points.at(*nearest_idx).lane_ids;
    }

    candidate_path.path = combineReferencePath(prepare_segment, shifted_path.path);
    candidate_path.shifted_path = shifted_path;
  } else {
    RCLCPP_ERROR_STREAM(
      rclcpp::get_logger("behavior_path_planner").get_child("lane_change").get_child("util"),
      "lane change end idx not found on target path.");
    return std::nullopt;
  }

  // check candidate path is in lanelet
  if (!isPathInLanelets(candidate_path.path, original_lanelets, target_lanelets)) {
    return std::nullopt;
  }

  return std::optional<LaneChangePath>{candidate_path};
}

LaneChangePaths getLaneChangePaths(
  const RouteHandler & route_handler, const lanelet::ConstLanelets & original_lanelets,
  const lanelet::ConstLanelets & target_lanelets, const Pose & pose, const Twist & twist,
  const BehaviorPathPlannerParameters & common_parameter, const LaneChangeParameters & parameter)
{
  LaneChangePaths candidate_paths{};

  if (original_lanelets.empty() || target_lanelets.empty()) {
    return candidate_paths;
  }

  // rename parameter
  const auto & backward_path_length = common_parameter.backward_path_length;
  const auto & forward_path_length = common_parameter.forward_path_length;
  const auto & lane_change_prepare_duration = parameter.lane_change_prepare_duration;
  const auto & minimum_lane_change_prepare_distance =
    common_parameter.minimum_lane_change_prepare_distance;
  const auto & minimum_lane_change_velocity = parameter.minimum_lane_change_velocity;
  const auto & maximum_deceleration = parameter.maximum_deceleration;
  const auto & lane_change_sampling_num = parameter.lane_change_sampling_num;

  // get velocity
  const double current_velocity = util::l2Norm(twist.linear);
  const double acceleration_resolution = std::abs(maximum_deceleration) / lane_change_sampling_num;
  const double target_distance =
    util::getArcLengthToTargetLanelet(original_lanelets, target_lanelets.front(), pose);

  const auto num_to_preferred_lane =
    std::abs(route_handler.getNumLaneToPreferredLane(target_lanelets.back()));

  const auto is_goal_in_route = route_handler.isInGoalRouteSection(target_lanelets.back());
  const auto end_of_lane_dist = std::invoke([&]() {
    const auto required_dist = util::calcLaneChangeBuffer(common_parameter, num_to_preferred_lane);
    if (is_goal_in_route) {
      return util::getSignedDistance(pose, route_handler.getGoalPose(), original_lanelets) -
             required_dist;
    }
    return util::getDistanceToEndOfLane(pose, original_lanelets) - required_dist;
  });

  const auto required_total_min_distance =
    util::calcLaneChangeBuffer(common_parameter, num_to_preferred_lane);

  const auto arc_position_from_current = lanelet::utils::getArcCoordinates(original_lanelets, pose);
  const auto arc_position_from_target = lanelet::utils::getArcCoordinates(target_lanelets, pose);

  const auto target_lane_length = lanelet::utils::getLaneletLength2d(target_lanelets);

  for (double acceleration = 0.0; acceleration >= -maximum_deceleration;
       acceleration -= acceleration_resolution) {
    const double prepare_speed = getExpectedVelocityWhenDecelerate(
      current_velocity, acceleration, lane_change_prepare_duration);

    // skip if velocity becomes less than zero before starting lane change
    if (prepare_speed < 0.0) {
      continue;
    }

    // get path on original lanes
    const double prepare_distance = getDistanceWhenDecelerate(
      current_velocity, acceleration, lane_change_prepare_duration,
      minimum_lane_change_prepare_distance);

    if (prepare_distance < target_distance) {
      continue;
    }

    const auto prepare_segment_reference = getLaneChangePathPrepareSegment(
      route_handler, original_lanelets, arc_position_from_current.length, backward_path_length,
      prepare_distance, std::max(prepare_speed, minimum_lane_change_velocity));

    const auto estimated_shift_length = lanelet::utils::getArcCoordinates(
      target_lanelets, prepare_segment_reference.points.front().point.pose);

    const auto [lane_changing_speed, lane_changing_distance] =
      calcLaneChangingSpeedAndDistanceWhenDecelerate(
        prepare_speed, estimated_shift_length.distance, acceleration, end_of_lane_dist,
        common_parameter, parameter);

    const auto lc_dist = LaneChangePhaseInfo{prepare_distance, lane_changing_distance};

    const auto lane_changing_segment_reference = getLaneChangePathLaneChangingSegment(
      route_handler, target_lanelets, forward_path_length, arc_position_from_target.length,
      target_lane_length, lc_dist, lane_changing_speed, required_total_min_distance);

    if (
      prepare_segment_reference.points.empty() || lane_changing_segment_reference.points.empty()) {
      RCLCPP_ERROR_STREAM(
        rclcpp::get_logger("behavior_path_planner").get_child("lane_change").get_child("util"),
        "reference path is empty!! something wrong...");
      continue;
    }

    const Pose & lane_changing_start_pose = prepare_segment_reference.points.back().point.pose;

    const auto resample_interval =
      calcLaneChangeResamplingInterval(lane_changing_distance, lane_changing_speed);
    const auto target_lane_reference_path = getReferencePathFromTargetLane(
      route_handler, target_lanelets, lane_changing_start_pose, target_lane_length, lc_dist,
      required_total_min_distance, forward_path_length, resample_interval, is_goal_in_route);
    if (target_lane_reference_path.points.empty()) {
      continue;
    }

    const ShiftLine shift_line = getLaneChangeShiftLine(
      prepare_segment_reference, lane_changing_segment_reference, target_lanelets,
      target_lane_reference_path);

    const auto lc_speed = LaneChangePhaseInfo{prepare_speed, lane_changing_speed};
    const auto candidate_path = constructCandidatePath(
      prepare_segment_reference, lane_changing_segment_reference, target_lane_reference_path,
      shift_line, original_lanelets, target_lanelets, acceleration, lc_dist, lc_speed, parameter);

    if (!candidate_path) {
      continue;
    }

    candidate_paths.push_back(*candidate_path);
  }

  return candidate_paths;
}

LaneChangePaths selectValidPaths(
  const LaneChangePaths & paths, const lanelet::ConstLanelets & current_lanes,
  const lanelet::ConstLanelets & target_lanes, const RouteHandler & route_handler,
  const Pose & current_pose, const Pose & goal_pose, const double minimum_lane_change_length)
{
  LaneChangePaths available_paths;

  for (const auto & path : paths) {
    if (hasEnoughDistance(
          path, current_lanes, target_lanes, current_pose, goal_pose, route_handler,
          minimum_lane_change_length)) {
      available_paths.push_back(path);
    }
  }

  return available_paths;
}

bool selectSafePath(
  const LaneChangePaths & paths, const lanelet::ConstLanelets & current_lanes,
  const lanelet::ConstLanelets & target_lanes,
  const PredictedObjects::ConstSharedPtr dynamic_objects, const Pose & current_pose,
  const Twist & current_twist, const BehaviorPathPlannerParameters & common_parameters,
  const LaneChangeParameters & ros_parameters, LaneChangePath * selected_path,
  std::unordered_map<std::string, CollisionCheckDebug> & debug_data)
{
  debug_data.clear();
  for (const auto & path : paths) {
    const size_t current_seg_idx = motion_utils::findFirstNearestSegmentIndexWithSoftConstraints(
      path.path.points, current_pose, common_parameters.ego_nearest_dist_threshold,
      common_parameters.ego_nearest_yaw_threshold);
    Pose ego_pose_before_collision;
    if (isLaneChangePathSafe(
          path, current_lanes, target_lanes, dynamic_objects, current_pose, current_seg_idx,
          current_twist, common_parameters, ros_parameters,
          common_parameters.expected_front_deceleration,
          common_parameters.expected_rear_deceleration, ego_pose_before_collision, debug_data, true,
          path.acceleration)) {
      *selected_path = path;
      return true;
    }
  }
  // set first path for force lane change if no valid path found
  if (!paths.empty()) {
    *selected_path = paths.front();
    return false;
  }

  return false;
}
bool hasEnoughDistance(
  const LaneChangePath & path, const lanelet::ConstLanelets & current_lanes,
  [[maybe_unused]] const lanelet::ConstLanelets & target_lanes, const Pose & current_pose,
  const Pose & goal_pose, const RouteHandler & route_handler,
  const double minimum_lane_change_length)
{
  const double lane_change_total_distance = path.length.sum();
  const int num = std::abs(route_handler.getNumLaneToPreferredLane(target_lanes.back()));
  const auto overall_graphs = route_handler.getOverallGraphPtr();

  const double lane_change_required_distance =
    static_cast<double>(num) * minimum_lane_change_length;

  if (
    lane_change_total_distance + lane_change_required_distance >
    util::getDistanceToEndOfLane(current_pose, current_lanes)) {
    return false;
  }

  if (
    route_handler.isInGoalRouteSection(current_lanes.back()) &&
    lane_change_total_distance + lane_change_required_distance >
      util::getSignedDistance(current_pose, goal_pose, current_lanes)) {
    return false;
  }

  // return is there is no target lanes. Else continue checking
  if (target_lanes.empty()) {
    return true;
  }

  if (lane_change_total_distance > util::getDistanceToEndOfLane(current_pose, target_lanes)) {
    return false;
  }

  return true;
}

bool isLaneChangePathSafe(
  const LaneChangePath & lane_change_path, const lanelet::ConstLanelets & current_lanes,
  const lanelet::ConstLanelets & target_lanes,
  const PredictedObjects::ConstSharedPtr dynamic_objects, const Pose & current_pose,
  const size_t current_seg_idx, const Twist & current_twist,
  const BehaviorPathPlannerParameters & common_parameters,
  const LaneChangeParameters & lane_change_parameters, const double front_decel,
  const double rear_decel, Pose & ego_pose_before_collision,
  std::unordered_map<std::string, CollisionCheckDebug> & debug_data, const bool use_buffer,
  const double acceleration)
{
  if (dynamic_objects == nullptr) {
    return true;
  }

  const auto & path = lane_change_path.path;
  if (path.points.empty() || target_lanes.empty() || current_lanes.empty()) {
    return false;
  }

  const double time_resolution = lane_change_parameters.prediction_time_resolution;
  const auto & lane_change_prepare_duration = lane_change_path.duration.prepare;
  const auto & enable_collision_check_at_prepare_phase =
    lane_change_parameters.enable_collision_check_at_prepare_phase;
  const auto & lane_changing_safety_check_duration = lane_change_path.duration.lane_changing;
  const double check_end_time = lane_change_prepare_duration + lane_changing_safety_check_duration;
  const double min_lc_speed{lane_change_parameters.minimum_lane_change_velocity};

  const auto vehicle_predicted_path = util::convertToPredictedPath(
    path, current_twist, current_pose, static_cast<double>(current_seg_idx), check_end_time,
    time_resolution, acceleration, min_lc_speed);
  const auto prepare_phase_ignore_target_speed_thresh =
    lane_change_parameters.prepare_phase_ignore_target_speed_thresh;

  const auto & vehicle_info = common_parameters.vehicle_info;

  std::vector<size_t> current_lane_object_indices{};
  std::vector<size_t> target_lane_object_indices{};
  std::vector<size_t> other_lane_object_indices{};
  {
    const auto lateral_buffer = (use_buffer) ? 0.5 : 0.0;
    const auto current_obj_filtering_buffer = lateral_buffer + common_parameters.vehicle_width / 2;

    filterObjectIndices(
      *dynamic_objects, lane_change_path.reference_lanelets, lane_change_path.target_lanelets, path,
      current_pose, common_parameters.forward_path_length, current_obj_filtering_buffer,
      current_lane_object_indices, target_lane_object_indices, other_lane_object_indices, true);
  }

  const auto assignDebugData = [](const PredictedObject & obj) {
    CollisionCheckDebug debug;
    const auto key = util::getUuidStr(obj);
    debug.current_pose = obj.kinematics.initial_pose_with_covariance.pose;
    debug.current_twist = obj.kinematics.initial_twist_with_covariance.twist;
    return std::make_pair(key, debug);
  };

  const auto appendDebugInfo =
    [&debug_data](std::pair<std::string, CollisionCheckDebug> & obj, bool && is_allowed) {
      const auto & key = obj.first;
      auto & element = obj.second;
      element.allow_lane_change = is_allowed;
      if (debug_data.find(key) != debug_data.end()) {
        debug_data[key] = element;
      } else {
        debug_data.insert(obj);
      }
    };

  for (const auto & i : current_lane_object_indices) {
    const auto & obj = dynamic_objects->objects.at(i);
    const auto object_speed =
      util::l2Norm(obj.kinematics.initial_twist_with_covariance.twist.linear);
    const double check_start_time = (enable_collision_check_at_prepare_phase &&
                                     (object_speed > prepare_phase_ignore_target_speed_thresh))
                                      ? 0.0
                                      : lane_change_prepare_duration;
    auto current_debug_data = assignDebugData(obj);
    const auto predicted_paths =
      util::getPredictedPathFromObj(obj, lane_change_parameters.use_all_predicted_path);
    for (const auto & obj_path : predicted_paths) {
      if (!util::isSafeInLaneletCollisionCheck(
            current_pose, current_twist, vehicle_predicted_path, vehicle_info, check_start_time,
            check_end_time, time_resolution, obj, obj_path, common_parameters, front_decel,
            rear_decel, ego_pose_before_collision, current_debug_data.second)) {
        appendDebugInfo(current_debug_data, false);
        return false;
      }
    }
  }

  // Collision check for objects in lane change target lane
  for (const auto & i : target_lane_object_indices) {
    const auto & obj = dynamic_objects->objects.at(i);
    auto current_debug_data = assignDebugData(obj);
    current_debug_data.second.ego_predicted_path.push_back(vehicle_predicted_path);
    bool is_object_in_target = false;
    if (lane_change_parameters.use_predicted_path_outside_lanelet) {
      is_object_in_target = true;
    } else {
      for (const auto & llt : target_lanes) {
        if (lanelet::utils::isInLanelet(obj.kinematics.initial_pose_with_covariance.pose, llt)) {
          is_object_in_target = true;
        }
      }
    }

    const auto predicted_paths =
      util::getPredictedPathFromObj(obj, lane_change_parameters.use_all_predicted_path);

    const auto object_speed =
      util::l2Norm(obj.kinematics.initial_twist_with_covariance.twist.linear);
    const double check_start_time = (enable_collision_check_at_prepare_phase &&
                                     (object_speed > prepare_phase_ignore_target_speed_thresh))
                                      ? 0.0
                                      : lane_change_prepare_duration;
    if (is_object_in_target) {
      for (const auto & obj_path : predicted_paths) {
        if (!util::isSafeInLaneletCollisionCheck(
              current_pose, current_twist, vehicle_predicted_path, vehicle_info, check_start_time,
              check_end_time, time_resolution, obj, obj_path, common_parameters, front_decel,
              rear_decel, ego_pose_before_collision, current_debug_data.second)) {
          appendDebugInfo(current_debug_data, false);
          return false;
        }
      }
    } else {
      if (!util::isSafeInFreeSpaceCollisionCheck(
            current_pose, current_twist, vehicle_predicted_path, vehicle_info, check_start_time,
            check_end_time, time_resolution, obj, common_parameters, front_decel, rear_decel,
            current_debug_data.second)) {
        appendDebugInfo(current_debug_data, false);
        return false;
      }
    }
    appendDebugInfo(current_debug_data, true);
  }
  return true;
}

ShiftLine getLaneChangeShiftLine(
  const PathWithLaneId & path1, const PathWithLaneId & path2,
  const lanelet::ConstLanelets & target_lanes, const PathWithLaneId & reference_path)
{
  const Pose & lane_change_start_on_self_lane = path1.points.back().point.pose;
  const Pose & lane_change_end_on_target_lane = path2.points.front().point.pose;
  const ArcCoordinates lane_change_start_on_self_lane_arc =
    lanelet::utils::getArcCoordinates(target_lanes, lane_change_start_on_self_lane);

  ShiftLine shift_line;
  shift_line.end_shift_length = lane_change_start_on_self_lane_arc.distance;
  shift_line.start = lane_change_start_on_self_lane;
  shift_line.end = lane_change_end_on_target_lane;
  shift_line.start_idx =
    motion_utils::findNearestIndex(reference_path.points, lane_change_start_on_self_lane.position);
  shift_line.end_idx =
    motion_utils::findNearestIndex(reference_path.points, lane_change_end_on_target_lane.position);

  RCLCPP_DEBUG(
    rclcpp::get_logger("behavior_path_planner")
      .get_child("lane_change")
      .get_child("util")
      .get_child("getLaneChangeShiftLine"),
    "shift_line distance: %f",
    util::getSignedDistance(shift_line.start, shift_line.end, target_lanes));
  return shift_line;
}

PathWithLaneId getLaneChangePathPrepareSegment(
  const RouteHandler & route_handler, const lanelet::ConstLanelets & original_lanelets,
  const double arc_length_from_current, const double backward_path_length,
  const double prepare_distance, const double prepare_speed)
{
  if (original_lanelets.empty()) {
    return PathWithLaneId();
  }

  const double s_start = arc_length_from_current - backward_path_length;
  const double s_end = arc_length_from_current + prepare_distance;

  RCLCPP_DEBUG(
    rclcpp::get_logger("behavior_path_planner")
      .get_child("lane_change")
      .get_child("util")
      .get_child("getLaneChangePathPrepareSegment"),
    "start: %f, end: %f", s_start, s_end);

  PathWithLaneId prepare_segment =
    route_handler.getCenterLinePath(original_lanelets, s_start, s_end);

  prepare_segment.points.back().point.longitudinal_velocity_mps = std::min(
    prepare_segment.points.back().point.longitudinal_velocity_mps,
    static_cast<float>(prepare_speed));

  return prepare_segment;
}

std::pair<double, double> calcLaneChangingSpeedAndDistanceWhenDecelerate(
  const double velocity, const double shift_length, const double deceleration,
  const double min_total_lc_len, const BehaviorPathPlannerParameters & com_param,
  const LaneChangeParameters & lc_param)
{
  const auto required_time = PathShifter::calcShiftTimeFromJerkAndJerk(
    shift_length, lc_param.lane_changing_lateral_jerk, lc_param.lane_changing_lateral_acc);

  const auto lane_changing_average_speed =
    std::max(velocity + deceleration * 0.5 * required_time, lc_param.minimum_lane_change_velocity);
  const auto expected_dist = lane_changing_average_speed * required_time;
  const auto lane_changing_distance =
    (expected_dist < min_total_lc_len) ? expected_dist : com_param.minimum_lane_change_length;

  RCLCPP_DEBUG(
    rclcpp::get_logger("behavior_path_planner")
      .get_child("lane_change")
      .get_child("util")
      .get_child("calcLaneChangingSpeedAndDistanceWhenDecelerate"),
    "required_time: %f [s] average_speed: %f [m/s], lane_changing_distance : %f [m]", required_time,
    lane_changing_average_speed, lane_changing_distance);

  return {lane_changing_average_speed, lane_changing_distance};
}

PathWithLaneId getLaneChangePathLaneChangingSegment(
  const RouteHandler & route_handler, const lanelet::ConstLanelets & target_lanelets,
  const double forward_path_length, const double arc_length_from_target,
  const double target_lane_length, const LaneChangePhaseInfo dist_prepare_to_lc_end,
  const double lane_changing_speed, const double total_required_min_dist)
{
  const double s_start = std::invoke([&arc_length_from_target, &dist_prepare_to_lc_end,
                                      &target_lane_length, &total_required_min_dist]() {
    const double dist_from_current_pose = arc_length_from_target + dist_prepare_to_lc_end.sum();
    const double end_of_lane_dist_without_buffer = target_lane_length - total_required_min_dist;
    return std::min(dist_from_current_pose, end_of_lane_dist_without_buffer);
  });

  const double s_end =
    std::invoke([&s_start, &forward_path_length, &target_lane_length, &total_required_min_dist]() {
      const double dist_from_start = s_start + forward_path_length;
      const double dist_from_end = target_lane_length - total_required_min_dist;
      return std::max(
        std::min(dist_from_start, dist_from_end), s_start + std::numeric_limits<double>::epsilon());
    });

  RCLCPP_DEBUG(
    rclcpp::get_logger("behavior_path_planner")
      .get_child("lane_change")
      .get_child("util")
      .get_child("getLaneChangePathLaneChangingSegment"),
    "start: %f, end: %f", s_start, s_end);

  PathWithLaneId lane_changing_segment =
    route_handler.getCenterLinePath(target_lanelets, s_start, s_end);
  for (auto & point : lane_changing_segment.points) {
    point.point.longitudinal_velocity_mps =
      std::min(point.point.longitudinal_velocity_mps, static_cast<float>(lane_changing_speed));
  }

  return lane_changing_segment;
}

PathWithLaneId getReferencePathFromTargetLane(
  const RouteHandler & route_handler, const lanelet::ConstLanelets & target_lanes,
  const Pose & lane_changing_start_pose, const double target_lane_length,
  const LaneChangePhaseInfo dist_prepare_to_lc_end, const double min_total_lane_changing_distance,
  const double forward_path_length, const double resample_interval, const bool is_goal_in_route)
{
  const ArcCoordinates lane_change_start_arc_position =
    lanelet::utils::getArcCoordinates(target_lanes, lane_changing_start_pose);

  const double s_start = lane_change_start_arc_position.length;
  const double s_end = std::invoke([&]() {
    const auto dist_from_lc_start = s_start + dist_prepare_to_lc_end.sum() + forward_path_length;
    if (is_goal_in_route) {
      const auto goal_arc_coordinates =
        lanelet::utils::getArcCoordinates(target_lanes, route_handler.getGoalPose());
      const auto dist_to_goal = goal_arc_coordinates.length - min_total_lane_changing_distance;
      return std::min(dist_from_lc_start, dist_to_goal);
    }
    const auto dist_from_end = target_lane_length - min_total_lane_changing_distance;
    return std::min(dist_from_lc_start, dist_from_end);
  });

  RCLCPP_DEBUG(
    rclcpp::get_logger("behavior_path_planner")
      .get_child("lane_change")
      .get_child("util")
      .get_child("getReferencePathFromTargetLane"),
    "start: %f, end: %f", s_start, s_end);

  const auto lane_changing_reference_path =
    route_handler.getCenterLinePath(target_lanes, s_start, s_end);

  return util::resamplePathWithSpline(
    lane_changing_reference_path, resample_interval, true,
    {0.0, dist_prepare_to_lc_end.lane_changing});
}

bool isEgoWithinOriginalLane(
  const lanelet::ConstLanelets & current_lanes, const Pose & current_pose,
  const BehaviorPathPlannerParameters & common_param)
{
  const auto lane_length = lanelet::utils::getLaneletLength2d(current_lanes);
  const auto lane_poly = lanelet::utils::getPolygonFromArcLength(current_lanes, 0, lane_length);
  const auto vehicle_poly =
    util::getVehiclePolygon(current_pose, common_param.vehicle_width, common_param.base_link2front);
  return boost::geometry::within(
    lanelet::utils::to2D(vehicle_poly).basicPolygon(),
    lanelet::utils::to2D(lane_poly).basicPolygon());
}

void get_turn_signal_info(
  const LaneChangePath & lane_change_path, TurnSignalInfo * turn_signal_info)
{
  turn_signal_info->desired_start_point = lane_change_path.turn_signal_info.desired_start_point;
  turn_signal_info->required_start_point = lane_change_path.turn_signal_info.required_start_point;
  turn_signal_info->required_end_point = lane_change_path.turn_signal_info.required_end_point;
  turn_signal_info->desired_end_point = lane_change_path.turn_signal_info.desired_end_point;
}

std::vector<DrivableLanes> generateDrivableLanes(
  const RouteHandler & route_handler, const lanelet::ConstLanelets & current_lanes,
  const lanelet::ConstLanelets & lane_change_lanes)
{
  size_t current_lc_idx = 0;
  std::vector<DrivableLanes> drivable_lanes(current_lanes.size());
  for (size_t i = 0; i < current_lanes.size(); ++i) {
    const auto & current_lane = current_lanes.at(i);
    drivable_lanes.at(i).left_lane = current_lane;
    drivable_lanes.at(i).right_lane = current_lane;

    const auto left_lane = route_handler.getLeftLanelet(current_lane);
    const auto right_lane = route_handler.getRightLanelet(current_lane);
    if (!left_lane && !right_lane) {
      continue;
    }

    for (size_t lc_idx = current_lc_idx; lc_idx < lane_change_lanes.size(); ++lc_idx) {
      const auto & lc_lane = lane_change_lanes.at(lc_idx);
      if (left_lane && lc_lane.id() == left_lane->id()) {
        drivable_lanes.at(i).left_lane = lc_lane;
        current_lc_idx = lc_idx;
        break;
      }

      if (right_lane && lc_lane.id() == right_lane->id()) {
        drivable_lanes.at(i).right_lane = lc_lane;
        current_lc_idx = lc_idx;
        break;
      }
    }
  }

  for (size_t i = current_lc_idx + 1; i < lane_change_lanes.size(); ++i) {
    const auto & lc_lane = lane_change_lanes.at(i);
    DrivableLanes drivable_lane;
    drivable_lane.left_lane = lc_lane;
    drivable_lane.right_lane = lc_lane;
    drivable_lanes.push_back(drivable_lane);
  }

  return drivable_lanes;
}

std::optional<LaneChangePath> getAbortPaths(
  const std::shared_ptr<const PlannerData> & planner_data, const LaneChangePath & selected_path,
  [[maybe_unused]] const Pose & ego_pose_before_collision,
  const BehaviorPathPlannerParameters & common_param,
  [[maybe_unused]] const LaneChangeParameters & lane_change_param)
{
  const auto & route_handler = planner_data->route_handler;
  const auto current_speed = util::l2Norm(planner_data->self_odometry->twist.twist.linear);
  const auto current_pose = planner_data->self_odometry->pose.pose;
  const auto reference_lanelets = selected_path.reference_lanelets;

  const auto ego_nearest_dist_threshold = planner_data->parameters.ego_nearest_dist_threshold;
  const auto ego_nearest_yaw_threshold = planner_data->parameters.ego_nearest_yaw_threshold;
  const double minimum_lane_change_length = util::calcLaneChangeBuffer(common_param, 1);

  const auto & lane_changing_path = selected_path.path;
  const auto lane_changing_end_pose_idx = std::invoke([&]() {
    constexpr double s_start = 0.0;
    const double s_end =
      lanelet::utils::getLaneletLength2d(reference_lanelets) - minimum_lane_change_length;

    const auto ref = route_handler->getCenterLinePath(reference_lanelets, s_start, s_end);
    return motion_utils::findFirstNearestIndexWithSoftConstraints(
      lane_changing_path.points, ref.points.back().point.pose, ego_nearest_dist_threshold,
      ego_nearest_yaw_threshold);
  });

  const auto ego_pose_idx = motion_utils::findFirstNearestIndexWithSoftConstraints(
    lane_changing_path.points, current_pose, ego_nearest_dist_threshold, ego_nearest_yaw_threshold);

  const auto get_abort_idx_and_distance = [&](const double param_time) {
    double turning_point_dist{0.0};
    if (ego_pose_idx > lane_changing_end_pose_idx) {
      return std::make_pair(ego_pose_idx, turning_point_dist);
    }

    constexpr auto min_speed{2.77};
    const auto desired_distance = std::max(min_speed, current_speed) * param_time;
    const auto & points = lane_changing_path.points;
    size_t idx{0};
    for (idx = ego_pose_idx; idx < lane_changing_end_pose_idx; ++idx) {
      const auto dist_to_ego =
        util::getSignedDistance(current_pose, points.at(idx).point.pose, reference_lanelets);
      turning_point_dist = dist_to_ego;
      if (dist_to_ego > desired_distance) {
        break;
      }
    }
    return std::make_pair(idx, turning_point_dist);
  };

  const auto abort_delta_time = lane_change_param.abort_delta_time;
  const auto [abort_start_idx, abort_start_dist] = get_abort_idx_and_distance(abort_delta_time);
  const auto [abort_return_idx, abort_return_dist] =
    get_abort_idx_and_distance(abort_delta_time * 2);

  if (abort_start_idx >= abort_return_idx) {
    RCLCPP_ERROR_STREAM(
      rclcpp::get_logger("behavior_path_planner").get_child("lane_change").get_child("util"),
      "abort start idx and return idx is equal. can't compute abort path.");
    return std::nullopt;
  }

  if (!hasEnoughDistanceToLaneChangeAfterAbort(
        *route_handler, reference_lanelets, current_pose, abort_return_dist, common_param)) {
    RCLCPP_ERROR_STREAM(
      rclcpp::get_logger("behavior_path_planner").get_child("lane_change").get_child("util"),
      "insufficient distance to abort.");
    return std::nullopt;
  }

  const auto abort_start_pose = lane_changing_path.points.at(abort_start_idx).point.pose;
  const auto abort_return_pose = lane_changing_path.points.at(abort_return_idx).point.pose;
  const auto arc_position =
    lanelet::utils::getArcCoordinates(reference_lanelets, abort_return_pose);
  const PathWithLaneId reference_lane_segment = std::invoke([&]() {
    const double minimum_lane_change_length =
      common_param.backward_length_buffer_for_end_of_lane + common_param.minimum_lane_change_length;

    const double s_start = arc_position.length;
    double s_end =
      lanelet::utils::getLaneletLength2d(reference_lanelets) - minimum_lane_change_length;

    if (route_handler->isInGoalRouteSection(selected_path.target_lanelets.back())) {
      const auto goal_arc_coordinates = lanelet::utils::getArcCoordinates(
        selected_path.target_lanelets, route_handler->getGoalPose());
      s_end = std::min(s_end, goal_arc_coordinates.length) - minimum_lane_change_length;
    }
    PathWithLaneId ref = route_handler->getCenterLinePath(reference_lanelets, s_start, s_end);
    ref.points.back().point.longitudinal_velocity_mps = std::min(
      ref.points.back().point.longitudinal_velocity_mps,
      static_cast<float>(lane_change_param.minimum_lane_change_velocity));
    return ref;
  });

  ShiftLine shift_line;
  shift_line.start = abort_start_pose;
  shift_line.end = abort_return_pose;
  shift_line.end_shift_length = -arc_position.distance;
  shift_line.start_idx = abort_start_idx;
  shift_line.end_idx = abort_return_idx;

  PathShifter path_shifter;
  path_shifter.setPath(lane_changing_path);
  path_shifter.addShiftLine(shift_line);
  const auto lateral_jerk = behavior_path_planner::PathShifter::calcJerkFromLatLonDistance(
    shift_line.end_shift_length, abort_start_dist, current_speed);

  if (lateral_jerk > lane_change_param.abort_max_lateral_jerk) {
    return std::nullopt;
  }

  ShiftedPath shifted_path;
  // offset front side
  // bool offset_back = false;
  if (!path_shifter.generate(&shifted_path)) {
    RCLCPP_ERROR_STREAM(
      rclcpp::get_logger("behavior_path_planner").get_child("lane_change").get_child("util"),
      "failed to generate abort shifted path.");
  }

  PathWithLaneId start_to_abort_return_pose;
  start_to_abort_return_pose.points.insert(
    start_to_abort_return_pose.points.end(), shifted_path.path.points.begin(),
    shifted_path.path.points.begin() + abort_return_idx);
  start_to_abort_return_pose.points.insert(
    start_to_abort_return_pose.points.end(), reference_lane_segment.points.begin(),
    reference_lane_segment.points.end());

  auto abort_path = selected_path;
  abort_path.shifted_path = shifted_path;
  abort_path.path = start_to_abort_return_pose;
  abort_path.shift_line = shift_line;
  return std::optional<LaneChangePath>{abort_path};
}

double getLateralShift(const LaneChangePath & path)
{
  const auto start_idx = path.shift_line.start_idx;
  const auto end_idx = path.shift_line.end_idx;

  return path.shifted_path.shift_length.at(end_idx) - path.shifted_path.shift_length.at(start_idx);
}

bool hasEnoughDistanceToLaneChangeAfterAbort(
  const RouteHandler & route_handler, const lanelet::ConstLanelets & current_lanes,
  const Pose & current_pose, const double abort_return_dist,
  const BehaviorPathPlannerParameters & common_param)
{
  const auto minimum_lane_change_distance = common_param.minimum_lane_change_prepare_distance +
                                            common_param.minimum_lane_change_length +
                                            common_param.backward_length_buffer_for_end_of_lane;
  const auto abort_plus_lane_change_distance = abort_return_dist + minimum_lane_change_distance;
  if (abort_plus_lane_change_distance > util::getDistanceToEndOfLane(current_pose, current_lanes)) {
    return false;
  }

  if (
    route_handler.isInGoalRouteSection(current_lanes.back()) &&
    abort_plus_lane_change_distance >
      util::getSignedDistance(current_pose, route_handler.getGoalPose(), current_lanes)) {
    return false;
  }

  return true;
}
}  // namespace behavior_path_planner::lane_change_utils
