#include <stdio.h>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <fstream>
#include <array>
#include <condition_variable>
#include <ros/ros.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>
#include "WGS84toCartesian.hpp"
#include "estimator.h"
#include "parameters.h"
#include "utility/visualization.h"
#include "sensor_msgs/NavSatFix.h"
#include <mavros_msgs/CommandBool.h>
// #include <mavros_msgs/State.h>
#include <mavros_msgs/ExtendedState.h>
#include <mavros_msgs/AttitudeTarget.h>
#include <mavros_msgs/PositionTarget.h>
#include <mavros_msgs/ActuatorControl.h>
#include <mavros_msgs/SetMode.h>
#include "common_msgs/drone_state.h"





Estimator estimator;

std::condition_variable con;
double current_time = -1;
queue<sensor_msgs::ImuConstPtr> imu_buf;
queue<sensor_msgs::PointCloudConstPtr> feature_buf;
queue<sensor_msgs::PointCloudConstPtr> relo_buf;
int sum_of_wait = 0;

std::mutex m_buf;
std::mutex m_state;
std::mutex i_buf;
std::mutex m_estimator;

double latest_time;
Eigen::Vector3d tmp_P;
Eigen::Quaterniond tmp_Q;
Eigen::Vector3d tmp_V;
Eigen::Vector3d tmp_Ba;
Eigen::Vector3d tmp_Bg;
Eigen::Vector3d acc_0;
Eigen::Vector3d gyr_0;
bool init_feature = 0;
bool init_imu = 1;
double last_imu_t = 0;
bool get_ref_gps_position = false;
bool get_ref_height = false;
common_msgs::drone_state drone_state_from_fcu;
common_msgs::drone_state drone_state_to_pub;
ros::Publisher pub_navsatfix;
int flag = 0;


void predict(const sensor_msgs::ImuConstPtr &imu_msg)//预测imu状态
{
     //获取当前时间
    double t = imu_msg->header.stamp.toSec();
        //首帧判断
    if (init_imu)
    {
        latest_time = t;
        init_imu = 0;
        return;
    }
         //获取dt并传递时间
    double dt = t - latest_time;
    latest_time = t;
    //获取当前时刻的IMU采样数据
    double dx = imu_msg->linear_acceleration.x;
    double dy = imu_msg->linear_acceleration.y;
    double dz = imu_msg->linear_acceleration.z;
    Eigen::Vector3d linear_acceleration{dx, dy, dz};//可以方便地将线性加速度的三个分量组合成一个向量

    double rx = imu_msg->angular_velocity.x;
    double ry = imu_msg->angular_velocity.y;
    double rz = imu_msg->angular_velocity.z;
    Eigen::Vector3d angular_velocity{rx, ry, rz};
     //注意，以下数据都是世界坐标系下的
    Eigen::Vector3d un_acc_0 = tmp_Q * (acc_0 - tmp_Ba) - estimator.g;

    Eigen::Vector3d un_gyr = 0.5 * (gyr_0 + angular_velocity) - tmp_Bg;
    tmp_Q = tmp_Q * Utility::deltaQ(un_gyr * dt);

    Eigen::Vector3d un_acc_1 = tmp_Q * (linear_acceleration - tmp_Ba) - estimator.g;

    Eigen::Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);

    tmp_P = tmp_P + dt * tmp_V + 0.5 * dt * dt * un_acc;
    tmp_V = tmp_V + dt * un_acc;

    acc_0 = linear_acceleration;
    gyr_0 = angular_velocity;
}

void update()
{
    TicToc t_predict;
    latest_time = current_time;
    tmp_P = estimator.Ps[WINDOW_SIZE];
    tmp_Q = estimator.Rs[WINDOW_SIZE];
    tmp_V = estimator.Vs[WINDOW_SIZE];
    tmp_Ba = estimator.Bas[WINDOW_SIZE];
    tmp_Bg = estimator.Bgs[WINDOW_SIZE];
    acc_0 = estimator.acc_0;
    gyr_0 = estimator.gyr_0;

    queue<sensor_msgs::ImuConstPtr> tmp_imu_buf = imu_buf;
    for (sensor_msgs::ImuConstPtr tmp_imu_msg; !tmp_imu_buf.empty(); tmp_imu_buf.pop())
        predict(tmp_imu_buf.front());

}

