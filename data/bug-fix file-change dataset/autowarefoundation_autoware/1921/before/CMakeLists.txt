cmake_minimum_required(VERSION 2.8.12)
project(decision_maker)

## Compile as C++11, supported in ROS Kinetic and newer
add_compile_options(-lpthread -pg -Wall)

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pg")
SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pg")
SET(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -pg")


find_package(autoware_build_flags REQUIRED)

find_package(autoware_msgs REQUIRED)

find_package(autoware_config_msgs REQUIRED)

find_package(vector_map_msgs REQUIRED)

find_package(vector_map REQUIRED)
find_package(catkin REQUIRED COMPONENTS
        roscpp
        std_msgs
        tf
        geometry_msgs
        jsk_recognition_msgs
        dynamic_reconfigure
        amathutils_lib
        state_machine_lib
        autoware_msgs
        autoware_config_msgs
        vector_map_msgs
        )

catkin_package(
        CATKIN_DEPENDS roscpp
        tf
        std_msgs
        autoware_config_msgs
        autoware_msgs
        geometry_msgs
        vector_map_msgs
        vector_map
        jsk_recognition_msgs
        vector_map_msgs
        amathutils_lib
        state_machine_lib
)
include_directories(
        include
        ${catkin_INCLUDE_DIRS}
        ${vector_map_INCLUDE_DIRS}
        ${autoware_config_msgs_INCLUDE_DIRS}
        ${autoware_msgs_INCLUDE_DIRS}
)

add_executable(decision_maker_node
        nodes/decision_maker/decision_maker_node.cpp
        nodes/decision_maker/decision_maker_node_core.cpp
        nodes/decision_maker/decision_maker_node_decision.cpp
        nodes/decision_maker/decision_maker_node_init.cpp
        nodes/decision_maker/decision_maker_node_callback.cpp
        nodes/decision_maker/decision_maker_node_state_vehicle.cpp
        nodes/decision_maker/decision_maker_node_state_drive.cpp
        nodes/decision_maker/decision_maker_node_state_mission.cpp
        nodes/decision_maker/decision_maker_node_publish.cpp
        nodes/decision_maker/cross_road_area.cpp
        )
target_link_libraries(decision_maker_node
        ${catkin_LIBRARIES}
        ${vector_map_LIBRARIES}
        )
add_dependencies(decision_maker_node ${catkin_EXPORTED_TARGETS})

install(TARGETS decision_maker_node
        ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
        LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
        RUNTIME DESTINATION ${CATKIN_GLOBAL_BIN_DESTINATION}
        )

install(DIRECTORY launch
        DESTINATION ${CATKIN_PACKAGE_INCLUDE_DESTINATION}/launch/
        PATTERN ".svn" EXCLUDE
        )

# Install config files
foreach(dir config)
  install(DIRECTORY ${dir}/
    DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}/${dir})
endforeach(dir)
