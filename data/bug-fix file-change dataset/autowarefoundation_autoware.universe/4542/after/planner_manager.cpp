// Copyright 2023 TIER IV, Inc.
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

#include "behavior_path_planner/planner_manager.hpp"

#include "behavior_path_planner/utils/path_utils.hpp"
#include "behavior_path_planner/utils/utils.hpp"

#include <lanelet2_extension/utility/utilities.hpp>
#include <magic_enum.hpp>

#include <boost/format.hpp>

#include <memory>
#include <string>

namespace behavior_path_planner
{
PlannerManager::PlannerManager(rclcpp::Node & node, const bool verbose)
: logger_(node.get_logger().get_child("planner_manager")),
  clock_(*node.get_clock()),
  verbose_{verbose}
{
  processing_time_.emplace("total_time", 0.0);
}

BehaviorModuleOutput PlannerManager::run(const std::shared_ptr<PlannerData> & data)
{
  resetProcessingTime();
  stop_watch_.tic("total_time");
  debug_info_.clear();

  if (!root_lanelet_) {
    root_lanelet_ = updateRootLanelet(data);
  }

  std::for_each(
    manager_ptrs_.begin(), manager_ptrs_.end(), [&data](const auto & m) { m->setData(data); });

  auto result_output = [&]() {
    const bool is_any_approved_module_running =
      std::any_of(approved_module_ptrs_.begin(), approved_module_ptrs_.end(), [](const auto & m) {
        return m->getCurrentStatus() == ModuleStatus::RUNNING ||
               m->getCurrentStatus() == ModuleStatus::WAITING_APPROVAL;
      });

    // IDLE is a state in which an execution has been requested but not yet approved.
    // once approved, it basically turns to running.
    const bool is_any_candidate_module_running_or_idle =
      std::any_of(candidate_module_ptrs_.begin(), candidate_module_ptrs_.end(), [](const auto & m) {
        return m->getCurrentStatus() == ModuleStatus::RUNNING ||
               m->getCurrentStatus() == ModuleStatus::WAITING_APPROVAL ||
               m->getCurrentStatus() == ModuleStatus::IDLE;
      });

    const bool is_any_module_running =
      is_any_approved_module_running || is_any_candidate_module_running_or_idle;

    const bool is_out_of_route = utils::isEgoOutOfRoute(
      data->self_odometry->pose.pose, data->prev_modified_goal, data->route_handler);

    if (!is_any_module_running && is_out_of_route) {
      BehaviorModuleOutput output = utils::createGoalAroundPath(data);
      generateCombinedDrivableArea(output, data);
      RCLCPP_WARN_THROTTLE(
        logger_, clock_, 5000,
        "Ego is out of route, no module is running. Skip running scene modules.");
      return output;
    }

    while (rclcpp::ok()) {
      /**
       * STEP1: get approved modules' output
       */
      const auto approved_modules_output = runApprovedModules(data);

      /**
       * STEP2: check modules that need to be launched
       */
      const auto request_modules = getRequestModules(approved_modules_output);

      /**
       * STEP3: if there is no module that need to be launched, return approved modules' output
       */
      if (request_modules.empty()) {
        processing_time_.at("total_time") = stop_watch_.toc("total_time", true);
        return approved_modules_output;
      }

      /**
       * STEP4: if there is module that should be launched, execute the module
       */
      const auto [highest_priority_module, candidate_modules_output] =
        runRequestModules(request_modules, data, approved_modules_output);
      if (!highest_priority_module) {
        processing_time_.at("total_time") = stop_watch_.toc("total_time", true);
        return approved_modules_output;
      }

      /**
       * STEP5: if the candidate module's modification is NOT approved yet, return the result.
       * NOTE: the result is output of the candidate module, but the output path don't contains path
       * shape modification that needs approval. On the other hand, it could include velocity
       * profile modification.
       */
      if (highest_priority_module->isWaitingApproval()) {
        processing_time_.at("total_time") = stop_watch_.toc("total_time", true);
        return candidate_modules_output;
      }

      /**
       * STEP6: if the candidate module is approved, push the module into approved_module_ptrs_
       */
      addApprovedModule(highest_priority_module);
      clearCandidateModules();
      debug_info_.emplace_back(highest_priority_module, Action::ADD, "To Approval");
    }
    return BehaviorModuleOutput{};
  }();

  generateCombinedDrivableArea(result_output, data);

  return result_output;
}

// NOTE: To deal with some policies about drivable area generation, currently DrivableAreaInfo is
// quite messy. Needs to be refactored.
void PlannerManager::generateCombinedDrivableArea(
  BehaviorModuleOutput & output, const std::shared_ptr<PlannerData> & data) const
{
  if (!output.path || output.path->points.empty()) {
    RCLCPP_ERROR_STREAM(logger_, "[generateCombinedDrivableArea] Output path is empty!");
    return;
  }

  const auto & di = output.drivable_area_info;
  constexpr double epsilon = 1e-3;

  const auto is_driving_forward_opt = motion_utils::isDrivingForward(output.path->points);
  const bool is_driving_forward = is_driving_forward_opt ? *is_driving_forward_opt : true;

  if (epsilon < std::abs(di.drivable_margin)) {
    // for single free space pull over
    utils::generateDrivableArea(
      *output.path, data->parameters.vehicle_length, di.drivable_margin, is_driving_forward);
  } else if (di.is_already_expanded) {
    // for single side shift
    utils::generateDrivableArea(
      *output.path, di.drivable_lanes, false, false, data->parameters.vehicle_length, data,
      is_driving_forward);
  } else {
    const auto shorten_lanes = utils::cutOverlappedLanes(*output.path, di.drivable_lanes);

    const auto & dp = data->drivable_area_expansion_parameters;
    const auto expanded_lanes = utils::expandLanelets(
      shorten_lanes, dp.drivable_area_left_bound_offset, dp.drivable_area_right_bound_offset,
      dp.drivable_area_types_to_skip);

    // for other modules where multiple modules may be launched
    utils::generateDrivableArea(
      *output.path, expanded_lanes, di.enable_expanding_hatched_road_markings,
      di.enable_expanding_intersection_areas, data->parameters.vehicle_length, data,
      is_driving_forward);
  }

  // extract obstacles from drivable area
  utils::extractObstaclesFromDrivableArea(*output.path, di.obstacles);
}

std::vector<SceneModulePtr> PlannerManager::getRequestModules(
  const BehaviorModuleOutput & previous_module_output) const
{
  if (!previous_module_output.path) {
    RCLCPP_ERROR_STREAM(
      logger_, "Current module output is null. Skip candidate module check."
                 << "\n      - Approved  module list: " << getNames(approved_module_ptrs_)
                 << "\n      - Candidate module list: " << getNames(candidate_module_ptrs_));
    return {};
  }

  std::vector<SceneModulePtr> request_modules{};

  /**
   * check whether it is possible to push back more modules to approved modules.
   */
  {
    const auto find_block_module = [this](const auto & m) {
      return !getManager(m)->isSimultaneousExecutableAsApprovedModule();
    };

    const auto itr =
      std::find_if(approved_module_ptrs_.begin(), approved_module_ptrs_.end(), find_block_module);

    if (itr != approved_module_ptrs_.end()) {
      return {};
    }
  }

  const auto toc = [this](const auto & name) {
    processing_time_.at(name) += stop_watch_.toc(name, true);
  };

  for (const auto & manager_ptr : manager_ptrs_) {
    stop_watch_.tic(manager_ptr->name());

    /**
     * don't launch candidate module if approved modules already exist.
     */
    if (!approved_module_ptrs_.empty()) {
      if (!manager_ptr->isSimultaneousExecutableAsApprovedModule()) {
        toc(manager_ptr->name());
        continue;
      }
    }

    /**
     * launch new candidate module.
     */
    {
      const auto name = manager_ptr->name();
      const auto find_same_name_module = [&name](const auto & m) { return m->name() == name; };
      const auto itr = std::find_if(
        candidate_module_ptrs_.begin(), candidate_module_ptrs_.end(), find_same_name_module);

      if (itr == candidate_module_ptrs_.end()) {
        if (manager_ptr->canLaunchNewModule()) {
          manager_ptr->updateIdleModuleInstance();
          if (manager_ptr->isExecutionRequested(previous_module_output)) {
            request_modules.emplace_back(manager_ptr->getIdleModule());
          }
        }

        toc(manager_ptr->name());
        continue;
      }
    }

    /**
     * module already exist in candidate modules. check whether other modules can be launch as
     * candidate. if locked, break this loop.
     */
    {
      const auto name = manager_ptr->name();
      const auto find_block_module = [&name](const auto & m) {
        return m->name() == name && m->isLockedNewModuleLaunch();
      };
      const auto itr = std::find_if(
        candidate_module_ptrs_.begin(), candidate_module_ptrs_.end(), find_block_module);

      if (itr != candidate_module_ptrs_.end()) {
        request_modules.clear();
        request_modules.emplace_back(*itr);
        toc(manager_ptr->name());
        break;
      }
    }

    /**
     * module already exist. keep using it as candidate.
     */
    {
      const auto name = manager_ptr->name();
      const auto find_launched_module = [&name](const auto & m) { return m->name() == name; };
      const auto itr = std::find_if(
        candidate_module_ptrs_.begin(), candidate_module_ptrs_.end(), find_launched_module);

      if (itr != candidate_module_ptrs_.end()) {
        request_modules.emplace_back(*itr);
        toc(manager_ptr->name());
        continue;
      }
    }

    /**
     * same name module doesn't exist in candidate modules. launch new module.
     */
    {
      if (!manager_ptr->canLaunchNewModule()) {
        toc(manager_ptr->name());
        continue;
      }

      manager_ptr->updateIdleModuleInstance();
      if (!manager_ptr->isExecutionRequested(previous_module_output)) {
        toc(manager_ptr->name());
        continue;
      }

      request_modules.emplace_back(manager_ptr->getIdleModule());
    }

    toc(manager_ptr->name());
  }

  return request_modules;
}

SceneModulePtr PlannerManager::selectHighestPriorityModule(
  std::vector<SceneModulePtr> & request_modules) const
{
  if (request_modules.empty()) {
    return {};
  }

  sortByPriority(request_modules);

  return request_modules.front();
}

std::pair<SceneModulePtr, BehaviorModuleOutput> PlannerManager::runRequestModules(
  const std::vector<SceneModulePtr> & request_modules, const std::shared_ptr<PlannerData> & data,
  const BehaviorModuleOutput & previous_module_output)
{
  // modules that are filtered by simultaneous executable condition.
  std::vector<SceneModulePtr> executable_modules;

  // modules that are not approved after running yet.
  std::vector<SceneModulePtr> waiting_approved_modules;

  // modules that are already approved after running.
  std::vector<SceneModulePtr> already_approved_modules;

  // all request modules planning results.
  std::unordered_map<std::string, BehaviorModuleOutput> results;

  /**
   * sort by priority. sorted_request_modules.front() is the highest priority module.
   */
  auto sorted_request_modules = request_modules;
  sortByPriority(sorted_request_modules);

  /**
   * remove non-executable modules.
   */
  for (const auto & module_ptr : sorted_request_modules) {
    if (!getManager(module_ptr)->isSimultaneousExecutableAsCandidateModule()) {
      if (executable_modules.empty()) {
        executable_modules.push_back(module_ptr);
        break;
      }
    }

    const auto itr =
      std::find_if(executable_modules.begin(), executable_modules.end(), [this](const auto & m) {
        return !getManager(m)->isSimultaneousExecutableAsCandidateModule();
      });

    if (itr == executable_modules.end()) {
      executable_modules.push_back(module_ptr);
    }
  }

  /**
   * run executable modules.
   */
  for (const auto & module_ptr : executable_modules) {
    const auto & manager_ptr = getManager(module_ptr);

    if (!manager_ptr->exist(module_ptr)) {
      manager_ptr->registerNewModule(
        std::weak_ptr<SceneModuleInterface>(module_ptr), previous_module_output);
    }

    results.emplace(module_ptr->name(), run(module_ptr, data, previous_module_output));
  }

  /**
   * remove expired modules.
   */
  {
    const auto remove_expired_modules = [this](auto & m) {
      if (m->getCurrentStatus() == ModuleStatus::FAILURE) {
        deleteExpiredModules(m);
        return true;
      }

      if (m->getCurrentStatus() == ModuleStatus::SUCCESS) {
        deleteExpiredModules(m);
        return true;
      }

      return false;
    };

    executable_modules.erase(
      std::remove_if(executable_modules.begin(), executable_modules.end(), remove_expired_modules),
      executable_modules.end());

    std::for_each(
      manager_ptrs_.begin(), manager_ptrs_.end(), [](const auto & m) { m->updateObserver(); });
  }

  /**
   * return null data if valid candidate module doesn't exist.
   */
  if (executable_modules.empty()) {
    clearCandidateModules();
    return std::make_pair(nullptr, BehaviorModuleOutput{});
  }

  /**
   * separate by approve condition.
   */
  std::for_each(executable_modules.begin(), executable_modules.end(), [&](const auto & m) {
    if (m->isWaitingApproval()) {
      waiting_approved_modules.push_back(m);
    } else {
      already_approved_modules.push_back(m);
    }
  });

  /**
   * choice highest priority module.
   */
  const auto module_ptr = [&]() {
    if (!already_approved_modules.empty()) {
      return selectHighestPriorityModule(already_approved_modules);
    }

    if (!waiting_approved_modules.empty()) {
      return selectHighestPriorityModule(waiting_approved_modules);
    }

    return SceneModulePtr();
  }();

  /**
   * register candidate modules.
   */
  updateCandidateModules(executable_modules, module_ptr);

  return std::make_pair(module_ptr, results.at(module_ptr->name()));
}

BehaviorModuleOutput PlannerManager::runApprovedModules(const std::shared_ptr<PlannerData> & data)
{
  std::unordered_map<std::string, BehaviorModuleOutput> results;
  BehaviorModuleOutput output = getReferencePath(data);
  results.emplace("root", output);

  /**
   * execute all approved modules.
   */
  std::for_each(approved_module_ptrs_.begin(), approved_module_ptrs_.end(), [&](const auto & m) {
    output = run(m, data, output);
    results.emplace(m->name(), output);
  });

  /**
   * pop waiting approve module. push it back candidate modules. if waiting approve module is found
   * in iteration std::find_if, not only the module but also the next one are removed from
   * approved_module_ptrs_ since the next module's input (= waiting approve module's output) maybe
   * change significantly. And, only the waiting approve module is pushed back to
   * candidate_module_ptrs_.
   */
  {
    const auto waiting_approval_modules = [](const auto & m) { return m->isWaitingApproval(); };

    const auto itr = std::find_if(
      approved_module_ptrs_.begin(), approved_module_ptrs_.end(), waiting_approval_modules);

    if (itr != approved_module_ptrs_.end()) {
      clearCandidateModules();
      candidate_module_ptrs_.push_back(*itr);

      std::for_each(
        std::next(itr), approved_module_ptrs_.end(), [this](auto & m) { deleteExpiredModules(m); });
      std::for_each(
        manager_ptrs_.begin(), manager_ptrs_.end(), [](const auto & m) { m->updateObserver(); });

      debug_info_.emplace_back(*itr, Action::MOVE, "Back To Waiting Approval");
    }

    approved_module_ptrs_.erase(itr, approved_module_ptrs_.end());
  }

  /**
   * remove failure modules. these modules' outputs are discarded as invalid plan.
   */
  {
    const auto itr = std::find_if(
      approved_module_ptrs_.begin(), approved_module_ptrs_.end(),
      [](const auto & m) { return m->getCurrentStatus() == ModuleStatus::FAILURE; });

    std::for_each(itr, approved_module_ptrs_.end(), [this](auto & m) {
      debug_info_.emplace_back(m, Action::DELETE, "From Approved");
      deleteExpiredModules(m);
    });

    std::for_each(
      manager_ptrs_.begin(), manager_ptrs_.end(), [](const auto & m) { m->updateObserver(); });

    if (itr != approved_module_ptrs_.end()) {
      clearCandidateModules();
    }

    approved_module_ptrs_.erase(itr, approved_module_ptrs_.end());
  }

  if (approved_module_ptrs_.empty()) {
    return results.at("root");
  }

  /**
   * use the last module's output as approved modules planning result.
   */
  const auto output_module_name = approved_module_ptrs_.back()->name();
  const auto approved_modules_output = [&output_module_name, &results]() {
    if (results.count(output_module_name) == 0) {
      return results.at("root");
    }
    return results.at(output_module_name);
  }();

  /**
   * remove success module immediately. if lane change module has succeeded, update root lanelet.
   */
  {
    const auto success_module_itr = std::partition(
      approved_module_ptrs_.begin(), approved_module_ptrs_.end(),
      [](const auto & m) { return m->getCurrentStatus() != ModuleStatus::SUCCESS; });

    const auto success_lane_change = std::any_of(
      success_module_itr, approved_module_ptrs_.end(),
      [](const auto & m) { return m->name().find("lane_change") != std::string::npos; });

    if (success_lane_change) {
      root_lanelet_ = updateRootLanelet(data, true);
    }

    std::for_each(success_module_itr, approved_module_ptrs_.end(), [&](auto & m) {
      debug_info_.emplace_back(m, Action::DELETE, "From Approved");
      deleteExpiredModules(m);
    });

    approved_module_ptrs_.erase(success_module_itr, approved_module_ptrs_.end());

    std::for_each(
      manager_ptrs_.begin(), manager_ptrs_.end(), [](const auto & m) { m->updateObserver(); });
  }

  return approved_modules_output;
}

void PlannerManager::updateCandidateModules(
  const std::vector<SceneModulePtr> & request_modules,
  const SceneModulePtr & highest_priority_module)
{
  const auto exist = [](const auto & module_ptr, const auto & module_ptrs) {
    const auto itr = std::find_if(
      module_ptrs.begin(), module_ptrs.end(),
      [&module_ptr](const auto & m) { return m->name() == module_ptr->name(); });

    return itr != module_ptrs.end();
  };

  /**
   * unregister expired modules
   */
  {
    const auto candidate_to_remove = [&](auto & itr) {
      if (!exist(itr, request_modules)) {
        deleteExpiredModules(itr);
        return true;
      }
      return itr->name() == highest_priority_module->name() &&
             !highest_priority_module->isWaitingApproval();
    };

    candidate_module_ptrs_.erase(
      std::remove_if(
        candidate_module_ptrs_.begin(), candidate_module_ptrs_.end(), candidate_to_remove),
      candidate_module_ptrs_.end());

    std::for_each(
      manager_ptrs_.begin(), manager_ptrs_.end(), [](const auto & m) { m->updateObserver(); });
  }

  /**
   * register running candidate modules
   */
  for (const auto & m : request_modules) {
    if (
      m->name() == highest_priority_module->name() &&
      !highest_priority_module->isWaitingApproval()) {
      continue;
    }

    if (!exist(m, candidate_module_ptrs_)) {
      candidate_module_ptrs_.push_back(m);
    }
  }

  /**
   * sort by priority. sorted_request_modules.front() is the highest priority module.
   */
  sortByPriority(candidate_module_ptrs_);
}

void PlannerManager::resetRootLanelet(const std::shared_ptr<PlannerData> & data)
{
  if (!root_lanelet_) {
    root_lanelet_ = updateRootLanelet(data);
    return;
  }

  // when lane change module is running, don't update root lanelet.
  const bool is_lane_change_running = std::invoke([&]() {
    const auto lane_change_itr = std::find_if(
      approved_module_ptrs_.begin(), approved_module_ptrs_.end(),
      [](const auto & m) { return m->name().find("lane_change") != std::string::npos; });
    return lane_change_itr != approved_module_ptrs_.end();
  });
  if (is_lane_change_running) {
    return;
  }

  const auto root_lanelet = updateRootLanelet(data);

  // if root_lanelet is not route lanelets, reset root lanelet.
  // this can be caused by rerouting.
  const auto & route_handler = data->route_handler;
  if (!route_handler->isRouteLanelet(root_lanelet_.get())) {
    root_lanelet_ = root_lanelet;
    return;
  }

  // check ego is in same lane
  if (root_lanelet_.get().id() == root_lanelet.id()) {
    return;
  }

  // check ego is in next lane
  const auto next_lanelets = route_handler->getRoutingGraphPtr()->following(root_lanelet_.get());
  for (const auto & next : next_lanelets) {
    if (next.id() == root_lanelet.id()) {
      return;
    }
  }

  if (!approved_module_ptrs_.empty()) {
    return;
  }

  for (const auto & m : candidate_module_ptrs_) {
    if (m->isLockedNewModuleLaunch()) {
      return;
    }
  }

  root_lanelet_ = root_lanelet;

  RCLCPP_INFO_EXPRESSION(logger_, verbose_, "change ego's following lane. reset.");
}

void PlannerManager::print() const
{
  if (!verbose_) {
    return;
  }

  const auto get_status = [](const auto & m) {
    return magic_enum::enum_name(m->getCurrentStatus());
  };

  size_t max_string_num = 0;

  std::ostringstream string_stream;
  string_stream << "\n";
  string_stream << "***********************************************************\n";
  string_stream << "                  planner manager status\n";
  string_stream << "-----------------------------------------------------------\n";
  string_stream << "registered modules: ";
  for (const auto & m : manager_ptrs_) {
    string_stream << "[" << m->name() << "]";
    max_string_num = std::max(max_string_num, m->name().length());
  }

  string_stream << "\n";
  string_stream << "approved modules  : ";
  for (const auto & m : approved_module_ptrs_) {
    string_stream << "[" << m->name() << "(" << get_status(m) << ")"
                  << "]->";
  }

  string_stream << "\n";
  string_stream << "candidate module  : ";
  for (const auto & m : candidate_module_ptrs_) {
    string_stream << "[" << m->name() << "(" << get_status(m) << ")"
                  << "]->";
  }

  string_stream << "\n";
  string_stream << "update module info: ";
  for (const auto & i : debug_info_) {
    string_stream << "[Module:" << i.module_name << " Status:" << magic_enum::enum_name(i.status)
                  << " Action:" << magic_enum::enum_name(i.action)
                  << " Description:" << i.description << "]\n"
                  << std::setw(28);
  }

  string_stream << "\n" << std::fixed << std::setprecision(1);
  string_stream << "processing time   : ";
  for (const auto & t : processing_time_) {
    string_stream << std::right << "[" << std::setw(max_string_num + 1) << std::left << t.first
                  << ":" << std::setw(4) << std::right << t.second << "ms]\n"
                  << std::setw(21);
  }

  RCLCPP_INFO_STREAM(logger_, string_stream.str());
}

std::shared_ptr<SceneModuleVisitor> PlannerManager::getDebugMsg()
{
  debug_msg_ptr_ = std::make_shared<SceneModuleVisitor>();
  for (const auto & approved_module : approved_module_ptrs_) {
    approved_module->acceptVisitor(debug_msg_ptr_);
  }

  for (const auto & candidate_module : candidate_module_ptrs_) {
    candidate_module->acceptVisitor(debug_msg_ptr_);
  }
  return debug_msg_ptr_;
}

std::string PlannerManager::getNames(const std::vector<SceneModulePtr> & modules) const
{
  std::stringstream ss;
  for (const auto & m : modules) {
    ss << "[" << m->name() << "], ";
  }
  return ss.str();
}

}  // namespace behavior_path_planner
