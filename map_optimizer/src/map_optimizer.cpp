#include "map_optimizer.h"

#include <iostream>
#include <fstream>
#include <string>
#include <stdlib.h>
#include <time.h>

#include "ros/ros.h"
#include "ros/console.h"

#include "tf/transform_listener.h"
#include "tf/transform_broadcaster.h"

#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>

#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/point_cloud_conversion.h>

#include <pcl/io/io.h>
#include <pcl/io/pcd_io.h>
#include <pcl/common/impl/common.hpp>
#include <pcl/io/vtk_lib_io.h>

#include <pcl_ros/point_cloud.h>

#include <Eigen/Core>

#include <boost/optional.hpp>
#include <boost/foreach.hpp>
#define foreach BOOST_FOREACH

using pcl_t = pcl::PointXYZ;
MapOptimizer::MapOptimizer() :
		totle_size_(0), node_(NULL), target_pose_index_(-1) {
	sel_mode = map_optimizer_msg::SelectMode::SEL_MODE_TARGET;
	init();
	tf::Transform t;
	makeSE2Marker(t);
	initMenu(t);
	server->applyChanges();

}

MapOptimizer::~MapOptimizer() {
	//free node_ buffer;
	delete node_;
}

void MapOptimizer::init() {
	if (!private_nh_.getParam(ros::this_node::getName() + "/mapdata_folder",
			mapdata_folder_))
		mapdata_folder_ = "";
	if (!private_nh_.getParam("baseFrame", baseFrame_))
		baseFrame_ = "map";

	poses_pub = private_nh_.advertise<visualization_msgs::MarkerArray>(
			"/map_optimizer/trajectory", 1, true); //publish latched trajectory
	selected_scans_pub = private_nh_.advertise<sensor_msgs::PointCloud>(
			"/map_optimizer/select_scans", 1, true);
	target_scan_pub = private_nh_.advertise<sensor_msgs::PointCloud>(
			"/map_optimizer/target_scans", 1, true);
	odom_select_sub =
			private_nh_.subscribe<map_optimizer_msg::OdomSelectList>(
					"/map_optimizer/odom_selected", 1,
					[this] (const map_optimizer_msg::OdomSelectList::ConstPtr& msg) {handleOdomSelect(msg);});

	loadMapData();
	publishPoses();

	server.reset( new interactive_markers::InteractiveMarkerServer("map_optmization_controls","",false) );

	//std::vector<int> ids = {0, 1, 2, 3};
	//showLaserScans(ids);
}

void MapOptimizer::handleSelectionMode(
		const map_optimizer_msg::SelectMode::ConstPtr& msg) {
	if (msg)
		sel_mode = msg->sel_mode;
}

void MapOptimizer::handleOdomSelect(
		const map_optimizer_msg::OdomSelectList::ConstPtr& msg) {
	std::vector<int> ids;
	if (sel_mode == map_optimizer_msg::SelectMode::SEL_MODE_REF)
	{
		for (int i = 0; i < msg->id_list.size(); ++i) {
				ids.push_back(std::stoi(msg->id_list[i]));
		}
		auto msg_out = createLaserScansMsg(ids);
		selected_scans_pub.publish(msg_out);
	}
	else if (sel_mode == map_optimizer_msg::SelectMode::SEL_MODE_TARGET && msg->id_list.size() > 0) {
		//generate and publish pcd
		int ind = std::stoi(msg->id_list[0]);
		ids.push_back(ind);

		auto msg_out = createLaserScansMsg(ids);
		target_scan_pub.publish(msg_out);
		//show 3d handler
		auto& n = node_map_[ids[0]];
		auto t = tf::Transform(tf::createQuaternionFromRPY(0, 0, n->pose.theta),
						tf::Point(n->pose.x, n->pose.y, 0));
		// update 3d handler marker pose
		geometry_msgs::Pose pose;
		tf::poseTFToMsg(t, pose);
		server->setPose( se2_marker_name, pose );
		server->applyChanges();
		// set global selected target pose index;
		target_pose_index_ = ind;

	}

}

