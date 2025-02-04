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

#include "behavior_path_planner/scene_module/avoidance/avoidance_utils.hpp"

#include "behavior_path_planner/path_utilities.hpp"
#include "behavior_path_planner/scene_module/avoidance/avoidance_module.hpp"
#include "behavior_path_planner/scene_module/avoidance/avoidance_module_data.hpp"
#include "behavior_path_planner/utilities.hpp"

#include <autoware_auto_tf2/tf2_autoware_auto_msgs.hpp>
#include <lanelet2_extension/utility/utilities.hpp>
#include <tier4_autoware_utils/tier4_autoware_utils.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace behavior_path_planner
{

using motion_utils::calcLongitudinalOffsetPoint;
using motion_utils::findNearestSegmentIndex;
using motion_utils::insertTargetPoint;
using tier4_autoware_utils::calcDistance2d;
using tier4_autoware_utils::calcOffsetPose;
using tier4_autoware_utils::calcYawDeviation;
using tier4_autoware_utils::createQuaternionFromRPY;
using tier4_autoware_utils::getPose;
using tier4_autoware_utils::pose2transform;

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

geometry_msgs::msg::Polygon toMsg(const tier4_autoware_utils::Polygon2d & polygon, const double z)
{
  geometry_msgs::msg::Polygon ret;
  for (const auto & p : polygon.outer()) {
    ret.points.push_back(createPoint32(p.x(), p.y(), z));
  }
  return ret;
}

/**
 * @brief update traveling distance, velocity and acceleration under constant jerk.
 * @param (x) current traveling distance [m/s]
 * @param (v) current velocity [m/s]
 * @param (a) current acceleration [m/ss]
 * @param (j) target jerk [m/sss]
 * @param (t) time [s]
 * @return updated traveling distance, velocity and acceleration
 */
std::tuple<double, double, double> update(
  const double x, const double v, const double a, const double j, const double t)
{
  const double a_new = a + j * t;
  const double v_new = v + a * t + 0.5 * j * t * t;
  const double x_new = x + v * t + 0.5 * a * t * t + (1.0 / 6.0) * j * t * t * t;

  return {x_new, v_new, a_new};
}

/**
 * @brief calculate distance until velocity is reached target velocity (TYPE: TRAPEZOID ACCELERATION
 * PROFILE). this type of profile has ZERO JERK time.
 *
 * [ACCELERATION PROFILE]
 *  a  ^
 *     |
 *  a0 *
 *     |*
 * ----+-*-------------------*------> t
 *     |  *                 *
 *     |   *               *
 *     | a1 ***************
 *     |
 *
 * [JERK PROFILE]
 *  j  ^
 *     |
 *     |               ja ****
 *     |                  *
 * ----+----***************---------> t
 *     |    *
 *     |    *
 *  jd ******
 *     |
 *
 * @param (v0) current velocity [m/s]
 * @param (a0) current acceleration [m/ss]
 * @param (am) minimum deceleration [m/ss]
 * @param (ja) maximum jerk [m/sss]
 * @param (jd) minimum jerk [m/sss]
 * @param (t_min) duration of constant deceleration [s]
 * @return moving distance until velocity is reached vt [m]
 */
double calcDecelDistPlanType1(
  const double v0, const double a0, const double am, const double ja, const double jd,
  const double t_min)
{
  constexpr double epsilon = 1e-3;

  // negative jerk time
  const double j1 = am < a0 ? jd : ja;
  const double t1 = epsilon < (am - a0) / j1 ? (am - a0) / j1 : 0.0;
  const auto [x1, v1, a1] = update(0.0, v0, a0, j1, t1);

  // zero jerk time
  const double t2 = epsilon < t_min ? t_min : 0.0;
  const auto [x2, v2, a2] = update(x1, v1, a1, 0.0, t2);

  // positive jerk time
  const double t3 = epsilon < (0.0 - am) / ja ? (0.0 - am) / ja : 0.0;
  const auto [x3, v3, a3] = update(x2, v2, a2, ja, t3);

  return x3;
}

/**
 * @brief calculate distance until velocity is reached target velocity (TYPE: TRIANGLE ACCELERATION
 * PROFILE), This type of profile do NOT have ZERO JERK time.
 *
 * [ACCELERATION PROFILE]
 *  a  ^
 *     |
 *  a0 *
 *     |*
 * ----+-*-----*--------------------> t
 *     |  *   *
 *     |   * *
 *     | a1 *
 *     |
 *
 * [JERK PROFILE]
 *  j  ^
 *     |
 *     | ja ****
 *     |    *
 * ----+----*-----------------------> t
 *     |    *
 *     |    *
 *  jd ******
 *     |
 *
 * @param (v0) current velocity [m/s]
 * @param (a0) current acceleration [m/ss]
 * @param (am) minimum deceleration [m/ss]
 * @param (ja) maximum jerk [m/sss]
 * @param (jd) minimum jerk [m/sss]
 * @return moving distance until velocity is reached vt [m]
 */
double calcDecelDistPlanType2(
  const double v0, const double vt, const double a0, const double ja, const double jd)
{
  constexpr double epsilon = 1e-3;

  const double a1_square = (vt - v0 - 0.5 * (0.0 - a0) / jd * a0) * (2.0 * ja * jd / (ja - jd));
  const double a1 = -std::sqrt(a1_square);

  // negative jerk time
  const double t1 = epsilon < (a1 - a0) / jd ? (a1 - a0) / jd : 0.0;
  const auto [x1, v1, no_use_a1] = update(0.0, v0, a0, jd, t1);

  // positive jerk time
  const double t2 = epsilon < (0.0 - a1) / ja ? (0.0 - a1) / ja : 0.0;
  const auto [x2, v2, a2] = update(x1, v1, a1, ja, t2);

  return x2;
}

/**
 * @brief calculate distance until velocity is reached target velocity (TYPE: LINEAR ACCELERATION
 * PROFILE). This type of profile has only positive jerk time.
 *
 * [ACCELERATION PROFILE]
 *  a  ^
 *     |
 * ----+----*-----------------------> t
 *     |   *
 *     |  *
 *     | *
 *     |*
 *  a0 *
 *     |
 *
 * [JERK PROFILE]
 *  j  ^
 *     |
 *  ja ******
 *     |    *
 *     |    *
 * ----+----*-----------------------> t
 *     |
 *
 * @param (v0) current velocity [m/s]
 * @param (a0) current acceleration [m/ss]
 * @param (ja) maximum jerk [m/sss]
 * @return moving distance until velocity is reached vt [m]
 */
double calcDecelDistPlanType3(const double v0, const double a0, const double ja)
{
  constexpr double epsilon = 1e-3;

  // positive jerk time
  const double t_acc = (0.0 - a0) / ja;
  const double t1 = epsilon < t_acc ? t_acc : 0.0;
  const auto [x1, v1, a1] = update(0.0, v0, a0, ja, t1);

  return x1;
}

}  // namespace

