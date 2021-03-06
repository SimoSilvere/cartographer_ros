/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "cartographer/common/configuration_file_resolver.h"
#include "cartographer/common/math.h"
#include "cartographer/io/file_writer.h"
#include "cartographer/io/points_processor.h"
#include "cartographer/io/points_processor_pipeline_builder.h"
#include "cartographer/io/proto_stream.h"
#include "cartographer/mapping/proto/pose_graph.pb.h"
#include "cartographer/sensor/point_cloud.h"
#include "cartographer/sensor/range_data.h"
#include "cartographer/transform/transform_interpolation_buffer.h"
#include "cartographer_ros/msg_conversion.h"
#include "cartographer_ros/ros_map_writing_points_processor.h"
#include "cartographer_ros/split_string.h"
#include "cartographer_ros/time_conversion.h"
#include "cartographer_ros/urdf_reader.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "ros/ros.h"
#include "ros/time.h"
#include "rosbag/bag.h"
#include "rosbag/view.h"
#include "tf2_eigen/tf2_eigen.h"
#include "tf2_msgs/TFMessage.h"
#include "tf2_ros/buffer.h"
#include "urdf/model.h"
#include "pcl/point_cloud.h"
#include "pcl/point_types.h"
#include "pcl_conversions/pcl_conversions.h"
#include <fstream>   //file writing
//#include "cartographer/io/color.h"

DEFINE_string(configuration_directory, "",
              "First directory in which configuration files are searched, "
              "second is always the Cartographer installation to allow "
              "including files from there.");
DEFINE_string(configuration_basename, "",
              "Basename, i.e. not containing any directory prefix, of the "
              "configuration file.");
DEFINE_string(
    urdf_filename, "",
    "URDF file that contains static links for your sensor configuration.");
DEFINE_string(bag_filenames, "",
              "Bags to process, must be in the same order as the trajectories "
              "in 'pose_graph_filename'.");
DEFINE_string(pose_graph_filename, "",
              "Proto stream file containing the pose graph.");
DEFINE_bool(use_bag_transforms, true,
            "Whether to read and use the transforms from the bag.");
DEFINE_string(output_file_prefix, "",
              "Will be prefixed to all output file names and can be used to "
              "define the output directory. If empty, the first bag filename "
              "will be used.");

//std::string trajectoryName = "trajectory.txt";