void MapOptimizer::updateTargetLaserPose(const tf::Transform& t, int ind)
{
	sensor_msgs::PointCloudPtr msg_out = boost::make_shared< sensor_msgs::PointCloud>();
	msg_out->header.stamp = ros::Time::now();
	msg_out->header.frame_id = baseFrame_;

	if (node_map_.find(ind) == node_map_.end()) return;
//		auto& n = node_map_[ind];
//		auto A = tf::Transform(tf::createQuaternionFromRPY(0, 0, n->pose.theta),
//				tf::Point(n->pose.x, n->pose.y, 0));
	auto& pc = scans_buf_[ind];

	msg_out->points.reserve(pc->points.size());
	for (int j = 0; j < pc->points.size(); ++j) {
				tf::Vector3 tmp(pc->points[j].x, pc->points[j].y, pc->points[j].z);
				auto v = t(tmp);

				msg_out->points.emplace_back();
				auto& p_out = msg_out->points.back();
				p_out.x = float(v.x());
				p_out.y = float(v.y());
				p_out.z = float(v.z());
	//			ROS_INFO("point[%f, %f]", p_out.x, p_out.y);
			}
	target_scan_pub.publish(msg_out);
}

void MapOptimizer::initMenu(const tf::Transform& t)
{
	auto entry = menu_handler_.insert( "SelectMode" );
	h_mode_ref_laser_ = menu_handler_.insert(entry, "Ref_Laser",
			[this] (const visualization_msgs::InteractiveMarkerFeedbackConstPtr &feedback) {menuModeCB(feedback);});
	h_mode_target_laser_ = menu_handler_.insert(entry, "Target_Laser",
			[this] (const visualization_msgs::InteractiveMarkerFeedbackConstPtr &feedback) {menuModeCB(feedback);} );
	menu_handler_.setCheckState( h_mode_ref_laser_, interactive_markers::MenuHandler::CHECKED );
	menu_handler_.setCheckState( h_mode_target_laser_, interactive_markers::MenuHandler::UNCHECKED );
	h_mode_last_ = h_mode_ref_laser_;
	menu_handler_.apply(*server, se2_marker_name);
//	server->applyChanges();
}
void MapOptimizer::menuModeCB(const visualization_msgs::InteractiveMarkerFeedbackConstPtr &feedback)
{
	auto handle = feedback->menu_entry_id;
	interactive_markers::MenuHandler::CheckState state;
	menu_handler_.getCheckState( handle, state );
	ROS_INFO("handler: %d, ref: %d, target: %d\n", handle, h_mode_ref_laser_, h_mode_target_laser_);
	if(handle == h_mode_ref_laser_)
	{
		menu_handler_.setCheckState( h_mode_last_, interactive_markers::MenuHandler::UNCHECKED );
		sel_mode = map_optimizer_msg::SelectMode::SEL_MODE_REF;
		menu_handler_.setCheckState( h_mode_ref_laser_, interactive_markers::MenuHandler::CHECKED );
		h_mode_last_ = h_mode_ref_laser_;
	}else if(handle == h_mode_target_laser_)
	{
		menu_handler_.setCheckState( h_mode_last_, interactive_markers::MenuHandler::UNCHECKED );
		sel_mode = map_optimizer_msg::SelectMode::SEL_MODE_TARGET;
		menu_handler_.setCheckState( h_mode_target_laser_, interactive_markers::MenuHandler::CHECKED );
		h_mode_last_ = h_mode_target_laser_;
	}
	menu_handler_.reApply( *server );
	server->applyChanges();
}

void MapOptimizer::menuRefineCB(const visualization_msgs::InteractiveMarkerFeedbackConstPtr &feedback)
{

}

void MapOptimizer::menuGenMapCB(const visualization_msgs::InteractiveMarkerFeedbackConstPtr &feedback)
{

}