bool isOnRight(const ObjectData & obj) { return obj.lateral < 0.0; }

double calcShiftLength(
  const bool & is_object_on_right, const double & overhang_dist, const double & avoid_margin)
{
  const auto shift_length =
    is_object_on_right ? (overhang_dist + avoid_margin) : (overhang_dist - avoid_margin);
  return std::fabs(shift_length) > 1e-3 ? shift_length : 0.0;
}

bool isSameDirectionShift(const bool & is_object_on_right, const double & shift_length)
{
  return (is_object_on_right == std::signbit(shift_length));
}

ShiftedPath toShiftedPath(const PathWithLaneId & path)
{
  ShiftedPath out;
  out.path = path;
  out.shift_length.resize(path.points.size());
  std::fill(out.shift_length.begin(), out.shift_length.end(), 0.0);
  return out;
}

ShiftLineArray toShiftLineArray(const AvoidLineArray & avoid_points)
{
  ShiftLineArray shift_lines;
  for (const auto & ap : avoid_points) {
    shift_lines.push_back(ap);
  }
  return shift_lines;
}

size_t findPathIndexFromArclength(
  const std::vector<double> & path_arclength_arr, const double target_arc)
{
  if (path_arclength_arr.empty()) {
    return 0;
  }

  for (size_t i = 0; i < path_arclength_arr.size(); ++i) {
    if (path_arclength_arr.at(i) > target_arc) {
      return i;
    }
  }
  return path_arclength_arr.size() - 1;
}

