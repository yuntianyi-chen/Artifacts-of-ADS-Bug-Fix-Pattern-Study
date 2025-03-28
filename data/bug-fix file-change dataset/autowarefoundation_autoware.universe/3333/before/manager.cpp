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

#include "scene_module/run_out/manager.hpp"

namespace behavior_velocity_planner
{
namespace
{
}  // namespace

RunOutModuleManager::RunOutModuleManager(rclcpp::Node & node)
: SceneModuleManagerInterface(node, getModuleName())
{
  // Vehicle Parameters
  {
    const auto vehicle_info = vehicle_info_util::VehicleInfoUtil(node).getVehicleInfo();
    auto & p = planner_param_.vehicle_param;
    p.base_to_front = vehicle_info.wheel_base_m + vehicle_info.front_overhang_m;
    p.base_to_rear = vehicle_info.rear_overhang_m;
    p.width = vehicle_info.vehicle_width_m;
    p.wheel_tread = vehicle_info.wheel_tread_m;
    p.right_overhang = vehicle_info.right_overhang_m;
    p.left_overhang = vehicle_info.left_overhang_m;
  }

  const std::string ns(getModuleName());

  {
    auto & p = planner_param_.smoother;
    // smoother parameters are already declared in behavior velocity node, so use get parameter
    p.start_jerk = node.get_parameter("backward.start_jerk").get_value<double>();
  }

  {
    auto & p = planner_param_.common;
    p.normal_min_jerk = node.declare_parameter<double>(".normal.min_jerk");
    p.normal_min_acc = node.declare_parameter<double>(".normal.min_acc");
    p.limit_min_jerk = node.declare_parameter<double>(".limit.min_jerk");
    p.limit_min_acc = node.declare_parameter<double>(".limit.min_acc");
  }

  {
    auto & p = planner_param_.run_out;
    p.detection_method = node.declare_parameter<std::string>(ns + ".detection_method");
    p.use_partition_lanelet = node.declare_parameter<bool>(ns + ".use_partition_lanelet");
    p.specify_decel_jerk = node.declare_parameter<bool>(ns + ".specify_decel_jerk");
    p.stop_margin = node.declare_parameter<double>(ns + ".stop_margin");
    p.passing_margin = node.declare_parameter<double>(ns + ".passing_margin");
    p.deceleration_jerk = node.declare_parameter<double>(ns + ".deceleration_jerk");
    p.detection_distance = node.declare_parameter<double>(ns + ".detection_distance");
    p.detection_span = node.declare_parameter<double>(ns + ".detection_span");
    p.min_vel_ego_kmph = node.declare_parameter<double>(ns + ".min_vel_ego_kmph");
  }

  {
    auto & p = planner_param_.detection_area;
    const std::string ns_da = ns + ".detection_area";
    p.margin_ahead = node.declare_parameter<double>(ns_da + ".margin_ahead");
    p.margin_behind = node.declare_parameter<double>(ns_da + ".margin_behind");
  }

  {
    auto & p = planner_param_.mandatory_area;
    const std::string ns_da = ns + ".mandatory_area";
    p.decel_jerk = node.declare_parameter<double>(ns_da + ".decel_jerk");
  }

  {
    auto & p = planner_param_.dynamic_obstacle;
    const std::string ns_do = ns + ".dynamic_obstacle";
    p.use_mandatory_area = node.declare_parameter<bool>(ns_do + ".use_mandatory_area");
    p.min_vel_kmph = node.declare_parameter<double>(ns_do + ".min_vel_kmph");
    p.max_vel_kmph = node.declare_parameter<double>(ns_do + ".max_vel_kmph");
    p.diameter = node.declare_parameter<double>(ns_do + ".diameter");
    p.height = node.declare_parameter<double>(ns_do + ".height");
    p.max_prediction_time = node.declare_parameter<double>(ns_do + ".max_prediction_time");
    p.time_step = node.declare_parameter<double>(ns_do + ".time_step");
    p.points_interval = node.declare_parameter<double>(ns_do + ".points_interval");
  }

  {
    auto & p = planner_param_.approaching;
    const std::string ns_a = ns + ".approaching";
    p.enable = node.declare_parameter<bool>(ns_a + ".enable");
    p.margin = node.declare_parameter<double>(ns_a + ".margin");
    p.limit_vel_kmph = node.declare_parameter<double>(ns_a + ".limit_vel_kmph");
  }

  {
    auto & p = planner_param_.state_param;
    const std::string ns_s = ns + ".state";
    p.stop_thresh = node.declare_parameter<double>(ns_s + ".stop_thresh");
    p.stop_time_thresh = node.declare_parameter<double>(ns_s + ".stop_time_thresh");
    p.disable_approach_dist = node.declare_parameter<double>(ns_s + ".disable_approach_dist");
    p.keep_approach_duration = node.declare_parameter<double>(ns_s + ".keep_approach_duration");
  }

  {
    auto & p = planner_param_.slow_down_limit;
    const std::string ns_m = ns + ".slow_down_limit";
    p.enable = node.declare_parameter<bool>(ns_m + ".enable");
    p.max_jerk = node.declare_parameter<double>(ns_m + ".max_jerk");
    p.max_acc = node.declare_parameter<double>(ns_m + ".max_acc");
  }

  debug_ptr_ = std::make_shared<RunOutDebug>(node);
  setDynamicObstacleCreator(node, debug_ptr_);
}

void RunOutModuleManager::launchNewModules(
  const autoware_auto_planning_msgs::msg::PathWithLaneId & path)
{
  if (path.points.empty()) {
    return;
  }

  constexpr int64_t module_id = 0;
  if (!isModuleRegistered(module_id)) {
    registerModule(std::make_shared<RunOutModule>(
      module_id, planner_data_, planner_param_, logger_.get_child("run_out_module"),
      std::move(dynamic_obstacle_creator_), debug_ptr_, clock_));
  }
}

std::function<bool(const std::shared_ptr<SceneModuleInterface> &)>
RunOutModuleManager::getModuleExpiredFunction(
  const autoware_auto_planning_msgs::msg::PathWithLaneId & path)
{
  return
    [&path]([[maybe_unused]] const std::shared_ptr<SceneModuleInterface> & scene_module) -> bool {
      return false;
    };
}

void RunOutModuleManager::setDynamicObstacleCreator(
  rclcpp::Node & node, std::shared_ptr<RunOutDebug> & debug_ptr)
{
  using run_out_utils::DetectionMethod;

  const auto detection_method_enum = run_out_utils::toEnum(planner_param_.run_out.detection_method);
  switch (detection_method_enum) {
    case DetectionMethod::Object:
      dynamic_obstacle_creator_ = std::make_unique<DynamicObstacleCreatorForObject>(
        node, debug_ptr, planner_param_.dynamic_obstacle);
      break;

    case DetectionMethod::ObjectWithoutPath:
      dynamic_obstacle_creator_ = std::make_unique<DynamicObstacleCreatorForObjectWithoutPath>(
        node, debug_ptr, planner_param_.dynamic_obstacle);
      break;

    case DetectionMethod::Points:
      dynamic_obstacle_creator_ = std::make_unique<DynamicObstacleCreatorForPoints>(
        node, debug_ptr, planner_param_.dynamic_obstacle);
      break;

    default:
      RCLCPP_WARN_STREAM(logger_, "detection method is invalid. use default method (Object).");
      dynamic_obstacle_creator_ = std::make_unique<DynamicObstacleCreatorForObject>(
        node, debug_ptr, planner_param_.dynamic_obstacle);
      break;
  }
}
}  // namespace behavior_velocity_planner