std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>>
getMeasurements()
{
    std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;

    while (true)
    {
        if (imu_buf.empty() || feature_buf.empty())
            return measurements;

        if (!(imu_buf.back()->header.stamp.toSec() > feature_buf.front()->header.stamp.toSec() + estimator.td))
        {
            //ROS_WARN("wait for imu, only should happen at the beginning");
            sum_of_wait++;
            return measurements;
        }

        if (!(imu_buf.front()->header.stamp.toSec() < feature_buf.front()->header.stamp.toSec() + estimator.td))
        {
            ROS_WARN("throw img, only should happen at the beginning");
            feature_buf.pop();
            continue;
        }
        sensor_msgs::PointCloudConstPtr img_msg = feature_buf.front();
        feature_buf.pop();

        std::vector<sensor_msgs::ImuConstPtr> IMUs;
        while (imu_buf.front()->header.stamp.toSec() < img_msg->header.stamp.toSec() + estimator.td)
        {
            IMUs.emplace_back(imu_buf.front());
            imu_buf.pop();
        }
        IMUs.emplace_back(imu_buf.front());
        if (IMUs.empty())
            ROS_WARN("no imu between two image");
        measurements.emplace_back(IMUs, img_msg);
    }
    return measurements;
}

void imu_callback(const sensor_msgs::ImuConstPtr &imu_msg)
{
    if (imu_msg->header.stamp.toSec() <= last_imu_t)
    {
        ROS_WARN("imu message in disorder!");
        return;
    }
    last_imu_t = imu_msg->header.stamp.toSec();

    m_buf.lock();
    imu_buf.push(imu_msg);
    m_buf.unlock();
    con.notify_one();

    last_imu_t = imu_msg->header.stamp.toSec();

    {
        std::lock_guard<std::mutex> lg(m_state);
        predict(imu_msg);
        std_msgs::Header header = imu_msg->header;
        header.frame_id = "world";
        if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
            pubLatestOdometry(tmp_P, tmp_Q, tmp_V, header);
    }
}


void feature_callback(const sensor_msgs::PointCloudConstPtr &feature_msg)
{
    if (!init_feature)
    {
        //skip the first detected feature, which doesn't contain optical flow speed
        init_feature = 1;
        return;
    }
    m_buf.lock();
    feature_buf.push(feature_msg);
    m_buf.unlock();
    con.notify_one();
}

void restart_callback(const std_msgs::BoolConstPtr &restart_msg)
{
    if (restart_msg->data == true)
    {
        ROS_WARN("restart the estimator!");
        m_buf.lock();
        while(!feature_buf.empty())
            feature_buf.pop();
        while(!imu_buf.empty())
            imu_buf.pop();
        m_buf.unlock();
        m_estimator.lock();
        estimator.clearState();
        estimator.setParameter();
        m_estimator.unlock();
        current_time = -1;
        last_imu_t = 0;
    }
    return;
}

void relocalization_callback(const sensor_msgs::PointCloudConstPtr &points_msg)
{
    //printf("relocalization callback! \n");
    m_buf.lock();
    relo_buf.push(points_msg);
    m_buf.unlock();
}

void logNavSatFix(const sensor_msgs::NavSatFix& msg) {
    // 打开日志文件以附加模式写入
    std::ofstream log_file("/home/zhangyuanwei/slamworks/vins-mono-catkin_ws/src/VINS-Mono/vins_estimator/navsatfix_log.txt", std::ios_base::app);
    if (log_file.is_open()) {
        // 写入经纬度和高度信息
        log_file << std::fixed << std::setprecision(8);
        log_file << "Timestamp: " << msg.header.stamp << "\n";
        log_file << "Latitude: " << msg.latitude << "\n";
        log_file << "Longitude: " << msg.longitude << "\n";
        log_file << "-----------------------------------\n";
        log_file.close();
    } else {
        ROS_ERROR("Unable to open log file for writing");
    }
}

