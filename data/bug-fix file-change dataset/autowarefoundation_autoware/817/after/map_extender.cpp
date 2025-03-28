/*
 *  Copyright (c) 2015, Nagoya University
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither the name of Autoware nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 Localization program using Normal Distributions Transform

 Yuki KITSUKAWA
 */

#include <iostream>
#include <sstream>
#include <fstream>
#include <string>

#include <ros/ros.h>
#include <std_msgs/String.h>
#include <std_msgs/Bool.h>
#include <sensor_msgs/PointCloud2.h>
#include <geometry_msgs/TwistStamped.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>

#include <velodyne_pointcloud/point_types.h>
#include <velodyne_pointcloud/rawdata.h>

#include <tf/tf.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_datatypes.h>
#include <tf/transform_listener.h>

#include <pcl/io/io.h>
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/registration/ndt.h>
#include <pcl/filters/approximate_voxel_grid.h>
#include <pcl/filters/voxel_grid.h>

struct pose
{
  double x;
  double y;
  double z;
  double roll;
  double pitch;
  double yaw;
};

//Can't load if typed "pcl::PointCloud<pcl::PointXYZRGB> map, add;"
static pcl::PointCloud<pcl::PointXYZI> map;
static pcl::PointCloud<pcl::PointXYZI>::Ptr map_ptr;
pcl::PointCloud<pcl::PointXYZI>::Ptr additional_map_ptr(new pcl::PointCloud<pcl::PointXYZI>);
pcl::PointCloud<pcl::PointXYZI>::Ptr filtered_additional_map_ptr(new pcl::PointCloud<pcl::PointXYZI>);

static pcl::NormalDistributionsTransform<pcl::PointXYZI, pcl::PointXYZI> ndt;

// Default values
static int iter = 1000; // Maximum iterations
static float ndt_res = 1.0; // Resolution
static double step_size = 0.05; // Step size
static double trans_eps = 0.005; // Transformation epsilon
static double voxel_leaf_size = 1.0;

static ros::Publisher ndt_map_pub;

static bool _get_height = false;
static bool hasMapSet = false;

static std::string input_pcd, output_pcd, output_pcd_rgb;

static void initialpose_callback(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& input)
{

  pose guess_pose;

  tf::TransformListener listener;
  tf::StampedTransform transform;
  try
  {
    ros::Time now = ros::Time(0);
    listener.waitForTransform("/map", "/world", now, ros::Duration(10.0));
    listener.lookupTransform("/map", "world", now, transform);
  }
  catch (tf::TransformException& ex)
  {
    ROS_ERROR("%s", ex.what());
  }

  tf::Quaternion q(input->pose.pose.orientation.x, input->pose.pose.orientation.y, input->pose.pose.orientation.z, input->pose.pose.orientation.w);
  tf::Matrix3x3 m(q);

  guess_pose.x = input->pose.pose.position.x + transform.getOrigin().x();
  guess_pose.y = input->pose.pose.position.y + transform.getOrigin().y();
  guess_pose.z = input->pose.pose.position.z + transform.getOrigin().z();
  m.getRPY(guess_pose.roll, guess_pose.pitch, guess_pose.yaw);

  if (_get_height == true)
  {
    double min_distance = DBL_MAX;
    double nearest_z = guess_pose.z;
    for (const auto& p : map)
    {
      double distance = hypot(guess_pose.x - p.x, guess_pose.y - p.y);
      if (distance < min_distance)
      {
        min_distance = distance;
        nearest_z = p.z;
      }
    }
    guess_pose.z = nearest_z;
  }

  Eigen::Translation3f init_translation(guess_pose.x, guess_pose.y, guess_pose.z);
  Eigen::AngleAxisf init_rotation_x(guess_pose.roll, Eigen::Vector3f::UnitX());
  Eigen::AngleAxisf init_rotation_y(guess_pose.pitch, Eigen::Vector3f::UnitY());
  Eigen::AngleAxisf init_rotation_z(guess_pose.yaw, Eigen::Vector3f::UnitZ());

  Eigen::Matrix4f init_guess = (init_translation * init_rotation_z * init_rotation_y * init_rotation_x).matrix();

  pcl::PointCloud<pcl::PointXYZI>::Ptr output_cloud(new pcl::PointCloud<pcl::PointXYZI>);
  ndt.align(*output_cloud, init_guess);

  Eigen::Matrix4f t = ndt.getFinalTransformation();

  pcl::PointCloud<pcl::PointXYZI>::Ptr transformed_additional_map_ptr (new pcl::PointCloud<pcl::PointXYZI>());
  transformed_additional_map_ptr->header.frame_id = "/map";
  pcl::transformPointCloud(*additional_map_ptr, *transformed_additional_map_ptr, t);
  sensor_msgs::PointCloud2::Ptr msg_ptr(new sensor_msgs::PointCloud2);

  pcl::toROSMsg(*transformed_additional_map_ptr, *msg_ptr);
  msg_ptr->header.frame_id = "/map";
  ndt_map_pub.publish(*msg_ptr);


  pcl::io::savePCDFileBinary(output_pcd, *transformed_additional_map_ptr);
  std::cout << "Saved " << output_pcd << ": " << transformed_additional_map_ptr->points.size() << " points." << std::endl;

  pcl::PointCloud<pcl::PointXYZRGB> cloud_rgb;
  cloud_rgb.width = transformed_additional_map_ptr->width;
  cloud_rgb.height = transformed_additional_map_ptr->height;
  cloud_rgb.points.resize(cloud_rgb.width * cloud_rgb.height);

  for(size_t i = 0; i < cloud_rgb.points.size(); i++){
    cloud_rgb.points[i].x = transformed_additional_map_ptr->points[i].x;
    cloud_rgb.points[i].y = transformed_additional_map_ptr->points[i].y;
    cloud_rgb.points[i].z = transformed_additional_map_ptr->points[i].z;
    cloud_rgb.points[i].rgb = 255 << 16 | 255 << 8 | 255;
  }

  pcl::io::savePCDFileBinary(output_pcd_rgb, cloud_rgb);
  std::cout << "Saved " << output_pcd_rgb << ": " <<  cloud_rgb.points.size() << " points." << std::endl;

  std::cout << "-----------------------------------------------------------------" << std::endl;
  std::cout << "Sequence number: " << input->header.seq << std::endl;
  std::cout << "Number of scan points: " << additional_map_ptr->size() << " points." << std::endl;
  std::cout << "Number of filtered scan points: " << filtered_additional_map_ptr->size() << " points." << std::endl;
  std::cout << "NDT has converged: " << ndt.hasConverged() << std::endl;
  std::cout << "Fitness score: " << ndt.getFitnessScore() << std::endl;
  std::cout << "Number of iteration: " << ndt.getFinalNumIteration() << std::endl;
  std::cout << "Transformation Matrix:" << std::endl;
  std::cout << t << std::endl;
  std::cout << "-----------------------------------------------------------------" << std::endl;
}

