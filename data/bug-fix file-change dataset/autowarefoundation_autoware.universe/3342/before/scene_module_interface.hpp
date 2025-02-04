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

#ifndef BEHAVIOR_PATH_PLANNER__SCENE_MODULE__SCENE_MODULE_INTERFACE_HPP_
#define BEHAVIOR_PATH_PLANNER__SCENE_MODULE__SCENE_MODULE_INTERFACE_HPP_

#include "behavior_path_planner/data_manager.hpp"
#include "behavior_path_planner/module_status.hpp"
#include "behavior_path_planner/scene_module/scene_module_visitor.hpp"
#include "behavior_path_planner/utilities.hpp"

#include <behavior_path_planner/steering_factor_interface.hpp>
#include <behavior_path_planner/turn_signal_decider.hpp>
#include <rclcpp/rclcpp.hpp>
#include <route_handler/route_handler.hpp>
#include <rtc_interface/rtc_interface.hpp>

#include <autoware_adapi_v1_msgs/msg/steering_factor_array.hpp>
#include <autoware_auto_planning_msgs/msg/path_with_lane_id.hpp>
#include <tier4_planning_msgs/msg/avoidance_debug_msg_array.hpp>
#include <unique_identifier_msgs/msg/uuid.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace behavior_path_planner
{
using autoware_adapi_v1_msgs::msg::SteeringFactor;
using autoware_auto_planning_msgs::msg::PathWithLaneId;
using rtc_interface::RTCInterface;
using steering_factor_interface::SteeringFactorInterface;
using tier4_autoware_utils::generateUUID;
using tier4_planning_msgs::msg::AvoidanceDebugMsgArray;
using unique_identifier_msgs::msg::UUID;
using visualization_msgs::msg::MarkerArray;
using PlanResult = PathWithLaneId::SharedPtr;

class SceneModuleInterface
{
public:
  SceneModuleInterface(
    const std::string & name, rclcpp::Node & node,
    const std::unordered_map<std::string, std::shared_ptr<RTCInterface>> & rtc_interface_ptr_map)
  : name_{name},
    logger_{node.get_logger().get_child(name)},
    clock_{node.get_clock()},
    is_waiting_approval_{false},
    is_locked_new_module_launch_{false},
    current_state_{ModuleStatus::SUCCESS},
    rtc_interface_ptr_map_(rtc_interface_ptr_map)
  {
#ifdef USE_OLD_ARCHITECTURE
    std::string module_ns;
    module_ns.resize(name.size());
    std::transform(name.begin(), name.end(), module_ns.begin(), tolower);

    const auto ns = std::string("~/debug/") + module_ns;
    pub_debug_marker_ = node.create_publisher<MarkerArray>(ns, 20);
#endif

    for (auto itr = rtc_interface_ptr_map_.begin(); itr != rtc_interface_ptr_map_.end(); ++itr) {
      uuid_map_.emplace(itr->first, generateUUID());
    }
  }

  virtual ~SceneModuleInterface() = default;

  /**
   * @brief Return SUCCESS if plan is not needed or plan is successfully finished,
   *        FAILURE if plan has failed, RUNNING if plan is on going.
   *        These condition is to be implemented in each modules.
   */
  virtual ModuleStatus updateState() = 0;

  /**
   * @brief If the module plan customized reference path while waiting approval, it should output
   * SUCCESS. Otherwise, it should output FAILURE to check execution request of next module.
   */
  virtual ModuleStatus getNodeStatusWhileWaitingApproval() const { return ModuleStatus::FAILURE; }

  /**
   * @brief Return true if the module has request for execution (not necessarily feasible)
   */
  virtual bool isExecutionRequested() const = 0;

  /**
   * @brief Return true if the execution is available (e.g. safety check for lane change)
   */
  virtual bool isExecutionReady() const = 0;

  /**
   * @brief Calculate path. This function is called with the plan is approved.
   */
  virtual BehaviorModuleOutput plan() = 0;

  /**
   * @brief Calculate path under waiting_approval condition.
   *        The default implementation is just to return the reference path.
   */
  virtual BehaviorModuleOutput planWaitingApproval()
  {
    BehaviorModuleOutput out;
    out.path = util::generateCenterLinePath(planner_data_);
    const auto candidate = planCandidate();
    path_candidate_ = std::make_shared<PathWithLaneId>(candidate.path_candidate);
    return out;
  }

  /**
   * @brief Get candidate path. This information is used for external judgement.
   */
  virtual CandidateOutput planCandidate() const = 0;

  /**
   * @brief update data for planning. Note that the call of this function does not mean
   *        that the module executed. It should only updates the data necessary for
   *        planCandidate (e.g., resampling of path).
   */
  virtual void updateData() {}

  /**
   * @brief Execute module. Once this function is executed,
   *        it will continue to run as long as it is in the RUNNING state.
   */
  virtual BehaviorModuleOutput run()
  {
    current_state_ = ModuleStatus::RUNNING;

    updateData();

    if (!isWaitingApproval()) {
      return plan();
    }

    // module is waiting approval. Check it.
    if (isActivated()) {
      RCLCPP_DEBUG(logger_, "Was waiting approval, and now approved. Do plan().");
      return plan();
    } else {
      RCLCPP_DEBUG(logger_, "keep waiting approval... Do planCandidate().");
      return planWaitingApproval();
    }
  }

  /**
   * @brief Called on the first time when the module goes into RUNNING.
   */
  void onEntry()
  {
    RCLCPP_DEBUG(getLogger(), "%s %s", name_.c_str(), __func__);

#ifdef USE_OLD_ARCHITECTURE
    current_state_ = ModuleStatus::SUCCESS;
#else
    current_state_ = ModuleStatus::IDLE;
#endif

    processOnEntry();
  }

  virtual void processOnEntry() {}

  /**
   * @brief Called when the module exit from RUNNING.
   */
  void onExit()
  {
    RCLCPP_DEBUG(getLogger(), "%s %s", name_.c_str(), __func__);

    current_state_ = ModuleStatus::SUCCESS;
    clearWaitingApproval();
    removeRTCStatus();
    unlockNewModuleLaunch();
    steering_factor_interface_ptr_->clearSteeringFactors();

    processOnExit();
  }

  virtual void processOnExit() {}

  /**
   * @brief Publish status if the module is requested to run
   */
  void publishRTCStatus()
  {
    for (auto itr = rtc_interface_ptr_map_.begin(); itr != rtc_interface_ptr_map_.end(); ++itr) {
      if (itr->second) {
        itr->second->publishCooperateStatus(clock_->now());
      }
    }
  }

  /**
   * @brief Return true if the activation command is received
   */
  bool isActivated()
  {
    for (auto itr = rtc_interface_ptr_map_.begin(); itr != rtc_interface_ptr_map_.end(); ++itr) {
      if (itr->second->isRegistered(uuid_map_.at(itr->first))) {
        return itr->second->isActivated(uuid_map_.at(itr->first));
      }
    }
    return false;
  }

  void publishSteeringFactor()
  {
    if (!steering_factor_interface_ptr_) {
      return;
    }
    steering_factor_interface_ptr_->publishSteeringFactor(clock_->now());
  }

  void lockRTCCommand()
  {
    for (auto itr = rtc_interface_ptr_map_.begin(); itr != rtc_interface_ptr_map_.end(); ++itr) {
      if (itr->second) {
        itr->second->lockCommandUpdate();
      }
    }
  }

  void unlockRTCCommand()
  {
    for (auto itr = rtc_interface_ptr_map_.begin(); itr != rtc_interface_ptr_map_.end(); ++itr) {
      if (itr->second) {
        itr->second->unlockCommandUpdate();
      }
    }
  }

  /**
   * @brief set previous module's output as input for this module
   */
  void setPreviousModuleOutput(const BehaviorModuleOutput & previous_module_output)
  {
    previous_module_output_ = previous_module_output;
  }

  /**
   * @brief set planner data
   */
  void setData(const std::shared_ptr<const PlannerData> & data) { planner_data_ = data; }

#ifdef USE_OLD_ARCHITECTURE
  void publishDebugMarker() { pub_debug_marker_->publish(debug_marker_); }
#endif

  bool isWaitingApproval() const { return is_waiting_approval_; }

  bool isLockedNewModuleLaunch() const { return is_locked_new_module_launch_; }

  void resetPathCandidate() { path_candidate_.reset(); }

  void resetPathReference() { path_reference_.reset(); }

  PlanResult getPathCandidate() const { return path_candidate_; }

  PlanResult getPathReference() const { return path_reference_; }

  MarkerArray getDebugMarkers() { return debug_marker_; }

  ModuleStatus getCurrentStatus() const { return current_state_; }

  virtual void acceptVisitor(const std::shared_ptr<SceneModuleVisitor> & visitor) const = 0;

  std::string name() const { return name_; }

  rclcpp::Logger getLogger() const { return logger_; }

private:
  std::string name_;

  rclcpp::Logger logger_;

#ifdef USE_OLD_ARCHITECTURE
  rclcpp::Publisher<MarkerArray>::SharedPtr pub_debug_marker_;
#endif

  BehaviorModuleOutput previous_module_output_;

protected:
  // TODO(murooka) Remove this function when BT-based architecture will be removed
  std::unordered_map<std::string, std::shared_ptr<RTCInterface>> createRTCInterfaceMap(
    rclcpp::Node & node, const std::string & name, const std::vector<std::string> & rtc_types)
  {
    std::unordered_map<std::string, std::shared_ptr<RTCInterface>> rtc_interface_ptr_map;
    for (const auto & rtc_type : rtc_types) {
      const auto snake_case_name = util::convertToSnakeCase(name);
      const auto rtc_interface_name =
        rtc_type == "" ? snake_case_name : snake_case_name + "_" + rtc_type;
      rtc_interface_ptr_map.emplace(
        rtc_type, std::make_shared<RTCInterface>(&node, rtc_interface_name));
    }
    return rtc_interface_ptr_map;
  }

  void updateRTCStatus(const double start_distance, const double finish_distance)
  {
    for (auto itr = rtc_interface_ptr_map_.begin(); itr != rtc_interface_ptr_map_.end(); ++itr) {
      if (itr->second) {
        itr->second->updateCooperateStatus(
          uuid_map_.at(itr->first), isExecutionReady(), start_distance, finish_distance,
          clock_->now());
      }
    }
  }

  void removeRTCStatus()
  {
    for (auto itr = rtc_interface_ptr_map_.begin(); itr != rtc_interface_ptr_map_.end(); ++itr) {
      if (itr->second) {
        itr->second->clearCooperateStatus();
      }
    }
  }

  BehaviorModuleOutput getPreviousModuleOutput() const { return previous_module_output_; }

  void lockNewModuleLaunch() { is_locked_new_module_launch_ = true; }

  void unlockNewModuleLaunch() { is_locked_new_module_launch_ = false; }

  void waitApproval() { is_waiting_approval_ = true; }

  void clearWaitingApproval() { is_waiting_approval_ = false; }

  rclcpp::Clock::SharedPtr clock_;

  std::shared_ptr<const PlannerData> planner_data_;

  std::unique_ptr<SteeringFactorInterface> steering_factor_interface_ptr_;

  bool is_waiting_approval_;
  bool is_locked_new_module_launch_;

  std::unordered_map<std::string, UUID> uuid_map_;

  PlanResult path_candidate_;
  PlanResult path_reference_;

  ModuleStatus current_state_;

  std::unordered_map<std::string, std::shared_ptr<RTCInterface>> rtc_interface_ptr_map_;

  mutable MarkerArray debug_marker_;
};

}  // namespace behavior_path_planner

#endif  // BEHAVIOR_PATH_PLANNER__SCENE_MODULE__SCENE_MODULE_INTERFACE_HPP_