std::vector<size_t> concatParentIds(
  const std::vector<size_t> & ids1, const std::vector<size_t> & ids2)
{
  std::set<size_t> id_set{ids1.begin(), ids1.end()};
  for (const auto id : ids2) {
    id_set.insert(id);
  }
  const auto v = std::vector<size_t>{id_set.begin(), id_set.end()};
  return v;
}

double lerpShiftLengthOnArc(double arc, const AvoidLine & ap)
{
  if (ap.start_longitudinal <= arc && arc < ap.end_longitudinal) {
    if (std::abs(ap.getRelativeLongitudinal()) < 1.0e-5) {
      return ap.end_shift_length;
    }
    const auto start_weight = (ap.end_longitudinal - arc) / ap.getRelativeLongitudinal();
    return start_weight * ap.start_shift_length + (1.0 - start_weight) * ap.end_shift_length;
  }
  return 0.0;
}

void clipByMinStartIdx(const AvoidLineArray & shift_lines, PathWithLaneId & path)
{
  if (path.points.empty()) {
    return;
  }

  size_t min_start_idx = std::numeric_limits<size_t>::max();
  for (const auto & sl : shift_lines) {
    min_start_idx = std::min(min_start_idx, sl.start_idx);
  }
  min_start_idx = std::min(min_start_idx, path.points.size() - 1);
  path.points =
    std::vector<PathPointWithLaneId>{path.points.begin() + min_start_idx, path.points.end()};
}

void fillLongitudinalAndLengthByClosestFootprint(
  const PathWithLaneId & path, const PredictedObject & object, const Point & ego_pos,
  ObjectData & obj)
{
  tier4_autoware_utils::Polygon2d object_poly{};
  util::calcObjectPolygon(object, &object_poly);

  const double distance = motion_utils::calcSignedArcLength(
    path.points, ego_pos, object.kinematics.initial_pose_with_covariance.pose.position);
  double min_distance = distance;
  double max_distance = distance;
  for (const auto & p : object_poly.outer()) {
    const auto point = tier4_autoware_utils::createPoint(p.x(), p.y(), 0.0);
    const double arc_length = motion_utils::calcSignedArcLength(path.points, ego_pos, point);
    min_distance = std::min(min_distance, arc_length);
    max_distance = std::max(max_distance, arc_length);
  }
  obj.longitudinal = min_distance;
  obj.length = max_distance - min_distance;
  return;
}

void fillLongitudinalAndLengthByClosestEnvelopeFootprint(
  const PathWithLaneId & path, const Point & ego_pos, ObjectData & obj)
{
  const double distance = motion_utils::calcSignedArcLength(
    path.points, ego_pos, obj.object.kinematics.initial_pose_with_covariance.pose.position);
  double min_distance = distance;
  double max_distance = distance;
  for (const auto & p : obj.envelope_poly.outer()) {
    const auto point = tier4_autoware_utils::createPoint(p.x(), p.y(), 0.0);
    const double arc_length = motion_utils::calcSignedArcLength(path.points, ego_pos, point);
    min_distance = std::min(min_distance, arc_length);
    max_distance = std::max(max_distance, arc_length);
  }
  obj.longitudinal = min_distance;
  obj.length = max_distance - min_distance;
  return;
}

double calcOverhangDistance(
  const ObjectData & object_data, const Pose & base_pose, Point & overhang_pose)
{
  double largest_overhang = isOnRight(object_data) ? -100.0 : 100.0;  // large number

  tier4_autoware_utils::Polygon2d object_poly{};
  util::calcObjectPolygon(object_data.object, &object_poly);

  for (const auto & p : object_poly.outer()) {
    const auto point = tier4_autoware_utils::createPoint(p.x(), p.y(), 0.0);
    const auto lateral = tier4_autoware_utils::calcLateralDeviation(base_pose, point);

    const auto & overhang_pose_on_right = [&overhang_pose, &largest_overhang, &point, &lateral]() {
      if (lateral > largest_overhang) {
        overhang_pose = point;
      }
      return std::max(largest_overhang, lateral);
    };

    const auto & overhang_pose_on_left = [&overhang_pose, &largest_overhang, &point, &lateral]() {
      if (lateral < largest_overhang) {
        overhang_pose = point;
      }
      return std::min(largest_overhang, lateral);
    };

    largest_overhang = isOnRight(object_data) ? overhang_pose_on_right() : overhang_pose_on_left();
  }
  return largest_overhang;
}