static void map_callback(const sensor_msgs::PointCloud2::ConstPtr& input)
{
  if (hasMapSet == false) {
    pcl::fromROSMsg(*input, map);

    pcl::PointCloud<pcl::PointXYZI>::Ptr map_ptr(new pcl::PointCloud<pcl::PointXYZI>(map));

    ndt.setResolution(ndt_res);
    ndt.setInputTarget(map_ptr);

    hasMapSet = true;
    std::cout << "Points Map: " << map_ptr->size() << " points." << std::endl;
  }
}

int main(int argc, char **argv)
{
    if(argc != 2){
      std::cout << "Usage: rosrun map_tools map_extender \"input\"" << std::endl;
      exit(1);
    }

    ros::init(argc, argv, "map_extender");
    ros::NodeHandle n;

    input_pcd = output_pcd = output_pcd_rgb = argv[1];
    int tmp = input_pcd.find_last_of("/");
    std::string prefix = "extended_";
    std::string prefix_rgb = "extended_rgb_";
    output_pcd.insert(tmp+1, prefix);
    output_pcd_rgb.insert(tmp+1, prefix_rgb);
    std::cout << input_pcd << std::endl;
    std::cout << output_pcd << std::endl;
    std::cout << output_pcd_rgb << std::endl;

    if(pcl::io::loadPCDFile<pcl::PointXYZI> (input_pcd, *additional_map_ptr) == -1){
      std::cout << "Couldn't read " << input_pcd << "." << std::endl;
      return(-1);
    }
    std::cout << input_pcd << ": " << additional_map_ptr->size() << " points." << std::endl;

    pcl::VoxelGrid<pcl::PointXYZI> voxel_grid_filter;
    voxel_grid_filter.setLeafSize(voxel_leaf_size, voxel_leaf_size, voxel_leaf_size);
    voxel_grid_filter.setInputCloud(additional_map_ptr);
    voxel_grid_filter.filter(*filtered_additional_map_ptr);

    ndt.setInputSource(filtered_additional_map_ptr);
    ndt.setMaximumIterations(iter);
    ndt.setStepSize(step_size);
    ndt.setTransformationEpsilon(trans_eps);

    ndt_map_pub = n.advertise<sensor_msgs::PointCloud2>("/extended_map", 10, true);

    ros::Subscriber map_sub = n.subscribe("points_map", 10, map_callback);
    ros::Subscriber initialpose_sub = n.subscribe("initialpose", 10, initialpose_callback);

    ros::spin();

    return 0;
}
