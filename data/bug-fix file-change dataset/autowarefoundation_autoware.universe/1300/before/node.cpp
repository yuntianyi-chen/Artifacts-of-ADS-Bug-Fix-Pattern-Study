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

#include "behavior_velocity_planner/node.hpp"

#include <lanelet2_extension/utility/message_conversion.hpp>
#include <tier4_autoware_utils/ros/wait_for_param.hpp>
#include <utilization/path_utilization.hpp>

#include <diagnostic_msgs/msg/diagnostic_status.hpp>
#include <visualization_msgs/msg/marker_array.hpp>

#include <lanelet2_routing/Route.h>
#include <pcl/common/transforms.h>

#ifdef ROS_DISTRO_GALACTIC
#include <tf2_eigen/tf2_eigen.h>
#else
#include <tf2_eigen/tf2_eigen.hpp>
#endif

#include <functional>
#include <memory>

// Scene modules
#include <scene_module/blind_spot/manager.hpp>
#include <scene_module/crosswalk/manager.hpp>
#include <scene_module/detection_area/manager.hpp>
#include <scene_module/intersection/manager.hpp>
#include <scene_module/no_stopping_area/manager.hpp>
#include <scene_module/occlusion_spot/manager.hpp>
#include <scene_module/run_out/manager.hpp>
#include <scene_module/stop_line/manager.hpp>
#include <scene_module/traffic_light/manager.hpp>
#include <scene_module/virtual_traffic_light/manager.hpp>

namespace
{
rclcpp::SubscriptionOptions createSubscriptionOptions(rclcpp::Node * node_ptr)
{
  rclcpp::CallbackGroup::SharedPtr callback_group =
    node_ptr->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

  auto sub_opt = rclcpp::SubscriptionOptions();
  sub_opt.callback_group = callback_group;

  return sub_opt;
}
}  // namespace

