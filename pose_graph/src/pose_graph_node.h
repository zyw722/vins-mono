// pose_graph_node.h
#include <vector>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include "sensor_msgs/NavSatFix.h"
#include <mavros_msgs/CommandBool.h>
// #include <mavros_msgs/State.h>
#include <mavros_msgs/ExtendedState.h>
#include <mavros_msgs/AttitudeTarget.h>
#include <mavros_msgs/PositionTarget.h>
#include <mavros_msgs/ActuatorControl.h>
#include <mavros_msgs/SetMode.h>
#include "common_msgs/drone_state.h"

#ifndef POSE_GRAPH_NODE.h
#define POSE_GRAPH_NODE.h

// 声明要访问的全局变量
extern common_msgs::drone_state drone_state_from_fcu;

// 声明要访问的函数
void gps_cb();

#endif // FILE1_H