double calcEnvelopeOverhangDistance(
  const ObjectData & object_data, const Pose & base_pose, Point & overhang_pose)
{
  double largest_overhang = isOnRight(object_data) ? -100.0 : 100.0;  // large number

  for (const auto & p : object_data.envelope_poly.outer()) {
    const auto point = tier4_autoware_utils::createPoint(p.x(), p.y(), 0.0);
    const auto lateral = tier4_autoware_utils::calcLateralDeviation(base_pose, point);

    const auto & overhang_pose_on_right = [&overhang_pose, &largest_overhang, &point, &lateral]() {
      if (lateral > largest_overhang) {
        overhang_pose = point;
      }
      return std::max(largest_overhang, lateral);
    };

    const auto & overhang_pose_on_left = [&overhang_pose, &largest_overhang, &point, &lateral]() {
      if (lateral < largest_overhang) {
        overhang_pose = point;
      }
      return std::min(largest_overhang, lateral);
    };

    largest_overhang = isOnRight(object_data) ? overhang_pose_on_right() : overhang_pose_on_left();
  }
  return largest_overhang;
}

void setEndData(
  AvoidLine & ap, const double length, const geometry_msgs::msg::Pose & end, const size_t end_idx,
  const double end_dist)
{
  ap.end_shift_length = length;
  ap.end = end;
  ap.end_idx = end_idx;
  ap.end_longitudinal = end_dist;
}

void setStartData(
  AvoidLine & ap, const double start_shift_length, const geometry_msgs::msg::Pose & start,
  const size_t start_idx, const double start_dist)
{
  ap.start_shift_length = start_shift_length;
  ap.start = start;
  ap.start_idx = start_idx;
  ap.start_longitudinal = start_dist;
}

std::string getUuidStr(const ObjectData & obj)
{
  std::stringstream hex_value;
  for (const auto & uuid : obj.object.object_id.uuid) {
    hex_value << std::hex << std::setfill('0') << std::setw(2) << +uuid;
  }
  return hex_value.str();
}

std::vector<std::string> getUuidStr(const ObjectDataArray & objs)
{
  std::vector<std::string> uuids;
  uuids.reserve(objs.size());
  for (const auto & o : objs) {
    uuids.push_back(getUuidStr(o));
  }
  return uuids;
}

Polygon2d createEnvelopePolygon(
  const ObjectData & object_data, const Pose & closest_pose, const double envelope_buffer)
{
  namespace bg = boost::geometry;
  using tier4_autoware_utils::Point2d;
  using tier4_autoware_utils::Polygon2d;
  using Box = bg::model::box<Point2d>;

  Polygon2d object_polygon{};
  util::calcObjectPolygon(object_data.object, &object_polygon);

  const auto toPolygon2d = [](const geometry_msgs::msg::Polygon & polygon) {
    Polygon2d ret{};

    for (const auto & p : polygon.points) {
      ret.outer().push_back(Point2d(p.x, p.y));
    }

    return ret;
  };

  Pose pose_2d = closest_pose;
  pose_2d.orientation = createQuaternionFromRPY(0.0, 0.0, tf2::getYaw(closest_pose.orientation));

  TransformStamped geometry_tf{};
  geometry_tf.transform = pose2transform(pose_2d);

  tf2::Transform tf;
  tf2::fromMsg(geometry_tf.transform, tf);
  TransformStamped inverse_geometry_tf{};
  inverse_geometry_tf.transform = tf2::toMsg(tf.inverse());

  geometry_msgs::msg::Polygon out_ros_polygon{};
  tf2::doTransform(
    toMsg(object_polygon, closest_pose.position.z), out_ros_polygon, inverse_geometry_tf);

  const auto envelope_box = bg::return_envelope<Box>(toPolygon2d(out_ros_polygon));

  Polygon2d envelope_poly{};
  bg::convert(envelope_box, envelope_poly);

  geometry_msgs::msg::Polygon envelope_ros_polygon{};
  tf2::doTransform(
    toMsg(envelope_poly, closest_pose.position.z), envelope_ros_polygon, geometry_tf);

  std::vector<Polygon2d> offset_polygons{};
  bg::strategy::buffer::distance_symmetric<double> distance_strategy(envelope_buffer);
  bg::strategy::buffer::join_miter join_strategy;
  bg::strategy::buffer::end_flat end_strategy;
  bg::strategy::buffer::side_straight side_strategy;
  bg::strategy::buffer::point_circle point_strategy;
  bg::buffer(
    toPolygon2d(envelope_ros_polygon), offset_polygons, distance_strategy, side_strategy,
    join_strategy, end_strategy, point_strategy);

  return offset_polygons.front();
}

