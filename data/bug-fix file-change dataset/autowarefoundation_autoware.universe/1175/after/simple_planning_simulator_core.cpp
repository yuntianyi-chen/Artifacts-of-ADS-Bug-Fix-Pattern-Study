// Copyright 2021 The Autoware Foundation.
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

#include "simple_planning_simulator/simple_planning_simulator_core.hpp"

#include "autoware_auto_tf2/tf2_autoware_auto_msgs.hpp"
#include "common/types.hpp"
#include "motion_common/motion_common.hpp"
#include "rclcpp_components/register_node_macro.hpp"
#include "simple_planning_simulator/vehicle_model/sim_model.hpp"
#include "tier4_autoware_utils/ros/update_param.hpp"
#include "vehicle_info_util/vehicle_info_util.hpp"

#include <tf2/LinearMath/Quaternion.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

using namespace std::literals::chrono_literals;

namespace
{

autoware_auto_vehicle_msgs::msg::VelocityReport to_velocity_report(
  const std::shared_ptr<SimModelInterface> vehicle_model_ptr)
{
  autoware_auto_vehicle_msgs::msg::VelocityReport velocity;
  velocity.longitudinal_velocity = static_cast<float32_t>(vehicle_model_ptr->getVx());
  velocity.lateral_velocity = 0.0F;
  velocity.heading_rate = static_cast<float32_t>(vehicle_model_ptr->getWz());
  return velocity;
}

nav_msgs::msg::Odometry to_odometry(const std::shared_ptr<SimModelInterface> vehicle_model_ptr)
{
  nav_msgs::msg::Odometry odometry;
  odometry.pose.pose.position.x = vehicle_model_ptr->getX();
  odometry.pose.pose.position.y = vehicle_model_ptr->getY();
  odometry.pose.pose.orientation = motion::motion_common::from_angle(vehicle_model_ptr->getYaw());
  odometry.twist.twist.linear.x = vehicle_model_ptr->getVx();
  odometry.twist.twist.angular.z = vehicle_model_ptr->getWz();

  return odometry;
}

autoware_auto_vehicle_msgs::msg::SteeringReport to_steering_report(
  const std::shared_ptr<SimModelInterface> vehicle_model_ptr)
{
  autoware_auto_vehicle_msgs::msg::SteeringReport steer;
  steer.steering_tire_angle = static_cast<float32_t>(vehicle_model_ptr->getSteer());
  return steer;
}

}  // namespace

