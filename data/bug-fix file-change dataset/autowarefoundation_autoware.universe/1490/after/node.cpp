// Copyright 2022 TIER IV, Inc.
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

#include "obstacle_cruise_planner/node.hpp"

#include "motion_utils/trajectory/tmp_conversion.hpp"
#include "obstacle_cruise_planner/polygon_utils.hpp"
#include "obstacle_cruise_planner/utils.hpp"
#include "perception_utils/predicted_path_utils.hpp"
#include "tier4_autoware_utils/geometry/boost_polygon_utils.hpp"
#include "tier4_autoware_utils/ros/update_param.hpp"

#include <boost/format.hpp>

#include <algorithm>
#include <chrono>

namespace
{
VelocityLimitClearCommand createVelocityLimitClearCommandMsg(const rclcpp::Time & current_time)
{
  VelocityLimitClearCommand msg;
  msg.stamp = current_time;
  msg.sender = "obstacle_cruise_planner";
  msg.command = true;
  return msg;
}

// TODO(murooka) make this function common
size_t findExtendedNearestIndex(
  const Trajectory traj, const geometry_msgs::msg::Pose & pose, const double max_dist,
  const double max_yaw)
{
  const auto nearest_idx = motion_utils::findNearestIndex(traj.points, pose, max_dist, max_yaw);
  if (nearest_idx) {
    return nearest_idx.get();
  }
  return motion_utils::findNearestIndex(traj.points, pose.position);
}

Trajectory trimTrajectoryFrom(const Trajectory & input, const double nearest_idx)
{
  Trajectory output{};

  for (size_t i = nearest_idx; i < input.points.size(); ++i) {
    output.points.push_back(input.points.at(i));
  }

  return output;
}

bool isFrontObstacle(
  const Trajectory & traj, const size_t ego_idx, const geometry_msgs::msg::Point & obj_pos)
{
  size_t obj_idx = motion_utils::findNearestSegmentIndex(traj.points, obj_pos);

  const double ego_to_obj_distance =
    motion_utils::calcSignedArcLength(traj.points, ego_idx, obj_idx);

  if (ego_to_obj_distance < 0) {
    return false;
  }

  return true;
}

TrajectoryPoint calcLinearPoint(
  const TrajectoryPoint & p_from, const TrajectoryPoint & p_to, const double length)
{
  TrajectoryPoint output;
  const double dx = p_to.pose.position.x - p_from.pose.position.x;
  const double dy = p_to.pose.position.y - p_from.pose.position.y;
  const double norm = std::hypot(dx, dy);

  output = p_to;
  output.pose.position.x += length * dx / norm;
  output.pose.position.y += length * dy / norm;

  return output;
}

// TODO(murooka) replace with spline interpolation
Trajectory decimateTrajectory(const Trajectory & input, const double step_length)
{
  Trajectory output{};

  if (input.points.empty()) {
    return output;
  }

  double trajectory_length_sum = 0.0;
  double next_length = 0.0;

  constexpr double epsilon = 1e-3;
  for (int i = 0; i < static_cast<int>(input.points.size()) - 1; ++i) {
    const auto & p_front = input.points.at(i);
    const auto & p_back = input.points.at(i + 1);

    if (next_length <= trajectory_length_sum + epsilon) {
      const auto p_interpolate =
        calcLinearPoint(p_front, p_back, next_length - trajectory_length_sum);
      output.points.push_back(p_interpolate);
      next_length += step_length;
      continue;
    }

    trajectory_length_sum += tier4_autoware_utils::calcDistance2d(p_front, p_back);
  }

  // avoid "Same points are given"
  if (
    !input.points.empty() && !output.points.empty() &&
    epsilon < tier4_autoware_utils::calcDistance2d(input.points.back(), output.points.back())) {
    output.points.push_back(input.points.back());
  }

  return output;
}

PredictedPath getHighestConfidencePredictedPath(const PredictedObject & predicted_object)
{
  const auto reliable_path = std::max_element(
    predicted_object.kinematics.predicted_paths.begin(),
    predicted_object.kinematics.predicted_paths.end(),
    [](const PredictedPath & a, const PredictedPath & b) { return a.confidence < b.confidence; });
  return *reliable_path;
}

bool isAngleAlignedWithTrajectory(
  const Trajectory & traj, const geometry_msgs::msg::Pose & pose, const double threshold_angle)
{
  if (traj.points.empty()) {
    return false;
  }

  const double obj_yaw = tf2::getYaw(pose.orientation);

  const size_t nearest_idx = motion_utils::findNearestIndex(traj.points, pose.position);
  const double traj_yaw = tf2::getYaw(traj.points.at(nearest_idx).pose.orientation);

  const double diff_yaw = tier4_autoware_utils::normalizeRadian(obj_yaw - traj_yaw);

  // TODO(perception team) Currently predicted objects does not have orientation availability even
  // though sometimes orientation is not available.
  const bool is_aligned =
    std::abs(diff_yaw) <= threshold_angle || std::abs(M_PI - std::abs(diff_yaw)) <= threshold_angle;
  return is_aligned;
}

double calcAlignedAdaptiveCruise(
  const PredictedObject & predicted_object, const Trajectory & trajectory)
{
  const auto & object_pos = predicted_object.kinematics.initial_pose_with_covariance.pose.position;
  const auto & object_vel =
    predicted_object.kinematics.initial_twist_with_covariance.twist.linear.x;

  const size_t object_idx = motion_utils::findNearestIndex(trajectory.points, object_pos);

  const double object_yaw =
    tf2::getYaw(predicted_object.kinematics.initial_pose_with_covariance.pose.orientation);
  const double traj_yaw = tf2::getYaw(trajectory.points.at(object_idx).pose.orientation);

  return object_vel * std::cos(object_yaw - traj_yaw);
}

double calcObjectMaxLength(const autoware_auto_perception_msgs::msg::Shape & shape)
{
  if (shape.type == autoware_auto_perception_msgs::msg::Shape::BOUNDING_BOX) {
    return std::hypot(shape.dimensions.x / 2.0, shape.dimensions.y / 2.0);
  } else if (shape.type == autoware_auto_perception_msgs::msg::Shape::CYLINDER) {
    return shape.dimensions.x / 2.0;
  } else if (shape.type == autoware_auto_perception_msgs::msg::Shape::POLYGON) {
    double max_length_to_point = 0.0;
    for (const auto rel_point : shape.footprint.points) {
      const double length_to_point = std::hypot(rel_point.x, rel_point.y);
      if (max_length_to_point < length_to_point) {
        max_length_to_point = length_to_point;
      }
    }
    return max_length_to_point;
  }

  throw std::logic_error("The shape type is not supported in obstacle_cruise_planner.");
}
}  // namespace