void getEdgePoints(
  const Polygon2d & object_polygon, const double threshold, std::vector<Point> & edge_points)
{
  if (object_polygon.outer().size() < 2) {
    return;
  }

  const size_t num_points = object_polygon.outer().size();
  for (size_t i = 0; i < num_points - 1; ++i) {
    const auto & curr_p = object_polygon.outer().at(i);
    const auto & next_p = object_polygon.outer().at(i + 1);
    const auto & prev_p =
      i == 0 ? object_polygon.outer().at(num_points - 2) : object_polygon.outer().at(i - 1);
    const Eigen::Vector2d current_to_next(next_p.x() - curr_p.x(), next_p.y() - curr_p.y());
    const Eigen::Vector2d current_to_prev(prev_p.x() - curr_p.x(), prev_p.y() - curr_p.y());
    const double inner_val = current_to_next.dot(current_to_prev);
    if (std::fabs(inner_val) > threshold) {
      continue;
    }

    const auto edge_point = tier4_autoware_utils::createPoint(curr_p.x(), curr_p.y(), 0.0);
    edge_points.push_back(edge_point);
  }
}

void sortPolygonPoints(
  const std::vector<PolygonPoint> & points, std::vector<PolygonPoint> & sorted_points)
{
  sorted_points = points;
  if (points.size() <= 2) {
    // sort data based on longitudinal distance to the boundary
    std::sort(
      sorted_points.begin(), sorted_points.end(),
      [](const PolygonPoint & a, const PolygonPoint & b) { return a.lon_dist < b.lon_dist; });
    return;
  }

  // sort data based on lateral distance to the boundary
  std::sort(
    sorted_points.begin(), sorted_points.end(), [](const PolygonPoint & a, const PolygonPoint & b) {
      return std::fabs(a.lat_dist_to_bound) > std::fabs(b.lat_dist_to_bound);
    });
  PolygonPoint first_point;
  PolygonPoint second_point;
  if (sorted_points.at(0).lon_dist < sorted_points.at(1).lon_dist) {
    first_point = sorted_points.at(0);
    second_point = sorted_points.at(1);
  } else {
    first_point = sorted_points.at(1);
    second_point = sorted_points.at(0);
  }

  for (size_t i = 2; i < sorted_points.size(); ++i) {
    const auto & next_point = sorted_points.at(i);
    if (next_point.lon_dist < first_point.lon_dist) {
      sorted_points = {next_point, first_point, second_point};
      return;
    } else if (second_point.lon_dist < next_point.lon_dist) {
      sorted_points = {first_point, second_point, next_point};
      return;
    }
  }

  sorted_points = {first_point, second_point};
}

void getPointData(
  const std::vector<Point> & bound, const std::vector<Point> & edge_points,
  const double lat_dist_to_path, std::vector<PolygonPoint> & edge_points_data)
{
  for (const auto & edge_point : edge_points) {
    PolygonPoint edge_point_data;
    edge_point_data.point = edge_point;
    edge_point_data.lat_dist_to_bound = motion_utils::calcLateralOffset(bound, edge_point);
    edge_point_data.lon_dist = motion_utils::calcSignedArcLength(bound, 0, edge_point);
    if (lat_dist_to_path >= 0.0 && edge_point_data.lat_dist_to_bound > 0.0) {
      continue;
    } else if (lat_dist_to_path < 0.0 && edge_point_data.lat_dist_to_bound < 0.0) {
      continue;
    }

    edge_points_data.push_back(edge_point_data);
  }
}