namespace behavior_velocity_planner
{
namespace
{
geometry_msgs::msg::PoseStamped transform2pose(
  const geometry_msgs::msg::TransformStamped & transform)
{
  geometry_msgs::msg::PoseStamped pose;
  pose.header = transform.header;
  pose.pose.position.x = transform.transform.translation.x;
  pose.pose.position.y = transform.transform.translation.y;
  pose.pose.position.z = transform.transform.translation.z;
  pose.pose.orientation = transform.transform.rotation;
  return pose;
}

autoware_auto_planning_msgs::msg::Path to_path(
  const autoware_auto_planning_msgs::msg::PathWithLaneId & path_with_id)
{
  autoware_auto_planning_msgs::msg::Path path;
  for (const auto & path_point : path_with_id.points) {
    path.points.push_back(path_point.point);
  }
  return path;
}
}  // namespace

BehaviorVelocityPlannerNode::BehaviorVelocityPlannerNode(const rclcpp::NodeOptions & node_options)
: Node("behavior_velocity_planner_node", node_options),
  tf_buffer_(this->get_clock()),
  tf_listener_(tf_buffer_),
  planner_data_(*this)
{
  using std::placeholders::_1;
  // Trigger Subscriber
  trigger_sub_path_with_lane_id_ =
    this->create_subscription<autoware_auto_planning_msgs::msg::PathWithLaneId>(
      "~/input/path_with_lane_id", 1, std::bind(&BehaviorVelocityPlannerNode::onTrigger, this, _1),
      createSubscriptionOptions(this));

  // Subscribers
  sub_predicted_objects_ =
    this->create_subscription<autoware_auto_perception_msgs::msg::PredictedObjects>(
      "~/input/dynamic_objects", 1,
      std::bind(&BehaviorVelocityPlannerNode::onPredictedObjects, this, _1),
      createSubscriptionOptions(this));
  sub_no_ground_pointcloud_ = this->create_subscription<sensor_msgs::msg::PointCloud2>(
    "~/input/no_ground_pointcloud", rclcpp::SensorDataQoS(),
    std::bind(&BehaviorVelocityPlannerNode::onNoGroundPointCloud, this, _1),
    createSubscriptionOptions(this));
  sub_vehicle_odometry_ = this->create_subscription<nav_msgs::msg::Odometry>(
    "~/input/vehicle_odometry", 1,
    std::bind(&BehaviorVelocityPlannerNode::onVehicleVelocity, this, _1),
    createSubscriptionOptions(this));
  sub_lanelet_map_ = this->create_subscription<autoware_auto_mapping_msgs::msg::HADMapBin>(
    "~/input/vector_map", rclcpp::QoS(10).transient_local(),
    std::bind(&BehaviorVelocityPlannerNode::onLaneletMap, this, _1),
    createSubscriptionOptions(this));
  sub_traffic_signals_ =
    this->create_subscription<autoware_auto_perception_msgs::msg::TrafficSignalArray>(
      "~/input/traffic_signals", 10,
      std::bind(&BehaviorVelocityPlannerNode::onTrafficSignals, this, _1),
      createSubscriptionOptions(this));
  sub_external_crosswalk_states_ = this->create_subscription<tier4_api_msgs::msg::CrosswalkStatus>(
    "~/input/external_crosswalk_states", 10,
    std::bind(&BehaviorVelocityPlannerNode::onExternalCrosswalkStates, this, _1),
    createSubscriptionOptions(this));
  sub_external_intersection_states_ =
    this->create_subscription<tier4_api_msgs::msg::IntersectionStatus>(
      "~/input/external_intersection_states", 10,
      std::bind(&BehaviorVelocityPlannerNode::onExternalIntersectionStates, this, _1));
  sub_external_velocity_limit_ = this->create_subscription<VelocityLimit>(
    "~/input/external_velocity_limit_mps", rclcpp::QoS{1}.transient_local(),
    std::bind(&BehaviorVelocityPlannerNode::onExternalVelocityLimit, this, _1));
  sub_external_traffic_signals_ =
    this->create_subscription<autoware_auto_perception_msgs::msg::TrafficSignalArray>(
      "~/input/external_traffic_signals", 10,
      std::bind(&BehaviorVelocityPlannerNode::onExternalTrafficSignals, this, _1),
      createSubscriptionOptions(this));
  sub_virtual_traffic_light_states_ =
    this->create_subscription<tier4_v2x_msgs::msg::VirtualTrafficLightStateArray>(
      "~/input/virtual_traffic_light_states", 10,
      std::bind(&BehaviorVelocityPlannerNode::onVirtualTrafficLightStates, this, _1),
      createSubscriptionOptions(this));
  sub_occupancy_grid_ = this->create_subscription<nav_msgs::msg::OccupancyGrid>(
    "~/input/occupancy_grid", 1, std::bind(&BehaviorVelocityPlannerNode::onOccupancyGrid, this, _1),
    createSubscriptionOptions(this));

  // set velocity smoother param
  onParam();

  // Publishers
  path_pub_ = this->create_publisher<autoware_auto_planning_msgs::msg::Path>("~/output/path", 1);
  stop_reason_diag_pub_ =
    this->create_publisher<diagnostic_msgs::msg::DiagnosticStatus>("~/output/stop_reason", 1);
  debug_viz_pub_ = this->create_publisher<visualization_msgs::msg::MarkerArray>("~/debug/path", 1);

  // Parameters
  forward_path_length_ = this->declare_parameter("forward_path_length", 1000.0);
  backward_path_length_ = this->declare_parameter("backward_path_length", 5.0);
  // TODO(yukkysaito): This will become unnecessary when acc output from localization is available.
  planner_data_.accel_lowpass_gain_ = this->declare_parameter("lowpass_gain", 0.5);
  planner_data_.stop_line_extend_length = this->declare_parameter("stop_line_extend_length", 5.0);

  // Initialize PlannerManager
  if (this->declare_parameter("launch_crosswalk", true)) {
    planner_manager_.launchSceneModule(std::make_shared<CrosswalkModuleManager>(*this));
    planner_manager_.launchSceneModule(std::make_shared<WalkwayModuleManager>(*this));
  }
  if (this->declare_parameter("launch_traffic_light", true)) {
    planner_manager_.launchSceneModule(std::make_shared<TrafficLightModuleManager>(*this));
  }
  if (this->declare_parameter("launch_intersection", true)) {
    // intersection module should be before merge from private to declare intersection parameters
    planner_manager_.launchSceneModule(std::make_shared<IntersectionModuleManager>(*this));
    planner_manager_.launchSceneModule(std::make_shared<MergeFromPrivateModuleManager>(*this));
  }
  if (this->declare_parameter("launch_blind_spot", true)) {
    planner_manager_.launchSceneModule(std::make_shared<BlindSpotModuleManager>(*this));
  }
  if (this->declare_parameter("launch_detection_area", true)) {
    planner_manager_.launchSceneModule(std::make_shared<DetectionAreaModuleManager>(*this));
  }
  if (this->declare_parameter("launch_virtual_traffic_light", true)) {
    planner_manager_.launchSceneModule(std::make_shared<VirtualTrafficLightModuleManager>(*this));
  }
  // this module requires all the stop line.Therefore this modules should be placed at the bottom.
  if (this->declare_parameter("launch_no_stopping_area", true)) {
    planner_manager_.launchSceneModule(std::make_shared<NoStoppingAreaModuleManager>(*this));
  }
  // permanent stop line module should be after no stopping area
  if (this->declare_parameter("launch_stop_line", true)) {
    planner_manager_.launchSceneModule(std::make_shared<StopLineModuleManager>(*this));
  }
  // to calculate ttc it's better to be after stop line
  if (this->declare_parameter("launch_occlusion_spot", true)) {
    planner_manager_.launchSceneModule(std::make_shared<OcclusionSpotModuleManager>(*this));
  }
  if (this->declare_parameter("launch_run_out", false)) {
    planner_manager_.launchSceneModule(std::make_shared<RunOutModuleManager>(*this));
  }
}

// NOTE: argument planner_data must not be referenced for multithreading
bool BehaviorVelocityPlannerNode::isDataReady(const PlannerData planner_data) const
{
  const auto & d = planner_data;

  // from tf
  if (d.current_pose.header.frame_id == "") {
    return false;
  }

  // from callbacks
  if (!d.current_velocity) {
    return false;
  }
  if (!d.current_accel) {
    return false;
  }
  if (!d.predicted_objects) {
    return false;
  }
  if (!d.no_ground_pointcloud) {
    return false;
  }
  if (!d.route_handler_) {
    return false;
  }
  if (!d.route_handler_->isMapMsgReady()) {
    return false;
  }
  if (!d.velocity_smoother_) {
    return false;
  }
  return true;
}

void BehaviorVelocityPlannerNode::onOccupancyGrid(
  const nav_msgs::msg::OccupancyGrid::ConstSharedPtr msg)
{
  std::lock_guard<std::mutex> lock(mutex_);
  planner_data_.occupancy_grid = msg;
}

void BehaviorVelocityPlannerNode::onPredictedObjects(
  const autoware_auto_perception_msgs::msg::PredictedObjects::ConstSharedPtr msg)
{
  std::lock_guard<std::mutex> lock(mutex_);
  planner_data_.predicted_objects = msg;
}

void BehaviorVelocityPlannerNode::onNoGroundPointCloud(
  const sensor_msgs::msg::PointCloud2::ConstSharedPtr msg)
{
  geometry_msgs::msg::TransformStamped transform;
  try {
    transform = tf_buffer_.lookupTransform(
      "map", msg->header.frame_id, msg->header.stamp, rclcpp::Duration::from_seconds(0.1));
  } catch (tf2::TransformException & e) {
    RCLCPP_WARN(get_logger(), "no transform found for no_ground_pointcloud: %s", e.what());
    return;
  }

  pcl::PointCloud<pcl::PointXYZ> pc;
  pcl::fromROSMsg(*msg, pc);

  Eigen::Affine3f affine = tf2::transformToEigen(transform.transform).cast<float>();
  pcl::PointCloud<pcl::PointXYZ>::Ptr pc_transformed(new pcl::PointCloud<pcl::PointXYZ>);
  if (!pc.empty()) {
    pcl::transformPointCloud(pc, *pc_transformed, affine);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    planner_data_.no_ground_pointcloud = pc_transformed;
  }
}

void BehaviorVelocityPlannerNode::onVehicleVelocity(
  const nav_msgs::msg::Odometry::ConstSharedPtr msg)
{
  std::lock_guard<std::mutex> lock(mutex_);

  auto current_velocity = std::make_shared<geometry_msgs::msg::TwistStamped>();
  current_velocity->header = msg->header;
  current_velocity->twist = msg->twist.twist;
  planner_data_.current_velocity = current_velocity;

  planner_data_.updateCurrentAcc();

  // Add velocity to buffer
  planner_data_.velocity_buffer.push_front(*current_velocity);
  const auto now = this->now();
  while (true) {
    // Check oldest data time
    const auto time_diff = now - planner_data_.velocity_buffer.back().header.stamp;

    // Finish when oldest data is newer than threshold
    if (time_diff.seconds() <= PlannerData::velocity_buffer_time_sec) {
      break;
    }

    // Remove old data
    planner_data_.velocity_buffer.pop_back();
  }
}

void BehaviorVelocityPlannerNode::onParam()
{
  planner_data_.velocity_smoother_ =
    std::make_unique<motion_velocity_smoother::AnalyticalJerkConstrainedSmoother>(*this);
  return;
}

void BehaviorVelocityPlannerNode::onLaneletMap(
  const autoware_auto_mapping_msgs::msg::HADMapBin::ConstSharedPtr msg)
{
  std::lock_guard<std::mutex> lock(mutex_);

  // Load map
  planner_data_.route_handler_ = std::make_shared<route_handler::RouteHandler>(*msg);
}

void BehaviorVelocityPlannerNode::onTrafficSignals(
  const autoware_auto_perception_msgs::msg::TrafficSignalArray::ConstSharedPtr msg)
{
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto & signal : msg->signals) {
    autoware_auto_perception_msgs::msg::TrafficSignalStamped traffic_signal;
    traffic_signal.header = msg->header;
    traffic_signal.signal = signal;
    planner_data_.traffic_light_id_map[signal.map_primitive_id] = traffic_signal;
  }
}

