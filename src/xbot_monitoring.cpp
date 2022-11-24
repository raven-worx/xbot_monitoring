//
// Created by Clemens Elflein on 22.11.22.
// Copyright (c) 2022 Clemens Elflein. All rights reserved.
//
#include <filesystem>

#include "ros/ros.h"
#include <memory>
#include <boost/regex.hpp>
#include "xbot_msgs/SensorInfo.h"
#include "xbot_msgs/Map.h"
#include "xbot_msgs/SensorDataString.h"
#include "xbot_msgs/SensorDataDouble.h"
#include "xbot_msgs/RobotState.h"
#include <mqtt/async_client.h>
#include <nlohmann/json.hpp>
#include "geometry_msgs/Twist.h"
#include "std_msgs/String.h"

using json = nlohmann::json;

void publish_sensor_metadata();
void publish_map();

// Maps a topic to a subscriber.
std::map<std::string, ros::Subscriber> active_subscribers;
std::map<std::string, xbot_msgs::SensorInfo> found_sensors;
std::vector<ros::Subscriber> sensor_data_subscribers;

ros::NodeHandle *n;

// The MQTT Client
std::shared_ptr<mqtt::async_client> client_;

std::mutex mqtt_callback_mutex;

// Publisher for cmd_vel and commands
ros::Publisher cmd_vel_pub;
ros::Publisher command_pub;

class MqttCallback : public mqtt::callback {
    void connected(const mqtt::string &string) override {
        ROS_INFO_STREAM("MQTT Connected");
        publish_sensor_metadata();
        publish_map();


        client_->subscribe("/teleop", 0);
        client_->subscribe("/command", 0);
    }

public:
    void message_arrived(mqtt::const_message_ptr ptr) override {
        if(ptr->get_topic() == "/teleop") {
            ROS_INFO_STREAM("joy!");
            try {
                json json = json::from_bson(ptr->get_payload().begin(), ptr->get_payload().end());

                ROS_INFO_STREAM_THROTTLE(0.5,"vx:" << json["vx"] << " vr: " << json["vz"]);
                geometry_msgs::Twist t;
                t.linear.x = json["vx"];
                t.angular.z = json["vz"];
                cmd_vel_pub.publish(t);
            } catch (const json::exception &e) {
                ROS_ERROR_STREAM("Error decoding /teleop bson: " << e.what());
            }
        } else if(ptr->get_topic() == "/command") {
            ROS_INFO_STREAM("Got command: " + ptr->get_payload());

        }
    }
};

MqttCallback mqtt_callback;

json map;
bool has_map = false;

void setupMqttClient() {
    // MQTT connection options
    mqtt::connect_options connect_options_;

    // basic client connection options
    connect_options_.set_automatic_reconnect(true);
    connect_options_.set_clean_session(true);
    connect_options_.set_keep_alive_interval(1000);
    connect_options_.set_max_inflight(10);

    // create MQTT client
    std::string uri = "tcp" + std::string("://") + "127.0.0.1" +
                      std::string(":") + std::to_string(1883);

    try {
        client_ = std::make_shared<mqtt::async_client>(
                uri, "xbot_monitoring");
        client_->set_callback(mqtt_callback);

        client_->connect(connect_options_);

    } catch (const mqtt::exception &e) {
        ROS_ERROR("Client could not be initialized: %s", e.what());
        exit(EXIT_FAILURE);
    }


}

void try_publish(std::string topic, std::string data, bool retain = false) {
    try {
        if (retain) {
            // QOS 1 so that the data actually arrives at the client at least once.
            client_->publish(topic, data, 1, true);
        } else {
            client_->publish(topic, data);
        }
    } catch (const mqtt::exception &e) {
        // client disconnected or something, we drop it.
    }
}
void try_publish_binary(std::string topic, const void *data, size_t size, bool retain = false) {
    try {
        if (retain) {
            // QOS 1 so that the data actually arrives at the client at least once.
            client_->publish(topic, data, size, 1, true);
        } else {
            client_->publish(topic, data, size);
        }
    } catch (const mqtt::exception &e) {
        // client disconnected or something, we drop it.
    }
}

