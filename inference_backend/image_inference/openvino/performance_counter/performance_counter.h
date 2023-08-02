#pragma once

#include <mutex>
#include <chrono>
#include <map>
#include <string>
#include <gst/gst.h>
class PerformanceCounter{
public:
    PerformanceCounter(const std::string& model_name);
    bool NewInference(size_t& buffer_size );
    ~PerformanceCounter();
private:

    unsigned int bps_update_interval;
    std::mutex mutex;
    unsigned int buffers_received;
    unsigned int settling_time_in_s;
    const std::string _model_name;
    //ts
    GstClockTime init_time,last_ts;
    
    //mqtt
    int hasMQTT;
    class MQTTImpl *_mqtt;
    char mqtt_client_id[256];
};

