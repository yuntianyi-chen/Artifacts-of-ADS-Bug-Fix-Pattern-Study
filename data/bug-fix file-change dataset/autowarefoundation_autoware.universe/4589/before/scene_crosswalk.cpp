// Copyright 2020 Tier IV, Inc.
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

#include "scene_crosswalk.hpp"

#include <autoware_auto_tf2/tf2_autoware_auto_msgs.hpp>
#include <behavior_velocity_planner_common/utilization/path_utilization.hpp>
#include <behavior_velocity_planner_common/utilization/util.hpp>
#include <motion_utils/distance/distance.hpp>
#include <rclcpp/rclcpp.hpp>
#include <tier4_autoware_utils/tier4_autoware_utils.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <vector>

namespace behavior_velocity_planner
{
namespace bg = boost::geometry;
using motion_utils::calcArcLength;
using motion_utils::calcDecelDistWithJerkAndAccConstraints;
using motion_utils::calcLateralOffset;
using motion_utils::calcLongitudinalOffsetPoint;
using motion_utils::calcLongitudinalOffsetPose;
using motion_utils::calcSignedArcLength;
using motion_utils::findNearestSegmentIndex;
using motion_utils::insertTargetPoint;
using motion_utils::resamplePath;
using tier4_autoware_utils::createPoint;
using tier4_autoware_utils::getPose;
using tier4_autoware_utils::Point2d;
using tier4_autoware_utils::Polygon2d;
using tier4_autoware_utils::pose2transform;
using tier4_autoware_utils::toHexString;

namespace
{
geometry_msgs::msg::Point32 createPoint32(const double x, const double y, const double z)
{
  geometry_msgs::msg::Point32 p;
  p.x = x;
  p.y = y;
  p.z = z;
  return p;
}

std::vector<geometry_msgs::msg::Point> toGeometryPointVector(
  const Polygon2d & polygon, const double z)
{
  std::vector<geometry_msgs::msg::Point> points;
  for (const auto & p : polygon.outer()) {
    points.push_back(createPoint(p.x(), p.y(), z));
  }
  return points;
}

Polygon2d createOneStepPolygon(
  const geometry_msgs::msg::Pose & p_front, const geometry_msgs::msg::Pose & p_back,
  const geometry_msgs::msg::Polygon & base_polygon)
{
  Polygon2d one_step_polygon{};

  {
    geometry_msgs::msg::Polygon out_polygon{};
    geometry_msgs::msg::TransformStamped geometry_tf{};
    geometry_tf.transform = pose2transform(p_front);
    tf2::doTransform(base_polygon, out_polygon, geometry_tf);

    for (const auto & p : out_polygon.points) {
      one_step_polygon.outer().push_back(Point2d(p.x, p.y));
    }
  }

  {
    geometry_msgs::msg::Polygon out_polygon{};
    geometry_msgs::msg::TransformStamped geometry_tf{};
    geometry_tf.transform = pose2transform(p_back);
    tf2::doTransform(base_polygon, out_polygon, geometry_tf);

    for (const auto & p : out_polygon.points) {
      one_step_polygon.outer().push_back(Point2d(p.x, p.y));
    }
  }

  Polygon2d hull_polygon{};
  bg::convex_hull(one_step_polygon, hull_polygon);
  bg::correct(hull_polygon);

  return hull_polygon;
}

void sortCrosswalksByDistance(
  const PathWithLaneId & ego_path, const geometry_msgs::msg::Point & ego_pos,
  lanelet::ConstLanelets & crosswalks)
{
  const auto compare = [&](const lanelet::ConstLanelet & l1, const lanelet::ConstLanelet & l2) {
    const auto l1_intersects =
      getPolygonIntersects(ego_path, l1.polygon2d().basicPolygon(), ego_pos, 2);
    const auto l2_intersects =
      getPolygonIntersects(ego_path, l2.polygon2d().basicPolygon(), ego_pos, 2);

    if (l1_intersects.empty() || l2_intersects.empty()) {
      return true;
    }

    const auto dist_l1 = calcSignedArcLength(ego_path.points, size_t(0), l1_intersects.front());

    const auto dist_l2 = calcSignedArcLength(ego_path.points, size_t(0), l2_intersects.front());

    return dist_l1 < dist_l2;
  };

  std::sort(crosswalks.begin(), crosswalks.end(), compare);
}

std::vector<Point2d> calcOverlappingPoints(const Polygon2d & polygon1, const Polygon2d & polygon2)
{
  // NOTE: If one polygon is fully inside the other polygon, the result is empty.
  std::vector<Point2d> intersection{};
  bg::intersection(polygon1, polygon2, intersection);

  if (bg::within(polygon1, polygon2)) {
    for (const auto & p : polygon1.outer()) {
      intersection.push_back(Point2d{p.x(), p.y()});
    }
  }
  if (bg::within(polygon2, polygon1)) {
    for (const auto & p : polygon2.outer()) {
      intersection.push_back(Point2d{p.x(), p.y()});
    }
  }

  return intersection;
}

StopFactor createStopFactor(
  const geometry_msgs::msg::Pose & stop_pose,
  const std::vector<geometry_msgs::msg::Point> stop_factor_points = {})
{
  StopFactor stop_factor;
  stop_factor.stop_factor_points = stop_factor_points;
  stop_factor.stop_pose = stop_pose;
  return stop_factor;
}

std::optional<geometry_msgs::msg::Pose> toStdOptional(
  const boost::optional<geometry_msgs::msg::Pose> & boost_pose)
{
  if (boost_pose) {
    return *boost_pose;
  }
  return std::nullopt;
}
}  // namespace

CrosswalkModule::CrosswalkModule(
  const int64_t module_id, const lanelet::LaneletMapPtr & lanelet_map_ptr,
  const PlannerParam & planner_param, const bool use_regulatory_element,
  const rclcpp::Logger & logger, const rclcpp::Clock::SharedPtr clock)
: SceneModuleInterface(module_id, logger, clock),
  module_id_(module_id),
  planner_param_(planner_param),
  use_regulatory_element_(use_regulatory_element)
{
  velocity_factor_.init(VelocityFactor::CROSSWALK);
  passed_safety_slow_point_ = false;

  if (use_regulatory_element_) {
    const auto reg_elem_ptr = std::dynamic_pointer_cast<const lanelet::autoware::Crosswalk>(
      lanelet_map_ptr->regulatoryElementLayer.get(module_id));
    stop_lines_ = reg_elem_ptr->stopLines();
    crosswalk_ = reg_elem_ptr->crosswalkLanelet();
  } else {
    const auto stop_line = getStopLineFromMap(module_id_, lanelet_map_ptr, "crosswalk_id");
    if (stop_line) {
      stop_lines_.push_back(*stop_line);
    }
    crosswalk_ = lanelet_map_ptr->laneletLayer.get(module_id);
  }
}

bool CrosswalkModule::modifyPathVelocity(PathWithLaneId * path, StopReason * stop_reason)
{
  if (path->points.size() < 2) {
    RCLCPP_DEBUG(logger_, "Do not interpolate because path size is less than 2.");
    return {};
  }

  stop_watch_.tic("total_processing_time");
  RCLCPP_INFO_EXPRESSION(
    logger_, planner_param_.show_processing_time,
    "=========== module_id: %ld ===========", module_id_);

  *stop_reason = planning_utils::initializeStopReason(StopReason::CROSSWALK);
  const auto & ego_pos = planner_data_->current_odometry->pose.position;
  const auto objects_ptr = planner_data_->predicted_objects;

  // Initialize debug data
  debug_data_ = DebugData(planner_data_);
  for (const auto & p : crosswalk_.polygon2d().basicPolygon()) {
    debug_data_.crosswalk_polygon.push_back(createPoint(p.x(), p.y(), ego_pos.z));
  }
  recordTime(1);

  // Calculate intersection between path and crosswalks
  const auto path_intersects =
    getPolygonIntersects(*path, crosswalk_.polygon2d().basicPolygon(), ego_pos, 2);

  // Apply safety slow down speed if defined in Lanelet2 map
  if (crosswalk_.hasAttribute("safety_slow_down_speed")) {
    applySafetySlowDownSpeed(*path, path_intersects);
  }
  recordTime(2);

  // Calculate stop point with margin
  const auto p_stop_line = getStopPointWithMargin(*path, path_intersects);
  // TODO(murooka) add a guard of p_stop_line
  const auto default_stop_pose = toStdOptional(
    calcLongitudinalOffsetPose(path->points, p_stop_line->first, p_stop_line->second));

  // Resample path sparsely for less computation cost
  constexpr double resample_interval = 4.0;
  const auto sparse_resample_path =
    resamplePath(*path, resample_interval, false, true, true, false);

  // Decide to stop for crosswalk users
  const auto stop_factor_for_crosswalk_users = checkStopForCrosswalkUsers(
    *path, sparse_resample_path, p_stop_line, path_intersects, default_stop_pose);

  // Decide to stop for stuck vehicle
  const auto stop_factor_for_stuck_vehicles = checkStopForStuckVehicles(
    sparse_resample_path, objects_ptr->objects, path_intersects, default_stop_pose);

  // Get nearest stop factor
  const auto nearest_stop_factor =
    getNearestStopFactor(*path, stop_factor_for_crosswalk_users, stop_factor_for_stuck_vehicles);
  recordTime(3);

  // Set safe or unsafe
  setSafe(!nearest_stop_factor);

  // Set distance
  // NOTE: If no stop point is inserted, distance to the virtual stop line has to be calculated.
  setDistanceToStop(*path, default_stop_pose, nearest_stop_factor);

  // plan Go/Stop
  if (isActivated()) {
    planGo(*path, nearest_stop_factor);
  } else {
    planStop(*path, nearest_stop_factor, default_stop_pose, stop_reason);
  }
  recordTime(4);

  return true;
}

// NOTE: The stop point will be the returned point with the margin.
std::optional<std::pair<geometry_msgs::msg::Point, double>> CrosswalkModule::getStopPointWithMargin(
  const PathWithLaneId & ego_path,
  const std::vector<geometry_msgs::msg::Point> & path_intersects) const
{
  const auto & ego_pos = planner_data_->current_odometry->pose.position;
  const auto & base_link2front = planner_data_->vehicle_info_.max_longitudinal_offset_m;

  // If stop lines are found in the LL2 map.
  for (const auto & stop_line : stop_lines_) {
    const auto p_stop_lines = getLinestringIntersects(
      ego_path, lanelet::utils::to2D(stop_line).basicLineString(), ego_pos, 2);
    if (!p_stop_lines.empty()) {
      return std::make_pair(
        p_stop_lines.front(), -planner_param_.stop_distance_from_object - base_link2front);
    }
  }

  // If stop lines are not found in the LL2 map.
  if (!path_intersects.empty()) {
    return std::make_pair(
      path_intersects.front(), -planner_param_.stop_distance_from_crosswalk - base_link2front);
  }

  return {};
}

std::optional<StopFactor> CrosswalkModule::checkStopForCrosswalkUsers(
  const PathWithLaneId & ego_path, const PathWithLaneId & sparse_resample_path,
  const std::optional<std::pair<geometry_msgs::msg::Point, double>> & p_stop_line,
  const std::vector<geometry_msgs::msg::Point> & path_intersects,
  const std::optional<geometry_msgs::msg::Pose> & default_stop_pose)
{
  const auto & ego_pos = planner_data_->current_odometry->pose.position;
  const auto base_link2front = planner_data_->vehicle_info_.max_longitudinal_offset_m;
  const auto dist_ego_to_stop =
    calcSignedArcLength(ego_path.points, ego_pos, p_stop_line->first) + p_stop_line->second;

  // Calculate attention range for crosswalk
  const auto crosswalk_attention_range = getAttentionRange(sparse_resample_path, path_intersects);

  // Get attention area, which is ego's footprints on the crosswalk
  const auto attention_area = getAttentionArea(sparse_resample_path, crosswalk_attention_range);

  // Update object state
  updateObjectState(
    dist_ego_to_stop, sparse_resample_path, crosswalk_attention_range, attention_area);

  // Check if ego moves forward enough to ignore yield.
  if (!path_intersects.empty()) {
    const double base_link2front = planner_data_->vehicle_info_.max_longitudinal_offset_m;
    const double dist_ego2crosswalk =
      calcSignedArcLength(ego_path.points, ego_pos, path_intersects.front());
    if (dist_ego2crosswalk - base_link2front < planner_param_.max_offset_to_crosswalk_for_yield) {
      return {};
    }
  }

  // Check pedestrian for stop
  // NOTE: first stop point and its minimum distance from ego to stop
  std::optional<std::pair<geometry_msgs::msg::Point, double>> nearest_stop_info;
  std::vector<geometry_msgs::msg::Point> stop_factor_points;
  for (const auto & object : object_info_manager_.getObject()) {
    const auto & collision_point = object.collision_point;
    if (collision_point) {
      const auto & collision_state = object.collision_state;
      if (collision_state != CollisionState::YIELD) {
        continue;
      }

      stop_factor_points.push_back(object.position);

      const auto dist_ego2cp =
        calcSignedArcLength(
          sparse_resample_path.points, ego_pos, collision_point->collision_point) -
        planner_param_.stop_distance_from_object;
      if (!nearest_stop_info || dist_ego2cp - base_link2front < nearest_stop_info->second) {
        nearest_stop_info =
          std::make_pair(collision_point->collision_point, dist_ego2cp - base_link2front);
      }
    }
  }

  // Check if stop is required
  if (!nearest_stop_info) {
    return {};
  }

  // Check if the ego should stop beyond the stop line.
  const bool stop_at_stop_line =
    dist_ego_to_stop < nearest_stop_info->second &&
    nearest_stop_info->second < dist_ego_to_stop + planner_param_.far_object_threshold;

  if (stop_at_stop_line) {
    // Stop at the stop line
    if (default_stop_pose) {
      return createStopFactor(*default_stop_pose, stop_factor_points);
    }
  } else {
    // Stop beyond the stop line
    const auto stop_pose = calcLongitudinalOffsetPose(
      ego_path.points, nearest_stop_info->first, planner_param_.stop_distance_from_object);
    if (stop_pose) {
      return createStopFactor(*stop_pose, stop_factor_points);
    }
  }
  return {};
}

std::pair<double, double> CrosswalkModule::getAttentionRange(
  const PathWithLaneId & ego_path, const std::vector<geometry_msgs::msg::Point> & path_intersects)
{
  stop_watch_.tic(__func__);

  if (path_intersects.size() < 2) {
    return std::make_pair(0.0, 0.0);
  }

  const auto & ego_pos = planner_data_->current_odometry->pose.position;
  const auto near_attention_range =
    calcSignedArcLength(ego_path.points, ego_pos, path_intersects.front()) -
    planner_param_.crosswalk_attention_range;
  const auto far_attention_range =
    calcSignedArcLength(ego_path.points, ego_pos, path_intersects.back()) +
    planner_param_.crosswalk_attention_range;

  const auto [clamped_near_attention_range, clamped_far_attention_range] =
    clampAttentionRangeByNeighborCrosswalks(ego_path, near_attention_range, far_attention_range);

  RCLCPP_INFO_EXPRESSION(
    logger_, planner_param_.show_processing_time, "%s : %f ms", __func__,
    stop_watch_.toc(__func__, true));

  return std::make_pair(
    std::max(0.0, clamped_near_attention_range), std::max(0.0, clamped_far_attention_range));
}

void CrosswalkModule::insertDecelPointWithDebugInfo(
  const geometry_msgs::msg::Point & stop_point, const float target_velocity,
  PathWithLaneId & output) const
{
  const auto stop_pose = planning_utils::insertDecelPoint(stop_point, output, target_velocity);
  if (!stop_pose) {
    return;
  }

  debug_data_.first_stop_pose = getPose(*stop_pose);

  if (std::abs(target_velocity) < 1e-3) {
    debug_data_.stop_poses.push_back(*stop_pose);
  } else {
    debug_data_.slow_poses.push_back(*stop_pose);
  }
}

float CrosswalkModule::calcTargetVelocity(
  const geometry_msgs::msg::Point & stop_point, const PathWithLaneId & ego_path) const
{
  const auto max_jerk = planner_param_.max_slow_down_jerk;
  const auto max_accel = planner_param_.max_slow_down_accel;
  const auto & ego_pos = planner_data_->current_odometry->pose.position;
  const auto ego_vel = planner_data_->current_velocity->twist.linear.x;

  if (ego_vel < planner_param_.no_relax_velocity) {
    return 0.0;
  }

  const auto ego_acc = planner_data_->current_acceleration->accel.accel.linear.x;
  const auto dist_deceleration = calcSignedArcLength(ego_path.points, ego_pos, stop_point);
  const auto feasible_velocity = planning_utils::calcDecelerationVelocityFromDistanceToTarget(
    max_jerk, max_accel, ego_acc, ego_vel, dist_deceleration);

  constexpr double margin_velocity = 0.5;  // 1.8 km/h
  return margin_velocity < feasible_velocity ? feasible_velocity : 0.0;
}

std::pair<double, double> CrosswalkModule::clampAttentionRangeByNeighborCrosswalks(
  const PathWithLaneId & ego_path, const double near_attention_range,
  const double far_attention_range)
{
  stop_watch_.tic(__func__);

  const auto & ego_pos = planner_data_->current_odometry->pose.position;

  const auto p_near = calcLongitudinalOffsetPoint(ego_path.points, ego_pos, near_attention_range);
  const auto p_far = calcLongitudinalOffsetPoint(ego_path.points, ego_pos, far_attention_range);

  if (!p_near || !p_far) {
    return std::make_pair(near_attention_range, far_attention_range);
  }

  const auto near_idx = findNearestSegmentIndex(ego_path.points, p_near.get());
  const auto far_idx = findNearestSegmentIndex(ego_path.points, p_far.get()) + 1;

  std::set<int64_t> lane_ids;
  for (size_t i = near_idx; i < far_idx; ++i) {
    for (const auto & id : ego_path.points.at(i).lane_ids) {
      lane_ids.insert(id);
    }
  }

  lanelet::ConstLanelets crosswalks{};
  constexpr int PEDESTRIAN_GRAPH_ID = 1;
  const auto lanelet_map_ptr = planner_data_->route_handler_->getLaneletMapPtr();
  const auto overall_graphs_ptr = planner_data_->route_handler_->getOverallGraphPtr();

  for (const auto & id : lane_ids) {
    const auto ll = lanelet_map_ptr->laneletLayer.get(id);
    const auto conflicting_crosswalks =
      overall_graphs_ptr->conflictingInGraph(ll, PEDESTRIAN_GRAPH_ID);
    for (const auto & crosswalk : conflicting_crosswalks) {
      const auto itr = std::find_if(
        crosswalks.begin(), crosswalks.end(),
        [&](const lanelet::ConstLanelet & ll) { return ll.id() == crosswalk.id(); });
      if (itr == crosswalks.end()) {
        crosswalks.push_back(crosswalk);
      }
    }
  }

  sortCrosswalksByDistance(ego_path, ego_pos, crosswalks);

  std::optional<lanelet::ConstLanelet> prev_crosswalk{std::nullopt};
  std::optional<lanelet::ConstLanelet> next_crosswalk{std::nullopt};

  if (!crosswalks.empty()) {
    for (size_t i = 0; i < crosswalks.size() - 1; ++i) {
      const auto ll_front = crosswalks.at(i);
      const auto ll_back = crosswalks.at(i + 1);

      if (ll_front.id() == module_id_ && ll_back.id() != module_id_) {
        next_crosswalk = ll_back;
      }

      if (ll_front.id() != module_id_ && ll_back.id() == module_id_) {
        prev_crosswalk = ll_front;
      }
    }
  }

  const double clamped_near_attention_range = [&]() {
    if (!prev_crosswalk) {
      return near_attention_range;
    }
    auto reverse_ego_path = ego_path;
    std::reverse(reverse_ego_path.points.begin(), reverse_ego_path.points.end());

    const auto prev_crosswalk_intersects = getPolygonIntersects(
      reverse_ego_path, prev_crosswalk->polygon2d().basicPolygon(), ego_pos, 2);
    if (prev_crosswalk_intersects.empty()) {
      return near_attention_range;
    }

    const auto dist_to_prev_crosswalk =
      calcSignedArcLength(ego_path.points, ego_pos, prev_crosswalk_intersects.front());
    return std::max(near_attention_range, dist_to_prev_crosswalk);
  }();

  const double clamped_far_attention_range = [&]() {
    if (!next_crosswalk) {
      return far_attention_range;
    }
    const auto next_crosswalk_intersects =
      getPolygonIntersects(ego_path, next_crosswalk->polygon2d().basicPolygon(), ego_pos, 2);

    if (next_crosswalk_intersects.empty()) {
      return far_attention_range;
    }

    const auto dist_to_next_crosswalk =
      calcSignedArcLength(ego_path.points, ego_pos, next_crosswalk_intersects.front());
    return std::min(far_attention_range, dist_to_next_crosswalk);
  }();

  const auto update_p_near =
    calcLongitudinalOffsetPoint(ego_path.points, ego_pos, near_attention_range);
  const auto update_p_far =
    calcLongitudinalOffsetPoint(ego_path.points, ego_pos, far_attention_range);

  if (update_p_near && update_p_far) {
    debug_data_.range_near_point = update_p_near.get();
    debug_data_.range_far_point = update_p_far.get();
  }

  RCLCPP_INFO_EXPRESSION(
    logger_, planner_param_.show_processing_time, "%s : %f ms", __func__,
    stop_watch_.toc(__func__, true));

  return std::make_pair(clamped_near_attention_range, clamped_far_attention_range);
}

std::optional<CollisionPoint> CrosswalkModule::getCollisionPoint(
  const PathWithLaneId & ego_path, const PredictedObject & object,
  const std::pair<double, double> & crosswalk_attention_range, const Polygon2d & attention_area)
{
  stop_watch_.tic(__func__);

  std::vector<CollisionPoint> ret{};

  const auto & obj_vel = object.kinematics.initial_twist_with_covariance.twist.linear;

  const auto & ego_pos = planner_data_->current_odometry->pose.position;
  const auto & ego_vel = planner_data_->current_velocity->twist.linear;

  const auto obj_polygon =
    createObjectPolygon(object.shape.dimensions.x, object.shape.dimensions.y);

  double minimum_stop_dist = std::numeric_limits<double>::max();
  std::optional<CollisionPoint> nearest_collision_point{std::nullopt};
  for (const auto & obj_path : object.kinematics.predicted_paths) {
    for (size_t i = 0; i < obj_path.path.size() - 1; ++i) {
      const auto & p_obj_front = obj_path.path.at(i);
      const auto & p_obj_back = obj_path.path.at(i + 1);
      const auto obj_one_step_polygon = createOneStepPolygon(p_obj_front, p_obj_back, obj_polygon);

      // Calculate intersection points between object and attention area
      const auto tmp_intersection = calcOverlappingPoints(obj_one_step_polygon, attention_area);
      if (tmp_intersection.empty()) {
        continue;
      }

      // Calculate nearest collision point among collisions with a predicted path
      double local_minimum_stop_dist = std::numeric_limits<double>::max();
      geometry_msgs::msg::Point local_nearest_collision_point{};
      for (const auto & p : tmp_intersection) {
        const auto cp = createPoint(p.x(), p.y(), ego_pos.z);
        const auto dist_ego2cp = calcSignedArcLength(ego_path.points, ego_pos, cp);

        if (dist_ego2cp < local_minimum_stop_dist) {
          local_minimum_stop_dist = dist_ego2cp;
          local_nearest_collision_point = cp;
        }
      }

      const auto dist_ego2cp =
        calcSignedArcLength(ego_path.points, ego_pos, local_nearest_collision_point);
      constexpr double eps = 1e-3;
      const auto dist_obj2cp =
        calcArcLength(obj_path.path) < eps
          ? 0.0
          : calcSignedArcLength(obj_path.path, size_t(0), local_nearest_collision_point);

      if (
        dist_ego2cp < crosswalk_attention_range.first ||
        crosswalk_attention_range.second < dist_ego2cp) {
        continue;
      }

      // Calculate nearest collision point among predicted paths
      if (dist_ego2cp < minimum_stop_dist) {
        minimum_stop_dist = dist_ego2cp;
        nearest_collision_point = createCollisionPoint(
          local_nearest_collision_point, dist_ego2cp, dist_obj2cp, ego_vel, obj_vel);
        debug_data_.obj_polygons.push_back(toGeometryPointVector(obj_one_step_polygon, ego_pos.z));
      }

      break;
    }
  }

  RCLCPP_INFO_EXPRESSION(
    logger_, planner_param_.show_processing_time, "%s : %f ms", __func__,
    stop_watch_.toc(__func__, true));

  return nearest_collision_point;
}

CollisionPoint CrosswalkModule::createCollisionPoint(
  const geometry_msgs::msg::Point & nearest_collision_point, const double dist_ego2cp,
  const double dist_obj2cp, const geometry_msgs::msg::Vector3 & ego_vel,
  const geometry_msgs::msg::Vector3 & obj_vel) const
{
  constexpr double min_ego_velocity = 1.38;  // [m/s]

  const auto & base_link2front = planner_data_->vehicle_info_.max_longitudinal_offset_m;

  const auto estimated_velocity = std::hypot(obj_vel.x, obj_vel.y);
  const auto velocity = std::max(planner_param_.min_object_velocity, estimated_velocity);

  CollisionPoint collision_point{};
  collision_point.collision_point = nearest_collision_point;
  collision_point.time_to_collision =
    std::max(0.0, dist_ego2cp - planner_param_.stop_distance_from_object - base_link2front) /
    std::max(ego_vel.x, min_ego_velocity);
  collision_point.time_to_vehicle = std::max(0.0, dist_obj2cp) / velocity;

  return collision_point;
}

void CrosswalkModule::applySafetySlowDownSpeed(
  PathWithLaneId & output, const std::vector<geometry_msgs::msg::Point> & path_intersects)
{
  if (path_intersects.empty()) {
    return;
  }

  const auto & ego_pos = planner_data_->current_odometry->pose.position;
  const auto ego_path = output;

  // Safety slow down speed in [m/s]
  const auto & safety_slow_down_speed =
    static_cast<float>(crosswalk_.attribute("safety_slow_down_speed").asDouble().get());

  if (!passed_safety_slow_point_) {
    // Safety slow down distance [m]
    const double safety_slow_down_distance =
      crosswalk_.attributeOr("safety_slow_down_distance", 2.0);

    // the range until to the point where ego will have a const safety slow down speed
    const double safety_slow_margin =
      planner_data_->vehicle_info_.max_longitudinal_offset_m + safety_slow_down_distance;
    const double safety_slow_point_range =
      calcSignedArcLength(ego_path.points, ego_pos, path_intersects.front()) - safety_slow_margin;

    const auto & p_safety_slow =
      calcLongitudinalOffsetPoint(ego_path.points, ego_pos, safety_slow_point_range);

    insertDecelPointWithDebugInfo(p_safety_slow.get(), safety_slow_down_speed, output);

    if (safety_slow_point_range < 0.0) {
      passed_safety_slow_point_ = true;
    }
  } else {
    // the range until to the point where ego will start accelerate
    const double safety_slow_end_point_range =
      calcSignedArcLength(ego_path.points, ego_pos, path_intersects.back());

    if (0.0 < safety_slow_end_point_range) {
      // insert constant ego speed until the end of the crosswalk
      for (auto & p : output.points) {
        const float original_velocity = p.point.longitudinal_velocity_mps;
        p.point.longitudinal_velocity_mps = std::min(original_velocity, safety_slow_down_speed);
      }
    }
  }
}

Polygon2d CrosswalkModule::getAttentionArea(
  const PathWithLaneId & sparse_resample_path,
  const std::pair<double, double> & crosswalk_attention_range) const
{
  const auto & ego_pos = planner_data_->current_odometry->pose.position;
  const auto ego_polygon = createVehiclePolygon(planner_data_->vehicle_info_);

  Polygon2d attention_area;
  for (size_t j = 0; j < sparse_resample_path.points.size() - 1; ++j) {
    const auto & p_ego_front = sparse_resample_path.points.at(j).point.pose;
    const auto & p_ego_back = sparse_resample_path.points.at(j + 1).point.pose;
    const auto front_length =
      calcSignedArcLength(sparse_resample_path.points, ego_pos, p_ego_front.position);
    const auto back_length =
      calcSignedArcLength(sparse_resample_path.points, ego_pos, p_ego_back.position);

    if (back_length < crosswalk_attention_range.first) {
      continue;
    }

    if (crosswalk_attention_range.second < front_length) {
      break;
    }

    const auto ego_one_step_polygon = createOneStepPolygon(p_ego_front, p_ego_back, ego_polygon);

    debug_data_.ego_polygons.push_back(toGeometryPointVector(ego_one_step_polygon, ego_pos.z));

    std::vector<Polygon2d> unions;
    bg::union_(attention_area, ego_one_step_polygon, unions);
    if (!unions.empty()) {
      attention_area = unions.front();
      bg::correct(attention_area);
    }
  }

  return attention_area;
}

std::optional<StopFactor> CrosswalkModule::checkStopForStuckVehicles(
  const PathWithLaneId & ego_path, const std::vector<PredictedObject> & objects,
  const std::vector<geometry_msgs::msg::Point> & path_intersects,
  const std::optional<geometry_msgs::msg::Pose> & stop_pose) const
{
  const auto & p = planner_param_;

  if (path_intersects.size() < 2 || !stop_pose) {
    return {};
  }

  for (const auto & object : objects) {
    if (!isVehicle(object)) {
      continue;
    }

    const auto & obj_vel = object.kinematics.initial_twist_with_covariance.twist.linear;
    if (p.stuck_vehicle_velocity < std::hypot(obj_vel.x, obj_vel.y)) {
      continue;
    }

    const auto & obj_pos = object.kinematics.initial_pose_with_covariance.pose.position;
    const auto lateral_offset = calcLateralOffset(ego_path.points, obj_pos);
    if (p.max_stuck_vehicle_lateral_offset < std::abs(lateral_offset)) {
      continue;
    }

    const auto & ego_pos = planner_data_->current_odometry->pose.position;
    const auto ego_vel = planner_data_->current_velocity->twist.linear.x;
    const auto ego_acc = planner_data_->current_acceleration->accel.accel.linear.x;

    const double near_attention_range =
      calcSignedArcLength(ego_path.points, ego_pos, path_intersects.back());
    const double far_attention_range = near_attention_range + p.stuck_vehicle_attention_range;

    const auto dist_ego2obj = calcSignedArcLength(ego_path.points, ego_pos, obj_pos);

    if (near_attention_range < dist_ego2obj && dist_ego2obj < far_attention_range) {
      // Plan STOP considering min_acc, max_jerk and min_jerk.
      const auto min_feasible_dist_ego2stop = calcDecelDistWithJerkAndAccConstraints(
        ego_vel, 0.0, ego_acc, p.min_acc_for_stuck_vehicle, p.max_jerk_for_stuck_vehicle,
        p.min_jerk_for_stuck_vehicle);
      if (!min_feasible_dist_ego2stop) {
        continue;
      }

      const double dist_ego2stop =
        calcSignedArcLength(ego_path.points, ego_pos, stop_pose->position);
      const double feasible_dist_ego2stop = std::max(*min_feasible_dist_ego2stop, dist_ego2stop);
      const double dist_to_ego =
        calcSignedArcLength(ego_path.points, ego_path.points.front().point.pose.position, ego_pos);

      const auto feasible_stop_pose =
        calcLongitudinalOffsetPose(ego_path.points, 0, dist_to_ego + feasible_dist_ego2stop);
      if (!feasible_stop_pose) {
        continue;
      }

      return createStopFactor(*feasible_stop_pose, {obj_pos});
    }
  }

  return {};
}

std::optional<StopFactor> CrosswalkModule::getNearestStopFactor(
  const PathWithLaneId & ego_path,
  const std::optional<StopFactor> & stop_factor_for_crosswalk_users,
  const std::optional<StopFactor> & stop_factor_for_stuck_vehicles)
{
  const auto get_distance_to_stop = [&](const auto stop_factor) -> std::optional<double> {
    const auto & ego_pos = planner_data_->current_odometry->pose.position;
    if (!stop_factor) return std::nullopt;
    return calcSignedArcLength(ego_path.points, ego_pos, stop_factor->stop_pose.position);
  };
  const auto dist_to_stop_for_crosswalk_users =
    get_distance_to_stop(stop_factor_for_crosswalk_users);
  const auto dist_to_stop_for_stuck_vehicles = get_distance_to_stop(stop_factor_for_stuck_vehicles);

  if (dist_to_stop_for_crosswalk_users) {
    if (dist_to_stop_for_stuck_vehicles) {
      if (*dist_to_stop_for_stuck_vehicles < *dist_to_stop_for_crosswalk_users) {
        return stop_factor_for_stuck_vehicles;
      }
    }
    return stop_factor_for_crosswalk_users;
  }

  if (dist_to_stop_for_stuck_vehicles) {
    return stop_factor_for_stuck_vehicles;
  }

  return {};
}

void CrosswalkModule::updateObjectState(
  const double dist_ego_to_stop, const PathWithLaneId & sparse_resample_path,
  const std::pair<double, double> & crosswalk_attention_range, const Polygon2d & attention_area)
{
  const auto & objects_ptr = planner_data_->predicted_objects;

  const auto traffic_lights_reg_elems =
    crosswalk_.regulatoryElementsAs<const lanelet::TrafficLight>();
  const bool has_traffic_light = !traffic_lights_reg_elems.empty();

  // Check if ego is yielding
  const bool is_ego_yielding = [&]() {
    const auto has_reached_stop_point = dist_ego_to_stop < planner_param_.stop_position_threshold;

    return planner_data_->isVehicleStopped(planner_param_.timeout_ego_stop_for_yield) &&
           has_reached_stop_point;
  }();

  const auto ignore_crosswalk = isRedSignalForPedestrians();
  debug_data_.ignore_crosswalk = ignore_crosswalk;

  // Update object state
  object_info_manager_.init();
  for (const auto & object : objects_ptr->objects) {
    const auto obj_uuid = toHexString(object.object_id);
    const auto & obj_pos = object.kinematics.initial_pose_with_covariance.pose.position;
    const auto & obj_vel = object.kinematics.initial_twist_with_covariance.twist.linear;

    // calculate collision point and state
    if (!isCrosswalkUserType(object)) {
      continue;
    }
    if (ignore_crosswalk) {
      continue;
    }

    const auto collision_point =
      getCollisionPoint(sparse_resample_path, object, crosswalk_attention_range, attention_area);
    object_info_manager_.update(
      obj_uuid, obj_pos, std::hypot(obj_vel.x, obj_vel.y), clock_->now(), is_ego_yielding,
      has_traffic_light, collision_point, planner_param_);

    if (collision_point) {
      const auto collision_state = object_info_manager_.getCollisionState(obj_uuid);
      debug_data_.collision_points.push_back(std::make_pair(*collision_point, collision_state));
    }
  }
  object_info_manager_.finalize();
}

bool CrosswalkModule::isRedSignalForPedestrians() const
{
  const auto traffic_lights_reg_elems =
    crosswalk_.regulatoryElementsAs<const lanelet::TrafficLight>();

  for (const auto & traffic_lights_reg_elem : traffic_lights_reg_elems) {
    lanelet::ConstLineStringsOrPolygons3d traffic_lights = traffic_lights_reg_elem->trafficLights();
    for (const auto & traffic_light : traffic_lights) {
      const auto ll_traffic_light = static_cast<lanelet::ConstLineString3d>(traffic_light);
      const auto traffic_signal_stamped = planner_data_->getTrafficSignal(ll_traffic_light.id());
      if (!traffic_signal_stamped) {
        continue;
      }

      if (
        planner_param_.traffic_light_state_timeout <
        (clock_->now() - traffic_signal_stamped->header.stamp).seconds()) {
        continue;
      }

      const auto & lights = traffic_signal_stamped->signal.lights;
      if (lights.empty()) {
        continue;
      }

      if (lights.front().color == TrafficLight::RED) {
        return true;
      }
    }
  }

  return false;
}

bool CrosswalkModule::isVehicle(const PredictedObject & object)
{
  if (object.classification.empty()) {
    return false;
  }

  const auto & label = object.classification.front().label;

  if (label == ObjectClassification::CAR) {
    return true;
  }

  if (label == ObjectClassification::TRUCK) {
    return true;
  }

  if (label == ObjectClassification::BUS) {
    return true;
  }

  if (label == ObjectClassification::TRAILER) {
    return true;
  }

  if (label == ObjectClassification::MOTORCYCLE) {
    return true;
  }

  return false;
}

bool CrosswalkModule::isCrosswalkUserType(const PredictedObject & object) const
{
  if (object.classification.empty()) {
    return false;
  }

  const auto & label = object.classification.front().label;

  if (label == ObjectClassification::UNKNOWN && planner_param_.look_unknown) {
    return true;
  }

  if (label == ObjectClassification::BICYCLE && planner_param_.look_bicycle) {
    return true;
  }

  if (label == ObjectClassification::MOTORCYCLE && planner_param_.look_motorcycle) {
    return true;
  }

  if (label == ObjectClassification::PEDESTRIAN && planner_param_.look_pedestrian) {
    return true;
  }

  return false;
}

geometry_msgs::msg::Polygon CrosswalkModule::createObjectPolygon(
  const double width_m, const double length_m)
{
  geometry_msgs::msg::Polygon polygon{};

  polygon.points.push_back(createPoint32(length_m / 2.0, -width_m / 2.0, 0.0));
  polygon.points.push_back(createPoint32(length_m / 2.0, width_m / 2.0, 0.0));
  polygon.points.push_back(createPoint32(-length_m / 2.0, width_m / 2.0, 0.0));
  polygon.points.push_back(createPoint32(-length_m / 2.0, -width_m / 2.0, 0.0));

  return polygon;
}

geometry_msgs::msg::Polygon CrosswalkModule::createVehiclePolygon(
  const vehicle_info_util::VehicleInfo & vehicle_info)
{
  const auto & i = vehicle_info;
  const auto & front_m = i.max_longitudinal_offset_m;
  const auto & width_m = i.vehicle_width_m / 2.0;
  const auto & back_m = i.rear_overhang_m;

  geometry_msgs::msg::Polygon polygon{};

  polygon.points.push_back(createPoint32(front_m, -width_m, 0.0));
  polygon.points.push_back(createPoint32(front_m, width_m, 0.0));
  polygon.points.push_back(createPoint32(-back_m, width_m, 0.0));
  polygon.points.push_back(createPoint32(-back_m, -width_m, 0.0));

  return polygon;
}

void CrosswalkModule::setDistanceToStop(
  const PathWithLaneId & ego_path,
  const std::optional<geometry_msgs::msg::Pose> & default_stop_pose,
  const std::optional<StopFactor> & stop_factor)
{
  // calculate stop position
  const auto stop_pos = [&]() -> std::optional<geometry_msgs::msg::Point> {
    if (stop_factor) return stop_factor->stop_pose.position;
    if (default_stop_pose) return default_stop_pose->position;
    return std::nullopt;
  }();

  // Set distance
  if (stop_pos) {
    const auto & ego_pos = planner_data_->current_odometry->pose.position;
    const double dist_ego2stop = calcSignedArcLength(ego_path.points, ego_pos, *stop_pos);
    setDistance(dist_ego2stop);
  }
}

void CrosswalkModule::planGo(
  PathWithLaneId & ego_path, const std::optional<StopFactor> & stop_factor) const
{
  // Plan slow down
  const auto target_velocity = calcTargetVelocity(stop_factor->stop_pose.position, ego_path);
  insertDecelPointWithDebugInfo(
    stop_factor->stop_pose.position,
    std::max(planner_param_.min_slow_down_velocity, target_velocity), ego_path);
}

void CrosswalkModule::planStop(
  PathWithLaneId & ego_path, const std::optional<StopFactor> & nearest_stop_factor,
  const std::optional<geometry_msgs::msg::Pose> & default_stop_pose, StopReason * stop_reason)
{
  const auto stop_factor = [&]() -> std::optional<StopFactor> {
    if (nearest_stop_factor) return *nearest_stop_factor;
    if (default_stop_pose) return createStopFactor(*default_stop_pose);
    return std::nullopt;
  }();

  // Plan stop
  insertDecelPointWithDebugInfo(stop_factor->stop_pose.position, 0.0, ego_path);
  planning_utils::appendStopReason(*stop_factor, stop_reason);
  velocity_factor_.set(
    ego_path.points, planner_data_->current_odometry->pose, stop_factor->stop_pose,
    VelocityFactor::UNKNOWN);
}
}  // namespace behavior_velocity_planner
