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

#include "behavior_path_planner/utilities.hpp"

#include <lanelet2_extension/utility/message_conversion.hpp>
#include <lanelet2_extension/utility/query.hpp>
#include <lanelet2_extension/utility/utilities.hpp>
#include <opencv2/opencv.hpp>
#include <tier4_autoware_utils/tier4_autoware_utils.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace drivable_area_utils
{
double quantize(const double val, const double resolution)
{
  return std::round(val / resolution) * resolution;
}

void updateMinMaxPosition(
  const Eigen::Vector2d & point, boost::optional<double> & min_x, boost::optional<double> & min_y,
  boost::optional<double> & max_x, boost::optional<double> & max_y)
{
  min_x = min_x ? std::min(min_x.get(), point.x()) : point.x();
  min_y = min_y ? std::min(min_y.get(), point.y()) : point.y();
  max_x = max_x ? std::max(max_x.get(), point.x()) : point.x();
  max_y = max_y ? std::max(max_y.get(), point.y()) : point.y();
}

bool sumLengthFromTwoPoints(
  const Eigen::Vector2d & base_point, const Eigen::Vector2d & target_point,
  const double length_threshold, double & sum_length, boost::optional<double> & min_x,
  boost::optional<double> & min_y, boost::optional<double> & max_x, boost::optional<double> & max_y)
{
  const double norm_length = (base_point - target_point).norm();
  sum_length += norm_length;
  if (length_threshold < sum_length) {
    const double diff_length = norm_length - (sum_length - length_threshold);
    const Eigen::Vector2d interpolated_point =
      base_point + diff_length * (target_point - base_point).normalized();
    updateMinMaxPosition(interpolated_point, min_x, min_y, max_x, max_y);

    const bool is_end = true;
    return is_end;
  }

  updateMinMaxPosition(target_point, min_x, min_y, max_x, max_y);
  const bool is_end = false;
  return is_end;
}

std::array<double, 4> getLaneletScope(
  const lanelet::ConstLanelets & lanes, const size_t nearest_lane_idx,
  const geometry_msgs::msg::Pose & current_pose, const double forward_lane_length,
  const double backward_lane_length, const double lane_margin)
{
  // define functions to get right/left bounds as a vector
  const auto get_bound_funcs =
    std::vector<std::function<lanelet::ConstLineString2d(const lanelet::ConstLanelet & lane)>>{
      [](const lanelet::ConstLanelet & lane) -> lanelet::ConstLineString2d {
        return lane.rightBound2d();
      },
      [](const lanelet::ConstLanelet & lane) -> lanelet::ConstLineString2d {
        return lane.leftBound2d();
      }};

  // calculate min/max x and y
  boost::optional<double> min_x;
  boost::optional<double> min_y;
  boost::optional<double> max_x;
  boost::optional<double> max_y;

  for (const auto & get_bound_func : get_bound_funcs) {
    // search nearest point index to current pose
    const auto & nearest_bound = get_bound_func(lanes.at(nearest_lane_idx));
    if (nearest_bound.empty()) {
      continue;
    }

    std::vector<geometry_msgs::msg::Point> points;
    for (const auto & point : nearest_bound) {
      geometry_msgs::msg::Point p;
      p.x = point.x();
      p.y = point.y();
      points.push_back(p);
    }
    const size_t nearest_segment_idx =
      tier4_autoware_utils::findNearestSegmentIndex(points, current_pose.position);

    // forward lanelet
    const auto forward_offset_length =
      tier4_autoware_utils::calcSignedArcLength(points, current_pose.position, nearest_segment_idx);
    double sum_length = std::min(forward_offset_length, 0.0);
    size_t current_lane_idx = nearest_lane_idx;
    auto current_lane = lanes.at(current_lane_idx);
    size_t current_point_idx = nearest_segment_idx;
    while (true) {
      const auto & bound = get_bound_func(current_lane);
      if (current_point_idx != bound.size() - 1) {
        const Eigen::Vector2d & current_point = bound[current_point_idx].basicPoint();
        const Eigen::Vector2d & next_point = bound[current_point_idx + 1].basicPoint();
        const bool is_end_lane = drivable_area_utils::sumLengthFromTwoPoints(
          current_point, next_point, forward_lane_length + lane_margin, sum_length, min_x, min_y,
          max_x, max_y);
        if (is_end_lane) {
          break;
        }

        ++current_point_idx;
      } else {
        const auto previous_lane = current_lane;
        const size_t previous_point_idx = get_bound_func(previous_lane).size() - 1;
        const auto & previous_bound = get_bound_func(previous_lane);
        drivable_area_utils::updateMinMaxPosition(
          previous_bound[previous_point_idx].basicPoint(), min_x, min_y, max_x, max_y);

        if (current_lane_idx == lanes.size() - 1) {
          break;
        }

        current_lane_idx += 1;
        current_lane = lanes.at(current_lane_idx);
        current_point_idx = 0;
        const auto & current_bound = get_bound_func(current_lane);

        const Eigen::Vector2d & prev_point = previous_bound[previous_point_idx].basicPoint();
        const Eigen::Vector2d & current_point = current_bound[current_point_idx].basicPoint();
        const bool is_end_lane = drivable_area_utils::sumLengthFromTwoPoints(
          prev_point, current_point, forward_lane_length + lane_margin, sum_length, min_x, min_y,
          max_x, max_y);
        if (is_end_lane) {
          break;
        }
      }
    }

    // backward lanelet
    current_point_idx = nearest_segment_idx + 1;
    const auto backward_offset_length = tier4_autoware_utils::calcSignedArcLength(
      points, nearest_segment_idx + 1, current_pose.position);
    sum_length = std::min(backward_offset_length, 0.0);
    current_lane_idx = nearest_lane_idx;
    current_lane = lanes.at(current_lane_idx);
    while (true) {
      const auto & bound = get_bound_func(current_lane);
      if (current_point_idx != 0) {
        const Eigen::Vector2d & current_point = bound[current_point_idx].basicPoint();
        const Eigen::Vector2d & prev_point = bound[current_point_idx - 1].basicPoint();
        const bool is_end_lane = drivable_area_utils::sumLengthFromTwoPoints(
          current_point, prev_point, backward_lane_length + lane_margin, sum_length, min_x, min_y,
          max_x, max_y);
        if (is_end_lane) {
          break;
        }

        --current_point_idx;
      } else {
        const auto next_lane = current_lane;
        const size_t next_point_idx = 0;
        const auto & next_bound = get_bound_func(next_lane);
        drivable_area_utils::updateMinMaxPosition(
          next_bound[next_point_idx].basicPoint(), min_x, min_y, max_x, max_y);

        if (current_lane_idx == 0) {
          break;
        }

        current_lane_idx -= 1;
        current_lane = lanes.at(current_lane_idx);
        const auto & current_bound = get_bound_func(current_lane);
        current_point_idx = current_bound.size() - 1;

        const Eigen::Vector2d & next_point = next_bound[next_point_idx].basicPoint();
        const Eigen::Vector2d & current_point = current_bound[current_point_idx].basicPoint();
        const bool is_end_lane = drivable_area_utils::sumLengthFromTwoPoints(
          next_point, current_point, backward_lane_length + lane_margin, sum_length, min_x, min_y,
          max_x, max_y);
        if (is_end_lane) {
          break;
        }
      }
    }
  }

  if (!min_x || !min_y || !max_x || !max_y) {
    const double x = current_pose.position.x;
    const double y = current_pose.position.y;
    return {x, y, x, y};
  }

  return {min_x.get(), min_y.get(), max_x.get(), max_y.get()};
}
}  // namespace drivable_area_utils