namespace simulation
{
namespace simple_planning_simulator
{

SimplePlanningSimulator::SimplePlanningSimulator(const rclcpp::NodeOptions & options)
: Node("simple_planning_simulator", options), tf_buffer_(get_clock()), tf_listener_(tf_buffer_)
{
  simulated_frame_id_ = declare_parameter("simulated_frame_id", "base_link");
  origin_frame_id_ = declare_parameter("origin_frame_id", "odom");
  add_measurement_noise_ = declare_parameter("add_measurement_noise", false);
  current_engage_ = declare_parameter<bool>("initial_engage_state");

  using rclcpp::QoS;
  using std::placeholders::_1;
  using std::placeholders::_2;

  sub_init_pose_ = create_subscription<PoseWithCovarianceStamped>(
    "/initialpose", QoS{1}, std::bind(&SimplePlanningSimulator::on_initialpose, this, _1));
  sub_ackermann_cmd_ = create_subscription<AckermannControlCommand>(
    "input/ackermann_control_command", QoS{1},
    std::bind(&SimplePlanningSimulator::on_ackermann_cmd, this, _1));
  sub_gear_cmd_ = create_subscription<GearCommand>(
    "input/gear_command", QoS{1}, std::bind(&SimplePlanningSimulator::on_gear_cmd, this, _1));
  sub_turn_indicators_cmd_ = create_subscription<TurnIndicatorsCommand>(
    "input/turn_indicators_command", QoS{1},
    std::bind(&SimplePlanningSimulator::on_turn_indicators_cmd, this, _1));
  sub_hazard_lights_cmd_ = create_subscription<HazardLightsCommand>(
    "input/hazard_lights_command", QoS{1},
    std::bind(&SimplePlanningSimulator::on_hazard_lights_cmd, this, _1));
  sub_trajectory_ = create_subscription<Trajectory>(
    "input/trajectory", QoS{1}, std::bind(&SimplePlanningSimulator::on_trajectory, this, _1));
  sub_engage_ = create_subscription<Engage>(
    "input/engage", rclcpp::QoS{1}, std::bind(&SimplePlanningSimulator::on_engage, this, _1));

  pub_control_mode_report_ =
    create_publisher<ControlModeReport>("output/control_mode_report", QoS{1});
  pub_gear_report_ = create_publisher<GearReport>("output/gear_report", QoS{1});
  pub_turn_indicators_report_ =
    create_publisher<TurnIndicatorsReport>("output/turn_indicators_report", QoS{1});
  pub_hazard_lights_report_ =
    create_publisher<HazardLightsReport>("output/hazard_lights_report", QoS{1});
  pub_current_pose_ = create_publisher<PoseStamped>("/current_pose", QoS{1});
  pub_velocity_ = create_publisher<VelocityReport>("output/twist", QoS{1});
  pub_odom_ = create_publisher<Odometry>("output/odometry", QoS{1});
  pub_steer_ = create_publisher<SteeringReport>("output/steering", QoS{1});
  pub_tf_ = create_publisher<tf2_msgs::msg::TFMessage>("/tf", QoS{1});

  /* set param callback */
  set_param_res_ = this->add_on_set_parameters_callback(
    std::bind(&SimplePlanningSimulator::on_parameter, this, _1));

  timer_sampling_time_ms_ = static_cast<uint32_t>(declare_parameter("timer_sampling_time_ms", 25));
  on_timer_ = create_wall_timer(
    std::chrono::milliseconds(timer_sampling_time_ms_),
    std::bind(&SimplePlanningSimulator::on_timer, this));

  tier4_api_utils::ServiceProxyNodeInterface proxy(this);
  group_api_service_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  srv_set_pose_ = proxy.create_service<tier4_external_api_msgs::srv::InitializePose>(
    "/api/simulator/set/pose", std::bind(&SimplePlanningSimulator::on_set_pose, this, _1, _2),
    rmw_qos_profile_services_default, group_api_service_);

  // set vehicle model type
  initialize_vehicle_model();

  // set initialize source
  const auto initialize_source = declare_parameter("initialize_source", "INITIAL_POSE_TOPIC");
  RCLCPP_INFO(this->get_logger(), "initialize_source : %s", initialize_source.c_str());
  if (initialize_source == "ORIGIN") {
    Pose p;
    p.orientation.w = 1.0;          // yaw = 0
    set_initial_state(p, Twist{});  // initialize with 0 for all variables
  } else if (initialize_source == "INITIAL_POSE_TOPIC") {
    // initialpose sub already exists. Do nothing.
  }

  // measurement noise
  {
    std::random_device seed;
    auto & m = measurement_noise_;
    m.rand_engine_ = std::make_shared<std::mt19937>(seed());
    float64_t pos_noise_stddev = declare_parameter("pos_noise_stddev", 1e-2);
    float64_t vel_noise_stddev = declare_parameter("vel_noise_stddev", 1e-2);
    float64_t rpy_noise_stddev = declare_parameter("rpy_noise_stddev", 1e-4);
    float64_t steer_noise_stddev = declare_parameter("steer_noise_stddev", 1e-4);
    m.pos_dist_ = std::make_shared<std::normal_distribution<>>(0.0, pos_noise_stddev);
    m.vel_dist_ = std::make_shared<std::normal_distribution<>>(0.0, vel_noise_stddev);
    m.rpy_dist_ = std::make_shared<std::normal_distribution<>>(0.0, rpy_noise_stddev);
    m.steer_dist_ = std::make_shared<std::normal_distribution<>>(0.0, steer_noise_stddev);

    x_stddev_ = declare_parameter("x_stddev", 0.0001);
    y_stddev_ = declare_parameter("y_stddev", 0.0001);
  }
}

void SimplePlanningSimulator::initialize_vehicle_model()
{
  const auto vehicle_model_type_str = declare_parameter("vehicle_model_type", "IDEAL_STEER_VEL");

  RCLCPP_INFO(this->get_logger(), "vehicle_model_type = %s", vehicle_model_type_str.c_str());

  const float64_t vel_lim = declare_parameter("vel_lim", 50.0);
  const float64_t vel_rate_lim = declare_parameter("vel_rate_lim", 7.0);
  const float64_t steer_lim = declare_parameter("steer_lim", 1.0);
  const float64_t steer_rate_lim = declare_parameter("steer_rate_lim", 5.0);
  const float64_t acc_time_delay = declare_parameter("acc_time_delay", 0.1);
  const float64_t acc_time_constant = declare_parameter("acc_time_constant", 0.1);
  const float64_t vel_time_delay = declare_parameter("vel_time_delay", 0.25);
  const float64_t vel_time_constant = declare_parameter("vel_time_constant", 0.5);
  const float64_t steer_time_delay = declare_parameter("steer_time_delay", 0.24);
  const float64_t steer_time_constant = declare_parameter("steer_time_constant", 0.27);
  const auto vehicle_info = vehicle_info_util::VehicleInfoUtil(*this).getVehicleInfo();
  const float64_t wheelbase = vehicle_info.wheel_base_m;

  if (vehicle_model_type_str == "IDEAL_STEER_VEL") {
    vehicle_model_type_ = VehicleModelType::IDEAL_STEER_VEL;
    vehicle_model_ptr_ = std::make_shared<SimModelIdealSteerVel>(wheelbase);
  } else if (vehicle_model_type_str == "IDEAL_STEER_ACC") {
    vehicle_model_type_ = VehicleModelType::IDEAL_STEER_ACC;
    vehicle_model_ptr_ = std::make_shared<SimModelIdealSteerAcc>(wheelbase);
  } else if (vehicle_model_type_str == "IDEAL_STEER_ACC_GEARED") {
    vehicle_model_type_ = VehicleModelType::IDEAL_STEER_ACC_GEARED;
    vehicle_model_ptr_ = std::make_shared<SimModelIdealSteerAccGeared>(wheelbase);
  } else if (vehicle_model_type_str == "DELAY_STEER_VEL") {
    vehicle_model_type_ = VehicleModelType::DELAY_STEER_VEL;
    vehicle_model_ptr_ = std::make_shared<SimModelDelaySteerVel>(
      vel_lim, steer_lim, vel_rate_lim, steer_rate_lim, wheelbase, timer_sampling_time_ms_ / 1000.0,
      vel_time_delay, vel_time_constant, steer_time_delay, steer_time_constant);
  } else if (vehicle_model_type_str == "DELAY_STEER_ACC") {
    vehicle_model_type_ = VehicleModelType::DELAY_STEER_ACC;
    vehicle_model_ptr_ = std::make_shared<SimModelDelaySteerAcc>(
      vel_lim, steer_lim, vel_rate_lim, steer_rate_lim, wheelbase, timer_sampling_time_ms_ / 1000.0,
      acc_time_delay, acc_time_constant, steer_time_delay, steer_time_constant);
  } else if (vehicle_model_type_str == "DELAY_STEER_ACC_GEARED") {
    vehicle_model_type_ = VehicleModelType::DELAY_STEER_ACC_GEARED;
    vehicle_model_ptr_ = std::make_shared<SimModelDelaySteerAccGeared>(
      vel_lim, steer_lim, vel_rate_lim, steer_rate_lim, wheelbase, timer_sampling_time_ms_ / 1000.0,
      acc_time_delay, acc_time_constant, steer_time_delay, steer_time_constant);
  } else {
    throw std::invalid_argument("Invalid vehicle_model_type: " + vehicle_model_type_str);
  }
}

rcl_interfaces::msg::SetParametersResult SimplePlanningSimulator::on_parameter(
  const std::vector<rclcpp::Parameter> & parameters)
{
  rcl_interfaces::msg::SetParametersResult result;
  result.successful = true;
  result.reason = "success";

  try {
    tier4_autoware_utils::updateParam(parameters, "x_stddev", x_stddev_);
    tier4_autoware_utils::updateParam(parameters, "y_stddev", y_stddev_);
  } catch (const rclcpp::exceptions::InvalidParameterTypeException & e) {
    result.successful = false;
    result.reason = e.what();
  }

  return result;
}

void SimplePlanningSimulator::on_timer()
{
  if (!is_initialized_) {
    RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 5000, "waiting initialization...");
    return;
  }