void MapOptimizer::processMarkerFeedback( const visualization_msgs::InteractiveMarkerFeedbackConstPtr &feedback )
{
	std::ostringstream s;
	  s << "Feedback from marker '" << feedback->marker_name << "' "
	      << " / control '" << feedback->control_name << "'";

	  std::ostringstream mouse_point_ss;
	  tf::Transform newT;
	  if( feedback->mouse_point_valid )
	  {
	    mouse_point_ss << " at " << feedback->mouse_point.x
	                   << ", " << feedback->mouse_point.y
	                   << ", " << feedback->mouse_point.z
	                   << " in frame " << feedback->header.frame_id;
	  }

	  switch ( feedback->event_type )
	  {
	    case visualization_msgs::InteractiveMarkerFeedback::BUTTON_CLICK:
	      ROS_INFO_STREAM( s.str() << ": button click" << mouse_point_ss.str() << "." );
	      break;

	    case visualization_msgs::InteractiveMarkerFeedback::MENU_SELECT:
	      ROS_INFO_STREAM( s.str() << ": menu item " << feedback->menu_entry_id << " clicked" << mouse_point_ss.str() << "." );
	      break;

	    case visualization_msgs::InteractiveMarkerFeedback::POSE_UPDATE:
	      ROS_INFO_STREAM( s.str() << ": pose changed"
	          << "\nposition = "
	          << feedback->pose.position.x
	          << ", " << feedback->pose.position.y
	          << ", " << feedback->pose.position.z
	          << "\norientation = "
	          << feedback->pose.orientation.w
	          << ", " << feedback->pose.orientation.x
	          << ", " << feedback->pose.orientation.y
	          << ", " << feedback->pose.orientation.z
	          << "\nframe: " << feedback->header.frame_id
	          << " time: " << feedback->header.stamp.sec << "sec, "
	          << feedback->header.stamp.nsec << " nsec" );

	      tf::poseMsgToTF(feedback->pose, newT);
	      updateTargetLaserPose(newT, target_pose_index_);
	      break;

	    case visualization_msgs::InteractiveMarkerFeedback::MOUSE_DOWN:
	      ROS_INFO_STREAM( s.str() << ": mouse down" << mouse_point_ss.str() << "." );
	      break;

	    case visualization_msgs::InteractiveMarkerFeedback::MOUSE_UP:
	      ROS_INFO_STREAM( s.str() << ": mouse up" << mouse_point_ss.str() << "." );
	      break;
	  }

	  server->applyChanges();
}

void MapOptimizer::loadMapData() {
	if (mapdata_folder_.empty()) {
		ROS_INFO("Empty map data folder!");
		return;
	}
	std::string pose_file(mapdata_folder_ + "/pose.csv");

	ROS_INFO("MapOptimizer::loadMapData: %s", pose_file.c_str());
	std::ifstream pose_stream(pose_file.c_str());

	if (!pose_stream.good()) {
		ROS_ERROR("Fail to load pose file: %s", pose_file.c_str());
		return;
	}

	int num_poses = 0;
	//pose_stream >> num_poses;
	scans_buf_.clear();
	//scan_ids.reserve(num_poses);
	TNode* parent = NULL;
	std::string line;
	totle_size_ = 0;
	while (std::getline(pose_stream, line))
	//for(int i = 0; i < num_poses; ++i)
	{
		int id;
		double bag_time;
		double weight, gweight;
		GMapping::OrientedPoint pose;

		std::stringstream stream(line);
		std::string s;
		std::getline(stream, s, ',');
		id = std::stoi(s);
		std::getline(stream, s, ',');
		bag_time = std::stof(s);

		std::getline(stream, s, ',');
		weight = std::stof(s);

		std::getline(stream, s, ',');
		gweight = std::stof(s);

		std::getline(stream, s, ',');
		pose.x = std::stof(s);

		std::getline(stream, s, ',');
		pose.y = std::stof(s);

		std::getline(stream, s, ',');
		pose.theta = std::stof(s);	// - 55 * M_PI / 180;

		//pose_stream >> id >> bag_time >> weight >> gweight >> pose.x >> pose.y >> pose.theta;
		ROS_INFO_STREAM(
				"data: "<< id <<", " << bag_time<<", " << weight <<", " << gweight <<", [" << pose.x <<", " << pose.y <<", " << pose.theta<<"]");
		TNode* node = new TNode(pose, weight, parent);
		//load scan for this node
		std::string scan_file(
				mapdata_folder_ + "/" + std::to_string(id) + ".pcd");
		auto scanPtr = loadLaserScan(scan_file);
		scans_buf_[id] = scanPtr;
		node_map_[id] = node;
		totle_size_++;
		parent = node;	//TODO: reverse the list.
	}
	//reserve the node list
//	auto p0 = parent, p1 = parent;
//	for(GMapping::GridSlamProcessor::TNode* n = parent;  n;  n = n->parent)
//	{
//		if(n==parent) continue;
//		if(p0 == parent) p0->parent = 0;
//		else p0->parent = p1;
//
//		p1 = p0;
//		p0 = n;
//	}
//	parent = p0;

	if (node_)
		delete node_;
	node_ = parent;
	pose_stream.close();
}

