
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
//#include <WiFiClientSecure.h>
#include <MQTT.h>
#include "CircularBuffer.h"

#include "config.h"

#include "debug.h"

typedef char AddressString[17];

void startTemperatureSensors();
void writeAddress(char* out, uint8_t* addr);
void sensorLoop();
void publishLoop();
bool startWiFiSTA();
void mqttBegin();
bool mqttPublishSensor(char* addr, float temp);
void mqttLoop();
void mqttStop();

static OneWire oneWire(ONE_WIRE_BUS);
static DallasTemperature sensors(&oneWire);
static DeviceAddress sensorAddresses[MAX_SENSORS];
static AddressString sensorAddressStrings[MAX_SENSORS];
static CircularBuffer<float,HISTORY_SIZE> history[MAX_SENSORS];
static uint8_t sensorAddressCount;
static WiFiClient* client = nullptr;
static MQTTClient* mqttClient = nullptr;


void setup() {
#ifdef DEBUG
  Serial.begin(115200);
#endif

    startTemperatureSensors();
    startWiFiSTA();
}

void loop() {
    sensorLoop();
    publishLoop();
    mqttLoop();
}


void startTemperatureSensors() {
    sensors.begin();

    uint8_t deviceCount = min(sensors.getDeviceCount(), (uint8_t)MAX_SENSORS);
    LOGD("%d devices found", deviceCount);

    for (uint8_t i=0; i<deviceCount; i++) {
        uint8_t* addr = sensorAddresses[sensorAddressCount];
        char* addrString = sensorAddressStrings[sensorAddressCount];
        if (sensors.getAddress(addr, i)) {
            writeAddress(addrString, addr);
            LOGD("Device %d has address %s", i, addrString);
            sensorAddressCount++;
        } else {
            LOGD("Invalid address for device %d", i);
        }
    }
}

void writeAddress(char* out, uint8_t* addr) {
    int pos = 0;
    for (uint8_t i=0; i<8; i++) {
        pos += sprintf(out+pos, "%02X", addr[i]);
    }
}

void sensorLoop() {
    static uint32_t lastReading = 0;
    if (millis() - lastReading > SAMPLE_RATE) {
        sensors.requestTemperatures();
        for (uint8_t i=0; i<sensorAddressCount; i++) {
            uint8_t* addr = sensorAddresses[i];
            float temp = sensors.getTempC(addr);
            history[i].push(temp);
        }
        lastReading = millis();
    }
}

void publishLoop() {
    static uint32_t lastPublication = 0;
    if (millis() - lastPublication > PUBLISH_RATE) {
        for (uint8_t i=0; i<sensorAddressCount; i++) {
            float avgTemp = 0.0;
            CircularBuffer<float,HISTORY_SIZE> &hist = history[i];
            uint8_t size = hist.size();
            for (int j=0; j<size; j++) {
              float temp = hist[j];
              avgTemp += temp;
            }
            avgTemp /= size;

            char* addrString = sensorAddressStrings[i];
            mqttPublishSensor(addrString, avgTemp);
        }
        lastPublication = millis();
    }
}

bool startWiFiSTA() {
  WiFi.setHostname(HOSTNAME);
  WiFi.enableSTA(true);
  WiFi.setAutoReconnect(true);
  wl_status_t status = WiFi.begin(SSID, PASSWORD);
  if (status != WL_CONNECT_FAILED) {
    mqttBegin();
    LOGD("Started WiFi station: mode %d (status %d)", WiFi.getMode(), status);
    return true;
  } else {
    LOGE("Failed to start WiFi station");
    return false;
  }
}

bool stopWiFi() {
  mqttStop();
  WiFi.disconnect(true);
  LOGD("Stopped WiFi: mode %d", WiFi.getMode());
  return true;
}



static bool isMqttRunning = false;

void mqttBegin() {
    if (!isMqttRunning) {
        client = new WiFiClient();
        mqttClient = new MQTTClient(256);
        mqttClient->begin(MQTT_BROKER, MQTT_PORT, *client);
        isMqttRunning = true;
    }
}

void mqttLoop() {
    if (isMqttRunning) {
        mqttClient->loop();
    }
}

void mqttStop() {
  if (isMqttRunning) {
    mqttClient->disconnect();
    if (mqttClient) {
      delete mqttClient;
      mqttClient = nullptr;
    }
    if (client) {
      delete client;
      client = nullptr;
    }
    isMqttRunning = false;
  }
}

bool ensureConnected() {
  static uint32_t lastAttempt = 0;
  bool isConn = mqttClient->connected();
  const uint32_t ms = millis();
  if (!isConn && (ms - lastAttempt) > RECONNECT_DELAY) {
    LOGMEM("pre-MQTT-ensureConnected");
    LOGD("WiFi station status: %d", WiFi.status());
    //loadCertificates(client);
    isConn = mqttClient->connect(HOSTNAME, MQTT_USER, MQTT_PASSWORD);
    LOGD("MQTT connection: %d", mqttClient->returnCode());
    if (isConn) {
      lastAttempt = 0;
      //freeCertificates(client);
    } else {
      lastAttempt = ms;
      LOGE("MQTT connection error: %d", mqttClient->lastError());
      /*
      char errMsg[100] = {'\0'};
      int errCode = client->lastError(errMsg, 100);
      LOGE("MQTT client connection error (%d): %s", errCode, errMsg);
      */
    }
  }
  return isConn;
}

bool mqttPublishSensor(char* addr, float temp) {
  static char payload[PAYLOAD_BUFFER_SIZE];
  static char topicBuf[TOPIC_BUFFER_SIZE];
  if (isMqttRunning && WiFi.isConnected()) {
    if (ensureConnected()) {
      sprintf(topicBuf, "%s/%s", TOPIC, addr);
      sprintf(payload, "%f", temp);
      // remove any leading '/'
      char* topic = (topicBuf[0] == '/') ? topicBuf+1 : topicBuf;

      return mqttClient->publish(topic, payload);
    }
  }
  return false;
}