namespace cartographer_ros {
namespace {

constexpr char kTfStaticTopic[] = "/tf_static";
namespace carto = ::cartographer;


void WriteCustomBinaryPlyHeader(int num_points, std::ofstream& file_writer) {
  std::ostringstream stream;
  stream << "ply\n"
         << "format binary_little_endian 1.0\n"
         << "comment Point cloud hypnotized and smoothly lured from the basket by the almighty snake charmer: Kamikaze Viper\n"
         << "element vertex " << std::setw(15) << std::setfill('0')
         << num_points << "\n"
         << "property float x\n"
         << "property float y\n"
         << "property float z\n"
         << "property float x_rot\n"
         << "property float y_rot\n"
         << "property float z_rot\n"
         << "property double time\n"
         << "end_header\n";
  std::string out = stream.str();
  file_writer.write((char*)out.data(), out.size());
}
void write_one_node_ply(carto::transform::Rigid3d& pose, double& time, std::ofstream& file_writer){
	//get xyz and rotations
	float x,y,z,x_rot,y_rot,z_rot;
	
	x = float(pose.translation().x());
	y = float(pose.translation().y());
	z = float(pose.translation().z());
	auto euler = pose.rotation().toRotationMatrix().eulerAngles(0, 1, 2);
	x_rot = float(euler[0]);
	y_rot = float(euler[1]);
	z_rot = float(euler[2]);

	//time = carto::common::TicksToUnixSeconds(node.timestamp());
	//time = FromRos(node.timestamp())

	//write trajectory node to file
	char buffer[32];
	memcpy(buffer, &x, sizeof(float));
	memcpy(buffer + 4, &y, sizeof(float));
	memcpy(buffer + 8, &z, sizeof(float));
	memcpy(buffer + 12, &x_rot, sizeof(float));
	memcpy(buffer + 16, &y_rot, sizeof(float));
	memcpy(buffer + 20, &z_rot, sizeof(float));
	memcpy(buffer + 24, &time, sizeof(double));
	//write to file
	file_writer.write(buffer,32);
}
void write_lidar_poses_to_ply(std::vector<::cartographer::mapping::proto::Trajectory>& all_trajectories, std::string& trajectoryFileName, std::string&  trajectoryLidarFileName, std::string& tracking_frame, tf2_ros::Buffer& tf_buffer){
	
	std::string lidar_frame = std::string("horizontal_vlp16_link");
	
	//init transfomation interpolation buffer
	std::vector<carto::transform::TransformInterpolationBuffer> transform_interpolation_buffers;
	int num_trajectory_points=0;
  	for (size_t i = 0; i < all_trajectories.size(); ++i) {
		num_trajectory_points = num_trajectory_points + all_trajectories[i].node_size();
		transform_interpolation_buffers.push_back(carto::transform::TransformInterpolationBuffer(all_trajectories[i]));
  	}
	
	//create file writers
	//tracking frame
	std::ofstream file_writer_tracking;
  	file_writer_tracking.open(trajectoryFileName, std::ios::out | std::ios::binary);
	//lidar
	std::ofstream file_writer_lidar;
  	file_writer_lidar.open(trajectoryLidarFileName, std::ios::out | std::ios::binary);
	
	//create header 
	WriteCustomBinaryPlyHeader(num_trajectory_points, file_writer_tracking);
	WriteCustomBinaryPlyHeader(num_trajectory_points, file_writer_lidar);
	
	//go through all trajectories
	for (size_t i = 0; i < all_trajectories.size(); ++i) {
		if (all_trajectories[i].node_size() == 0) {
    			continue;
  		}	
		
		//go through all nodes in trajectory
		for (const carto::mapping::proto::Trajectory::Node& node : all_trajectories[i].node()) {
			
			//get time stamp
			double time = carto::common::TicksToUnixSeconds(node.timestamp());
			if (!transform_interpolation_buffers[i].Has(carto::common::FromUniversal(node.timestamp()))) {
			      continue;
			}
			//get transform
			carto::transform::Rigid3d tracking_pose = carto::transform::ToRigid3(node.pose());
			const carto::transform::Rigid3d tracking_to_map = transform_interpolation_buffers[i].Lookup(carto::common::FromUniversal(node.timestamp()));
			const carto::transform::Rigid3d sensor_to_tracking = ToRigid3d(tf_buffer.lookupTransform(tracking_frame, lidar_frame, ToRos(carto::common::FromUniversal(node.timestamp()))));
    			carto::transform::Rigid3d sensor_to_map = (tracking_to_map * sensor_to_tracking);
			
			//write node to file 
			write_one_node_ply(tracking_pose, time, file_writer_tracking);
			write_one_node_ply(sensor_to_map, time, file_writer_lidar);
		}
	}
	//close file
  	file_writer_tracking.close();
	file_writer_lidar.close();
}
void WriteCustomBinaryPlyTrajectory(carto::mapping::proto::Trajectory& trajectory, std::ofstream& file_writer) {

  if (trajectory.node_size() == 0) {
    return;
  }

  for (const carto::mapping::proto::Trajectory::Node& node : trajectory.node()) {
	//get transform
	carto::transform::Rigid3d transform = carto::transform::ToRigid3(node.pose());
	//get xyz and rotations
	float x,y,z,x_rot,y_rot,z_rot;
	double time;
	
	x = float(transform.translation().x());
	y = float(transform.translation().y());
	z = float(transform.translation().z());
	auto euler = transform.rotation().toRotationMatrix().eulerAngles(0, 1, 2);
	x_rot = float(euler[0]);
	y_rot = float(euler[1]);
	z_rot = float(euler[2]);

	time = carto::common::TicksToUnixSeconds(node.timestamp());
	//time = FromRos(node.timestamp())

	//write trajectory node to file
	char buffer[32];
	memcpy(buffer, &x, sizeof(float));
	memcpy(buffer + 4, &y, sizeof(float));
	memcpy(buffer + 8, &z, sizeof(float));
	memcpy(buffer + 12, &x_rot, sizeof(float));
	memcpy(buffer + 16, &y_rot, sizeof(float));
	memcpy(buffer + 20, &z_rot, sizeof(float));
	memcpy(buffer + 24, &time, sizeof(double));
	//write to file
	file_writer.write(buffer,32);
  }
}



void Run(const std::string& pose_graph_filename, const std::string& urdf_filename, std::string& tracking_frame) {

 carto::mapping::proto::PoseGraph pose_graph_proto = carto::io::DeserializePoseGraphFromFile(pose_graph_filename);
  
 //ros transforms buffer
 tf2_ros::Buffer tf_buffer;
 if (!urdf_filename.empty()) {
	ReadStaticTransformsFromUrdf(urdf_filename, &tf_buffer);
 }

  // This vector must outlive the pipeline.
  std::vector<::cartographer::mapping::proto::Trajectory> all_trajectories(
      pose_graph_proto.trajectory().begin(),
      pose_graph_proto.trajectory().end());
  
  //how many points is going to be written
  int num_trajectory_points=0;
  for (size_t i = 0; i < all_trajectories.size(); ++i) {
	num_trajectory_points = num_trajectory_points + all_trajectories[i].node_size();
  }
  /*//file_writer
  std::string trajectoryFileName = pose_graph_filename + "_traj.ply";
  std::ofstream file_writer;
  file_writer.open(trajectoryFileName, std::ios::out | std::ios::binary);
  //file_writer.write( (char*)&my_double, sizeof(double));
  
  
  //write header
  WriteCustomBinaryPlyHeader(num_trajectory_points, file_writer);
  
  //write all data from all trajectories
  for (size_t i = 0; i < all_trajectories.size(); ++i) {
	WriteCustomBinaryPlyTrajectory(all_trajectories[i], file_writer);
  }

  //close file
  file_writer.close();
  */
  
  std::string trajectoryFileName = pose_graph_filename + "_traj.ply";
  std::string trajectoryLidarFileName = pose_graph_filename + "_lidar_traj.ply";
  write_lidar_poses_to_ply(all_trajectories, trajectoryFileName, trajectoryLidarFileName, tracking_frame, tf_buffer);
}

}  // namespace
}  // namespace cartographer_ros