  // update vehicle dynamics
  {
    const float64_t dt = delta_time_.get_dt(get_clock()->now());

    if (current_engage_) {
      vehicle_model_ptr_->update(dt);
    }
  }

  // set current state
  current_odometry_ = to_odometry(vehicle_model_ptr_);
  current_odometry_.pose.pose.position.z = get_z_pose_from_trajectory(
    current_odometry_.pose.pose.position.x, current_odometry_.pose.pose.position.y);

  current_velocity_ = to_velocity_report(vehicle_model_ptr_);
  current_steer_ = to_steering_report(vehicle_model_ptr_);

  if (add_measurement_noise_) {
    add_measurement_noise(current_odometry_, current_velocity_, current_steer_);
  }

  // add estimate covariance
  {
    current_odometry_.pose.covariance[0 * 6 + 0] = x_stddev_;
    current_odometry_.pose.covariance[1 * 6 + 1] = y_stddev_;
  }

  // publish vehicle state
  publish_odometry(current_odometry_);
  publish_velocity(current_velocity_);
  publish_steering(current_steer_);

  publish_control_mode_report();
  publish_gear_report();
  publish_turn_indicators_report();
  publish_hazard_lights_report();
  publish_tf(current_odometry_);
}

void SimplePlanningSimulator::on_initialpose(const PoseWithCovarianceStamped::ConstSharedPtr msg)
{
  // save initial pose
  Twist initial_twist;
  PoseStamped initial_pose;
  initial_pose.header = msg->header;
  initial_pose.pose = msg->pose.pose;
  set_initial_state_with_transform(initial_pose, initial_twist);
}

void SimplePlanningSimulator::on_set_pose(
  const InitializePose::Request::ConstSharedPtr request,
  const InitializePose::Response::SharedPtr response)
{
  // save initial pose
  Twist initial_twist;
  PoseStamped initial_pose;
  initial_pose.header = request->pose.header;
  initial_pose.pose = request->pose.pose.pose;
  set_initial_state_with_transform(initial_pose, initial_twist);
  response->status = tier4_api_utils::response_success();
}

void SimplePlanningSimulator::on_ackermann_cmd(
  const autoware_auto_control_msgs::msg::AckermannControlCommand::ConstSharedPtr msg)
{
  current_ackermann_cmd_ptr_ = msg;
  set_input(
    msg->lateral.steering_tire_angle, msg->longitudinal.speed, msg->longitudinal.acceleration);
}

void SimplePlanningSimulator::set_input(const float steer, const float vel, const float accel)
{
  using autoware_auto_vehicle_msgs::msg::GearCommand;
  Eigen::VectorXd input(vehicle_model_ptr_->getDimU());

  // TODO(Watanabe): The definition of the sign of acceleration in REVERSE mode is different
  // between .auto and proposal.iv, and will be discussed later.
  float acc = accel;
  if (!current_gear_cmd_ptr_) {
    acc = 0.0;
  } else if (
    current_gear_cmd_ptr_->command == GearCommand::REVERSE ||
    current_gear_cmd_ptr_->command == GearCommand::REVERSE_2) {
    acc = -accel;
  }

  if (
    vehicle_model_type_ == VehicleModelType::IDEAL_STEER_VEL ||
    vehicle_model_type_ == VehicleModelType::DELAY_STEER_VEL) {
    input << vel, steer;
  } else if (  // NOLINT
    vehicle_model_type_ == VehicleModelType::IDEAL_STEER_ACC ||
    vehicle_model_type_ == VehicleModelType::DELAY_STEER_ACC) {
    input << acc, steer;
  } else if (  // NOLINT
    vehicle_model_type_ == VehicleModelType::IDEAL_STEER_ACC_GEARED ||
    vehicle_model_type_ == VehicleModelType::DELAY_STEER_ACC_GEARED) {
    input << acc, steer;
  }
  vehicle_model_ptr_->setInput(input);
}

void SimplePlanningSimulator::on_gear_cmd(const GearCommand::ConstSharedPtr msg)
{
  current_gear_cmd_ptr_ = msg;

  if (
    vehicle_model_type_ == VehicleModelType::IDEAL_STEER_ACC_GEARED ||
    vehicle_model_type_ == VehicleModelType::DELAY_STEER_ACC_GEARED) {
    vehicle_model_ptr_->setGear(current_gear_cmd_ptr_->command);
  }
}

void SimplePlanningSimulator::on_turn_indicators_cmd(
  const TurnIndicatorsCommand::ConstSharedPtr msg)
{
  current_turn_indicators_cmd_ptr_ = msg;
}

void SimplePlanningSimulator::on_hazard_lights_cmd(const HazardLightsCommand::ConstSharedPtr msg)
{
  current_hazard_lights_cmd_ptr_ = msg;
}

void SimplePlanningSimulator::on_trajectory(const Trajectory::ConstSharedPtr msg)
{
  current_trajectory_ptr_ = msg;
}

void SimplePlanningSimulator::on_engage(const Engage::ConstSharedPtr msg)
{
  current_engage_ = msg->engage;
}

void SimplePlanningSimulator::add_measurement_noise(
  Odometry & odom, VelocityReport & vel, SteeringReport & steer) const
{
  auto & n = measurement_noise_;
  odom.pose.pose.position.x += (*n.pos_dist_)(*n.rand_engine_);
  odom.pose.pose.position.y += (*n.pos_dist_)(*n.rand_engine_);
  const auto velocity_noise = (*n.vel_dist_)(*n.rand_engine_);
  odom.twist.twist.linear.x += velocity_noise;
  float32_t yaw = motion::motion_common::to_angle(odom.pose.pose.orientation);
  yaw += static_cast<float>((*n.rpy_dist_)(*n.rand_engine_));
  odom.pose.pose.orientation = motion::motion_common::from_angle(yaw);

  vel.longitudinal_velocity += static_cast<float32_t>(velocity_noise);

  steer.steering_tire_angle += static_cast<float32_t>((*n.steer_dist_)(*n.rand_engine_));
}

void SimplePlanningSimulator::set_initial_state_with_transform(
  const PoseStamped & pose_stamped, const Twist & twist)
{
  auto transform = get_transform_msg(origin_frame_id_, pose_stamped.header.frame_id);
  Pose pose;
  pose.position.x = pose_stamped.pose.position.x + transform.transform.translation.x;
  pose.position.y = pose_stamped.pose.position.y + transform.transform.translation.y;
  pose.position.z = pose_stamped.pose.position.z + transform.transform.translation.z;
  pose.orientation = pose_stamped.pose.orientation;
  set_initial_state(pose, twist);
}

void SimplePlanningSimulator::set_initial_state(const Pose & pose, const Twist & twist)
{
  const float64_t x = pose.position.x;
  const float64_t y = pose.position.y;
  const float64_t yaw = ::motion::motion_common::to_angle(pose.orientation);
  const float64_t vx = twist.linear.x;
  const float64_t steer = 0.0;
  const float64_t accx = 0.0;

  Eigen::VectorXd state(vehicle_model_ptr_->getDimX());

  if (vehicle_model_type_ == VehicleModelType::IDEAL_STEER_VEL) {
    state << x, y, yaw;
  } else if (  // NOLINT
    vehicle_model_type_ == VehicleModelType::IDEAL_STEER_ACC ||
    vehicle_model_type_ == VehicleModelType::IDEAL_STEER_ACC_GEARED) {
    state << x, y, yaw, vx;
  } else if (  // NOLINT
    vehicle_model_type_ == VehicleModelType::DELAY_STEER_VEL) {
    state << x, y, yaw, vx, steer;
  } else if (  // NOLINT
    vehicle_model_type_ == VehicleModelType::DELAY_STEER_ACC ||
    vehicle_model_type_ == VehicleModelType::DELAY_STEER_ACC_GEARED) {
    state << x, y, yaw, vx, steer, accx;
  }
  vehicle_model_ptr_->setState(state);

  is_initialized_ = true;
}

double SimplePlanningSimulator::get_z_pose_from_trajectory(const double x, const double y)
{
  // calculate closest point on trajectory
  if (!current_trajectory_ptr_) {
    return 0.0;
  }

  const double max_sqrt_dist = std::numeric_limits<double>::max();
  double min_sqrt_dist = max_sqrt_dist;
  size_t index;
  bool found = false;
  for (size_t i = 0; i < current_trajectory_ptr_->points.size(); ++i) {
    const double dist_x = (current_trajectory_ptr_->points.at(i).pose.position.x - x);
    const double dist_y = (current_trajectory_ptr_->points.at(i).pose.position.y - y);
    double sqrt_dist = dist_x * dist_x + dist_y * dist_y;
    if (sqrt_dist < min_sqrt_dist) {
      min_sqrt_dist = sqrt_dist;
      index = i;
      found = true;
    }
  }
  if (found) {
    return current_trajectory_ptr_->points.at(index).pose.position.z;
  }

  return 0.0;
}

TransformStamped SimplePlanningSimulator::get_transform_msg(
  const std::string parent_frame, const std::string child_frame)
{
  TransformStamped transform;
  while (true) {
    try {
      const auto time_point = tf2::TimePoint(std::chrono::milliseconds(0));
      transform = tf_buffer_.lookupTransform(
        parent_frame, child_frame, time_point, tf2::durationFromSec(0.0));
      break;
    } catch (tf2::TransformException & ex) {
      RCLCPP_ERROR(this->get_logger(), "%s", ex.what());
      rclcpp::sleep_for(std::chrono::milliseconds(500));
    }
  }
  return transform;
}

void SimplePlanningSimulator::publish_velocity(const VelocityReport & velocity)
{
  VelocityReport msg = velocity;
  msg.header.stamp = get_clock()->now();
  msg.header.frame_id = simulated_frame_id_;
  pub_velocity_->publish(msg);
}

void SimplePlanningSimulator::publish_odometry(const Odometry & odometry)
{
  Odometry msg = odometry;
  msg.header.frame_id = origin_frame_id_;
  msg.header.stamp = get_clock()->now();
  msg.child_frame_id = simulated_frame_id_;
  pub_odom_->publish(msg);
}

void SimplePlanningSimulator::publish_steering(const SteeringReport & steer)
{
  SteeringReport msg = steer;
  msg.stamp = get_clock()->now();
  pub_steer_->publish(msg);
}

void SimplePlanningSimulator::publish_control_mode_report()
{
  ControlModeReport msg;
  msg.stamp = get_clock()->now();
  if (current_engage_) {
    msg.mode = ControlModeReport::AUTONOMOUS;
  } else {
    msg.mode = ControlModeReport::MANUAL;
  }
  pub_control_mode_report_->publish(msg);
}

void SimplePlanningSimulator::publish_gear_report()
{
  if (!current_gear_cmd_ptr_) {
    return;
  }
  GearReport msg;
  msg.stamp = get_clock()->now();
  msg.report = current_gear_cmd_ptr_->command;
  pub_gear_report_->publish(msg);
}

void SimplePlanningSimulator::publish_turn_indicators_report()
{
  if (!current_turn_indicators_cmd_ptr_) {
    return;
  }
  TurnIndicatorsReport msg;
  msg.stamp = get_clock()->now();
  msg.report = current_turn_indicators_cmd_ptr_->command;
  pub_turn_indicators_report_->publish(msg);
}

void SimplePlanningSimulator::publish_hazard_lights_report()
{
  if (!current_hazard_lights_cmd_ptr_) {
    return;
  }
  HazardLightsReport msg;
  msg.stamp = get_clock()->now();
  msg.report = current_hazard_lights_cmd_ptr_->command;
  pub_hazard_lights_report_->publish(msg);
}

void SimplePlanningSimulator::publish_tf(const Odometry & odometry)
{
  TransformStamped tf;
  tf.header.stamp = get_clock()->now();
  tf.header.frame_id = origin_frame_id_;
  tf.child_frame_id = simulated_frame_id_;
  tf.transform.translation.x = odometry.pose.pose.position.x;
  tf.transform.translation.y = odometry.pose.pose.position.y;
  tf.transform.translation.z = odometry.pose.pose.position.z;
  tf.transform.rotation = odometry.pose.pose.orientation;

  tf2_msgs::msg::TFMessage tf_msg{};
  tf_msg.transforms.emplace_back(std::move(tf));
  pub_tf_->publish(tf_msg);
}
}  // namespace simple_planning_simulator
}  // namespace simulation

RCLCPP_COMPONENTS_REGISTER_NODE(simulation::simple_planning_simulator::SimplePlanningSimulator)