namespace behavior_path_planner
{
namespace util
{
using autoware_auto_perception_msgs::msg::ObjectClassification;
using autoware_auto_perception_msgs::msg::Shape;
using geometry_msgs::msg::PoseWithCovarianceStamped;

std::vector<Point> convertToPointArray(const PathWithLaneId & path)
{
  std::vector<Point> point_array;
  for (const auto & pt : path.points) {
    point_array.push_back(pt.point.pose.position);
  }
  return point_array;
}

double l2Norm(const Vector3 vector)
{
  return std::sqrt(std::pow(vector.x, 2) + std::pow(vector.y, 2) + std::pow(vector.z, 2));
}

FrenetCoordinate3d convertToFrenetCoordinate3d(
  const std::vector<Point> & linestring, const Point & search_point_geom)
{
  FrenetCoordinate3d frenet_coordinate;

  const size_t nearest_segment_idx =
    tier4_autoware_utils::findNearestSegmentIndex(linestring, search_point_geom);
  const double longitudinal_length = tier4_autoware_utils::calcLongitudinalOffsetToSegment(
    linestring, nearest_segment_idx, search_point_geom);
  frenet_coordinate.length =
    tier4_autoware_utils::calcSignedArcLength(linestring, 0, nearest_segment_idx) +
    longitudinal_length;
  frenet_coordinate.distance =
    tier4_autoware_utils::calcLateralOffset(linestring, search_point_geom);

  return frenet_coordinate;
}

FrenetCoordinate3d convertToFrenetCoordinate3d(
  const PathWithLaneId & path, const Point & search_point_geom)
{
  const auto linestring = convertToPointArray(path);
  return convertToFrenetCoordinate3d(linestring, search_point_geom);
}

std::vector<Point> convertToGeometryPointArray(const PathWithLaneId & path)
{
  std::vector<Point> converted_path;
  converted_path.reserve(path.points.size());
  for (const auto & point_with_id : path.points) {
    converted_path.push_back(point_with_id.point.pose.position);
  }
  return converted_path;
}

std::vector<Point> convertToGeometryPointArray(const PredictedPath & path)
{
  std::vector<Point> converted_path;

  converted_path.reserve(path.path.size());
  for (const auto & pose : path.path) {
    converted_path.push_back(pose.position);
  }
  return converted_path;
}

PoseArray convertToGeometryPoseArray(const PathWithLaneId & path)
{
  PoseArray converted_array;
  converted_array.header = path.header;

  converted_array.poses.reserve(path.points.size());
  for (const auto & point_with_id : path.points) {
    converted_array.poses.push_back(point_with_id.point.pose);
  }
  return converted_array;
}

PredictedPath convertToPredictedPath(
  const PathWithLaneId & path, const Twist & vehicle_twist, const Pose & vehicle_pose,
  const double duration, const double resolution, const double acceleration)
{
  PredictedPath predicted_path{};
  predicted_path.time_step = rclcpp::Duration::from_seconds(resolution);
  predicted_path.path.reserve(std::min(path.points.size(), static_cast<size_t>(100)));
  if (path.points.empty()) {
    return predicted_path;
  }

  const auto & geometry_points = convertToGeometryPointArray(path);
  FrenetCoordinate3d vehicle_pose_frenet =
    convertToFrenetCoordinate3d(geometry_points, vehicle_pose.position);
  auto clock{rclcpp::Clock{RCL_ROS_TIME}};
  rclcpp::Time start_time = clock.now();
  double vehicle_speed = std::abs(vehicle_twist.linear.x);
  constexpr double min_speed = 1.0;
  if (vehicle_speed < min_speed) {
    vehicle_speed = min_speed;
    RCLCPP_DEBUG_STREAM_THROTTLE(
      rclcpp::get_logger("behavior_path_planner").get_child("utilities"), clock, 1000,
      "cannot convert PathWithLaneId with zero velocity, using minimum value " << min_speed
                                                                               << " [m/s] instead");
  }

  double length = 0;
  double prev_vehicle_speed = vehicle_speed;

  // first point
  const auto pt = lerpByLength(geometry_points, vehicle_pose_frenet.length);
  Pose predicted_pose;
  predicted_pose.position = pt;
  predicted_path.path.push_back(predicted_pose);

  for (double t = resolution; t < duration; t += resolution) {
    double accelerated_velocity = prev_vehicle_speed + acceleration * t;
    double travel_distance = 0;
    if (accelerated_velocity < min_speed) {
      travel_distance = min_speed * resolution;
    } else {
      travel_distance =
        prev_vehicle_speed * resolution + 0.5 * acceleration * resolution * resolution;
    }

    length += travel_distance;
    const auto pt = lerpByLength(geometry_points, vehicle_pose_frenet.length + length);
    Pose predicted_pose;
    predicted_pose.position = pt;
    predicted_path.path.push_back(predicted_pose);
    prev_vehicle_speed = accelerated_velocity;
  }
  return predicted_path;
}

PredictedPath resamplePredictedPath(
  const PredictedPath & input_path, const double resolution, const double duration)
{
  PredictedPath resampled_path{};

  for (double t = 0.0; t < duration; t += resolution) {
    Pose pose;
    if (!lerpByTimeStamp(input_path, t, &pose)) {
      continue;
    }
    resampled_path.path.push_back(pose);
  }

  return resampled_path;
}

Pose lerpByPose(const Pose & p1, const Pose & p2, const double t)
{
  tf2::Transform tf_transform1, tf_transform2;
  tf2::fromMsg(p1, tf_transform1);
  tf2::fromMsg(p2, tf_transform2);
  const auto & tf_point = tf2::lerp(tf_transform1.getOrigin(), tf_transform2.getOrigin(), t);
  const auto & tf_quaternion =
    tf2::slerp(tf_transform1.getRotation(), tf_transform2.getRotation(), t);

  Pose pose{};
  pose.position = tf2::toMsg(tf_point, pose.position);
  pose.orientation = tf2::toMsg(tf_quaternion);
  return pose;
}

Point lerpByPoint(const Point & p1, const Point & p2, const double t)
{
  tf2::Vector3 v1, v2;
  v1.setValue(p1.x, p1.y, p1.z);
  v2.setValue(p2.x, p2.y, p2.z);

  const auto lerped_point = v1.lerp(v2, t);

  Point point;
  point.x = lerped_point.x();
  point.y = lerped_point.y();
  point.z = lerped_point.z();
  return point;
}

Point lerpByLength(const std::vector<Point> & point_array, const double length)
{
  Point lerped_point;
  if (point_array.empty()) {
    return lerped_point;
  }
  Point prev_pt = point_array.front();
  double accumulated_length = 0;
  for (const auto & pt : point_array) {
    const double distance = tier4_autoware_utils::calcDistance3d(prev_pt, pt);
    if (accumulated_length + distance > length) {
      return lerpByPoint(prev_pt, pt, (length - accumulated_length) / distance);
    }
    accumulated_length += distance;
    prev_pt = pt;
  }

  return point_array.back();
}

bool lerpByTimeStamp(const PredictedPath & path, const double t_query, Pose * lerped_pt)
{
  const rclcpp::Duration time_step(path.time_step);
  auto clock{rclcpp::Clock{RCL_ROS_TIME}};
  if (lerped_pt == nullptr) {
    RCLCPP_WARN_STREAM_THROTTLE(
      rclcpp::get_logger("behavior_path_planner").get_child("utilities"), clock, 1000,
      "failed to lerp by time due to nullptr pt");
    return false;
  }
  if (path.path.empty()) {
    RCLCPP_WARN_STREAM_THROTTLE(
      rclcpp::get_logger("behavior_path_planner").get_child("utilities"), clock, 1000,
      "Empty path. Failed to interpolate path by time!");
    return false;
  }

  const double t_final = time_step.seconds() * static_cast<double>(path.path.size() - 1);
  if (t_query > t_final) {
    RCLCPP_DEBUG_STREAM(
      rclcpp::get_logger("behavior_path_planner").get_child("utilities"),
      "failed to interpolate path by time!" << std::endl
                                            << "t_final    : " << t_final
                                            << "query time : " << t_query);
    *lerped_pt = path.path.back();

    return false;
  }

  for (size_t i = 1; i < path.path.size(); i++) {
    const auto & pt = path.path.at(i);
    const auto & prev_pt = path.path.at(i - 1);
    const double t = time_step.seconds() * static_cast<double>(i);
    if (t_query <= t) {
      const double prev_t = time_step.seconds() * static_cast<double>(i - 1);
      const double duration = time_step.seconds();
      const double offset = t_query - prev_t;
      const double ratio = offset / duration;
      *lerped_pt = lerpByPose(prev_pt, pt, ratio);
      return true;
    }
  }

  RCLCPP_ERROR_STREAM(
    rclcpp::get_logger("behavior_path_planner").get_child("utilities"),
    "Something failed in function: ");
  return false;
}

bool lerpByDistance(
  const behavior_path_planner::PullOutPath & path, const double & s, Pose * lerped_pt,
  const lanelet::ConstLanelets & road_lanes)
{
  if (lerped_pt == nullptr) {
    // ROS_WARN_STREAM_THROTTLE(1, "failed to lerp by distance due to nullptr pt");
    return false;
  }

  for (size_t i = 1; i < path.path.points.size(); i++) {
    const auto & pt = path.path.points.at(i).point.pose;
    const auto & prev_pt = path.path.points.at(i - 1).point.pose;
    if (
      (s >= lanelet::utils::getArcCoordinates(road_lanes, prev_pt).length) &&
      (s < lanelet::utils::getArcCoordinates(road_lanes, pt).length)) {
      const double distance = lanelet::utils::getArcCoordinates(road_lanes, pt).length -
                              lanelet::utils::getArcCoordinates(road_lanes, prev_pt).length;
      const auto offset = s - lanelet::utils::getArcCoordinates(road_lanes, prev_pt).length;
      const auto ratio = offset / distance;
      *lerped_pt = lerpByPose(prev_pt, pt, ratio);
      return true;
    }
  }

  // ROS_ERROR_STREAM("Something failed in function: " << __func__);
  return false;
}

double getDistanceBetweenPredictedPaths(
  const PredictedPath & object_path, const PredictedPath & ego_path, const double start_time,
  const double end_time, const double resolution)
{
  double min_distance = std::numeric_limits<double>::max();
  const auto ego_path_point_array = convertToGeometryPointArray(ego_path);
  for (double t = start_time; t < end_time; t += resolution) {
    Pose object_pose, ego_pose;
    if (!lerpByTimeStamp(object_path, t, &object_pose)) {
      continue;
    }
    if (!lerpByTimeStamp(ego_path, t, &ego_pose)) {
      continue;
    }
    double distance = tier4_autoware_utils::calcDistance3d(object_pose, ego_pose);
    if (distance < min_distance) {
      min_distance = distance;
    }
  }
  return min_distance;
}

double getDistanceBetweenPredictedPathAndObject(
  const PredictedObject & object, const PredictedPath & ego_path, const double start_time,
  const double end_time, const double resolution)
{
  auto clock{rclcpp::Clock{RCL_ROS_TIME}};
  auto t_delta{rclcpp::Duration::from_seconds(resolution)};
  double min_distance = std::numeric_limits<double>::max();
  rclcpp::Time ros_start_time = clock.now() + rclcpp::Duration::from_seconds(start_time);
  rclcpp::Time ros_end_time = clock.now() + rclcpp::Duration::from_seconds(end_time);
  const auto ego_path_point_array = convertToGeometryPointArray(ego_path);
  Polygon2d obj_polygon;
  if (!calcObjectPolygon(object, &obj_polygon)) {
    return min_distance;
  }
  for (double t = start_time; t < end_time; t += resolution) {
    Pose ego_pose;
    if (!lerpByTimeStamp(ego_path, t, &ego_pose)) {
      continue;
    }
    Point2d ego_point{ego_pose.position.x, ego_pose.position.y};

    double distance = boost::geometry::distance(obj_polygon, ego_point);
    if (distance < min_distance) {
      min_distance = distance;
    }
  }
  return min_distance;
}

double getDistanceBetweenPredictedPathAndObjectPolygon(
  const PredictedObject & object, const PullOutPath & ego_path,
  const tier4_autoware_utils::LinearRing2d & vehicle_footprint, double distance_resolution,
  const lanelet::ConstLanelets & road_lanes)
{
  double min_distance = std::numeric_limits<double>::max();

  const auto ego_path_point_array = convertToGeometryPointArray(ego_path.path);
  Polygon2d obj_polygon;
  if (!calcObjectPolygon(object, &obj_polygon)) {
    // ROS_ERROR("calcObjectPolygon failed");
    return min_distance;
  }
  const auto s_start =
    lanelet::utils::getArcCoordinates(road_lanes, ego_path.path.points.front().point.pose).length;
  const auto s_end = lanelet::utils::getArcCoordinates(road_lanes, ego_path.shift_point.end).length;

  for (auto s = s_start + distance_resolution; s < s_end; s += distance_resolution) {
    Pose ego_pose;
    if (!lerpByDistance(ego_path, s, &ego_pose, road_lanes)) {
      // ROS_ERROR("lerp failed");
      continue;
    }
    const auto vehicle_footprint_on_path =
      transformVector(vehicle_footprint, tier4_autoware_utils::pose2transform(ego_pose));
    Point2d ego_point{ego_pose.position.x, ego_pose.position.y};
    for (const auto & vehicle_footprint : vehicle_footprint_on_path) {
      double distance = boost::geometry::distance(obj_polygon, vehicle_footprint);
      if (distance < min_distance) {
        min_distance = distance;
      }
    }
  }
  return min_distance;
}
// only works with consecutive lanes
std::vector<size_t> filterObjectsByLanelets(
  const PredictedObjects & objects, const lanelet::ConstLanelets & target_lanelets,
  const double start_arc_length, const double end_arc_length)
{
  std::vector<size_t> indices;
  if (target_lanelets.empty()) {
    return {};
  }
  const auto polygon =
    lanelet::utils::getPolygonFromArcLength(target_lanelets, start_arc_length, end_arc_length);
  const auto polygon2d = lanelet::utils::to2D(polygon).basicPolygon();
  if (polygon2d.empty()) {
    // no lanelet polygon
    return {};
  }

  for (size_t i = 0; i < objects.objects.size(); i++) {
    const auto & obj = objects.objects.at(i);
    // create object polygon
    Polygon2d obj_polygon;
    if (!calcObjectPolygon(obj, &obj_polygon)) {
      continue;
    }
    // create lanelet polygon
    Polygon2d lanelet_polygon;
    for (const auto & lanelet_point : polygon2d) {
      lanelet_polygon.outer().emplace_back(lanelet_point.x(), lanelet_point.y());
    }

    lanelet_polygon.outer().push_back(lanelet_polygon.outer().front());

    // check the object does not intersect the lanelet
    if (!boost::geometry::disjoint(lanelet_polygon, obj_polygon)) {
      indices.push_back(i);
      continue;
    }
  }
  return indices;
}

// works with random lanelets
std::vector<size_t> filterObjectsByLanelets(
  const PredictedObjects & objects, const lanelet::ConstLanelets & target_lanelets)
{
  std::vector<size_t> indices;
  if (target_lanelets.empty()) {
    return {};
  }

  for (size_t i = 0; i < objects.objects.size(); i++) {
    // create object polygon
    const auto & obj = objects.objects.at(i);
    // create object polygon
    Polygon2d obj_polygon;
    if (!calcObjectPolygon(obj, &obj_polygon)) {
      RCLCPP_ERROR_STREAM(
        rclcpp::get_logger("behavior_path_planner").get_child("utilities"),
        "Failed to calcObjectPolygon...!!!");
      continue;
    }

    for (const auto & llt : target_lanelets) {
      // create lanelet polygon
      const auto polygon2d = llt.polygon2d().basicPolygon();
      if (polygon2d.empty()) {
        // no lanelet polygon
        continue;
      }
      Polygon2d lanelet_polygon;
      for (const auto & lanelet_point : polygon2d) {
        lanelet_polygon.outer().emplace_back(lanelet_point.x(), lanelet_point.y());
      }

      lanelet_polygon.outer().push_back(lanelet_polygon.outer().front());

      // check the object does not intersect the lanelet
      if (!boost::geometry::disjoint(lanelet_polygon, obj_polygon)) {
        indices.push_back(i);
        break;
      }
    }
  }
  return indices;
}

bool calcObjectPolygon(const PredictedObject & object, Polygon2d * object_polygon)
{
  const double obj_x = object.kinematics.initial_pose_with_covariance.pose.position.x;
  const double obj_y = object.kinematics.initial_pose_with_covariance.pose.position.y;
  if (object.shape.type == Shape::BOUNDING_BOX) {
    const double len_x = object.shape.dimensions.x;
    const double len_y = object.shape.dimensions.y;

    tf2::Transform tf_map2obj;
    tf2::fromMsg(object.kinematics.initial_pose_with_covariance.pose, tf_map2obj);

    // set vertices at map coordinate
    tf2::Vector3 p1_map, p2_map, p3_map, p4_map;

    p1_map.setX(len_x / 2);
    p1_map.setY(len_y / 2);
    p1_map.setZ(0.0);
    p1_map.setW(1.0);

    p2_map.setX(-len_x / 2);
    p2_map.setY(len_y / 2);
    p2_map.setZ(0.0);
    p2_map.setW(1.0);

    p3_map.setX(-len_x / 2);
    p3_map.setY(-len_y / 2);
    p3_map.setZ(0.0);
    p3_map.setW(1.0);

    p4_map.setX(len_x / 2);
    p4_map.setY(-len_y / 2);
    p4_map.setZ(0.0);
    p4_map.setW(1.0);

    // transform vertices from map coordinate to object coordinate
    tf2::Vector3 p1_obj, p2_obj, p3_obj, p4_obj;

    p1_obj = tf_map2obj * p1_map;
    p2_obj = tf_map2obj * p2_map;
    p3_obj = tf_map2obj * p3_map;
    p4_obj = tf_map2obj * p4_map;

    object_polygon->outer().emplace_back(p1_obj.x(), p1_obj.y());
    object_polygon->outer().emplace_back(p2_obj.x(), p2_obj.y());
    object_polygon->outer().emplace_back(p3_obj.x(), p3_obj.y());
    object_polygon->outer().emplace_back(p4_obj.x(), p4_obj.y());

  } else if (object.shape.type == Shape::CYLINDER) {
    const size_t N = 20;
    const double r = object.shape.dimensions.x / 2;
    for (size_t i = 0; i < N; ++i) {
      object_polygon->outer().emplace_back(
        obj_x + r * std::cos(2.0 * M_PI / N * i), obj_y + r * std::sin(2.0 * M_PI / N * i));
    }
  } else if (object.shape.type == Shape::POLYGON) {
    tf2::Transform tf_map2obj;
    tf2::fromMsg(object.kinematics.initial_pose_with_covariance.pose, tf_map2obj);
    const auto obj_points = object.shape.footprint.points;
    for (const auto & obj_point : obj_points) {
      tf2::Vector3 obj(obj_point.x, obj_point.y, obj_point.z);
      tf2::Vector3 tf_obj = tf_map2obj * obj;
      object_polygon->outer().emplace_back(tf_obj.x(), tf_obj.y());
    }
  } else {
    RCLCPP_WARN(
      rclcpp::get_logger("behavior_path_planner").get_child("utilities"), "Object shape unknown!");
    return false;
  }
  object_polygon->outer().push_back(object_polygon->outer().front());

  return true;
}

std::vector<double> calcObjectsDistanceToPath(
  const PredictedObjects & objects, const PathWithLaneId & ego_path)
{
  std::vector<double> distance_array;
  const auto ego_path_point_array = convertToGeometryPointArray(ego_path);
  for (const auto & obj : objects.objects) {
    Polygon2d obj_polygon;
    if (!calcObjectPolygon(obj, &obj_polygon)) {
      std::cerr << __func__ << ": fail to convert object to polygon" << std::endl;
      continue;
    }
    LineString2d ego_path_line;
    for (size_t j = 0; j < ego_path_point_array.size(); ++j) {
      boost::geometry::append(
        ego_path_line, Point2d(ego_path_point_array.at(j).x, ego_path_point_array.at(j).y));
    }
    const double distance = boost::geometry::distance(obj_polygon, ego_path_line);
    distance_array.push_back(distance);
  }
  return distance_array;
}

std::vector<size_t> filterObjectsByPath(
  const PredictedObjects & objects, const std::vector<size_t> & object_indices,
  const PathWithLaneId & ego_path, const double vehicle_width)
{
  std::vector<size_t> indices;
  const auto ego_path_point_array = convertToGeometryPointArray(ego_path);
  for (const auto & i : object_indices) {
    Polygon2d obj_polygon;
    if (!calcObjectPolygon(objects.objects.at(i), &obj_polygon)) {
      continue;
    }
    LineString2d ego_path_line;
    for (size_t j = 0; j < ego_path_point_array.size(); ++j) {
      boost::geometry::append(
        ego_path_line, Point2d(ego_path_point_array.at(j).x, ego_path_point_array.at(j).y));
    }
    const double distance = boost::geometry::distance(obj_polygon, ego_path_line);
    if (distance < vehicle_width) {
      indices.push_back(i);
    }
  }
  return indices;
}

PathWithLaneId removeOverlappingPoints(const PathWithLaneId & input_path)
{
  PathWithLaneId filtered_path;
  for (const auto & pt : input_path.points) {
    if (filtered_path.points.empty()) {
      filtered_path.points.push_back(pt);
      continue;
    }

    constexpr double min_dist = 0.001;
    if (
      tier4_autoware_utils::calcDistance3d(filtered_path.points.back().point, pt.point) <
      min_dist) {
      filtered_path.points.back().lane_ids.push_back(pt.lane_ids.front());
      filtered_path.points.back().point.longitudinal_velocity_mps = std::min(
        pt.point.longitudinal_velocity_mps,
        filtered_path.points.back().point.longitudinal_velocity_mps);
    } else {
      filtered_path.points.push_back(pt);
    }
  }
  filtered_path.drivable_area = input_path.drivable_area;
  return filtered_path;
}

template <typename T>
bool exists(std::vector<T> vec, T element)
{
  return std::find(vec.begin(), vec.end(), element) != vec.end();
}

boost::optional<size_t> findNearestIndexToGoal(
  const std::vector<autoware_auto_planning_msgs::msg::PathPointWithLaneId> & points,
  const geometry_msgs::msg::Pose & goal, const int64_t goal_lane_id,
  const double max_dist = std::numeric_limits<double>::max())
{
  if (points.empty()) {
    return boost::none;
  }

  size_t min_dist_index;
  double min_dist = std::numeric_limits<double>::max();
  {
    bool found = false;
    for (size_t i = 0; i < points.size(); ++i) {
      const double x = points.at(i).point.pose.position.x - goal.position.x;
      const double y = points.at(i).point.pose.position.y - goal.position.y;
      const double dist = std::hypot(x, y);
      if (dist < max_dist && dist < min_dist && exists(points.at(i).lane_ids, goal_lane_id)) {
        min_dist_index = i;
        min_dist = dist;
        found = true;
      }
    }
    if (!found) {
      return boost::none;
    }
  }

  size_t min_dist_out_of_range_index = min_dist_index;
  for (size_t i = min_dist_index; i != 0; --i) {
    const double x = points.at(i).point.pose.position.x - goal.position.x;
    const double y = points.at(i).point.pose.position.y - goal.position.y;
    const double dist = std::hypot(x, y);
    min_dist_out_of_range_index = i;
    if (max_dist < dist) {
      break;
    }
  }
  return min_dist_out_of_range_index;
}

// goal does not have z
bool setGoal(
  const double search_radius_range, [[maybe_unused]] const double search_rad_range,
  const PathWithLaneId & input, const Pose & goal, const int64_t goal_lane_id,
  PathWithLaneId * output_ptr)
{
  try {
    if (input.points.empty()) {
      return false;
    }

    // calculate refined_goal with interpolation
    PathPointWithLaneId refined_goal{};
    {  // NOTE: goal does not have valid z, that will be calculated by interpolation here
      const size_t closest_seg_idx =
        tier4_autoware_utils::findNearestSegmentIndex(input.points, goal.position);
      const double closest_to_goal_dist = tier4_autoware_utils::calcSignedArcLength(
        input.points, input.points.at(closest_seg_idx).point.pose.position,
        goal.position);  // TODO(murooka) implement calcSignedArcLength(points, idx, point)
      const double seg_dist = tier4_autoware_utils::calcSignedArcLength(
        input.points, closest_seg_idx, closest_seg_idx + 1);
      const double closest_z = input.points.at(closest_seg_idx).point.pose.position.z;
      const double next_z = input.points.at(closest_seg_idx + 1).point.pose.position.z;
      const double goal_z = std::abs(seg_dist) < 1e-6
                              ? next_z
                              : closest_z + (next_z - closest_z) * closest_to_goal_dist / seg_dist;

      refined_goal.point.pose = goal;
      refined_goal.point.pose.position.z = goal_z;
      refined_goal.point.longitudinal_velocity_mps = 0.0;
      refined_goal.lane_ids = input.points.back().lane_ids;
    }

    // calculate pre_goal
    double roll{};
    double pitch{};
    double yaw{};
    tf2::Quaternion tf2_quaternion(
      goal.orientation.x, goal.orientation.y, goal.orientation.z, goal.orientation.w);
    tf2::Matrix3x3 tf2_matrix(tf2_quaternion);
    tf2_matrix.getRPY(roll, pitch, yaw);

    // calculate pre_refined_goal with interpolation
    PathPointWithLaneId pre_refined_goal{};
    pre_refined_goal.point.pose.position.x = goal.position.x - std::cos(yaw);
    pre_refined_goal.point.pose.position.y = goal.position.y - std::sin(yaw);
    pre_refined_goal.point.pose.orientation = goal.orientation;

    {  // NOTE: interpolate z and velocity of pre_refined_goal
      const size_t closest_seg_idx = tier4_autoware_utils::findNearestSegmentIndex(
        input.points, pre_refined_goal.point.pose.position);
      const double closest_to_pre_goal_dist = tier4_autoware_utils::calcSignedArcLength(
        input.points, input.points.at(closest_seg_idx).point.pose.position,
        pre_refined_goal.point.pose.position);
      const double seg_dist = tier4_autoware_utils::calcSignedArcLength(
        input.points, closest_seg_idx,
        closest_seg_idx + 1);  // TODO(murooka) implement calcSignedArcLength(points, idx, point)

      const double closest_z = input.points.at(closest_seg_idx).point.pose.position.z;
      const double next_z = input.points.at(closest_seg_idx + 1).point.pose.position.z;
      pre_refined_goal.point.pose.position.z =
        std::abs(seg_dist) < 1e-06
          ? next_z
          : closest_z + (next_z - closest_z) * closest_to_pre_goal_dist / seg_dist;

      const double closest_vel = input.points.at(closest_seg_idx).point.longitudinal_velocity_mps;
      const double next_vel = input.points.at(closest_seg_idx + 1).point.longitudinal_velocity_mps;
      const double internal_div_ratio = std::clamp(closest_to_pre_goal_dist / seg_dist, 0.0, 1.0);
      pre_refined_goal.point.longitudinal_velocity_mps =
        std::abs(seg_dist) < 1e-06 ? next_vel
                                   : closest_vel + (next_vel - closest_vel) * internal_div_ratio;

      pre_refined_goal.lane_ids = input.points.back().lane_ids;
    }

    // find min_dist_index whose distance to goal is shorter than search_radius_range
    const auto min_dist_index_opt =
      findNearestIndexToGoal(input.points, goal, goal_lane_id, search_radius_range);
    if (!min_dist_index_opt) {
      return false;
    }
    const size_t min_dist_index =
      min_dist_index_opt.get();  // min_dist_idx point is inside the search_radius_range circle￼ ￼

    // find min_dist_out_of_circle_index whose distance to goal is longer than search_radius_range
    size_t min_dist_out_of_circle_index = 0;
    {
      // NOTE: type of i must be int since i will be -1 even if the condition is 0<=i
      for (int i = min_dist_index; 0 <= i; --i) {
        const double dist = tier4_autoware_utils::calcDistance2d(input.points.at(i).point, goal);
        min_dist_out_of_circle_index = i;
        if (search_radius_range < dist) {
          break;
        }
      }
    }

    for (size_t i = 0; i <= min_dist_out_of_circle_index; ++i) {
      output_ptr->points.push_back(input.points.at(i));
    }
    output_ptr->points.push_back(pre_refined_goal);
    output_ptr->points.push_back(refined_goal);

    output_ptr->drivable_area = input.drivable_area;
    return true;
  } catch (std::out_of_range & ex) {
    RCLCPP_ERROR_STREAM(
      rclcpp::get_logger("behavior_path_planner").get_child("utilities"),
      "failed to set goal: " << ex.what());
    return false;
  }
}

const Pose refineGoal(const Pose & goal, const lanelet::ConstLanelet & goal_lanelet)
{
  // return goal;
  const auto lanelet_point = lanelet::utils::conversion::toLaneletPoint(goal.position);
  const double distance = boost::geometry::distance(
    goal_lanelet.polygon2d().basicPolygon(), lanelet::utils::to2D(lanelet_point).basicPoint());
  if (distance < std::numeric_limits<double>::epsilon()) {
    return goal;
  }

  const auto segment = lanelet::utils::getClosestSegment(
    lanelet::utils::to2D(lanelet_point), goal_lanelet.centerline());
  if (segment.empty()) {
    return goal;
  }

  Pose refined_goal;
  {
    // find position
    const auto p1 = segment.front().basicPoint();
    const auto p2 = segment.back().basicPoint();
    const auto direction_vector = (p2 - p1).normalized();
    const auto p1_to_goal = lanelet_point.basicPoint() - p1;
    const double s = direction_vector.dot(p1_to_goal);
    const auto refined_point = p1 + direction_vector * s;

    refined_goal.position.x = refined_point.x();
    refined_goal.position.y = refined_point.y();
    refined_goal.position.z = refined_point.z();

    // find orientation
    const double yaw = std::atan2(direction_vector.y(), direction_vector.x());
    tf2::Quaternion tf_quat;
    tf_quat.setRPY(0, 0, yaw);
    refined_goal.orientation = tf2::toMsg(tf_quat);
  }
  return refined_goal;
}

PathWithLaneId refinePathForGoal(
  const double search_radius_range, const double search_rad_range, const PathWithLaneId & input,
  const Pose & goal, const int64_t goal_lane_id)
{
  PathWithLaneId filtered_path, path_with_goal;
  filtered_path = removeOverlappingPoints(input);

  // always set zero velocity at the end of path for safety
  if (!filtered_path.points.empty()) {
    filtered_path.points.back().point.longitudinal_velocity_mps = 0.0;
  }

  if (setGoal(
        search_radius_range, search_rad_range, filtered_path, goal, goal_lane_id,
        &path_with_goal)) {
    return path_with_goal;
  } else {
    return filtered_path;
  }
}

bool containsGoal(const lanelet::ConstLanelets & lanes, const lanelet::Id & goal_id)
{
  for (const auto & lane : lanes) {
    if (lane.id() == goal_id) {
      return true;
    }
  }
  return false;
}

// input lanes must be in sequence
OccupancyGrid generateDrivableArea(
  const lanelet::ConstLanelets & lanes, const double resolution, const double vehicle_length,
  const std::shared_ptr<const PlannerData> planner_data)
{
  const auto & params = planner_data->parameters;
  const auto route_handler = planner_data->route_handler;
  const auto current_pose = planner_data->self_pose;

  // search closest lanelet to current pose from given lanelets
  const int nearest_lane_idx = [&]() -> int {
    lanelet::ConstLanelet closest_lanelet;
    if (lanelet::utils::query::getClosestLanelet(lanes, current_pose->pose, &closest_lanelet)) {
      for (size_t i = 0; i < lanes.size(); ++i) {
        if (lanes.at(i).id() == closest_lanelet.id()) {
          return i;
        }
      }
    }
    return 0;
  }();

  // calculate min/max x and y
  const auto lanelet_scope = drivable_area_utils::getLaneletScope(
    lanes, nearest_lane_idx, current_pose->pose, params.drivable_lane_forward_length,
    params.drivable_lane_backward_length, params.drivable_lane_margin);

  const double min_x =
    drivable_area_utils::quantize(lanelet_scope.at(0) - params.drivable_area_margin, resolution);
  const double min_y =
    drivable_area_utils::quantize(lanelet_scope.at(1) - params.drivable_area_margin, resolution);
  const double max_x =
    drivable_area_utils::quantize(lanelet_scope.at(2) + params.drivable_area_margin, resolution);
  const double max_y =
    drivable_area_utils::quantize(lanelet_scope.at(3) + params.drivable_area_margin, resolution);

  const double width = max_x - min_x;
  const double height = max_y - min_y;

  lanelet::ConstLanelets drivable_lanes;
  {  // add lanes which covers initial and final footprints
    // 1. add preceding lanes before current pose
    const auto lanes_before_current_pose = route_handler->getLanesBeforePose(
      current_pose->pose, params.drivable_lane_backward_length + params.drivable_lane_margin);
    drivable_lanes.insert(
      drivable_lanes.end(), lanes_before_current_pose.begin(), lanes_before_current_pose.end());

    // 2. add lanes
    drivable_lanes.insert(drivable_lanes.end(), lanes.begin(), lanes.end());

    // 3. add succeeding lanes after goal
    if (containsGoal(lanes, route_handler->getGoalLaneId())) {
      const auto lanes_after_goal = route_handler->getLanesAfterGoal(vehicle_length);
      drivable_lanes.insert(drivable_lanes.end(), lanes_after_goal.begin(), lanes_after_goal.end());
    }
  }

  OccupancyGrid occupancy_grid;
  PoseStamped grid_origin;

  // calculate grid origin
  {
    grid_origin.header = current_pose->header;

    grid_origin.pose.position.x = min_x;
    grid_origin.pose.position.y = min_y;
    grid_origin.pose.position.z = current_pose->pose.position.z;
  }

  // header
  {
    occupancy_grid.header.stamp = current_pose->header.stamp;
    occupancy_grid.header.frame_id = "map";
  }

  // info
  {
    const int width_cell = std::round(width / resolution);
    const int height_cell = std::round(height / resolution);

    occupancy_grid.info.map_load_time = occupancy_grid.header.stamp;
    occupancy_grid.info.resolution = resolution;
    occupancy_grid.info.width = width_cell;
    occupancy_grid.info.height = height_cell;
    occupancy_grid.info.origin = grid_origin.pose;
  }

  // occupancy_grid.data = image;
  {
    constexpr uint8_t free_space = 0;
    constexpr uint8_t occupied_space = 100;
    // get transform
    tf2::Stamped<tf2::Transform> tf_grid2map, tf_map2grid;
    tf2::fromMsg(grid_origin, tf_grid2map);
    tf_map2grid.setData(tf_grid2map.inverse());
    const auto geom_tf_map2grid = tf2::toMsg(tf_map2grid);

    // convert lane polygons into cv type
    cv::Mat cv_image(
      occupancy_grid.info.width, occupancy_grid.info.height, CV_8UC1, cv::Scalar(occupied_space));
    for (const auto & lane : drivable_lanes) {
      lanelet::BasicPolygon2d lane_poly = lane.polygon2d().basicPolygon();

      if (lane.hasAttribute("intersection_area")) {
        const std::string area_id = lane.attributeOr("intersection_area", "none");
        const auto intersection_area =
          route_handler->getIntersectionAreaById(atoi(area_id.c_str()));
        const auto poly = lanelet::utils::to2D(intersection_area).basicPolygon();
        std::vector<lanelet::BasicPolygon2d> lane_polys{};
        if (boost::geometry::intersection(poly, lane_poly, lane_polys)) {
          lane_poly = lane_polys.front();
        }
      }

      // create drivable area using opencv
      std::vector<std::vector<cv::Point>> cv_polygons;
      std::vector<cv::Point> cv_polygon;
      for (const auto & p : lane_poly) {
        const double z = lane.polygon3d().basicPolygon().at(0).z();
        Point geom_pt = tier4_autoware_utils::createPoint(p.x(), p.y(), z);
        Point transformed_geom_pt;
        tf2::doTransform(geom_pt, transformed_geom_pt, geom_tf_map2grid);
        cv_polygon.push_back(toCVPoint(transformed_geom_pt, width, height, resolution));
      }
      cv_polygons.push_back(cv_polygon);
      // fill in drivable area and copy to occupancy grid
      cv::fillPoly(cv_image, cv_polygons, cv::Scalar(free_space));
    }

    // Closing
    // NOTE: Because of the discretization error, there may be some discontinuity between two
    // successive lanelets in the drivable area. This issue is dealt with by the erode/dilate
    // process.
    constexpr int num_iter = 1;
    cv::Mat cv_erode, cv_dilate;
    cv::erode(cv_image, cv_erode, cv::Mat(), cv::Point(-1, -1), num_iter);
    cv::dilate(cv_erode, cv_dilate, cv::Mat(), cv::Point(-1, -1), num_iter);

    // const auto & cv_image_reshaped = cv_dilate.reshape(1, 1);
    imageToOccupancyGrid(cv_dilate, &occupancy_grid);
    occupancy_grid.data[0] = 0;
    // cv_image_reshaped.copyTo(occupancy_grid.data);
  }

  return occupancy_grid;
}

double getDistanceToEndOfLane(const Pose & current_pose, const lanelet::ConstLanelets & lanelets)
{
  const auto & arc_coordinates = lanelet::utils::getArcCoordinates(lanelets, current_pose);
  const double lanelet_length = lanelet::utils::getLaneletLength3d(lanelets);
  return lanelet_length - arc_coordinates.length;
}

double getDistanceToNextIntersection(
  const Pose & current_pose, const lanelet::ConstLanelets & lanelets)
{
  const auto & arc_coordinates = lanelet::utils::getArcCoordinates(lanelets, current_pose);

  lanelet::ConstLanelet current_lanelet;
  if (!lanelet::utils::query::getClosestLanelet(lanelets, current_pose, &current_lanelet)) {
    return std::numeric_limits<double>::max();
  }

  double distance = 0;
  bool is_after_current_lanelet = false;
  for (const auto & llt : lanelets) {
    if (llt == current_lanelet) {
      is_after_current_lanelet = true;
    }
    if (is_after_current_lanelet && llt.hasAttribute("turn_direction")) {
      bool is_lane_change_yes = false;
      const auto right_line = llt.rightBound();
      if (right_line.hasAttribute(lanelet::AttributeNamesString::LaneChange)) {
        const auto attr = right_line.attribute(lanelet::AttributeNamesString::LaneChange);
        if (attr.value() == std::string("yes")) {
          is_lane_change_yes = true;
        }
      }
      const auto left_line = llt.leftBound();
      if (left_line.hasAttribute(lanelet::AttributeNamesString::LaneChange)) {
        const auto attr = left_line.attribute(lanelet::AttributeNamesString::LaneChange);
        if (attr.value() == std::string("yes")) {
          is_lane_change_yes = true;
        }
      }
      if (!is_lane_change_yes) {
        return distance - arc_coordinates.length;
      }
    }
    distance += lanelet::utils::getLaneletLength3d(llt);
  }

  return std::numeric_limits<double>::max();
}

double getDistanceToCrosswalk(
  const Pose & current_pose, const lanelet::ConstLanelets & lanelets,
  const lanelet::routing::RoutingGraphContainer & overall_graphs)
{
  const auto & arc_coordinates = lanelet::utils::getArcCoordinates(lanelets, current_pose);

  lanelet::ConstLanelet current_lanelet;
  if (!lanelet::utils::query::getClosestLanelet(lanelets, current_pose, &current_lanelet)) {
    return std::numeric_limits<double>::max();
  }

  double distance = 0;
  bool is_after_current_lanelet = false;
  for (const auto & llt : lanelets) {
    if (llt == current_lanelet) {
      is_after_current_lanelet = true;
    }
    // check lane change tag
    bool is_lane_change_yes = false;
    const auto right_line = llt.rightBound();
    if (right_line.hasAttribute(lanelet::AttributeNamesString::LaneChange)) {
      const auto attr = right_line.attribute(lanelet::AttributeNamesString::LaneChange);
      if (attr.value() == std::string("yes")) {
        is_lane_change_yes = true;
      }
    }
    const auto left_line = llt.leftBound();
    if (left_line.hasAttribute(lanelet::AttributeNamesString::LaneChange)) {
      const auto attr = left_line.attribute(lanelet::AttributeNamesString::LaneChange);
      if (attr.value() == std::string("yes")) {
        is_lane_change_yes = true;
      }
    }

    if (is_after_current_lanelet && !is_lane_change_yes) {
      const auto conflicting_crosswalks = overall_graphs.conflictingInGraph(llt, 1);
      if (!(conflicting_crosswalks.empty())) {
        // create centerline
        const lanelet::ConstLineString2d lanelet_centerline = llt.centerline2d();
        LineString2d centerline;
        for (const auto & point : lanelet_centerline) {
          boost::geometry::append(centerline, Point2d(point.x(), point.y()));
        }

        // create crosswalk polygon and calculate distance
        double min_distance_to_crosswalk = std::numeric_limits<double>::max();
        for (const auto & crosswalk : conflicting_crosswalks) {
          lanelet::CompoundPolygon2d lanelet_crosswalk_polygon = crosswalk.polygon2d();
          Polygon2d polygon;
          for (const auto & point : lanelet_crosswalk_polygon) {
            polygon.outer().emplace_back(point.x(), point.y());
          }
          polygon.outer().push_back(polygon.outer().front());

          std::vector<Point2d> points_intersection;
          boost::geometry::intersection(centerline, polygon, points_intersection);

          for (const auto & point : points_intersection) {
            lanelet::ConstLanelets lanelets = {llt};
            Pose pose_point;
            pose_point.position.x = point.x();
            pose_point.position.y = point.y();
            const lanelet::ArcCoordinates & arc_crosswalk =
              lanelet::utils::getArcCoordinates(lanelets, pose_point);

            const double distance_to_crosswalk = arc_crosswalk.length;
            if (distance_to_crosswalk < min_distance_to_crosswalk) {
              min_distance_to_crosswalk = distance_to_crosswalk;
            }
          }
        }
        if (distance + min_distance_to_crosswalk > arc_coordinates.length) {
          return distance + min_distance_to_crosswalk - arc_coordinates.length;
        }
      }
    }
    distance += lanelet::utils::getLaneletLength3d(llt);
  }

  return std::numeric_limits<double>::max();
}

double getSignedDistance(
  const Pose & current_pose, const Pose & goal_pose, const lanelet::ConstLanelets & lanelets)
{
  const auto arc_current = lanelet::utils::getArcCoordinates(lanelets, current_pose);
  const auto arc_goal = lanelet::utils::getArcCoordinates(lanelets, goal_pose);

  return arc_goal.length - arc_current.length;
}

std::vector<uint64_t> getIds(const lanelet::ConstLanelets & lanelets)
{
  std::vector<uint64_t> ids;
  for (const auto & llt : lanelets) {
    ids.push_back(llt.id());
  }
  return ids;
}

Path convertToPathFromPathWithLaneId(const PathWithLaneId & path_with_lane_id)
{
  Path path;
  path.header = path_with_lane_id.header;
  path.drivable_area = path_with_lane_id.drivable_area;
  for (const auto & pt_with_lane_id : path_with_lane_id.points) {
    path.points.push_back(pt_with_lane_id.point);
  }
  return path;
}

lanelet::Polygon3d getVehiclePolygon(
  const Pose & vehicle_pose, const double vehicle_width, const double base_link2front)
{
  tf2::Vector3 front_left, front_right, rear_left, rear_right;
  front_left.setValue(base_link2front, vehicle_width / 2, 0);
  front_right.setValue(base_link2front, -vehicle_width / 2, 0);
  rear_left.setValue(0, vehicle_width / 2, 0);
  rear_right.setValue(0, -vehicle_width / 2, 0);

  tf2::Transform tf;
  tf2::fromMsg(vehicle_pose, tf);
  const auto front_left_transformed = tf * front_left;
  const auto front_right_transformed = tf * front_right;
  const auto rear_left_transformed = tf * rear_left;
  const auto rear_right_transformed = tf * rear_right;

  lanelet::Polygon3d llt_poly;
  auto toPoint3d = [](const tf2::Vector3 & v) { return lanelet::Point3d(0, v.x(), v.y(), v.z()); };
  llt_poly.push_back(toPoint3d(front_left_transformed));
  llt_poly.push_back(toPoint3d(front_right_transformed));
  llt_poly.push_back(toPoint3d(rear_right_transformed));
  llt_poly.push_back(toPoint3d(rear_left_transformed));

  return llt_poly;
}

PathPointWithLaneId insertStopPoint(double length, PathWithLaneId * path)
{
  if (path->points.empty()) {
    RCLCPP_ERROR_STREAM(
      rclcpp::get_logger("behavior_path_planner").get_child("utilities"),
      "failed to insert stop point. path points is empty.");
    return PathPointWithLaneId();
  }

  double accumulated_length = 0;
  double insert_idx = 0;
  Pose stop_pose;
  for (size_t i = 1; i < path->points.size(); i++) {
    const auto prev_pose = path->points.at(i - 1).point.pose;
    const auto curr_pose = path->points.at(i).point.pose;
    const double segment_length = tier4_autoware_utils::calcDistance3d(prev_pose, curr_pose);
    accumulated_length += segment_length;
    if (accumulated_length > length) {
      insert_idx = i;
      const double ratio = 1 - (accumulated_length - length) / segment_length;
      stop_pose = lerpByPose(prev_pose, curr_pose, ratio);
      break;
    }
  }
  if (accumulated_length <= length) {
    RCLCPP_ERROR_STREAM(
      rclcpp::get_logger("behavior_path_planner").get_child("utilities"),
      "failed to insert stop point. length is longer than path length");
    return PathPointWithLaneId();
  }

  PathPointWithLaneId stop_point;
  stop_point.lane_ids = path->points.at(insert_idx).lane_ids;
  stop_point.point.pose = stop_pose;
  path->points.insert(path->points.begin() + insert_idx, stop_point);
  for (size_t i = insert_idx; i < path->points.size(); i++) {
    path->points.at(i).point.longitudinal_velocity_mps = 0.0;
    path->points.at(i).point.lateral_velocity_mps = 0.0;
  }
  return stop_point;
}

double getDistanceToShoulderBoundary(
  const lanelet::ConstLanelets & shoulder_lanelets, const Pose & pose)
{
  lanelet::ConstLanelet closest_shoulder_lanelet;
  lanelet::ArcCoordinates arc_coordinates;
  if (lanelet::utils::query::getClosestLanelet(
        shoulder_lanelets, pose, &closest_shoulder_lanelet)) {
    const auto lanelet_point = lanelet::utils::conversion::toLaneletPoint(pose.position);
    const auto & left_line_2d = lanelet::utils::to2D(closest_shoulder_lanelet.leftBound3d());
    arc_coordinates = lanelet::geometry::toArcCoordinates(
      left_line_2d, lanelet::utils::to2D(lanelet_point).basicPoint());

  } else {
    RCLCPP_ERROR_STREAM(
      rclcpp::get_logger("behavior_path_planner").get_child("utilities"),
      "closest shoulder lanelet not found.");
  }

  return arc_coordinates.distance;
}

double getDistanceToRightBoundary(const lanelet::ConstLanelets & lanelets, const Pose & pose)
{
  lanelet::ConstLanelet closest_shoulder_lanelet;
  lanelet::ArcCoordinates arc_coordinates;
  if (lanelet::utils::query::getClosestLanelet(lanelets, pose, &closest_shoulder_lanelet)) {
    const auto lanelet_point = lanelet::utils::conversion::toLaneletPoint(pose.position);
    const auto & right_line_2d = lanelet::utils::to2D(closest_shoulder_lanelet.rightBound3d());
    arc_coordinates = lanelet::geometry::toArcCoordinates(
      right_line_2d, lanelet::utils::to2D(lanelet_point).basicPoint());
  } else {
    RCLCPP_ERROR_STREAM(
      rclcpp::get_logger("behavior_path_planner").get_child("utilities"),
      "closest shoulder lanelet not found.");
  }

  return arc_coordinates.distance;
}

double getArcLengthToTargetLanelet(
  const lanelet::ConstLanelets & lanelet_sequence, const lanelet::ConstLanelet & target_lane,
  const Pose & pose)
{
  const auto arc_pose = lanelet::utils::getArcCoordinates(lanelet_sequence, pose);

  const auto target_center_line = target_lane.centerline().basicLineString();

  Pose front_pose, back_pose;

  {
    const auto front_point = lanelet::utils::conversion::toGeomMsgPt(target_center_line.front());
    const double front_yaw = lanelet::utils::getLaneletAngle(target_lane, front_point);
    front_pose.position = front_point;
    tf2::Quaternion tf_quat;
    tf_quat.setRPY(0, 0, front_yaw);
    front_pose.orientation = tf2::toMsg(tf_quat);
  }

  {
    const auto back_point = lanelet::utils::conversion::toGeomMsgPt(target_center_line.back());
    const double back_yaw = lanelet::utils::getLaneletAngle(target_lane, back_point);
    back_pose.position = back_point;
    tf2::Quaternion tf_quat;
    tf_quat.setRPY(0, 0, back_yaw);
    back_pose.orientation = tf2::toMsg(tf_quat);
  }

  const auto arc_front = lanelet::utils::getArcCoordinates(lanelet_sequence, front_pose);
  const auto arc_back = lanelet::utils::getArcCoordinates(lanelet_sequence, back_pose);

  return std::max(
    std::min(arc_front.length - arc_pose.length, arc_back.length - arc_pose.length), 0.0);
}

std::vector<Polygon2d> getTargetLaneletPolygons(
  const lanelet::PolygonLayer & map_polygons, lanelet::ConstLanelets & lanelets, const Pose & pose,
  const double check_length, const std::string & target_type)
{
  std::vector<Polygon2d> polygons;

  // create lanelet polygon
  const auto arclength = lanelet::utils::getArcCoordinates(lanelets, pose);
  const auto llt_polygon = lanelet::utils::getPolygonFromArcLength(
    lanelets, arclength.length, arclength.length + check_length);
  const auto llt_polygon_2d = lanelet::utils::to2D(llt_polygon).basicPolygon();

  // If the number of vertices is not enough to create polygon, return empty polygon container
  if (llt_polygon_2d.size() < 3) {
    return polygons;
  }

  Polygon2d llt_polygon_bg;
  for (const auto & llt_pt : llt_polygon_2d) {
    llt_polygon_bg.outer().emplace_back(llt_pt.x(), llt_pt.y());
  }
  llt_polygon_bg.outer().push_back(llt_polygon_bg.outer().front());

  for (const auto & map_polygon : map_polygons) {
    const std::string type = map_polygon.attributeOr(lanelet::AttributeName::Type, "");
    // If the target_type is different
    // or the number of vertices is not enough to create polygon, skip the loop
    if (type == target_type && map_polygon.size() > 2) {
      // create map polygon
      Polygon2d map_polygon_bg;
      for (const auto & pt : map_polygon) {
        map_polygon_bg.outer().emplace_back(pt.x(), pt.y());
      }
      map_polygon_bg.outer().push_back(map_polygon_bg.outer().front());
      if (boost::geometry::intersects(llt_polygon_bg, map_polygon_bg)) {
        polygons.push_back(map_polygon_bg);
      }
    }
  }
  return polygons;
}

void occupancyGridToImage(const OccupancyGrid & occupancy_grid, cv::Mat * cv_image)
{
  const int width = cv_image->cols;
  const int height = cv_image->rows;
  for (int x = width - 1; x >= 0; x--) {
    for (int y = height - 1; y >= 0; y--) {
      const int idx = (height - 1 - y) + (width - 1 - x) * height;
      const unsigned char intensity = occupancy_grid.data.at(idx);
      cv_image->at<unsigned char>(y, x) = intensity;
    }
  }
}

void imageToOccupancyGrid(const cv::Mat & cv_image, OccupancyGrid * occupancy_grid)
{
  const int width = cv_image.cols;
  const int height = cv_image.rows;
  occupancy_grid->data.clear();
  occupancy_grid->data.resize(width * height);
  for (int x = width - 1; x >= 0; x--) {
    for (int y = height - 1; y >= 0; y--) {
      const int idx = (height - 1 - y) + (width - 1 - x) * height;
      const unsigned char intensity = cv_image.at<unsigned char>(y, x);
      occupancy_grid->data.at(idx) = intensity;
    }
  }
}

cv::Point toCVPoint(
  const Point & geom_point, const double width_m, const double height_m, const double resolution)
{
  return cv::Point(
    static_cast<int>((height_m - geom_point.y) / resolution),
    static_cast<int>((width_m - geom_point.x) / resolution));
}

// TODO(Horibe) There is a similar function in route_handler.
std::shared_ptr<PathWithLaneId> generateCenterLinePath(
  const std::shared_ptr<const PlannerData> & planner_data)
{
  auto centerline_path = std::make_shared<PathWithLaneId>();

  const auto & p = planner_data->parameters;

  const auto & route_handler = planner_data->route_handler;
  const auto & pose = planner_data->self_pose;

  lanelet::ConstLanelet current_lane;
  if (!route_handler->getClosestLaneletWithinRoute(pose->pose, &current_lane)) {
    RCLCPP_ERROR(
      rclcpp::get_logger("behavior_path_planner").get_child("utilities"),
      "failed to find closest lanelet within route!!!");
    return {};  // TODO(Horibe) What should be returned?
  }

  // For lanelet_sequence with desired length
  lanelet::ConstLanelets lanelet_sequence = route_handler->getLaneletSequence(
    current_lane, pose->pose, p.backward_path_length, p.forward_path_length);

  *centerline_path = getCenterLinePath(
    *route_handler, lanelet_sequence, pose->pose, p.backward_path_length, p.forward_path_length, p);

  centerline_path->header = route_handler->getRouteHeader();

  centerline_path->drivable_area = util::generateDrivableArea(
    lanelet_sequence, p.drivable_area_resolution, p.vehicle_length, planner_data);

  return centerline_path;
}

// TODO(Azu) Some parts of is the same with generateCenterLinePath. Therefore it might be better if
// we can refactor some of the code for better readability
lanelet::ConstLineStrings3d getDrivableAreaForAllSharedLinestringLanelets(
  const std::shared_ptr<const PlannerData> & planner_data)
{
  const auto & p = planner_data->parameters;
  const auto & route_handler = planner_data->route_handler;
  const auto & ego_pose = planner_data->self_pose->pose;

  lanelet::ConstLanelet current_lane;
  if (!route_handler->getClosestLaneletWithinRoute(ego_pose, &current_lane)) {
    RCLCPP_ERROR(
      rclcpp::get_logger("behavior_path_planner").get_child("utilities"),
      "failed to find closest lanelet within route!!!");
    return {};
  }

  const auto current_lanes = route_handler->getLaneletSequence(
    current_lane, ego_pose, p.backward_path_length, p.forward_path_length);
  lanelet::ConstLineStrings3d linestring_shared;
  for (const auto & lane : current_lanes) {
    lanelet::ConstLineStrings3d furthest_line = route_handler->getFurthestLinestring(lane);
    linestring_shared.insert(linestring_shared.end(), furthest_line.begin(), furthest_line.end());
  }

  return linestring_shared;
}

PredictedObjects filterObjectsByVelocity(const PredictedObjects & objects, double lim_v)
{
  return filterObjectsByVelocity(objects, -lim_v, lim_v);
}

PredictedObjects filterObjectsByVelocity(
  const PredictedObjects & objects, double min_v, double max_v)
{
  PredictedObjects filtered;
  filtered.header = objects.header;
  for (const auto & obj : objects.objects) {
    const auto v = std::abs(obj.kinematics.initial_twist_with_covariance.twist.linear.x);
    if (min_v < v && v < max_v) {
      filtered.objects.push_back(obj);
    }
  }
  return filtered;
}

void shiftPose(Pose * pose, double shift_length)
{
  auto yaw = tf2::getYaw(pose->orientation);
  pose->position.x -= std::sin(yaw) * shift_length;
  pose->position.y += std::cos(yaw) * shift_length;
}

PathWithLaneId getCenterLinePath(
  const RouteHandler & route_handler, const lanelet::ConstLanelets & lanelet_sequence,
  const Pose & pose, const double backward_path_length, const double forward_path_length,
  const BehaviorPathPlannerParameters & parameter, double optional_length)
{
  PathWithLaneId reference_path;

  if (lanelet_sequence.empty()) {
    return reference_path;
  }

  const auto arc_coordinates = lanelet::utils::getArcCoordinates(lanelet_sequence, pose);
  const double s = arc_coordinates.length;
  const double s_backward = std::max(0., s - backward_path_length);
  double s_forward = s + forward_path_length;

  const double buffer = parameter.backward_length_buffer_for_end_of_lane +
                        optional_length;  // buffer for min_lane_change_length
  const int num_lane_change =
    std::abs(route_handler.getNumLaneToPreferredLane(lanelet_sequence.back()));
  const double lane_length = lanelet::utils::getLaneletLength2d(lanelet_sequence);
  const double lane_change_buffer =
    num_lane_change * (parameter.minimum_lane_change_length + buffer);

  if (route_handler.isDeadEndLanelet(lanelet_sequence.back())) {
    s_forward = std::min(s_forward, lane_length - lane_change_buffer);
  }

  if (route_handler.isInGoalRouteSection(lanelet_sequence.back())) {
    const auto goal_arc_coordinates =
      lanelet::utils::getArcCoordinates(lanelet_sequence, route_handler.getGoalPose());
    s_forward = std::min(s_forward, goal_arc_coordinates.length - lane_change_buffer);
  }

  return route_handler.getCenterLinePath(lanelet_sequence, s_backward, s_forward, true);
}

bool checkLaneIsInIntersection(
  const RouteHandler & route_handler, const PathWithLaneId & reference_path,
  const lanelet::ConstLanelets & lanelet_sequence, double & additional_length_to_add)
{
  if (lanelet_sequence.size() < 2 || reference_path.points.empty()) {
    return false;
  }

  const auto & path_points = reference_path.points;
  auto end_of_route_pose = path_points.back().point.pose;

  lanelet::ConstLanelet check_lane;
  if (!lanelet::utils::query::getClosestLanelet(lanelet_sequence, end_of_route_pose, &check_lane)) {
    return false;
  }

  lanelet::ConstLanelets lane_change_prohibited_lanes{check_lane};

  const auto checking_rev_iter = std::find_if(
    lanelet_sequence.crbegin(), lanelet_sequence.crend(),
    [&check_lane](const lanelet::ConstLanelet & lanelet) noexcept {
      return lanelet.id() == check_lane.id();
    });
  if (checking_rev_iter == lanelet_sequence.crend()) {
    return false;
  }

  const auto prev_lane = std::next(checking_rev_iter);
  if (prev_lane == lanelet_sequence.crend()) {
    return false;
  }

  const auto lanes = route_handler.getNextLanelets(*prev_lane);
  const auto isHaveNeighborWithTurnDirection = [&](const lanelet::ConstLanelets & lanes) noexcept {
    return std::any_of(lanes.cbegin(), lanes.cend(), [](const lanelet::ConstLanelet & lane) {
      return lane.hasAttribute("turn_direction");
    });
  };
  if (!isHaveNeighborWithTurnDirection(lanes)) {
    return false;
  }

  const auto checkAttribute = [](const lanelet::ConstLineString3d & linestring) noexcept {
    const auto & attribute_name = lanelet::AttributeNamesString::LaneChange;
    if (linestring.hasAttribute(attribute_name)) {
      const auto attr = linestring.attribute(attribute_name);
      if (attr.value() == std::string("yes")) {
        return true;
      }
    }
    return false;
  };

  const auto isLaneChangeAttributeYes =
    [checkAttribute](const lanelet::ConstLanelet & lanelet) noexcept {
      return (checkAttribute(lanelet.rightBound()) || checkAttribute(lanelet.leftBound()));
    };

  for (auto prev_ll_itr = prev_lane; prev_ll_itr != lanelet_sequence.crend(); ++prev_ll_itr) {
    if (!isLaneChangeAttributeYes(*prev_ll_itr)) {
      lane_change_prohibited_lanes.push_back(*prev_ll_itr);
    } else {
      break;
    }
  }

  std::reverse(lane_change_prohibited_lanes.begin(), lane_change_prohibited_lanes.end());
  const auto prohibited_arc_coordinate =
    lanelet::utils::getArcCoordinates(lane_change_prohibited_lanes, end_of_route_pose);

  constexpr double small_earlier_stopping_buffer = 0.2;
  additional_length_to_add =
    prohibited_arc_coordinate.length +
    small_earlier_stopping_buffer;  // additional a slight "buffer so that vehicle stop earlier"

  return true;
}

PathWithLaneId setDecelerationVelocity(
  const RouteHandler & route_handler, const PathWithLaneId & input,
  const lanelet::ConstLanelets & lanelet_sequence, const double lane_change_prepare_duration,
  const double lane_change_buffer)
{
  auto reference_path = input;
  if (
    route_handler.isDeadEndLanelet(lanelet_sequence.back()) &&
    lane_change_prepare_duration > std::numeric_limits<double>::epsilon()) {
    for (auto & point : reference_path.points) {
      const double lane_length = lanelet::utils::getLaneletLength2d(lanelet_sequence);
      const auto arclength = lanelet::utils::getArcCoordinates(lanelet_sequence, point.point.pose);
      const double distance_to_end =
        std::max(0.0, lane_length - lane_change_buffer - arclength.length);
      point.point.longitudinal_velocity_mps = std::min(
        point.point.longitudinal_velocity_mps,
        static_cast<float>(distance_to_end / lane_change_prepare_duration));
    }
  }
  return reference_path;
}

PathWithLaneId setDecelerationVelocity(
  const RouteHandler & route_handler, const PathWithLaneId & input,
  const lanelet::ConstLanelets & lanelet_sequence, const double distance_after_pullover,
  const double pullover_distance_min, const double distance_before_pull_over,
  const double deceleration_interval, Pose goal_pose)
{
  auto reference_path = input;
  const auto pullover_buffer =
    distance_after_pullover + pullover_distance_min + distance_before_pull_over;
  const auto arclength_goal_pose =
    lanelet::utils::getArcCoordinates(lanelet_sequence, goal_pose).length;
  const auto arclength_pull_over_start = arclength_goal_pose - pullover_buffer;
  const auto arclength_path_front =
    lanelet::utils::getArcCoordinates(lanelet_sequence, reference_path.points.front().point.pose)
      .length;

  if (
    route_handler.isDeadEndLanelet(lanelet_sequence.back()) &&
    pullover_distance_min > std::numeric_limits<double>::epsilon()) {
    for (auto & point : reference_path.points) {
      const auto arclength =
        lanelet::utils::getArcCoordinates(lanelet_sequence, point.point.pose).length;
      const double distance_to_pull_over_start =
        std::max(0.0, arclength_pull_over_start - arclength);
      point.point.longitudinal_velocity_mps = std::min(
        point.point.longitudinal_velocity_mps,
        static_cast<float>(distance_to_pull_over_start / deceleration_interval) *
          point.point.longitudinal_velocity_mps);
    }
  }

  double distance_to_pull_over_start =
    std::max(0.0, arclength_pull_over_start - arclength_path_front);
  const auto stop_point = util::insertStopPoint(distance_to_pull_over_start, &reference_path);

  return reference_path;
}

PathWithLaneId setDecelerationVelocity(
  const PathWithLaneId & input, const double target_velocity, const Pose target_pose,
  const double buffer, const double deceleration_interval)
{
  auto reference_path = input;

  for (auto & point : reference_path.points) {
    const auto arclength_to_target = std::max(
      0.0, tier4_autoware_utils::calcSignedArcLength(
             reference_path.points, point.point.pose.position, target_pose.position) +
             buffer);
    if (arclength_to_target > deceleration_interval) continue;
    point.point.longitudinal_velocity_mps = std::min(
      point.point.longitudinal_velocity_mps,
      static_cast<float>(
        (arclength_to_target / deceleration_interval) *
          (point.point.longitudinal_velocity_mps - target_velocity) +
        target_velocity));
  }

  const auto stop_point_length =
    tier4_autoware_utils::calcSignedArcLength(reference_path.points, 0, target_pose.position) +
    buffer;
  if (target_velocity == 0.0 && stop_point_length > 0) {
    const auto stop_point = util::insertStopPoint(stop_point_length, &reference_path);
  }

  return reference_path;
}

PathWithLaneId setDecelerationVelocityForTurnSignal(
  const PathWithLaneId & input, const Pose target_pose, const double turn_light_on_threshold_time)
{
  auto reference_path = input;

  for (auto & point : reference_path.points) {
    const auto arclength_to_target = std::max(
      0.0, tier4_autoware_utils::calcSignedArcLength(
             reference_path.points, point.point.pose.position, target_pose.position));
    point.point.longitudinal_velocity_mps = std::min(
      point.point.longitudinal_velocity_mps,
      static_cast<float>(arclength_to_target / turn_light_on_threshold_time));
  }

  const auto stop_point_length =
    tier4_autoware_utils::calcSignedArcLength(reference_path.points, 0, target_pose.position);
  if (stop_point_length > 0) {
    const auto stop_point = util::insertStopPoint(stop_point_length, &reference_path);
  }

  return reference_path;
}

std::uint8_t getHighestProbLabel(const std::vector<ObjectClassification> & classification)
{
  std::uint8_t label = ObjectClassification::UNKNOWN;
  float highest_prob = 0.0;
  for (const auto & _class : classification) {
    if (highest_prob < _class.probability) {
      highest_prob = _class.probability;
      label = _class.label;
    }
  }
  return label;
}

lanelet::ConstLanelets getCurrentLanes(const std::shared_ptr<const PlannerData> & planner_data)
{
  const auto & route_handler = planner_data->route_handler;
  const auto current_pose = planner_data->self_pose->pose;
  const auto common_parameters = planner_data->parameters;

  lanelet::ConstLanelet current_lane;
  if (!route_handler->getClosestLaneletWithinRoute(current_pose, &current_lane)) {
    // RCLCPP_ERROR(getLogger(), "failed to find closest lanelet within route!!!");
    std::cerr << "failed to find closest lanelet within route!!!" << std::endl;
    return {};  // TODO(Horibe) what should be returned?
  }

  // For current_lanes with desired length
  return route_handler->getLaneletSequence(
    current_lane, current_pose, common_parameters.backward_path_length,
    common_parameters.forward_path_length);
}

lanelet::ConstLanelets getExtendedCurrentLanes(
  const std::shared_ptr<const PlannerData> & planner_data)
{
  const auto & route_handler = planner_data->route_handler;
  const auto current_pose = planner_data->self_pose->pose;
  const auto common_parameters = planner_data->parameters;

  lanelet::ConstLanelet current_lane;
  if (!route_handler->getClosestLaneletWithinRoute(current_pose, &current_lane)) {
    RCLCPP_ERROR(
      rclcpp::get_logger("behavior_path_planner").get_child("utilities"),
      "failed to find closest lanelet within route!!!");
    return {};  // TODO(Horibe) what should be returned?
  }

  // For current_lanes with desired length
  auto current_lanes = route_handler->getLaneletSequence(
    current_lane, current_pose, common_parameters.backward_path_length,
    common_parameters.forward_path_length);

  // Add next_lanes
  const auto next_lanes = route_handler->getNextLanelets(current_lanes.back());
  if (!next_lanes.empty()) {
    // TODO(kosuke55) which lane should be added?
    current_lanes.push_back(next_lanes.front());
  }

  // Add prev_lane
  lanelet::ConstLanelets prev_lanes;
  if (route_handler->getPreviousLaneletsWithinRoute(current_lanes.front(), &prev_lanes)) {
    // TODO(kosuke55) which lane should be added?
    current_lanes.insert(current_lanes.begin(), 0, prev_lanes.front());
  }

  return current_lanes;
}

}  // namespace util
}  // namespace behavior_path_planner