sensor_msgs::PointCloudPtr MapOptimizer::loadLaserScan(
		const std::string& scan_file) {

	std::ifstream scan_stream(scan_file.c_str());
	if (!scan_stream.good()) {
		ROS_ERROR("Fail to load laser scan file: %s", scan_file.c_str());
		return 0;
	}
	auto scan = boost::make_shared<sensor_msgs::PointCloud>();

	pcl::PointCloud<pcl_t>::Ptr cloud(new pcl::PointCloud<pcl_t>);
	pcl::io::loadPCDFile(scan_file.c_str(), *cloud);
	sensor_msgs::PointCloud2 pc2;
	pcl::toROSMsg(*cloud, pc2);
	sensor_msgs::convertPointCloud2ToPointCloud(pc2, *scan);
//	ROS_INFO_STREAM("laser scan loaded: "<<*scan);
	return scan;
}




sensor_msgs::PointCloudPtr MapOptimizer::createLaserScansMsg(
		const std::vector<int>& ids) {
	sensor_msgs::PointCloudPtr msg_out = boost::make_shared< sensor_msgs::PointCloud>();
	msg_out->header.stamp = ros::Time::now();
	msg_out->header.frame_id = baseFrame_;
	for (int i = 0; i < ids.size(); ++i) {
		if (node_map_.find(i) == node_map_.end())
			continue;
		auto& n = node_map_[ids[i]];
		auto A = tf::Transform(tf::createQuaternionFromRPY(0, 0, n->pose.theta),
				tf::Point(n->pose.x, n->pose.y, 0));
		auto& pc = scans_buf_[ids[i]];
		//sensor_msgs::PointCloudPtr pc = boost::make_shared< sensor_msgs::PointCloud >();
		ROS_INFO_STREAM("pc id: "<< ids[i]<<" size: "<<pc->points.size());
		//projector_.projectLaser(*scan_, *pc);
		for (int j = 0; j < pc->points.size(); ++j) {
			tf::Vector3 tmp(pc->points[j].x, pc->points[j].y, pc->points[j].z);
			auto v = A(tmp);

			msg_out->points.emplace_back();
			auto& p_out = msg_out->points.back();
			p_out.x = float(v.x());
			p_out.y = float(v.y());
			p_out.z = float(v.z());
//			ROS_INFO("point[%f, %f]", p_out.x, p_out.y);
		}
	}
	ROS_INFO_STREAM("Final size: " << msg_out->points.size());
	return msg_out;
}