void publish_sensor_metadata() {
    std::unique_lock<std::mutex> lk(mqtt_callback_mutex);

    if(found_sensors.empty())
        return;

    json sensor_info;
    for (const auto &kv: found_sensors) {
        json info;
        info["sensor_id"] = kv.second.sensor_id;
        info["sensor_name"] = kv.second.sensor_name;

        switch (kv.second.value_type) {
            case xbot_msgs::SensorInfo::TYPE_STRING: {
                info["value_type"] = "STRING";
                break;
            }
            case xbot_msgs::SensorInfo::TYPE_DOUBLE: {
                info["value_type"] = "DOUBLE";
                break;
            }
            default: {
                info["value_type"] = "UNKNOWN";
                break;
            }


        }

        switch (kv.second.value_description) {
            case xbot_msgs::SensorInfo::VALUE_DESCRIPTION_TEMPERATURE: {
                info["value_description"] = "TEMPERATURE";
                break;
            }
            case xbot_msgs::SensorInfo::VALUE_DESCRIPTION_VELOCITY: {
                info["value_description"] = "VELOCITY";
                break;
            }
            case xbot_msgs::SensorInfo::VALUE_DESCRIPTION_ACCELERATION: {
                info["value_description"] = "ACCELERATION";
                break;
            }
            case xbot_msgs::SensorInfo::VALUE_DESCRIPTION_VOLTAGE: {
                info["value_description"] = "VOLTAGE";
                break;
            }
            case xbot_msgs::SensorInfo::VALUE_DESCRIPTION_CURRENT: {
                info["value_description"] = "CURRENT";
                break;
            }
            case xbot_msgs::SensorInfo::VALUE_DESCRIPTION_PERCENT: {
                info["value_description"] = "PERCENT";
                break;
            }
            default: {
                info["value_description"] = "UNKNOWN";
                break;
            }
        }

        info["unit"] = kv.second.unit;
        info["has_min_max"] = kv.second.has_min_max;
        info["min_value"] = kv.second.min_value;
        info["max_value"] = kv.second.max_value;
        info["has_critical_low"] = kv.second.has_critical_low;
        info["lower_critical_value"] = kv.second.lower_critical_value;
        info["has_critical_high"] = kv.second.has_critical_high;
        info["upper_critical_value"] = kv.second.upper_critical_value;
        sensor_info.push_back(info);
    }

    ROS_INFO_STREAM("updated sensor metadata:" << sensor_info.dump(5));
    try_publish("sensor_infos/json", sensor_info.dump(), true);
    json data;
    data["d"] = sensor_info;
    auto bson = json::to_bson(data);
    try_publish_binary("sensor_infos/bson", bson.data(), bson.size(), true);
}

void subscribe_to_sensor(std::string topic) {
    auto &sensor = found_sensors[topic];

    ROS_INFO_STREAM("Subscribing to sensor data for sensor with name: " << sensor.sensor_name);

    std::string data_topic = "xbot_monitoring/sensors/" + sensor.sensor_id + "/data";

    switch (sensor.value_type) {
        case xbot_msgs::SensorInfo::TYPE_DOUBLE: {
            ros::Subscriber s = n->subscribe<xbot_msgs::SensorDataDouble>(data_topic, 10, [&info = sensor](
                    const xbot_msgs::SensorDataDouble::ConstPtr &msg) {
                try_publish("sensors/" + info.sensor_id + "/data", std::to_string(msg->data));

                json data;
                data["d"] = msg->data;
                auto bson = json::to_bson(data);
                try_publish_binary("sensors/" + info.sensor_id + "/bson", bson.data(), bson.size());
            });
            sensor_data_subscribers.push_back(s);
            break;
        }
        case xbot_msgs::SensorInfo::TYPE_STRING: {
            ros::Subscriber s = n->subscribe<xbot_msgs::SensorDataString>(data_topic, 10, [&info = sensor](
                    const xbot_msgs::SensorDataString::ConstPtr &msg) {
                try_publish("sensors/" + info.sensor_id + "/data", msg->data);

                json data;
                data["d"] = msg->data;
                auto bson = json::to_bson(data);
                try_publish_binary("sensors/" + info.sensor_id + "/bson", bson.data(), bson.size());
            });
            sensor_data_subscribers.push_back(s);
            break;
        }
        default: {
            ROS_ERROR_STREAM("Inavlid Sensor Data Type: " << (int) sensor.value_type);
        }
    }
}

void robot_state_callback(const xbot_msgs::RobotState::ConstPtr &msg) {
    // Build a JSON and publish it
    json j;

    j["battery_percentage"] = msg->battery_percentage;
    j["gps_percentage"] = msg->gps_percentage;
    j["current_action_progress"] = msg->current_action_progress;
    j["current_state"] = msg->current_state;
    j["current_sub_state"] = msg->current_sub_state;
    j["pose"]["x"] = msg->robot_pose.pose.pose.position.x;
    j["pose"]["y"] = msg->robot_pose.pose.pose.position.y;
    j["pose"]["heading"] = msg->robot_pose.vehicle_heading;
    j["pose"]["pos_accuracy"] = msg->robot_pose.position_accuracy;
    j["pose"]["heading_accuracy"] = msg->robot_pose.orientation_accuracy;
    j["pose"]["heading_valid"] = msg->robot_pose.orientation_valid;

    try_publish("robot_state/json", j.dump());
    json data;
    data["d"] = j;
    auto bson = json::to_bson(data);
    try_publish_binary("robot_state/bson", bson.data(), bson.size());
}

void publish_map() {
    if(!has_map)
        return;
    try_publish("map/json", map.dump(), true);
    json data;
    data["d"] = map;
    auto bson = json::to_bson(data);
    try_publish_binary("map/bson", bson.data(), bson.size(), true);
}

