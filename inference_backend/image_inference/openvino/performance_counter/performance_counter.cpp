#include "performance_counter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <MQTTAsync.h>
#include <thread>
#include "inference_backend/logger.h"
#define MIN_INTERVAL 5000
#include <uuid/uuid.h>
std::string generate_client_id() {
    uuid_t binuuid;
    uuid_generate_random(binuuid);
    char uuid[37];
    // 36 character UUID string plus terminating character
    uuid_unparse(binuuid, uuid);
    return uuid;
}

int get_int_param_from_env(const char *env_name, int defaultValue = 0)
{
    if (getenv(env_name))
    {
        try
        {
            return atoi(getenv(env_name));
        }
        catch (...)
        { // just in case it wasnt a number
            return defaultValue;
        }
    }
    else
        return defaultValue;
}

class MQTTImpl{
public:
    
    MQTTImpl(const char*, const char* ,const char*);
    ~MQTTImpl();
    bool init();
    int publish( char* buf, int len);

private:
    MQTTAsync _client = NULL;
    bool connected;
    const std::string _address;
    const std::string _client_id;
    const std::string _topic;
    const uint32_t _max_connect_attempts;
    const uint32_t _max_reconnect_interval;
    uint32_t _connection_attempt;
    uint32_t _sleep_time;
    MQTTAsync_connectOptions _connect_options;

    void on_connect_success(MQTTAsync_successData * response)
    {
        connected=true;
    }

    void on_connect_failure(MQTTAsync_failureData * response) {
        connected=false;
        GVA_WARNING("_client_id %s Connection attempt to MQTT failed. res %d",_client_id.c_str(),response ? response->code : 0);
        try_reconnect();
    }

    void on_connection_lost(char *cause) {
        connected=false;
        GVA_WARNING("client_id %s Connection to MQTT lost. Cause: %s. Attempting to reconnect",_client_id.c_str(), cause);
        try_reconnect();
    }

    void try_reconnect() {
        if (_connection_attempt == _max_connect_attempts) {
            GVA_ERROR("client_id %s Failed to connect to MQTT after maximum configured attempts.",_client_id.c_str());
            return;
        }
        _connection_attempt++;
        _sleep_time = std::min(2 * _sleep_time, _max_reconnect_interval);

        std::this_thread::sleep_for(std::chrono::seconds(_sleep_time));
        GVA_WARNING("client_id %s Attempt %d to connect to MQTT.",_client_id.c_str(), _connection_attempt);
        auto c = MQTTAsync_connect(_client, &_connect_options);
        if (c != MQTTASYNC_SUCCESS) {
            GVA_WARNING( "client_id %s Failed to start connection attempt to MQTT. Error code %d.",_client_id.c_str(), c);
        }
        else
            connected=true;
    }
};

PerformanceCounter::PerformanceCounter(const std::string& model_name) : _model_name(model_name),settling_time_in_s(0)
{
    //try to get setting from env var
    //get env var
    char* _mqtt_broker = getenv("MQTT_IP_ADDRESS");
    char* _mqtt_topic = getenv("MqttTopic");
    char* _mqtt_port = getenv("MQTT_PORT");
    hasMQTT= _mqtt_broker != NULL && _mqtt_topic != NULL;
    hasMQTT &= getenv("ips_disable_mqtt_kpi") ? false : true;

    if(hasMQTT)
    {
        char mqtt_broker[256];
        char mqtt_topic[256];
        //configure topic 
        //add ips here hmmmon
        snprintf(mqtt_broker, sizeof(mqtt_broker), "tcp://%s:%s", _mqtt_broker , _mqtt_port ? _mqtt_port : "1883");
        snprintf(mqtt_topic,sizeof(mqtt_topic),"%s/kpi/metric/%s_ips/value",_mqtt_topic, model_name.c_str());
        snprintf(mqtt_client_id,sizeof(mqtt_client_id),"%s-%s",_model_name.c_str(), generate_client_id().c_str());
        _mqtt = new MQTTImpl(mqtt_broker,mqtt_topic,mqtt_client_id);
        hasMQTT = _mqtt->init();
        buffers_received=0;
        init_time  = last_ts = GST_CLOCK_TIME_NONE;
        //try get interval

        bps_update_interval= get_int_param_from_env("ips_update_interval_in_ms",(int)MIN_INTERVAL);
        settling_time_in_s = get_int_param_from_env("ips_settling_time_seconds");

    }
    else
        GVA_WARNING("broker ip and topic was not given. mqtt disabled");
}