void BehaviorVelocityPlannerNode::onExternalCrosswalkStates(
  const tier4_api_msgs::msg::CrosswalkStatus::ConstSharedPtr msg)
{
  std::lock_guard<std::mutex> lock(mutex_);
  planner_data_.external_crosswalk_status_input = *msg;
}

void BehaviorVelocityPlannerNode::onExternalIntersectionStates(
  const tier4_api_msgs::msg::IntersectionStatus::ConstSharedPtr msg)
{
  std::lock_guard<std::mutex> lock(mutex_);
  planner_data_.external_intersection_status_input = *msg;
}

void BehaviorVelocityPlannerNode::onExternalVelocityLimit(const VelocityLimit::ConstSharedPtr msg)
{
  planner_data_.external_velocity_limit = *msg;
}

void BehaviorVelocityPlannerNode::onExternalTrafficSignals(
  const autoware_auto_perception_msgs::msg::TrafficSignalArray::ConstSharedPtr msg)
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto & signal : msg->signals) {
    autoware_auto_perception_msgs::msg::TrafficSignalStamped traffic_signal;
    traffic_signal.header = msg->header;
    traffic_signal.signal = signal;
    planner_data_.external_traffic_light_id_map[signal.map_primitive_id] = traffic_signal;
  }
}

void BehaviorVelocityPlannerNode::onVirtualTrafficLightStates(
  const tier4_v2x_msgs::msg::VirtualTrafficLightStateArray::ConstSharedPtr msg)
{
  std::lock_guard<std::mutex> lock(mutex_);
  planner_data_.virtual_traffic_light_states = msg;
}