void MapOptimizer::publishPoses() {
	ROS_INFO("%s: create poses marker array", __FUNCTION__);
	visualization_msgs::MarkerArray posesMsg;
	ros::Time time = ros::Time::now();
	int id = totle_size_;
	boost::optional<Eigen::Vector2f> last_position;
	for (GMapping::GridSlamProcessor::TNode* n = node_; n; n = n->parent) {
//	    ROS_INFO("  %.3f %.3f %.3f",
//	              n->pose.x,
//	              n->pose.y,
//	              n->pose.theta);
		/*if(!n->reading)
		 {
		 ROS_DEBUG("Reading is NULL");
		 continue;
		 }*/
		auto v = Eigen::Vector2f(n->pose.x, n->pose.y);
		double dist = 0.5;
		if (last_position)
			dist = (v - *last_position).norm();

		last_position = v;

		visualization_msgs::Marker marker;
		marker.header.frame_id = baseFrame_;
		marker.header.stamp = time;
		marker.ns = "map_optimizer";
		marker.id = --id;
//		ROS_INFO_STREAM("id: " << marker.id);
		//marker.id = std::string("pos_") + itoa(id);
		marker.type = visualization_msgs::Marker::ARROW;
		marker.action = visualization_msgs::Marker::ADD;
		marker.scale.x = dist;
		marker.scale.y = 0.5;
		marker.scale.z = 0.5;
		marker.pose.position.x = n->pose.x;
		marker.pose.position.y = n->pose.y;
		marker.pose.position.z = 0.0;
		tf::Quaternion q = tf::createQuaternionFromRPY(0, 0, n->pose.theta);
		marker.pose.orientation.x = q.x();
		marker.pose.orientation.y = q.y();
		marker.pose.orientation.z = q.z();
		marker.pose.orientation.w = q.w();
		marker.color.r = 0.0f;
		marker.color.g = 1.0f;
		marker.color.b = 0.0f;
		marker.color.a = 0.5;
		marker.lifetime = ros::Duration();
		posesMsg.markers.push_back(marker);
	}
	ROS_INFO_STREAM("posesMsg.markers size: " << posesMsg.markers.size());
	poses_pub.publish(posesMsg);

}

void MapOptimizer::makeSE2Marker(const tf::Transform& t)
{
	  visualization_msgs::InteractiveMarker int_marker;
	  int_marker.header.frame_id = baseFrame_;
	  tf::poseTFToMsg(t, int_marker.pose);
//	  int_marker.pose.orientation = t.getRotation();
	  int_marker.scale = 10;
	  se2_marker_name = "se2_controller";
	  int_marker.name = se2_marker_name;
	  int_marker.description = "SE2 interactive controller";

	  // make a box
	  visualization_msgs::Marker marker;
	  marker.type = visualization_msgs::Marker::CUBE;
	  marker.scale.x = int_marker.scale * 0.45;
	  marker.scale.y = int_marker.scale * 0.45;
	  marker.scale.z = int_marker.scale * 0.45;
	  marker.color.r = 0.5;
	  marker.color.g = 0.5;
	  marker.color.b = 0.5;
	  marker.color.a = 1.0;
	  // insert a box
	  visualization_msgs::InteractiveMarkerControl control_box;
	  control_box.always_visible = true;
//	  control_box.interaction_mode = visualization_msgs::InteractiveMarkerControl::BUTTON;
	  control_box.markers.push_back( marker );
	  int_marker.controls.push_back( control_box );
	  int_marker.controls[0].interaction_mode = visualization_msgs::InteractiveMarkerControl::MOVE_ROTATE;

	  visualization_msgs::InteractiveMarkerControl control;

	  {
	    control.orientation.w = 1;
	    control.orientation.x = 1;
	    control.orientation.y = 0;
	    control.orientation.z = 0;
	    control.name = "move_x";
	    control.interaction_mode = visualization_msgs::InteractiveMarkerControl::MOVE_AXIS;
	    int_marker.controls.push_back(control);

	    control.orientation.w = 1;
	    control.orientation.x = 0;
	    control.orientation.y = 1;
	    control.orientation.z = 0;
	    control.name = "rotate_z";
	    control.interaction_mode = visualization_msgs::InteractiveMarkerControl::ROTATE_AXIS;
	    int_marker.controls.push_back(control);

	    control.orientation.w = 1;
	    control.orientation.x = 0;
	    control.orientation.y = 0;
	    control.orientation.z = 1;
	    control.name = "move_y";
	    control.interaction_mode = visualization_msgs::InteractiveMarkerControl::MOVE_AXIS;
	    int_marker.controls.push_back(control);
	  }

	  server->insert(int_marker);
	  server->setCallback(int_marker.name, [this] (const visualization_msgs::InteractiveMarkerFeedbackConstPtr &feedback) {processMarkerFeedback(feedback);});
}