std::vector<Point> updateBoundary(
  const std::vector<Point> & original_bound, const std::vector<Point> & edge_points,
  const std::vector<PolygonPoint> & sorted_points)
{
  if (edge_points.empty() || sorted_points.empty()) {
    return original_bound;
  }

  const size_t bound_size = original_bound.size();
  double min_dist_from_start_point = std::numeric_limits<double>::max();
  double min_dist_from_end_point = std::numeric_limits<double>::max();
  Point start_edge_point = edge_points.front();
  Point end_edge_point = edge_points.back();
  for (const auto & edge_point : edge_points) {
    const double dist_from_start_point =
      motion_utils::calcSignedArcLength(original_bound, 0, edge_point);
    const double dist_from_end_point =
      std::fabs(motion_utils::calcSignedArcLength(original_bound, bound_size - 1, edge_point));

    if (dist_from_start_point < min_dist_from_start_point) {
      start_edge_point = edge_point;
      min_dist_from_start_point = dist_from_start_point;
    }

    if (dist_from_end_point < min_dist_from_end_point) {
      end_edge_point = edge_point;
      min_dist_from_end_point = dist_from_end_point;
    }
  }

  // get start and end point
  const size_t start_segment_idx =
    motion_utils::findNearestSegmentIndex(original_bound, start_edge_point);
  const double front_offset = motion_utils::calcLongitudinalOffsetToSegment(
    original_bound, start_segment_idx, start_edge_point);
  const auto closest_front_point =
    motion_utils::calcLongitudinalOffsetPoint(original_bound, start_segment_idx, front_offset);
  const size_t end_segment_idx =
    motion_utils::findNearestSegmentIndex(original_bound, end_edge_point);
  const double end_offset =
    motion_utils::calcLongitudinalOffsetToSegment(original_bound, end_segment_idx, end_edge_point);
  const auto closest_end_point =
    motion_utils::calcLongitudinalOffsetPoint(original_bound, end_segment_idx, end_offset);

  std::vector<Point> updated_bound;

  const double min_dist = 1e-3;
  // copy original points until front point
  std::copy(
    original_bound.begin(), original_bound.begin() + start_segment_idx + 1,
    std::back_inserter(updated_bound));

  // insert closest front point
  if (
    closest_front_point &&
    tier4_autoware_utils::calcDistance2d(*closest_front_point, updated_bound.back()) > min_dist) {
    updated_bound.push_back(*closest_front_point);
  }

  // insert sorted points
  for (const auto & sorted_point : sorted_points) {
    if (tier4_autoware_utils::calcDistance2d(sorted_point.point, updated_bound.back()) > min_dist) {
      updated_bound.push_back(sorted_point.point);
    }
  }

  // insert closest end point
  if (
    closest_end_point &&
    tier4_autoware_utils::calcDistance2d(*closest_end_point, updated_bound.back()) > min_dist) {
    updated_bound.push_back(*closest_end_point);
  }

  // copy original points until the end of the original bound
  for (size_t i = end_segment_idx + 1; i < original_bound.size(); ++i) {
    if (
      tier4_autoware_utils::calcDistance2d(original_bound.at(i), updated_bound.back()) > min_dist) {
      updated_bound.push_back(original_bound.at(i));
    }
  }

  return updated_bound;
}

void generateDrivableArea(
  PathWithLaneId & path, const std::vector<DrivableLanes> & lanes, const double vehicle_length,
  const std::shared_ptr<const PlannerData> planner_data, const ObjectDataArray & objects,
  const bool enable_bound_clipping)
{
  util::generateDrivableArea(path, lanes, vehicle_length, planner_data);
  if (objects.empty() || !enable_bound_clipping) {
    return;
  }

  for (const auto & object : objects) {
    const auto & obj_pose = object.object.kinematics.initial_pose_with_covariance.pose;
    const auto & obj_poly = object.envelope_poly;
    constexpr double threshold = 0.01;

    // get edge points of the object
    std::vector<Point> edge_points;
    getEdgePoints(obj_poly, threshold, edge_points);

    // get a boundary that we have to change
    const double lat_dist_to_path = motion_utils::calcLateralOffset(path.points, obj_pose.position);
    auto & bound = lat_dist_to_path < 0.0 ? path.right_bound : path.left_bound;

    // get edge points data
    std::vector<PolygonPoint> edge_points_data;
    getPointData(bound, edge_points, lat_dist_to_path, edge_points_data);

    // sort points
    std::vector<PolygonPoint> sorted_points;
    sortPolygonPoints(edge_points_data, sorted_points);

    // update boundary
    bound = updateBoundary(bound, edge_points, sorted_points);
  }
}

double getLongitudinalVelocity(const Pose & p_ref, const Pose & p_target, const double v)
{
  return v * std::cos(calcYawDeviation(p_ref, p_target));
}