std::unique_ptr<cartographer::common::LuaParameterDictionary> LoadLuaDictionary(
    const std::string& configuration_directory,
    const std::string& configuration_basename) {
  auto file_resolver =
      absl::make_unique<cartographer::common::ConfigurationFileResolver>(
          std::vector<std::string>{configuration_directory});

  const std::string code =
      file_resolver->GetFileContentOrDie(configuration_basename);
  auto lua_parameter_dictionary =
      absl::make_unique<cartographer::common::LuaParameterDictionary>(
          code, std::move(file_resolver));
  return lua_parameter_dictionary;
}

int main(int argc, char** argv) {
  FLAGS_alsologtostderr = true;
  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, true);

  CHECK(!FLAGS_pose_graph_filename.empty())
      << "-pose_graph_filename is missing.";
  CHECK(!FLAGS_urdf_filename.empty())
      << "-urdf_filename is missing.";
  CHECK(!FLAGS_configuration_directory.empty())
      << "-configuration_directory is missing.";
  CHECK(!FLAGS_configuration_basename.empty())
      << "-configuration_basename is missing.";

  //options
  const auto lua_parameter_dictionary = LoadLuaDictionary(FLAGS_configuration_directory, FLAGS_configuration_basename);
  std::string tracking_frame =  lua_parameter_dictionary->GetString("tracking_frame");
  double moved_away_threshold_meters = lua_parameter_dictionary->GetDouble("moved_away_threshold_meters");
  lua_parameter_dictionary->GetDictionary("pipeline").get();

  ::cartographer_ros::Run(FLAGS_pose_graph_filename, FLAGS_urdf_filename, tracking_frame);
}