void map_callback(const xbot_msgs::Map::ConstPtr &msg) {
    // Build a JSON and publish it
    json j;

    j["docking_pose"]["x"] = msg->dockX;
    j["docking_pose"]["y"] = msg->dockY;
    j["docking_pose"]["heading"] = msg->dockHeading;

    j["meta"]["mapWidth"] = msg->mapWidth;
    j["meta"]["mapHeight"] = msg->mapHeight;
    j["meta"]["mapCenterX"] = msg->mapCenterX;
    j["meta"]["mapCenterY"] = msg->mapCenterY;


    json working_areas_j;
    for(const auto &area : msg->workingArea) {
        json area_j;
        area_j["name"] = area.name;
        {
            json outline_poly_j;
            for (const auto &pt: area.area.points) {
                json p_j;
                p_j["x"] = pt.x;
                p_j["y"] = pt.y;
                outline_poly_j.push_back(p_j);
            }
            area_j["outline"] = outline_poly_j;
        }
        json obstacle_polys_j;
        for(const auto &obstacle : area.obstacles) {
            json obstacle_poly_j;
            for(const auto &pt : obstacle.points) {
                json p_j;
                p_j["x"] = pt.x;
                p_j["y"] = pt.y;
                obstacle_poly_j.push_back(p_j);
            }
            obstacle_polys_j.push_back(obstacle_poly_j);
        }
        area_j["obstacles"] = obstacle_polys_j;
        working_areas_j.push_back(area_j);
    }
    json navigation_areas_j;

    for(const auto &area : msg->navigationAreas) {
        json area_j;
        area_j["name"] = area.name;
        {
            json outline_poly_j;
            for (const auto &pt: area.area.points) {
                json p_j;
                p_j["x"] = pt.x;
                p_j["y"] = pt.y;
                outline_poly_j.push_back(p_j);
            }
            area_j["outline"] = outline_poly_j;
        }
        json obstacle_polys_j;
        for(const auto &obstacle : area.obstacles) {
            json obstacle_poly_j;
            for(const auto &pt : obstacle.points) {
                json p_j;
                p_j["x"] = pt.x;
                p_j["y"] = pt.y;
                obstacle_poly_j.push_back(p_j);
            }
            obstacle_polys_j.push_back(obstacle_poly_j);
        }
        area_j["obstacles"] = obstacle_polys_j;
        navigation_areas_j.push_back(area_j);
    }

    j["working_areas"] = working_areas_j;
    j["navigation_areas"] = navigation_areas_j;


    map = j;
    has_map = true;

    publish_map();
}

int main(int argc, char **argv) {
    ros::init(argc, argv, "xbot_monitoring");
    has_map = false;

    // First setup MQTT
    setupMqttClient();


    n = new ros::NodeHandle();
    ros::NodeHandle paramNh("~");

    ros::Subscriber robotStateSubscriber = n->subscribe("xbot_monitoring/robot_state", 10, robot_state_callback);
    ros::Subscriber mapSubscriber = n->subscribe("xbot_monitoring/map", 10, map_callback);

    cmd_vel_pub = n->advertise<geometry_msgs::Twist>("xbot_monitoring/remote_cmd_vel", 1);
    command_pub = n->advertise<std_msgs::String>("xbot_monitoring/remote_command", 1);


    ros::AsyncSpinner spinner(1);
    spinner.start();

    ros::Rate sensor_check_rate(10.0);

    boost::regex topic_regex("/xbot_monitoring/sensors/.*/info");

    while (ros::ok()) {
        // Read the topics in /xbot_monitoring/sensors/.*/info and subscribe to them.
        ros::master::V_TopicInfo topics;
        ros::master::getTopics(topics);
        std::for_each(topics.begin(), topics.end(), [&](const ros::master::TopicInfo &item) {
            if (boost::regex_match(item.name, topic_regex)) {
                if (active_subscribers.count(item.name) == 0 && found_sensors.count(item.name) == 0) {
                    ROS_INFO_STREAM("found new sensor topic " << item.name);
                    active_subscribers[item.name] = n->subscribe<xbot_msgs::SensorInfo>(item.name, 1,
                                                                                        [topic = item.name](
                                                                                                const xbot_msgs::SensorInfo::ConstPtr &msg) {
                                                                                            ROS_INFO_STREAM(
                                                                                                    "got sensor info for sensor on topic "
                                                                                                            << msg->sensor_name
                                                                                                            << " on topic "
                                                                                                            << topic);
                                                                                            {
                                                                                                std::unique_lock<std::mutex> lk(
                                                                                                        mqtt_callback_mutex);

                                                                                                // Save the sensor info
                                                                                                found_sensors[topic] = *msg;
                                                                                            }
                                                                                            // Stop subscribing to infos
                                                                                            active_subscribers.erase(
                                                                                                    topic);
                                                                                            // Subscribe for data
                                                                                            subscribe_to_sensor(
                                                                                                    topic);
                                                                                            // republish sensor info
                                                                                            publish_sensor_metadata();
                                                                                        });
                }
            }
        });
        sensor_check_rate.sleep();
    }
    return 0;
}