// thread: visual-inertial odometry
//这样的代码通常用于在单独的线程中执行某个函数，以避免阻塞主线程。
//在这个例子中，measurement_process 线程将在后台执行 process 函数的内容。
//请确保在程序结束前等待线程的完成或进行适当的线程管理。
void process()
{
    while (true)
    {
        std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;
        std::unique_lock<std::mutex> lk(m_buf);
        con.wait(lk, [&]
                 {
            return (measurements = getMeasurements()).size() != 0;
                 });
        lk.unlock();
        m_estimator.lock();
        for (auto &measurement : measurements)//会迭代 measurements 向量中的每一个 std::pair 对象，并将其引用赋给 measurement，然后在循环体中使用 measurement 进行相应的操作。
        {
            auto img_msg = measurement.second;
            double dx = 0, dy = 0, dz = 0, rx = 0, ry = 0, rz = 0;
            for (auto &imu_msg : measurement.first)//会迭代 measurement.first 中的每一个 sensor_msgs::ImuConstPtr 对象，并将其引用赋给 imu_msg，然后在循环体中使用 imu_msg 进行相应的操作。
            {
                double t = imu_msg->header.stamp.toSec();
                double img_t = img_msg->header.stamp.toSec() + estimator.td;
                if (t <= img_t)
                { 
                    if (current_time < 0)
                        current_time = t;
                    double dt = t - current_time;
                    ROS_ASSERT(dt >= 0);
                    current_time = t;
                    dx = imu_msg->linear_acceleration.x;
                    dy = imu_msg->linear_acceleration.y;
                    dz = imu_msg->linear_acceleration.z;
                    rx = imu_msg->angular_velocity.x;
                    ry = imu_msg->angular_velocity.y;
                    rz = imu_msg->angular_velocity.z;
                    estimator.processIMU(dt, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
                    //printf("imu: dt:%f a: %f %f %f w: %f %f %f\n",dt, dx, dy, dz, rx, ry, rz);

                }
                else
                {
                    double dt_1 = img_t - current_time;
                    double dt_2 = t - img_t;
                    current_time = img_t;
                    ROS_ASSERT(dt_1 >= 0);
                    ROS_ASSERT(dt_2 >= 0);
                    ROS_ASSERT(dt_1 + dt_2 > 0);
                    double w1 = dt_2 / (dt_1 + dt_2);
                    double w2 = dt_1 / (dt_1 + dt_2);
                    dx = w1 * dx + w2 * imu_msg->linear_acceleration.x;
                    dy = w1 * dy + w2 * imu_msg->linear_acceleration.y;
                    dz = w1 * dz + w2 * imu_msg->linear_acceleration.z;
                    rx = w1 * rx + w2 * imu_msg->angular_velocity.x;
                    ry = w1 * ry + w2 * imu_msg->angular_velocity.y;
                    rz = w1 * rz + w2 * imu_msg->angular_velocity.z;
                    estimator.processIMU(dt_1, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
                    //printf("dimu: dt:%f a: %f %f %f w: %f %f %f\n",dt_1, dx, dy, dz, rx, ry, rz);
                }
            }
            // set relocalization frame
            sensor_msgs::PointCloudConstPtr relo_msg = NULL;
            while (!relo_buf.empty())
            {
                relo_msg = relo_buf.front();
                relo_buf.pop();
            }
            if (relo_msg != NULL)
            {
                vector<Vector3d> match_points;
                double frame_stamp = relo_msg->header.stamp.toSec();
                for (unsigned int i = 0; i < relo_msg->points.size(); i++)
                {
                    Vector3d u_v_id;
                    u_v_id.x() = relo_msg->points[i].x;
                    u_v_id.y() = relo_msg->points[i].y;
                    u_v_id.z() = relo_msg->points[i].z;
                    match_points.push_back(u_v_id);
                }
                Vector3d relo_t(relo_msg->channels[0].values[0], relo_msg->channels[0].values[1], relo_msg->channels[0].values[2]);
                Quaterniond relo_q(relo_msg->channels[0].values[3], relo_msg->channels[0].values[4], relo_msg->channels[0].values[5], relo_msg->channels[0].values[6]);
                Matrix3d relo_r = relo_q.toRotationMatrix();
                int frame_index;
                frame_index = relo_msg->channels[0].values[7];
                estimator.setReloFrame(frame_stamp, frame_index, match_points, relo_t, relo_r);
            }

            ROS_DEBUG("processing vision data with stamp %f \n", img_msg->header.stamp.toSec());

            TicToc t_s;
            map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> image;
            for (unsigned int i = 0; i < img_msg->points.size(); i++)
            {
                int v = img_msg->channels[0].values[i] + 0.5;
                int feature_id = v / NUM_OF_CAM;
                int camera_id = v % NUM_OF_CAM;
                double x = img_msg->points[i].x;
                double y = img_msg->points[i].y;
                double z = img_msg->points[i].z;
                double p_u = img_msg->channels[1].values[i];
                double p_v = img_msg->channels[2].values[i];
                double velocity_x = img_msg->channels[3].values[i];
                double velocity_y = img_msg->channels[4].values[i];
                ROS_ASSERT(z == 1);
                Eigen::Matrix<double, 7, 1> xyz_uv_velocity;
                xyz_uv_velocity << x, y, z, p_u, p_v, velocity_x, velocity_y;
                image[feature_id].emplace_back(camera_id,  xyz_uv_velocity);
            }
            estimator.processImage(image, img_msg->header);

            double whole_t = t_s.toc();
            printStatistics(estimator, whole_t);
            std_msgs::Header header = img_msg->header;
            header.frame_id = "world";
            
            //>>>>>>>>>>>>>>坐标转换和发布>>>>>>>>>>>>>>
            // std::array<double, 2> WGS84Reference{drone_state_from_fcu.gps_position.latitude, drone_state_from_fcu.gps_position.longitude};
            std::array<double, 2> WGS84Reference{38.8768504, 115.5059060};
            std::array<double, 2> CartesianPosition{
                estimator.Ps[WINDOW_SIZE].x(),
                estimator.Ps[WINDOW_SIZE].y()
            };
            std::array<double, 2> WGS84Position = wgs84::fromCartesian(WGS84Reference, CartesianPosition);      
            // 输出WGS84坐标
            // ROS_INFO("GPS Position: Latitude = %f, Longitude = %f, altitude = %f", WGS84Position[0], WGS84Position[1], estimator.Ps[WINDOW_SIZE].z());

            sensor_msgs::NavSatFix navsatfix_msg;
            navsatfix_msg.header = header;
            navsatfix_msg.header.frame_id = "world";
            navsatfix_msg.latitude = WGS84Position[0];
            navsatfix_msg.longitude = WGS84Position[1];
            navsatfix_msg.altitude = estimator.Ps[WINDOW_SIZE].z();
            pub_navsatfix.publish(navsatfix_msg);
            //将输出的经纬高写入日志
            logNavSatFix(navsatfix_msg);
            //<<<<<<<<<<<<<<坐标转换和发布<<<<<<<<<<<<<<


            pubOdometry(estimator, header);
            pubKeyPoses(estimator, header);
            pubCameraPose(estimator, header);
            pubPointCloud(estimator, header);
            pubTF(estimator, header);
            pubKeyframe(estimator);
            if (relo_msg != NULL)
                pubRelocalization(estimator);
            //ROS_ERROR("end: %f, at %f", img_msg->header.stamp.toSec(), ros::Time::now().toSec());
        }
        m_estimator.unlock();
        m_buf.lock();
        m_state.lock();
        if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
            update();
        m_state.unlock();
        m_buf.unlock();
    }
}

void gps_cb(const sensor_msgs::NavSatFix::ConstPtr &msg)
{
    drone_state_from_fcu.gps_position.latitude = msg->latitude;
    drone_state_from_fcu.gps_position.longitude = msg->longitude;
    drone_state_from_fcu.gps_position.altitude = msg->altitude;
    flag = 1;
    
    // std::array<double, 2> WGS84Reference{drone_state_from_fcu.gps_position.latitude, drone_state_from_fcu.gps_position.longitude};
    // // std::array<double, 2> WGS84Reference{0.000000, 0.000000};
    // std::array<double, 2> CartesianPosition{
    //     estimator.Ps[WINDOW_SIZE].x(),
    //     estimator.Ps[WINDOW_SIZE].y()
    // };
    // std::array<double, 2> WGS84Position = wgs84::fromCartesian(WGS84Reference, CartesianPosition);      
    // // 输出WGS84坐标
    // ROS_INFO("GPS Position: Latitude = %f, Longitude = %f, altitude = %f", WGS84Position[0], WGS84Position[1], estimator.Ps[WINDOW_SIZE].z());
    // std_msgs::Header header = header;
    // sensor_msgs::NavSatFix navsatfix_msg;
    // navsatfix_msg.header = header;
    // navsatfix_msg.header.frame_id = "world";
    // navsatfix_msg.latitude = WGS84Position[0];
    // navsatfix_msg.longitude = WGS84Position[1];
    // navsatfix_msg.altitude = estimator.Ps[WINDOW_SIZE].z();
    // pub_navsatfix.publish(navsatfix_msg);


}

// void pubNavsatfix(const Estimator &estimator, const std_msgs::Header &header)
// {
//     if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
//     {
//         // 将笛卡尔坐标转换为WGS84坐标
//         std::array<double, 2> WGS84Reference{drone_state_from_fcu.gps_position.latitude, drone_state_from_fcu.gps_position.longitude};
//         // std::array<double, 2> WGS84Reference{38.8770949, 115.5057705};
//         std::array<double, 2> CartesianPosition{
//             estimator.Ps[WINDOW_SIZE].x(),
//             estimator.Ps[WINDOW_SIZE].y()
//         };
//         std::array<double, 2> WGS84Position = wgs84::fromCartesian(WGS84Reference, CartesianPosition);
//         // 输出WGS84坐标
//         // ROS_INFO("GPS Position: Latitude = %f, Longitude = %f, altitude = %f", WGS84Position[0], WGS84Position[1], estimator.Ps[WINDOW_SIZE].z());

//         sensor_msgs::NavSatFix navsatfix_msg;
//         navsatfix_msg.header = header;
//         navsatfix_msg.header.frame_id = "world";
//         navsatfix_msg.latitude = WGS84Position[0];
//         navsatfix_msg.longitude = WGS84Position[1];
//         navsatfix_msg.altitude = estimator.Ps[WINDOW_SIZE].z();
//         pub_navsatfix.publish(navsatfix_msg);
//     }
// }

void GPS_callback(const sensor_msgs::NavSatFix::ConstPtr &gps_msg){
    ROS_INFO("GPS SUCCESSFUL!");
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "vins_estimator");
    
    ros::NodeHandle n("~");
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);
    readParameters(n);
    estimator.setParameter();
    
    /* 
    不论是否定义了 EIGEN_DONT_PARALLELIZE，都会执行 ROS_WARN("waiting for image and imu..."); 
    这条语句，生成警告级别的消息 "waiting for image and imu..."。这是一个ROS（Robot Operating System）
    中使用的消息打印语句，用于在运行时输出消息。
     */
#ifdef EIGEN_DONT_PARALLELIZE
    ROS_DEBUG("EIGEN_DONT_PARALLELIZE");
#endif
    ROS_WARN("waiting for image and imu...");
    //3、发布用于  RVIZ显示 和 pose_graph_node.cpp 接收 的Topic，本模块具体发布的内容详见输入输出
    //这个函数定义在utility/visualization.cpp里面：void registerPub(ros::NodeHandle &n)。

    registerPub(n);
    pub_navsatfix = n.advertise<sensor_msgs::NavSatFix>("navsatfix", 1000);


    ros::Subscriber sub_imu = n.subscribe(IMU_TOPIC, 2000, imu_callback, ros::TransportHints().tcpNoDelay());
    ros::Subscriber sub_image = n.subscribe("/feature_tracker/feature", 2000, feature_callback);
    ros::Subscriber sub_restart = n.subscribe("/feature_tracker/restart", 2000, restart_callback);
    ros::Subscriber sub_relo_points = n.subscribe("/pose_graph/match_points", 2000, relocalization_callback);
    ros::Subscriber sub_navsatfix = n.subscribe("/navsatfix_msg", 2000, GPS_callback);
    //确保订阅gps信息一次
    if(flag = 0)
    {
        ros::Subscriber sub_gps_position = n.subscribe("/mavros/global_position/global", 100, gps_cb);
    }
    //话题发布在registerPub函数中
    std::thread measurement_process{process};
    ros::spin();

    return 0;
}
