//
//  Copyright 2020 Tier IV, Inc. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//

#include "autoware_state_panel.hpp"

#include <QString>
#include <QVBoxLayout>
#include <rviz_common/display_context.hpp>

#include <memory>
#include <string>

inline std::string Bool2String(const bool var) { return var ? "True" : "False"; }

using std::placeholders::_1;

namespace rviz_plugins
{
AutowareStatePanel::AutowareStatePanel(QWidget * parent) : rviz_common::Panel(parent)
{
  // Gate Mode
  auto * gate_prefix_label_ptr = new QLabel("GATE: ");
  gate_prefix_label_ptr->setAlignment(Qt::AlignRight);
  gate_mode_label_ptr_ = new QLabel("INIT");
  gate_mode_label_ptr_->setAlignment(Qt::AlignCenter);
  auto * gate_layout = new QHBoxLayout;
  gate_layout->addWidget(gate_prefix_label_ptr);
  gate_layout->addWidget(gate_mode_label_ptr_);

  // Selector Mode
  auto * selector_prefix_label_ptr = new QLabel("SELECT: ");
  selector_prefix_label_ptr->setAlignment(Qt::AlignRight);
  selector_mode_label_ptr_ = new QLabel("INIT");
  selector_mode_label_ptr_->setAlignment(Qt::AlignCenter);
  auto * selector_layout = new QHBoxLayout;
  selector_layout->addWidget(selector_prefix_label_ptr);
  selector_layout->addWidget(selector_mode_label_ptr_);

  // State
  auto * state_prefix_label_ptr = new QLabel("STATE: ");
  state_prefix_label_ptr->setAlignment(Qt::AlignRight);
  autoware_state_label_ptr_ = new QLabel("INIT");
  autoware_state_label_ptr_->setAlignment(Qt::AlignCenter);
  auto * state_layout = new QHBoxLayout;
  state_layout->addWidget(state_prefix_label_ptr);
  state_layout->addWidget(autoware_state_label_ptr_);

  // Gear
  auto * gear_prefix_label_ptr = new QLabel("GEAR: ");
  gear_prefix_label_ptr->setAlignment(Qt::AlignRight);
  gear_label_ptr_ = new QLabel("INIT");
  gear_label_ptr_->setAlignment(Qt::AlignCenter);
  auto * gear_layout = new QHBoxLayout;
  gear_layout->addWidget(gear_prefix_label_ptr);
  gear_layout->addWidget(gear_label_ptr_);

  // Engage Status
  auto * engage_prefix_label_ptr = new QLabel("Engage: ");
  engage_prefix_label_ptr->setAlignment(Qt::AlignRight);
  engage_status_label_ptr_ = new QLabel("INIT");
  engage_status_label_ptr_->setAlignment(Qt::AlignCenter);
  auto * engage_status_layout = new QHBoxLayout;
  engage_status_layout->addWidget(engage_prefix_label_ptr);
  engage_status_layout->addWidget(engage_status_label_ptr_);

  // Autoware Engage Button
  engage_button_ptr_ = new QPushButton("Engage");
  connect(engage_button_ptr_, SIGNAL(clicked()), SLOT(onClickAutowareEngage()));

  auto * v_layout = new QVBoxLayout;
  v_layout->addLayout(gate_layout);
  v_layout->addLayout(selector_layout);
  v_layout->addLayout(state_layout);
  v_layout->addLayout(gear_layout);
  v_layout->addLayout(engage_status_layout);
  v_layout->addWidget(engage_button_ptr_);
  setLayout(v_layout);
}

void AutowareStatePanel::onInitialize()
{
  raw_node_ = this->getDisplayContext()->getRosNodeAbstraction().lock()->get_raw_node();

  sub_gate_mode_ = raw_node_->create_subscription<tier4_control_msgs::msg::GateMode>(
    "/control/current_gate_mode", 10, std::bind(&AutowareStatePanel::onGateMode, this, _1));

  sub_selector_mode_ =
    raw_node_->create_subscription<tier4_control_msgs::msg::ExternalCommandSelectorMode>(
      "/control/external_cmd_selector/current_selector_mode", 10,
      std::bind(&AutowareStatePanel::onSelectorMode, this, _1));

  sub_autoware_state_ =
    raw_node_->create_subscription<autoware_auto_system_msgs::msg::AutowareState>(
      "/autoware/state", 10, std::bind(&AutowareStatePanel::onAutowareState, this, _1));

  sub_gear_ = raw_node_->create_subscription<autoware_auto_vehicle_msgs::msg::GearReport>(
    "/vehicle/status/gear_status", 10, std::bind(&AutowareStatePanel::onShift, this, _1));

  sub_engage_ = raw_node_->create_subscription<tier4_external_api_msgs::msg::EngageStatus>(
    "/api/autoware/get/engage", 10, std::bind(&AutowareStatePanel::onEngageStatus, this, _1));

  client_engage_ = raw_node_->create_client<tier4_external_api_msgs::srv::Engage>(
    "/api/autoware/set/engage", rmw_qos_profile_services_default);
}

void AutowareStatePanel::onGateMode(const tier4_control_msgs::msg::GateMode::ConstSharedPtr msg)
{
  switch (msg->data) {
    case tier4_control_msgs::msg::GateMode::AUTO:
      gate_mode_label_ptr_->setText("AUTO");
      gate_mode_label_ptr_->setStyleSheet("background-color: #00FF00;");
      break;

    case tier4_control_msgs::msg::GateMode::EXTERNAL:
      gate_mode_label_ptr_->setText("EXTERNAL");
      gate_mode_label_ptr_->setStyleSheet("background-color: #FFFF00;");
      break;

    default:
      gate_mode_label_ptr_->setText("UNKNOWN");
      gate_mode_label_ptr_->setStyleSheet("background-color: #FF0000;");
      break;
  }
}

void AutowareStatePanel::onSelectorMode(
  const tier4_control_msgs::msg::ExternalCommandSelectorMode::ConstSharedPtr msg)
{
  switch (msg->data) {
    case tier4_control_msgs::msg::ExternalCommandSelectorMode::REMOTE:
      selector_mode_label_ptr_->setText("REMOTE");
      selector_mode_label_ptr_->setStyleSheet("background-color: #00FF00;");
      break;

    case tier4_control_msgs::msg::ExternalCommandSelectorMode::LOCAL:
      selector_mode_label_ptr_->setText("LOCAL");
      selector_mode_label_ptr_->setStyleSheet("background-color: #FFFF00;");
      break;

    case tier4_control_msgs::msg::ExternalCommandSelectorMode::NONE:
      selector_mode_label_ptr_->setText("NONE");
      selector_mode_label_ptr_->setStyleSheet("background-color: #FF0000;");
      break;

    default:
      selector_mode_label_ptr_->setText("UNKNOWN");
      selector_mode_label_ptr_->setStyleSheet("background-color: #FF0000;");
      break;
  }
}

void AutowareStatePanel::onAutowareState(
  const autoware_auto_system_msgs::msg::AutowareState::ConstSharedPtr msg)
{
  if (msg->state == autoware_auto_system_msgs::msg::AutowareState::INITIALIZING) {
    autoware_state_label_ptr_->setText("INITIALIZING");
    autoware_state_label_ptr_->setStyleSheet("background-color: #FFFF00;");
  } else if (msg->state == autoware_auto_system_msgs::msg::AutowareState::WAITING_FOR_ROUTE) {
    autoware_state_label_ptr_->setText("WAITING_FOR_ROUTE");
    autoware_state_label_ptr_->setStyleSheet("background-color: #FFFF00;");
  } else if (msg->state == autoware_auto_system_msgs::msg::AutowareState::PLANNING) {
    autoware_state_label_ptr_->setText("PLANNING");
    autoware_state_label_ptr_->setStyleSheet("background-color: #FFFF00;");
  } else if (msg->state == autoware_auto_system_msgs::msg::AutowareState::WAITING_FOR_ENGAGE) {
    autoware_state_label_ptr_->setText("WAITING_FOR_ENGAGE");
    autoware_state_label_ptr_->setStyleSheet("background-color: #00FFFF;");
  } else if (msg->state == autoware_auto_system_msgs::msg::AutowareState::DRIVING) {
    autoware_state_label_ptr_->setText("DRIVING");
    autoware_state_label_ptr_->setStyleSheet("background-color: #00FF00;");
  } else if (msg->state == autoware_auto_system_msgs::msg::AutowareState::ARRIVED_GOAL) {
    autoware_state_label_ptr_->setText("ARRIVED_GOAL");
    autoware_state_label_ptr_->setStyleSheet("background-color: #FF00FF;");
  } else if (msg->state == autoware_auto_system_msgs::msg::AutowareState::FINALIZING) {
    autoware_state_label_ptr_->setText("FINALIZING");
    autoware_state_label_ptr_->setStyleSheet("background-color: #FFFF00;");
  }
}

void AutowareStatePanel::onShift(
  const autoware_auto_vehicle_msgs::msg::GearReport::ConstSharedPtr msg)
{
  switch (msg->report) {
    case autoware_auto_vehicle_msgs::msg::GearReport::PARK:
      gear_label_ptr_->setText("PARKING");
      break;
    case autoware_auto_vehicle_msgs::msg::GearReport::REVERSE:
      gear_label_ptr_->setText("REVERSE");
      break;
    case autoware_auto_vehicle_msgs::msg::GearReport::DRIVE:
      gear_label_ptr_->setText("DRIVE");
      break;
    case autoware_auto_vehicle_msgs::msg::GearReport::LOW:
      gear_label_ptr_->setText("LOW");
      break;
  }
}

void AutowareStatePanel::onEngageStatus(
  const tier4_external_api_msgs::msg::EngageStatus::ConstSharedPtr msg)
{
  current_engage_ = msg->engage;
  engage_status_label_ptr_->setText(QString::fromStdString(Bool2String(current_engage_)));
}

void AutowareStatePanel::onClickAutowareEngage()
{
  auto req = std::make_shared<tier4_external_api_msgs::srv::Engage::Request>();
  req->engage = current_engage_ ? false : true;

  RCLCPP_INFO(raw_node_->get_logger(), "client request");

  if (!client_engage_->service_is_ready()) {
    RCLCPP_INFO(raw_node_->get_logger(), "client is unavailable");
    return;
  }

  client_engage_->async_send_request(
    req, [this](rclcpp::Client<tier4_external_api_msgs::srv::Engage>::SharedFuture result) {
      RCLCPP_INFO(
        raw_node_->get_logger(), "Status: %d, %s", result.get()->status.code,
        result.get()->status.message.c_str());
    });
}

}  // namespace rviz_plugins

#include <pluginlib/class_list_macros.hpp>
PLUGINLIB_EXPORT_CLASS(rviz_plugins::AutowareStatePanel, rviz_common::Panel)
