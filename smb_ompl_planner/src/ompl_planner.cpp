/*
 * Copyright (c) 2020, Vision for Robotics Lab
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * * Neither the name of the Vision for Robotics Lab, ETH Zurich nor the
 * names of its contributors may be used to endorse or promote products
 * derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * ompl_planner.cpp
 * @brief Implementation of the logic of the planner
 * @author: Luca Bartolomei
 * Created on: March 11, 2020
 */

#include "smb_ompl_planner/ompl_planner.h"

#include <costmap_2d/cost_values.h>
#include <costmap_2d/costmap_2d.h>
#include <pluginlib/class_list_macros.h>

// register this planner as a BaseOmplPlanner plugin
PLUGINLIB_EXPORT_CLASS(smb_ompl_planner::OmplPlanner,
                       nav_core::BaseGlobalPlanner)

namespace smb_ompl_planner
{

OmplPlanner::OmplPlanner() : costmap_(NULL), initialized_(false) {}

OmplPlanner::OmplPlanner(std::string name, costmap_2d::Costmap2D* costmap,
                         std::string frame_id)
    : costmap_(NULL), initialized_(false)
{
  // initialize the planner
  initialize(name, costmap, frame_id);
}

OmplPlanner::~OmplPlanner() {}

void OmplPlanner::initialize(std::string name,
                             costmap_2d::Costmap2DROS* costmap_ros)
{
  initialize(name, costmap_ros->getCostmap(), costmap_ros->getGlobalFrameID());
}

void OmplPlanner::initialize(std::string name, costmap_2d::Costmap2D* costmap,
                             std::string frame_id)
{
  if (!initialized_)
  {
    // Set up ROS and parameters
    ros::NodeHandle private_nh("~/" + name);
    costmap_ = costmap;
    frame_id_ = frame_id;

    unsigned int cx = costmap->getSizeInCellsX(),
                 cy = costmap->getSizeInCellsY();

    private_nh.param("old_navfn_behavior", old_navfn_behavior_, false);
    if (!old_navfn_behavior_)
      convert_offset_ = 0.5;
    else
      convert_offset_ = 0.0;

    plan_pub_ = private_nh.advertise<nav_msgs::Path>("plan", 1);
    make_plan_srv_ = private_nh.advertiseService(
        "make_plan", &OmplPlanner::makePlanService, this);

    // Now set up the planner
    RrtParameters rrt_params;
    private_nh.param("use_distance_threshold",
                     rrt_params.use_distance_threshold, true);
    private_nh.param("simplify_solution", rrt_params.simplify_solution, true);
    private_nh.param("trust_approx_solution", rrt_params.trust_approx_solution,
                     false);
    private_nh.param("verbose_planner", rrt_params.verbose_planner, false);

    private_nh.param("distance_threshold", rrt_params.distance_threshold, 1.1);
    private_nh.param("goal_bias", rrt_params.goal_bias, 0.05);
    private_nh.param("robot_radius", rrt_params.robot_radius, 0.5);
    private_nh.param("num_seconds_to_plan", rrt_params.num_seconds_to_plan,
                     5.0);

    private_nh.param("default_tolerance", default_tolerance_, 0.0);

    int planner_type;
    private_nh.param("planner_type", planner_type, 1);

    ompl_planner_ = std::make_shared<smb_ompl_planner::RrtPlanner>(
        rrt_params, RrtPlannerType(planner_type));
    orientation_filter_ = std::make_shared<OrientationFilter>();

    // Done
    initialized_ = true;
  }
  else
  {
    ROS_WARN("[Ompl Planner] This planner has already been initialized, you "
             "can't call it twice, doing nothing");
  }
}

void OmplPlanner::clearRobotCell(const geometry_msgs::PoseStamped& global_pose,
                                 unsigned int mx, unsigned int my)
{
  if (!initialized_)
  {
    ROS_ERROR("[Ompl Planner] his planner has not been initialized yet, but it "
              "is being used, please call initialize() before use");
    return;
  }

  // set the associated costs in the cost map to be free
  costmap_->setCost(mx, my, costmap_2d::FREE_SPACE);
}

bool OmplPlanner::makePlanService(nav_msgs::GetPlan::Request& req,
                                  nav_msgs::GetPlan::Response& resp)
{
  makePlan(req.start, req.goal, resp.plan.poses);

  resp.plan.header.stamp = ros::Time::now();
  resp.plan.header.frame_id = frame_id_;

  return true;
}

void OmplPlanner::mapToWorld(double mx, double my, double& wx, double& wy)
{
  wx = costmap_->getOriginX() +
       (mx + convert_offset_) * costmap_->getResolution();
  wy = costmap_->getOriginY() +
       (my + convert_offset_) * costmap_->getResolution();
}

bool OmplPlanner::worldToMap(double wx, double wy, double& mx, double& my)
{
  double origin_x = costmap_->getOriginX(), origin_y = costmap_->getOriginY();
  double resolution = costmap_->getResolution();

  if (wx < origin_x || wy < origin_y)
    return false;

  mx = (wx - origin_x) / resolution - convert_offset_;
  my = (wy - origin_y) / resolution - convert_offset_;

  if (mx < costmap_->getSizeInCellsX() && my < costmap_->getSizeInCellsY())
    return true;

  return false;
}

bool OmplPlanner::makePlan(const geometry_msgs::PoseStamped& start,
                           const geometry_msgs::PoseStamped& goal,
                           std::vector<geometry_msgs::PoseStamped>& plan)
{
  return makePlan(start, goal, default_tolerance_, plan);
}

bool OmplPlanner::makePlan(const geometry_msgs::PoseStamped& start,
                           const geometry_msgs::PoseStamped& goal,
                           const double tolerance,
                           std::vector<geometry_msgs::PoseStamped>& plan)
{
  boost::mutex::scoped_lock lock(mutex_);
  if (!initialized_)
  {
    ROS_ERROR("This planner has not been initialized yet, but it is being "
              "used, please call initialize() before use");
    return false;
  }

  // clear the plan, just in case
  plan.clear();

  ros::NodeHandle n;
  std::string global_frame = frame_id_;

  // until tf can handle transforming things that are way in the past... we'll
  // require the goal to be in our global frame
  if (goal.header.frame_id != global_frame)
  {
    ROS_ERROR("The goal pose passed to this planner must be in the %s frame.  "
              "It is instead in the %s frame.",
              global_frame.c_str(), goal.header.frame_id.c_str());
    return false;
  }

  if (start.header.frame_id != global_frame)
  {
    ROS_ERROR("The start pose passed to this planner must be in the %s frame.  "
              "It is instead in the %s frame.",
              global_frame.c_str(), start.header.frame_id.c_str());
    return false;
  }

  double wx = start.pose.position.x;
  double wy = start.pose.position.y;

  unsigned int start_x_i, start_y_i, goal_x_i, goal_y_i;
  double start_x, start_y, goal_x, goal_y;

  if (!costmap_->worldToMap(wx, wy, start_x_i, start_y_i))
  {
    ROS_WARN(
        "The robot's start position is off the global costmap. Planning will "
        "always fail, are you sure the robot has been properly localized?");
    return false;
  }
  if (old_navfn_behavior_)
  {
    start_x = start_x_i;
    start_y = start_y_i;
  }
  else
  {
    worldToMap(wx, wy, start_x, start_y);
  }

  wx = goal.pose.position.x;
  wy = goal.pose.position.y;

  if (!costmap_->worldToMap(wx, wy, goal_x_i, goal_y_i))
  {
    ROS_WARN_THROTTLE(1.0,
                      "The goal sent to the global planner is off the global "
                      "costmap. Planning will always fail to this goal.");
    return false;
  }
  if (old_navfn_behavior_)
  {
    goal_x = goal_x_i;
    goal_y = goal_y_i;
  }
  else
  {
    worldToMap(wx, wy, goal_x, goal_y);
  }

  // clear the starting cell within the costmap because we know it can't be an
  // obstacle
  clearRobotCell(start, start_x_i, start_y_i);

  int nx = costmap_->getSizeInCellsX(), ny = costmap_->getSizeInCellsY();
  double costmap_size_x = costmap_->getSizeInMetersX();
  double costmap_size_y = costmap_->getSizeInMetersY();
  double cell_size =
      std::max(costmap_size_x / double(nx), costmap_size_y / double(ny));

  Eigen::Vector2d lower_bound, upper_bound;
  lower_bound << -costmap_size_x / 2.0, -costmap_size_y / 2.0;
  upper_bound << costmap_size_x / 2.0, costmap_size_y / 2.0;

  // Set up the problem first
  Eigen::Vector2d start_eigen(start.pose.position.x, start.pose.position.y);
  Eigen::Vector2d goal_eigen(goal.pose.position.x, goal.pose.position.y);
  ompl_planner_->setBounds(lower_bound, upper_bound);
  ompl_planner_->setVoxelSize(cell_size);
  ompl_planner_->setupProblem(start_eigen, goal_eigen);

  // Planning here!
  std::vector<Eigen::Vector2d> waypoints;
  bool success;
  try
  {
    success = ompl_planner_->getPathBetweenWaypoints(start_eigen, goal_eigen,
                                                     waypoints);
  }
  catch (ompl::Exception& ex)
  {
    ROS_ERROR("[Ompl Planner] Ompl exception: %s", ex.what());
    return false;
  }

  if (!success)
  {
    ROS_ERROR("[Ompl Planner] Planning failed");
    return false;
  }

  // Save the path in the right format
  ROS_INFO("[Ompl Planner] Planning successful");

  geometry_msgs::PoseStamped pose_path = start;
  plan.push_back(pose_path);
  for (auto waypoint : waypoints)
  {
    pose_path.pose.position.x = waypoint.x();
    pose_path.pose.position.y = waypoint.y();
    plan.push_back(pose_path);
  }

  // add orientations if needed
  orientation_filter_->processPath(start, plan);

  // publish the plan for visualization purposes
  publishPlan(plan);

  return true;
}

void OmplPlanner::publishPlan(
    const std::vector<geometry_msgs::PoseStamped>& path)
{
  if (!initialized_)
  {
    ROS_ERROR("[Ompl Planner] This planner has not been initialized yet, but "
              "it is being used, please call initialize() before use");
    return;
  }

  // create a message for the plan
  nav_msgs::Path gui_path;
  gui_path.poses.resize(path.size());

  gui_path.header.frame_id = frame_id_;
  gui_path.header.stamp = ros::Time::now();

  // Extract the plan in world co-ordinates, we assume the path is all in the
  // same frame
  for (unsigned int i = 0; i < path.size(); i++)
  {
    gui_path.poses[i] = path[i];
  }

  plan_pub_.publish(gui_path);
}

void OmplPlanner::outlineMap(unsigned char* costarr, int nx, int ny,
                             unsigned char value)
{
  unsigned char* pc = costarr;
  for (int i = 0; i < nx; i++)
    *pc++ = value;
  pc = costarr + (ny - 1) * nx;
  for (int i = 0; i < nx; i++)
    *pc++ = value;
  pc = costarr;
  for (int i = 0; i < ny; i++, pc += nx)
    *pc = value;
  pc = costarr + nx - 1;
  for (int i = 0; i < ny; i++, pc += nx)
    *pc = value;
}

} // end namespace smb_ompl_planner