namespace motion_planning
{
ObstacleCruisePlannerNode::ObstacleCruisePlannerNode(const rclcpp::NodeOptions & node_options)
: Node("obstacle_cruise_planner", node_options),
  self_pose_listener_(this),
  in_objects_ptr_(nullptr),
  lpf_acc_ptr_(nullptr),
  vehicle_info_(vehicle_info_util::VehicleInfoUtil(*this).getVehicleInfo())
{
  using std::placeholders::_1;

  // subscriber
  trajectory_sub_ = create_subscription<Trajectory>(
    "~/input/trajectory", rclcpp::QoS{1},
    std::bind(&ObstacleCruisePlannerNode::onTrajectory, this, _1));
  smoothed_trajectory_sub_ = create_subscription<Trajectory>(
    "/planning/scenario_planning/trajectory", rclcpp::QoS{1},
    std::bind(&ObstacleCruisePlannerNode::onSmoothedTrajectory, this, _1));
  objects_sub_ = create_subscription<PredictedObjects>(
    "~/input/objects", rclcpp::QoS{1}, std::bind(&ObstacleCruisePlannerNode::onObjects, this, _1));
  odom_sub_ = create_subscription<Odometry>(
    "~/input/odometry", rclcpp::QoS{1},
    std::bind(&ObstacleCruisePlannerNode::onOdometry, this, std::placeholders::_1));

  // publisher
  trajectory_pub_ = create_publisher<Trajectory>("~/output/trajectory", 1);
  vel_limit_pub_ =
    create_publisher<VelocityLimit>("~/output/velocity_limit", rclcpp::QoS{1}.transient_local());
  clear_vel_limit_pub_ = create_publisher<VelocityLimitClearCommand>(
    "~/output/clear_velocity_limit", rclcpp::QoS{1}.transient_local());
  debug_calculation_time_pub_ = create_publisher<Float32Stamped>("~/debug/calculation_time", 1);
  debug_cruise_wall_marker_pub_ =
    create_publisher<visualization_msgs::msg::MarkerArray>("~/debug/cruise_wall_marker", 1);
  debug_stop_wall_marker_pub_ =
    create_publisher<visualization_msgs::msg::MarkerArray>("~/virtual_wall", 1);
  debug_marker_pub_ = create_publisher<visualization_msgs::msg::MarkerArray>("~/debug/marker", 1);

  // longitudinal_info
  const auto longitudinal_info = [&]() {
    const double max_accel = declare_parameter<double>("normal.max_acc");
    const double min_accel = declare_parameter<double>("normal.min_acc");
    const double max_jerk = declare_parameter<double>("normal.max_jerk");
    const double min_jerk = declare_parameter<double>("normal.min_jerk");
    const double limit_max_accel = declare_parameter<double>("limit.max_acc");
    const double limit_min_accel = declare_parameter<double>("limit.min_acc");
    const double limit_max_jerk = declare_parameter<double>("limit.max_jerk");
    const double limit_min_jerk = declare_parameter<double>("limit.min_jerk");

    const double min_ego_accel_for_rss = declare_parameter<double>("common.min_ego_accel_for_rss");
    const double min_object_accel_for_rss =
      declare_parameter<double>("common.min_object_accel_for_rss");
    const double idling_time = declare_parameter<double>("common.idling_time");
    const double safe_distance_margin = declare_parameter<double>("common.safe_distance_margin");

    return LongitudinalInfo{
      max_accel,
      min_accel,
      max_jerk,
      min_jerk,
      limit_max_accel,
      limit_min_accel,
      limit_max_jerk,
      limit_min_jerk,
      idling_time,
      min_ego_accel_for_rss,
      min_object_accel_for_rss,
      safe_distance_margin};
  }();

  is_showing_debug_info_ = declare_parameter<bool>("common.is_showing_debug_info");

  // low pass filter for ego acceleration
  const double lpf_gain_for_accel = declare_parameter<double>("common.lpf_gain_for_accel");
  lpf_acc_ptr_ = std::make_shared<LowpassFilter1d>(lpf_gain_for_accel);

  {  // Obstacle filtering parameters
    obstacle_filtering_param_.rough_detection_area_expand_width =
      declare_parameter<double>("obstacle_filtering.rough_detection_area_expand_width");
    obstacle_filtering_param_.detection_area_expand_width =
      declare_parameter<double>("obstacle_filtering.detection_area_expand_width");
    obstacle_filtering_param_.decimate_trajectory_step_length =
      declare_parameter<double>("obstacle_filtering.decimate_trajectory_step_length");
    obstacle_filtering_param_.crossing_obstacle_velocity_threshold =
      declare_parameter<double>("obstacle_filtering.crossing_obstacle_velocity_threshold");
    obstacle_filtering_param_.collision_time_margin =
      declare_parameter<double>("obstacle_filtering.collision_time_margin");
    obstacle_filtering_param_.outside_rough_detection_area_expand_width =
      declare_parameter<double>("obstacle_filtering.outside_rough_detection_area_expand_width");
    obstacle_filtering_param_.ego_obstacle_overlap_time_threshold =
      declare_parameter<double>("obstacle_filtering.ego_obstacle_overlap_time_threshold");
    obstacle_filtering_param_.max_prediction_time_for_collision_check =
      declare_parameter<double>("obstacle_filtering.max_prediction_time_for_collision_check");
    obstacle_filtering_param_.crossing_obstacle_traj_angle_threshold =
      declare_parameter<double>("obstacle_filtering.crossing_obstacle_traj_angle_threshold");
    obstacle_filtering_param_.stop_obstacle_hold_time_threshold =
      declare_parameter<double>("obstacle_filtering.stop_obstacle_hold_time_threshold");
    obstacle_filtering_param_.prediction_resampling_time_interval =
      declare_parameter<double>("obstacle_filtering.prediction_resampling_time_interval");
    obstacle_filtering_param_.prediction_resampling_time_horizon =
      declare_parameter<double>("obstacle_filtering.prediction_resampling_time_horizon");

    {
      if (declare_parameter<bool>("obstacle_filtering.ignored_outside_obstacle_type.unknown")) {
        obstacle_filtering_param_.ignored_outside_obstacle_types.push_back(
          ObjectClassification::UNKNOWN);
      }
      if (declare_parameter<bool>("obstacle_filtering.ignored_outside_obstacle_type.car")) {
        obstacle_filtering_param_.ignored_outside_obstacle_types.push_back(
          ObjectClassification::CAR);
      }
      if (declare_parameter<bool>("obstacle_filtering.ignored_outside_obstacle_type.truck")) {
        obstacle_filtering_param_.ignored_outside_obstacle_types.push_back(
          ObjectClassification::TRUCK);
      }
      if (declare_parameter<bool>("obstacle_filtering.ignored_outside_obstacle_type.bus")) {
        obstacle_filtering_param_.ignored_outside_obstacle_types.push_back(
          ObjectClassification::BUS);
      }
      if (declare_parameter<bool>("obstacle_filtering.ignored_outside_obstacle_type.trailer")) {
        obstacle_filtering_param_.ignored_outside_obstacle_types.push_back(
          ObjectClassification::TRAILER);
      }
      if (declare_parameter<bool>("obstacle_filtering.ignored_outside_obstacle_type.motorcycle")) {
        obstacle_filtering_param_.ignored_outside_obstacle_types.push_back(
          ObjectClassification::MOTORCYCLE);
      }
      if (declare_parameter<bool>("obstacle_filtering.ignored_outside_obstacle_type.bicycle")) {
        obstacle_filtering_param_.ignored_outside_obstacle_types.push_back(
          ObjectClassification::BICYCLE);
      }
      if (declare_parameter<bool>("obstacle_filtering.ignored_outside_obstacle_type.pedestrian")) {
        obstacle_filtering_param_.ignored_outside_obstacle_types.push_back(
          ObjectClassification::PEDESTRIAN);
      }
    }
  }

  {  // planning algorithm
    const std::string planning_algorithm_param =
      declare_parameter<std::string>("common.planning_algorithm");
    planning_algorithm_ = getPlanningAlgorithmType(planning_algorithm_param);

    if (planning_algorithm_ == PlanningAlgorithm::OPTIMIZATION_BASE) {
      planner_ptr_ =
        std::make_unique<OptimizationBasedPlanner>(*this, longitudinal_info, vehicle_info_);
    } else if (planning_algorithm_ == PlanningAlgorithm::PID_BASE) {
      planner_ptr_ = std::make_unique<PIDBasedPlanner>(*this, longitudinal_info, vehicle_info_);
    } else {
      std::logic_error("Designated algorithm is not supported.");
    }

    min_behavior_stop_margin_ = declare_parameter<double>("common.min_behavior_stop_margin");
    nearest_dist_deviation_threshold_ =
      declare_parameter<double>("common.nearest_dist_deviation_threshold");
    nearest_yaw_deviation_threshold_ =
      declare_parameter<double>("common.nearest_yaw_deviation_threshold");
    obstacle_velocity_threshold_from_cruise_to_stop_ =
      declare_parameter<double>("common.obstacle_velocity_threshold_from_cruise_to_stop");
    obstacle_velocity_threshold_from_stop_to_cruise_ =
      declare_parameter<double>("common.obstacle_velocity_threshold_from_stop_to_cruise");
    planner_ptr_->setParams(
      is_showing_debug_info_, min_behavior_stop_margin_, nearest_dist_deviation_threshold_,
      nearest_yaw_deviation_threshold_);
  }

  {  // cruise obstacle type
    if (declare_parameter<bool>("common.cruise_obstacle_type.unknown")) {
      cruise_obstacle_types_.push_back(ObjectClassification::UNKNOWN);
    }
    if (declare_parameter<bool>("common.cruise_obstacle_type.car")) {
      cruise_obstacle_types_.push_back(ObjectClassification::CAR);
    }
    if (declare_parameter<bool>("common.cruise_obstacle_type.truck")) {
      cruise_obstacle_types_.push_back(ObjectClassification::TRUCK);
    }
    if (declare_parameter<bool>("common.cruise_obstacle_type.bus")) {
      cruise_obstacle_types_.push_back(ObjectClassification::BUS);
    }
    if (declare_parameter<bool>("common.cruise_obstacle_type.trailer")) {
      cruise_obstacle_types_.push_back(ObjectClassification::TRAILER);
    }
    if (declare_parameter<bool>("common.cruise_obstacle_type.motorcycle")) {
      cruise_obstacle_types_.push_back(ObjectClassification::MOTORCYCLE);
    }
    if (declare_parameter<bool>("common.cruise_obstacle_type.bicycle")) {
      cruise_obstacle_types_.push_back(ObjectClassification::BICYCLE);
    }
    if (declare_parameter<bool>("common.cruise_obstacle_type.pedestrian")) {
      cruise_obstacle_types_.push_back(ObjectClassification::PEDESTRIAN);
    }
  }

  {  // stop obstacle type
    if (declare_parameter<bool>("common.stop_obstacle_type.unknown")) {
      stop_obstacle_types_.push_back(ObjectClassification::UNKNOWN);
    }
    if (declare_parameter<bool>("common.stop_obstacle_type.car")) {
      stop_obstacle_types_.push_back(ObjectClassification::CAR);
    }
    if (declare_parameter<bool>("common.stop_obstacle_type.truck")) {
      stop_obstacle_types_.push_back(ObjectClassification::TRUCK);
    }
    if (declare_parameter<bool>("common.stop_obstacle_type.bus")) {
      stop_obstacle_types_.push_back(ObjectClassification::BUS);
    }
    if (declare_parameter<bool>("common.stop_obstacle_type.trailer")) {
      stop_obstacle_types_.push_back(ObjectClassification::TRAILER);
    }
    if (declare_parameter<bool>("common.stop_obstacle_type.motorcycle")) {
      stop_obstacle_types_.push_back(ObjectClassification::MOTORCYCLE);
    }
    if (declare_parameter<bool>("common.stop_obstacle_type.bicycle")) {
      stop_obstacle_types_.push_back(ObjectClassification::BICYCLE);
    }
    if (declare_parameter<bool>("common.stop_obstacle_type.pedestrian")) {
      stop_obstacle_types_.push_back(ObjectClassification::PEDESTRIAN);
    }
  }

  // wait for first self pose
  self_pose_listener_.waitForFirstPose();

  // set parameter callback
  set_param_res_ = this->add_on_set_parameters_callback(
    std::bind(&ObstacleCruisePlannerNode::onParam, this, std::placeholders::_1));
}

ObstacleCruisePlannerNode::PlanningAlgorithm ObstacleCruisePlannerNode::getPlanningAlgorithmType(
  const std::string & param) const
{
  if (param == "pid_base") {
    return PlanningAlgorithm::PID_BASE;
  } else if (param == "optimization_base") {
    return PlanningAlgorithm::OPTIMIZATION_BASE;
  }
  return PlanningAlgorithm::INVALID;
}

rcl_interfaces::msg::SetParametersResult ObstacleCruisePlannerNode::onParam(
  const std::vector<rclcpp::Parameter> & parameters)
{
  planner_ptr_->updateCommonParam(parameters);
  planner_ptr_->updateParam(parameters);

  tier4_autoware_utils::updateParam<bool>(
    parameters, "common.is_showing_debug_info", is_showing_debug_info_);
  planner_ptr_->setParams(
    is_showing_debug_info_, min_behavior_stop_margin_, nearest_dist_deviation_threshold_,
    nearest_yaw_deviation_threshold_);

  // obstacle_filtering
  tier4_autoware_utils::updateParam<double>(
    parameters, "obstacle_filtering.rough_detection_area_expand_width",
    obstacle_filtering_param_.rough_detection_area_expand_width);
  tier4_autoware_utils::updateParam<double>(
    parameters, "obstacle_filtering.detection_area_expand_width",
    obstacle_filtering_param_.detection_area_expand_width);
  tier4_autoware_utils::updateParam<double>(
    parameters, "obstacle_filtering.decimate_trajectory_step_length",
    obstacle_filtering_param_.decimate_trajectory_step_length);
  tier4_autoware_utils::updateParam<double>(
    parameters, "obstacle_filtering.crossing_obstacle_velocity_threshold",
    obstacle_filtering_param_.crossing_obstacle_velocity_threshold);
  tier4_autoware_utils::updateParam<double>(
    parameters, "obstacle_filtering.collision_time_margin",
    obstacle_filtering_param_.collision_time_margin);
  tier4_autoware_utils::updateParam<double>(
    parameters, "obstacle_filtering.outside_rough_detection_area_expand_width",
    obstacle_filtering_param_.outside_rough_detection_area_expand_width);
  tier4_autoware_utils::updateParam<double>(
    parameters, "obstacle_filtering.ego_obstacle_overlap_time_threshold",
    obstacle_filtering_param_.ego_obstacle_overlap_time_threshold);
  tier4_autoware_utils::updateParam<double>(
    parameters, "obstacle_filtering.max_prediction_time_for_collision_check",
    obstacle_filtering_param_.max_prediction_time_for_collision_check);
  tier4_autoware_utils::updateParam<double>(
    parameters, "obstacle_filtering.crossing_obstacle_traj_angle_threshold",
    obstacle_filtering_param_.crossing_obstacle_traj_angle_threshold);
  tier4_autoware_utils::updateParam<double>(
    parameters, "obstacle_filtering.stop_obstacle_hold_time_threshold",
    obstacle_filtering_param_.stop_obstacle_hold_time_threshold);
  tier4_autoware_utils::updateParam<double>(
    parameters, "obstacle_filtering.prediction_resampling_time_interval",
    obstacle_filtering_param_.prediction_resampling_time_interval);
  tier4_autoware_utils::updateParam<double>(
    parameters, "obstacle_filtering.prediction_resampling_time_horizon",
    obstacle_filtering_param_.prediction_resampling_time_horizon);

  rcl_interfaces::msg::SetParametersResult result;
  result.successful = true;
  result.reason = "success";
  return result;
}

void ObstacleCruisePlannerNode::onObjects(const PredictedObjects::ConstSharedPtr msg)
{
  in_objects_ptr_ = msg;
}

void ObstacleCruisePlannerNode::onOdometry(const Odometry::ConstSharedPtr msg)
{
  if (current_twist_ptr_) {
    prev_twist_ptr_ = current_twist_ptr_;
  }

  current_twist_ptr_ = std::make_unique<geometry_msgs::msg::TwistStamped>();
  current_twist_ptr_->header = msg->header;
  current_twist_ptr_->twist = msg->twist.twist;
}

void ObstacleCruisePlannerNode::onSmoothedTrajectory(const Trajectory::ConstSharedPtr msg)
{
  planner_ptr_->setSmoothedTrajectory(msg);
}

void ObstacleCruisePlannerNode::onTrajectory(const Trajectory::ConstSharedPtr msg)
{
  const auto current_pose_ptr = self_pose_listener_.getCurrentPose();

  // check if subscribed variables are ready
  if (
    msg->points.empty() || !current_twist_ptr_ || !prev_twist_ptr_ || !in_objects_ptr_ ||
    !current_pose_ptr) {
    return;
  }

  stop_watch_.tic(__func__);

  // Get Target Obstacles
  DebugData debug_data;
  const auto is_driving_forward = motion_utils::isDrivingForwardWithTwist(msg->points);
  is_driving_forward_ = is_driving_forward ? is_driving_forward.get() : is_driving_forward_;
  const auto target_obstacles = getTargetObstacles(
    *msg, current_pose_ptr->pose, current_twist_ptr_->twist.linear.x, is_driving_forward_,
    debug_data);

  // create data for stop
  const auto stop_data =
    createStopData(*msg, current_pose_ptr->pose, target_obstacles, is_driving_forward_);

  // stop planning
  const auto stop_traj = planner_ptr_->generateStopTrajectory(stop_data, debug_data);

  // create data for cruise
  const auto cruise_data =
    createCruiseData(stop_traj, current_pose_ptr->pose, target_obstacles, is_driving_forward_);

  // cruise planning
  boost::optional<VelocityLimit> vel_limit;
  const auto output_traj =
    planner_ptr_->generateCruiseTrajectory(cruise_data, vel_limit, debug_data);

  // publisher external velocity limit if required
  publishVelocityLimit(vel_limit);

  // Publish trajectory
  trajectory_pub_->publish(output_traj);

  // publish debug data
  publishDebugData(debug_data);

  // publish and print calculation time
  const double calculation_time = stop_watch_.toc(__func__);
  publishCalculationTime(calculation_time);
  RCLCPP_INFO_EXPRESSION(
    rclcpp::get_logger("ObstacleCruisePlanner"), is_showing_debug_info_, "%s := %f [ms]", __func__,
    calculation_time);
}

bool ObstacleCruisePlannerNode::isCruiseObstacle(const uint8_t label)
{
  const auto & types = cruise_obstacle_types_;
  return std::find(types.begin(), types.end(), label) != types.end();
}

bool ObstacleCruisePlannerNode::isStopObstacle(const uint8_t label)
{
  const auto & types = stop_obstacle_types_;
  return std::find(types.begin(), types.end(), label) != types.end();
}

ObstacleCruisePlannerData ObstacleCruisePlannerNode::createStopData(
  const Trajectory & trajectory, const geometry_msgs::msg::Pose & current_pose,
  const std::vector<TargetObstacle> & obstacles, const bool is_driving_forward)
{
  const auto current_time = now();
  const double current_vel = current_twist_ptr_->twist.linear.x;
  const double current_accel = calcCurrentAccel();

  // create planner_stop data
  ObstacleCruisePlannerData planner_data;
  planner_data.current_time = current_time;
  planner_data.traj = trajectory;
  planner_data.current_pose = current_pose;
  planner_data.current_vel = current_vel;
  planner_data.current_acc = current_accel;
  planner_data.is_driving_forward = is_driving_forward;
  for (const auto & obstacle : obstacles) {
    // consider all target obstacles when driving backward
    if (!planner_data.is_driving_forward || obstacle.has_stopped) {
      planner_data.target_obstacles.push_back(obstacle);
    }
  }

  return planner_data;
}

ObstacleCruisePlannerData ObstacleCruisePlannerNode::createCruiseData(
  const Trajectory & trajectory, const geometry_msgs::msg::Pose & current_pose,
  const std::vector<TargetObstacle> & obstacles, const bool is_driving_forward)
{
  const auto current_time = now();
  const double current_vel = current_twist_ptr_->twist.linear.x;
  const double current_accel = calcCurrentAccel();

  // create planner_stop data
  ObstacleCruisePlannerData planner_data;
  planner_data.current_time = current_time;
  planner_data.traj = trajectory;
  planner_data.current_pose = current_pose;
  planner_data.current_vel = current_vel;
  planner_data.current_acc = current_accel;
  planner_data.is_driving_forward = is_driving_forward;
  for (const auto & obstacle : obstacles) {
    if (planner_data.is_driving_forward && !obstacle.has_stopped) {
      planner_data.target_obstacles.push_back(obstacle);
    }
  }

  return planner_data;
}

double ObstacleCruisePlannerNode::calcCurrentAccel() const
{
  const double diff_vel = current_twist_ptr_->twist.linear.x - prev_twist_ptr_->twist.linear.x;
  const double diff_time = std::max(
    (rclcpp::Time(current_twist_ptr_->header.stamp) - rclcpp::Time(prev_twist_ptr_->header.stamp))
      .seconds(),
    1e-03);

  const double accel = diff_vel / diff_time;

  return lpf_acc_ptr_->filter(accel);
}

std::vector<TargetObstacle> ObstacleCruisePlannerNode::getTargetObstacles(
  const Trajectory & trajectory, const geometry_msgs::msg::Pose & current_pose,
  const double current_vel, const bool is_driving_forward, DebugData & debug_data)
{
  stop_watch_.tic(__func__);

  const auto target_obstacles = filterObstacles(
    *in_objects_ptr_, trajectory, current_pose, current_vel, is_driving_forward, debug_data);

  // print calculation time
  const double calculation_time = stop_watch_.toc(__func__);
  RCLCPP_INFO_EXPRESSION(
    rclcpp::get_logger("ObstacleCruisePlanner"), is_showing_debug_info_, "  %s := %f [ms]",
    __func__, calculation_time);

  return target_obstacles;
}

std::vector<TargetObstacle> ObstacleCruisePlannerNode::filterObstacles(
  const PredictedObjects & predicted_objects, const Trajectory & traj,
  const geometry_msgs::msg::Pose & current_pose, const double current_vel,
  const bool is_driving_forward, DebugData & debug_data)
{
  const auto current_time = now();
  const auto time_stamp = rclcpp::Time(predicted_objects.header.stamp);

  const size_t ego_idx = findExtendedNearestIndex(
    traj, current_pose, nearest_dist_deviation_threshold_, nearest_yaw_deviation_threshold_);

  // calculate decimated trajectory
  const auto trimmed_traj = trimTrajectoryFrom(traj, ego_idx);
  const auto decimated_traj =
    decimateTrajectory(trimmed_traj, obstacle_filtering_param_.decimate_trajectory_step_length);
  if (decimated_traj.points.size() < 2) {
    return {};
  }

  // calculate decimated trajectory polygons
  const auto decimated_traj_polygons = polygon_utils::createOneStepPolygons(
    decimated_traj, vehicle_info_, obstacle_filtering_param_.detection_area_expand_width);
  debug_data.detection_polygons = decimated_traj_polygons;

  std::vector<TargetObstacle> target_obstacles;
  for (const auto & predicted_object : predicted_objects.objects) {
    const auto object_id = toHexString(predicted_object.object_id).substr(0, 4);

    // filter object whose label is not cruised or stopped
    const bool is_target_obstacle = isStopObstacle(predicted_object.classification.front().label) ||
                                    isCruiseObstacle(predicted_object.classification.front().label);
    if (!is_target_obstacle) {
      RCLCPP_INFO_EXPRESSION(
        get_logger(), is_showing_debug_info_, "Ignore obstacle (%s) since its label is not target.",
        object_id.c_str());
      continue;
    }

    const auto object_pose = obstacle_cruise_utils::getCurrentObjectPose(
      predicted_object, predicted_objects.header, current_time, false);
    const auto & object_velocity =
      predicted_object.kinematics.initial_twist_with_covariance.twist.linear.x;

    const bool is_front_obstacle = isFrontObstacle(traj, ego_idx, object_pose.pose.position);
    if (!is_front_obstacle) {
      RCLCPP_INFO_EXPRESSION(
        get_logger(), is_showing_debug_info_,
        "Ignore obstacle (%s) since it is not front obstacle.", object_id.c_str());
      continue;
    }

    // rough detection area filtering without polygons
    const double dist_from_obstacle_to_traj = [&]() {
      return motion_utils::calcLateralOffset(decimated_traj.points, object_pose.pose.position);
    }();
    const double obstacle_max_length = calcObjectMaxLength(predicted_object.shape);
    if (
      std::fabs(dist_from_obstacle_to_traj) >
      vehicle_info_.vehicle_width_m + obstacle_max_length +
        obstacle_filtering_param_.rough_detection_area_expand_width) {
      RCLCPP_INFO_EXPRESSION(
        get_logger(), is_showing_debug_info_,
        "Ignore obstacle (%s) since it is far from the trajectory.", object_id.c_str());
      continue;
    }

    // calculate collision points
    const auto obstacle_polygon =
      tier4_autoware_utils::toPolygon2d(object_pose.pose, predicted_object.shape);
    std::vector<geometry_msgs::msg::PointStamped> collision_points;
    const auto first_within_idx = polygon_utils::getFirstCollisionIndex(
      decimated_traj_polygons, obstacle_polygon, predicted_objects.header, collision_points);

    // precise detection area filtering with polygons
    geometry_msgs::msg::PointStamped nearest_collision_point;
    if (first_within_idx) {  // obstacles inside the trajectory
      // calculate nearest collision point
      nearest_collision_point = calcNearestCollisionPoint(
        first_within_idx.get(), collision_points, decimated_traj, is_driving_forward);
      debug_data.collision_points.push_back(nearest_collision_point.point);

      const bool is_angle_aligned = isAngleAlignedWithTrajectory(
        decimated_traj, object_pose.pose,
        obstacle_filtering_param_.crossing_obstacle_traj_angle_threshold);
      const double has_high_speed =
        std::abs(object_velocity) > obstacle_filtering_param_.crossing_obstacle_velocity_threshold;

      // ignore running vehicle crossing the ego trajectory with high speed with some condition
      if (!is_angle_aligned && has_high_speed) {
        const double collision_time_margin = calcCollisionTimeMargin(
          current_pose, current_vel, nearest_collision_point.point, predicted_object,
          first_within_idx.get(), decimated_traj, decimated_traj_polygons, is_driving_forward);
        if (collision_time_margin > obstacle_filtering_param_.collision_time_margin) {
          // Ignore vehicle obstacles inside the trajectory, which is crossing the trajectory with
          // high speed and does not collide with ego in a certain time.
          RCLCPP_INFO_EXPRESSION(
            get_logger(), is_showing_debug_info_,
            "Ignore inside obstacle (%s) since it will not collide with the ego.",
            object_id.c_str());
          debug_data.intentionally_ignored_obstacles.push_back(predicted_object);
          continue;
        }
      }
    } else {  // obstacles outside the trajectory
      const auto & types = obstacle_filtering_param_.ignored_outside_obstacle_types;
      if (
        std::find(types.begin(), types.end(), predicted_object.classification.front().label) !=
        types.end()) {
        RCLCPP_INFO_EXPRESSION(
          get_logger(), is_showing_debug_info_,
          "Ignore outside obstacle (%s) since its type is not designated.", object_id.c_str());
        continue;
      }

      if (
        std::fabs(dist_from_obstacle_to_traj) >
        vehicle_info_.vehicle_width_m + obstacle_max_length +
          obstacle_filtering_param_.outside_rough_detection_area_expand_width) {
        RCLCPP_INFO_EXPRESSION(
          get_logger(), is_showing_debug_info_,
          "Ignore outside obstacle (%s) since it is far from the trajectory.", object_id.c_str());
        continue;
      }

      const auto predicted_path = getHighestConfidencePredictedPath(predicted_object);
      const auto resampled_predicted_path = perception_utils::resamplePredictedPath(
        predicted_path, obstacle_filtering_param_.prediction_resampling_time_interval,
        obstacle_filtering_param_.prediction_resampling_time_horizon);

      std::vector<geometry_msgs::msg::PointStamped> future_collision_points;
      const auto collision_traj_poly_idx = polygon_utils::willCollideWithSurroundObstacle(
        decimated_traj, decimated_traj_polygons, predicted_objects.header, resampled_predicted_path,
        predicted_object.shape,
        vehicle_info_.vehicle_width_m + obstacle_filtering_param_.rough_detection_area_expand_width,
        obstacle_filtering_param_.ego_obstacle_overlap_time_threshold,
        obstacle_filtering_param_.max_prediction_time_for_collision_check, future_collision_points);

      if (!collision_traj_poly_idx) {
        // Ignore vehicle obstacles outside the trajectory, whose predicted path
        // overlaps the ego trajectory in a certain time.
        RCLCPP_INFO_EXPRESSION(
          get_logger(), is_showing_debug_info_,
          "Ignore outside obstacle (%s) since it will not collide with the ego.",
          object_id.c_str());
        debug_data.intentionally_ignored_obstacles.push_back(predicted_object);
        continue;
      }

      nearest_collision_point = calcNearestCollisionPoint(
        collision_traj_poly_idx.get(), future_collision_points, decimated_traj, is_driving_forward);
      debug_data.collision_points.push_back(nearest_collision_point.point);
    }

    // convert to obstacle type
    const bool is_on_ego_traj = first_within_idx ? true : false;
    const double trajectory_aligned_adaptive_cruise =
      calcAlignedAdaptiveCruise(predicted_object, traj);
    const auto target_obstacle = TargetObstacle(
      time_stamp, predicted_object, trajectory_aligned_adaptive_cruise, nearest_collision_point,
      is_on_ego_traj);
    target_obstacles.push_back(target_obstacle);
  }

  // update stop status
  updateHasStopped(target_obstacles);

  // Check target obstacles' consistency
  checkConsistency(time_stamp, predicted_objects, traj, target_obstacles);

  return target_obstacles;
}

void ObstacleCruisePlannerNode::updateHasStopped(std::vector<TargetObstacle> & target_obstacles)
{
  for (auto & obstacle : target_obstacles) {
    const bool is_cruise_obstacle = isCruiseObstacle(obstacle.classification.label);
    const bool is_stop_obstacle = isStopObstacle(obstacle.classification.label);

    if (is_stop_obstacle && !is_cruise_obstacle) {
      obstacle.has_stopped = true;
      continue;
    }

    if (is_cruise_obstacle) {
      const auto itr = std::find_if(
        prev_target_obstacles_.begin(), prev_target_obstacles_.end(),
        [&](const auto & prev_target_obstacle) {
          return obstacle.uuid == prev_target_obstacle.uuid;
        });
      const bool has_already_stopped = (itr != prev_target_obstacles_.end()) && itr->has_stopped;
      if (has_already_stopped) {
        if (obstacle.velocity < obstacle_velocity_threshold_from_stop_to_cruise_) {
          obstacle.has_stopped = true;
          continue;
        }
      } else {
        if (obstacle.velocity < obstacle_velocity_threshold_from_cruise_to_stop_) {
          obstacle.has_stopped = true;
          continue;
        }
      }
    }

    obstacle.has_stopped = false;
  }

  prev_target_obstacles_ = target_obstacles;
}

void ObstacleCruisePlannerNode::checkConsistency(
  const rclcpp::Time & current_time, const PredictedObjects & predicted_objects,
  const Trajectory & traj, std::vector<TargetObstacle> & target_obstacles)
{
  const auto current_closest_obstacle =
    obstacle_cruise_utils::getClosestStopObstacle(traj, target_obstacles);

  // If previous closest obstacle ptr is not set
  if (!prev_closest_obstacle_ptr_) {
    if (current_closest_obstacle) {
      prev_closest_obstacle_ptr_ = std::make_shared<TargetObstacle>(*current_closest_obstacle);
    }
    return;
  }

  // Put previous closest target obstacle if necessary
  const auto predicted_object_itr = std::find_if(
    predicted_objects.objects.begin(), predicted_objects.objects.end(),
    [&](PredictedObject predicted_object) {
      return obstacle_cruise_utils::toHexString(predicted_object.object_id) ==
             prev_closest_obstacle_ptr_->uuid;
    });

  // If previous closest obstacle is not in the current perception lists
  // just return the current target obstacles
  if (predicted_object_itr == predicted_objects.objects.end()) {
    return;
  }

  // Previous closest obstacle is in the perception lists
  const auto target_obstacle_itr = std::find_if(
    target_obstacles.begin(), target_obstacles.end(), [&](const TargetObstacle target_obstacle) {
      return target_obstacle.uuid == prev_closest_obstacle_ptr_->uuid;
    });

  // Previous closest obstacle is both in the perception lists and target obstacles
  if (target_obstacle_itr != target_obstacles.end()) {
    if (current_closest_obstacle) {
      if ((current_closest_obstacle->uuid == prev_closest_obstacle_ptr_->uuid)) {
        // prev_closest_obstacle is current_closest_obstacle just return the target obstacles(in
        // target obstacles)
        prev_closest_obstacle_ptr_ = std::make_shared<TargetObstacle>(*current_closest_obstacle);
      } else {
        // New obstacle becomes new stop obstacle
        prev_closest_obstacle_ptr_ = std::make_shared<TargetObstacle>(*current_closest_obstacle);
      }
    } else {
      // Previous closest stop obstacle becomes cruise obstacle
      prev_closest_obstacle_ptr_ = nullptr;
    }
  } else {
    // prev obstacle is not in the target obstacles, but in the perception list
    const double elapsed_time = (current_time - prev_closest_obstacle_ptr_->time_stamp).seconds();
    if (
      predicted_object_itr->kinematics.initial_twist_with_covariance.twist.linear.x <
        obstacle_velocity_threshold_from_stop_to_cruise_ &&
      elapsed_time < obstacle_filtering_param_.stop_obstacle_hold_time_threshold) {
      target_obstacles.push_back(*prev_closest_obstacle_ptr_);
      return;
    }

    if (current_closest_obstacle) {
      prev_closest_obstacle_ptr_ = std::make_shared<TargetObstacle>(*current_closest_obstacle);
    } else {
      prev_closest_obstacle_ptr_ = nullptr;
    }
  }
}

geometry_msgs::msg::PointStamped ObstacleCruisePlannerNode::calcNearestCollisionPoint(
  const size_t & first_within_idx,
  const std::vector<geometry_msgs::msg::PointStamped> & collision_points,
  const Trajectory & decimated_traj, const bool is_driving_forward)
{
  std::array<geometry_msgs::msg::Point, 2> segment_points;
  if (first_within_idx == 0) {
    const auto & traj_front_pose = decimated_traj.points.at(0).pose;
    const auto front_pos = tier4_autoware_utils::calcOffsetPose(
                             traj_front_pose, vehicle_info_.max_longitudinal_offset_m, 0.0, 0.0)
                             .position;
    if (is_driving_forward) {
      segment_points.at(0) = traj_front_pose.position;
      segment_points.at(1) = front_pos;
    } else {
      segment_points.at(0) = front_pos;
      segment_points.at(1) = traj_front_pose.position;
    }
  } else {
    const size_t seg_idx = first_within_idx - 1;
    segment_points.at(0) = decimated_traj.points.at(seg_idx).pose.position;
    segment_points.at(1) = decimated_traj.points.at(seg_idx + 1).pose.position;
  }

  size_t min_idx = 0;
  double min_dist = std::numeric_limits<double>::max();
  for (size_t cp_idx = 0; cp_idx < collision_points.size(); ++cp_idx) {
    const auto & collision_point = collision_points.at(cp_idx);
    const double dist =
      motion_utils::calcLongitudinalOffsetToSegment(segment_points, 0, collision_point.point);
    if (dist < min_dist) {
      min_dist = dist;
      min_idx = cp_idx;
    }
  }

  return collision_points.at(min_idx);
}

double ObstacleCruisePlannerNode::calcCollisionTimeMargin(
  const geometry_msgs::msg::Pose & current_pose, const double current_vel,
  const geometry_msgs::msg::Point & nearest_collision_point,
  const PredictedObject & predicted_object, const size_t first_within_idx,
  const Trajectory & decimated_traj,
  const std::vector<tier4_autoware_utils::Polygon2d> & decimated_traj_polygons,
  const bool is_driving_forward)
{
  const auto & object_pose = predicted_object.kinematics.initial_pose_with_covariance.pose;
  const auto & object_velocity =
    predicted_object.kinematics.initial_twist_with_covariance.twist.linear.x;
  const auto predicted_path = getHighestConfidencePredictedPath(predicted_object);
  const auto resampled_predicted_path = perception_utils::resamplePredictedPath(
    predicted_path, obstacle_filtering_param_.prediction_resampling_time_interval,
    obstacle_filtering_param_.prediction_resampling_time_horizon);

  const double time_to_collision = [&]() {
    const double abs_ego_offset = is_driving_forward
                                    ? std::abs(vehicle_info_.max_longitudinal_offset_m)
                                    : std::abs(vehicle_info_.min_longitudinal_offset_m);
    const double dist_from_ego_to_obstacle =
      motion_utils::calcSignedArcLength(
        decimated_traj.points, current_pose.position, nearest_collision_point) -
      abs_ego_offset;
    return dist_from_ego_to_obstacle / std::max(1e-6, std::abs(current_vel));
  }();

  const double time_to_obstacle_getting_out = [&]() {
    const auto obstacle_getting_out_idx = polygon_utils::getFirstNonCollisionIndex(
      decimated_traj_polygons, resampled_predicted_path, predicted_object.shape, first_within_idx);
    if (!obstacle_getting_out_idx) {
      return std::numeric_limits<double>::max();
    }

    const double dist_to_obstacle_getting_out = motion_utils::calcSignedArcLength(
      decimated_traj.points, object_pose.position, obstacle_getting_out_idx.get());

    return dist_to_obstacle_getting_out / object_velocity;
  }();

  return time_to_collision - time_to_obstacle_getting_out;
}

void ObstacleCruisePlannerNode::publishVelocityLimit(
  const boost::optional<VelocityLimit> & vel_limit)
{
  if (vel_limit) {
    vel_limit_pub_->publish(vel_limit.get());
    need_to_clear_vel_limit_ = true;
  } else {
    if (need_to_clear_vel_limit_) {
      const auto clear_vel_limit_msg = createVelocityLimitClearCommandMsg(now());
      clear_vel_limit_pub_->publish(clear_vel_limit_msg);
      need_to_clear_vel_limit_ = false;
    }
  }
}

void ObstacleCruisePlannerNode::publishDebugData(const DebugData & debug_data) const
{
  stop_watch_.tic(__func__);

  visualization_msgs::msg::MarkerArray debug_marker;
  const auto current_time = now();

  // obstacles to cruise
  for (size_t i = 0; i < debug_data.obstacles_to_cruise.size(); ++i) {
    const auto marker = obstacle_cruise_utils::getObjectMarker(
      debug_data.obstacles_to_cruise.at(i).pose, i, "obstacles_to_cruise", 0.7, 0.7, 0.0);
    debug_marker.markers.push_back(marker);
  }

  // obstacles to stop
  for (size_t i = 0; i < debug_data.obstacles_to_stop.size(); ++i) {
    const auto marker = obstacle_cruise_utils::getObjectMarker(
      debug_data.obstacles_to_stop.at(i).pose, i, "obstacles_to_stop", 1.0, 0.0, 0.0);
    debug_marker.markers.push_back(marker);
  }

  // intentionally ignored obstacles to cruise or stop
  for (size_t i = 0; i < debug_data.intentionally_ignored_obstacles.size(); ++i) {
    const auto marker = obstacle_cruise_utils::getObjectMarker(
      debug_data.intentionally_ignored_obstacles.at(i).kinematics.initial_pose_with_covariance.pose,
      i, "intentionally_ignored_obstacles", 0.0, 1.0, 0.0);
    debug_marker.markers.push_back(marker);
  }

  {  // footprint polygons
    auto marker = tier4_autoware_utils::createDefaultMarker(
      "map", current_time, "detection_polygons", 0, visualization_msgs::msg::Marker::LINE_LIST,
      tier4_autoware_utils::createMarkerScale(0.01, 0.0, 0.0),
      tier4_autoware_utils::createMarkerColor(0.0, 1.0, 0.0, 0.999));

    for (const auto & detection_polygon : debug_data.detection_polygons) {
      for (size_t dp_idx = 0; dp_idx < detection_polygon.outer().size(); ++dp_idx) {
        const auto & current_point = detection_polygon.outer().at(dp_idx);
        const auto & next_point =
          detection_polygon.outer().at((dp_idx + 1) % detection_polygon.outer().size());

        marker.points.push_back(
          tier4_autoware_utils::createPoint(current_point.x(), current_point.y(), 0.0));
        marker.points.push_back(
          tier4_autoware_utils::createPoint(next_point.x(), next_point.y(), 0.0));
      }
    }
    debug_marker.markers.push_back(marker);
  }

  {  // collision points
    for (size_t i = 0; i < debug_data.collision_points.size(); ++i) {
      auto marker = tier4_autoware_utils::createDefaultMarker(
        "map", current_time, "collision_points", i, visualization_msgs::msg::Marker::SPHERE,
        tier4_autoware_utils::createMarkerScale(0.25, 0.25, 0.25),
        tier4_autoware_utils::createMarkerColor(1.0, 0.0, 0.0, 0.999));
      marker.pose.position = debug_data.collision_points.at(i);
      debug_marker.markers.push_back(marker);
    }
  }

  debug_marker_pub_->publish(debug_marker);

  // wall for cruise and stop
  debug_cruise_wall_marker_pub_->publish(debug_data.cruise_wall_marker);
  debug_stop_wall_marker_pub_->publish(debug_data.stop_wall_marker);

  // print calculation time
  const double calculation_time = stop_watch_.toc(__func__);
  RCLCPP_INFO_EXPRESSION(
    rclcpp::get_logger("ObstacleCruisePlanner"), is_showing_debug_info_, "  %s := %f [ms]",
    __func__, calculation_time);
}

void ObstacleCruisePlannerNode::publishCalculationTime(const double calculation_time) const
{
  Float32Stamped calculation_time_msg;
  calculation_time_msg.stamp = now();
  calculation_time_msg.data = calculation_time;
  debug_calculation_time_pub_->publish(calculation_time_msg);
}
}  // namespace motion_planning
#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(motion_planning::ObstacleCruisePlannerNode)