void BehaviorVelocityPlannerNode::onTrigger(
  const autoware_auto_planning_msgs::msg::PathWithLaneId::ConstSharedPtr input_path_msg)
{
  mutex_.lock();  // for planner_data_
  // Check ready
  try {
    planner_data_.current_pose =
      transform2pose(tf_buffer_.lookupTransform("map", "base_link", tf2::TimePointZero));
  } catch (tf2::TransformException & e) {
    RCLCPP_INFO(get_logger(), "waiting for transform from `map` to `base_link`");
    mutex_.unlock();
    return;
  }

  if (!isDataReady(planner_data_)) {
    mutex_.unlock();
    return;
  }

  // NOTE: planner_data must not be referenced for multithreading
  const auto planner_data = planner_data_;
  mutex_.unlock();

  if (input_path_msg->points.empty()) {
    return;
  }

  const autoware_auto_planning_msgs::msg::Path output_path_msg =
    generatePath(input_path_msg, planner_data);

  path_pub_->publish(output_path_msg);
  stop_reason_diag_pub_->publish(planner_manager_.getStopReasonDiag());

  if (debug_viz_pub_->get_subscription_count() > 0) {
    publishDebugMarker(output_path_msg);
  }
}

bool BehaviorVelocityPlannerNode::isBackwardPath(
  const autoware_auto_planning_msgs::msg::PathWithLaneId & path) const
{
  const bool has_negative_velocity = std::any_of(
    path.points.begin(), path.points.end(),
    [&](const auto & p) { return p.point.longitudinal_velocity_mps < 0; });

  return has_negative_velocity;
}