bool isCentroidWithinLanelets(
  const PredictedObject & object, const lanelet::ConstLanelets & target_lanelets)
{
  if (target_lanelets.empty()) {
    return false;
  }

  const auto & object_pos = object.kinematics.initial_pose_with_covariance.pose.position;
  lanelet::BasicPoint2d object_centroid(object_pos.x, object_pos.y);

  for (const auto & llt : target_lanelets) {
    if (boost::geometry::within(object_centroid, llt.polygon2d().basicPolygon())) {
      return true;
    }
  }

  return false;
}

lanelet::ConstLanelets getTargetLanelets(
  const std::shared_ptr<const PlannerData> & planner_data, lanelet::ConstLanelets & route_lanelets,
  const double left_offset, const double right_offset)
{
  const auto & rh = planner_data->route_handler;

  lanelet::ConstLanelets target_lanelets{};
  for (const auto & lane : route_lanelets) {
    auto l_offset = 0.0;
    auto r_offset = 0.0;

    const auto opt_left_lane = rh->getLeftLanelet(lane);
    if (opt_left_lane) {
      target_lanelets.push_back(opt_left_lane.get());
    } else {
      l_offset = left_offset;
    }

    const auto opt_right_lane = rh->getRightLanelet(lane);
    if (opt_right_lane) {
      target_lanelets.push_back(opt_right_lane.get());
    } else {
      r_offset = right_offset;
    }

    const auto right_opposite_lanes = rh->getRightOppositeLanelets(lane);
    if (!right_opposite_lanes.empty()) {
      target_lanelets.push_back(right_opposite_lanes.front());
    }

    const auto expand_lane = lanelet::utils::getExpandedLanelet(lane, l_offset, r_offset);
    target_lanelets.push_back(expand_lane);
  }

  return target_lanelets;
}

double calcDecelDistWithJerkAndAccConstraints(
  const double current_vel, const double target_vel, const double current_acc, const double acc_min,
  const double jerk_acc, const double jerk_dec)
{
  constexpr double epsilon = 1e-3;
  const double t_dec =
    acc_min < current_acc ? (acc_min - current_acc) / jerk_dec : (acc_min - current_acc) / jerk_acc;
  const double t_acc = (0.0 - acc_min) / jerk_acc;
  const double t_min = (target_vel - current_vel - current_acc * t_dec -
                        0.5 * jerk_dec * t_dec * t_dec - 0.5 * acc_min * t_acc) /
                       acc_min;

  // check if it is possible to decelerate to the target velocity
  // by simply bringing the current acceleration to zero.
  const auto is_decel_needed =
    0.5 * (0.0 - current_acc) / jerk_acc * current_acc > target_vel - current_vel;

  if (t_min > epsilon) {
    return calcDecelDistPlanType1(current_vel, current_acc, acc_min, jerk_acc, jerk_dec, t_min);
  } else if (is_decel_needed || current_acc > epsilon) {
    return calcDecelDistPlanType2(current_vel, target_vel, current_acc, jerk_acc, jerk_dec);
  }

  return calcDecelDistPlanType3(current_vel, current_acc, jerk_acc);
}

void insertDecelPoint(
  const Point & p_src, const double offset, const double velocity, PathWithLaneId & path,
  boost::optional<Pose> & p_out)
{
  const auto decel_point = calcLongitudinalOffsetPoint(path.points, p_src, offset);

  if (!decel_point) {
    // TODO(Satoshi OTA)  Think later the process in the case of no decel point found.
    return;
  }

  const auto seg_idx = findNearestSegmentIndex(path.points, decel_point.get());
  const auto insert_idx = insertTargetPoint(seg_idx, decel_point.get(), path.points);

  if (!insert_idx) {
    // TODO(Satoshi OTA)  Think later the process in the case of no decel point found.
    return;
  }

  const auto insertVelocity = [&insert_idx](PathWithLaneId & path, const float v) {
    for (size_t i = insert_idx.get(); i < path.points.size(); ++i) {
      const auto & original_velocity = path.points.at(i).point.longitudinal_velocity_mps;
      path.points.at(i).point.longitudinal_velocity_mps = std::min(original_velocity, v);
    }
  };

  insertVelocity(path, velocity);

  p_out = getPose(path.points.at(insert_idx.get()));
}
}  // namespace behavior_path_planner