PerformanceCounter::~PerformanceCounter()
{
    if(_mqtt) delete _mqtt;
}

bool PerformanceCounter::NewInference(size_t& buffer_size ) {
    if(!hasMQTT) return false; //no mqtt enabled
    
    std::lock_guard<std::mutex> lock(mutex);
    GstClockTime ts = gst_util_get_timestamp ();
    if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (init_time)))
    {
        init_time = ts;
        GVA_WARNING("starting frame %llu",init_time);
    }

    if(GST_TIME_AS_SECONDS(GST_CLOCK_DIFF(init_time,ts)) > settling_time_in_s)
    {
        if(G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (last_ts)))
        {
            last_ts = ts;
        }

        buffers_received+=buffer_size;
        if (GST_TIME_AS_MSECONDS(GST_CLOCK_DIFF (last_ts, ts)) >= bps_update_interval)
        {
            char buf[256];
            auto time_diff = (gdouble) (ts - last_ts) / GST_SECOND;
            GVA_WARNING("[%s] sending frame %d interval %llu ts %llu time_diff %lf update_interval %d",mqtt_client_id, buffers_received, last_ts, ts, time_diff, bps_update_interval);
            auto rr = (gdouble) (buffers_received) / time_diff;
            int len = snprintf(buf,sizeof(buf),"%.2f",rr);
            _mqtt->publish(buf,len);
            buffers_received=0;
            last_ts=ts;
            //last_ts= GST_CLOCK_TIME_NONE;
        }
    }

    return true;
}

int MQTTImpl::publish(char* buf, int len)
{
    if (connected && len > 0 && buf) {
        MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
        opts.context = this;

        MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
        pubmsg.payload = (void *)buf;
        pubmsg.payloadlen =  len;
        pubmsg.qos = 1;
        pubmsg.retained = 0;
        MQTTAsync_sendMessage(_client, _topic.c_str(), &pubmsg, &opts);
    }

    return 0;
}

bool MQTTImpl::init()
{
    if(_address.empty() || _client_id.empty() || _topic.empty()) return false;
    _connection_attempt = 1;
    
    //connection here
    MQTTAsync_createOptions create_opts = MQTTAsync_createOptions_initializer;
    create_opts.sendWhileDisconnected = 1;
    create_opts.MQTTVersion = MQTTVERSION_5;

    auto sts = MQTTAsync_createWithOptions(&_client, _address.c_str(), _client_id.c_str(), MQTTCLIENT_PERSISTENCE_NONE, nullptr, &create_opts);
    if (sts != MQTTASYNC_SUCCESS) {
        GVA_WARNING("Failed to create MQTTAsync handler. Error code: %d", sts);
        return false;
    }

    sts = MQTTAsync_setCallbacks(
        _client, this,
        [](void *context, char *cause) {
            if (!context) {
                GVA_WARNING("Got null context on mqtt connect_lost callback");
                return;
            }
            static_cast<MQTTImpl *>(context)->on_connection_lost(cause);
        },NULL, NULL
    );
    
    GVA_WARNING("client_id %s trying to connect to %s with topic %s",_client_id.c_str(),_address.c_str(),_topic.c_str());
    auto c = MQTTAsync_connect(_client, &_connect_options);
    if (c != MQTTASYNC_SUCCESS) {
        GVA_WARNING("Failed to start connection attempt to MQTT. Error code %d.", c);
        return false;
    }

    return true;
}

MQTTImpl::MQTTImpl(const char* address, const char* topic, const char* clientID) : connected(false), _sleep_time(1),_max_reconnect_interval(1), _max_connect_attempts(10), _address(address), _topic(topic), _connection_attempt(0), _client_id(clientID)
{
    _connect_options = MQTTAsync_connectOptions_initializer;
    _connect_options.keepAliveInterval = 20;
    _connect_options.cleansession = 1;
    _connect_options.context = this;
    _connect_options.onSuccess = [](void *context, MQTTAsync_successData *response) {
        if (!context) {
            GVA_ERROR("Got null context on mqtt connect_success callback");
            return;
        }
        static_cast<MQTTImpl *>(context)->on_connect_success(response);
    };
    _connect_options.onFailure = [](void *context, MQTTAsync_failureData *response) {
        if (!context) {
            GVA_ERROR("Got null context on mqtt connect_failure callback");
            return;
        }
        static_cast<MQTTImpl *>(context)->on_connect_failure(response);
    };
}

MQTTImpl::~MQTTImpl()
{
    if (_client) {
        MQTTAsync_destroy(&_client);
    }
}