autoware_auto_planning_msgs::msg::Path BehaviorVelocityPlannerNode::generatePath(
  const autoware_auto_planning_msgs::msg::PathWithLaneId::ConstSharedPtr input_path_msg,
  const PlannerData & planner_data)
{
  autoware_auto_planning_msgs::msg::Path output_path_msg;

  // TODO(someone): support negative velocity
  if (isBackwardPath(*input_path_msg)) {
    RCLCPP_WARN_THROTTLE(
      get_logger(), *get_clock(), 3000,
      "Negative velocity is NOT supported. just converting path_with_lane_id to path");
    output_path_msg = to_path(*input_path_msg);
    output_path_msg.header.frame_id = "map";
    output_path_msg.header.stamp = this->now();
    output_path_msg.drivable_area = input_path_msg->drivable_area;
    return output_path_msg;
  }

  // Plan path velocity
  const auto velocity_planned_path = planner_manager_.planPathVelocity(
    std::make_shared<const PlannerData>(planner_data), *input_path_msg);

  // screening
  const auto filtered_path = filterLitterPathPoint(to_path(velocity_planned_path));

  // interpolation
  const auto interpolated_path_msg = interpolatePath(filtered_path, forward_path_length_);

  // check stop point
  output_path_msg = filterStopPathPoint(interpolated_path_msg);

  output_path_msg.header.frame_id = "map";
  output_path_msg.header.stamp = this->now();

  // TODO(someone): This must be updated in each scene module, but copy from input message for now.
  output_path_msg.drivable_area = input_path_msg->drivable_area;

  return output_path_msg;
}

void BehaviorVelocityPlannerNode::publishDebugMarker(
  const autoware_auto_planning_msgs::msg::Path & path)
{
  visualization_msgs::msg::MarkerArray output_msg;
  for (size_t i = 0; i < path.points.size(); ++i) {
    visualization_msgs::msg::Marker marker;
    marker.header = path.header;
    marker.id = i;
    marker.type = visualization_msgs::msg::Marker::ARROW;
    marker.pose = path.points.at(i).pose;
    marker.scale.y = marker.scale.z = 0.05;
    marker.scale.x = 0.25;
    marker.action = visualization_msgs::msg::Marker::ADD;
    marker.lifetime = rclcpp::Duration::from_seconds(0.5);
    marker.color.a = 0.999;  // Don't forget to set the alpha!
    marker.color.r = 1.0;
    marker.color.g = 1.0;
    marker.color.b = 1.0;
    output_msg.markers.push_back(marker);
  }
  debug_viz_pub_->publish(output_msg);
}
}  // namespace behavior_velocity_planner

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(behavior_velocity_planner::BehaviorVelocityPlannerNode)
