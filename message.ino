#include <DHT.h>
#include <Redis.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <fauxmoESP.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <IRremoteESP8266.h> 

#define WIFI_SSID ""
#define WIFI_PASS ""

#define REDIS_ADDR ""
#define REDIS_PORT 6379
#define REDIS_PASSWORD ""

#define MQTT_UUID ""
#define MQTT_HOST ""
#define MQTT_PORT 8884
#define MQTT_USER ""
#define MQTT_PASS ""
#define MQTT_TOPIC_RECEIVE ""
#define MQTT_TOPIC_SEND_TEMP ""

#define PIN_LDR A0

#define PIN_DHT D3
#define DHT_TYPE DHT11

#define PIN_IR_SEND 4
#define PIN_IR_RECV D1
#define IR_FREQUENCY 38

struct {
  String on;
  String off;
} ALEXA_CACHE;

struct {
  String button;
  String topic;
} IR_CREATE;

fauxmoESP alexa;

IRrecv irrecv = IRrecv(PIN_IR_RECV);
IRsend irsend = IRsend(PIN_IR_SEND);

DHT dht = DHT(PIN_DHT, DHT_TYPE);

WiFiClient wifiClientMqtt;
PubSubClient mqtt = PubSubClient(wifiClientMqtt);

WiFiClient wifiClientRedis;
Redis redis = Redis(wifiClientRedis);

void setup() {
  setupSerial();
  Serial.println("[~Setup] start");
  setupSensors();
  setupMqtt();
  connectWifi();
  connectRedis();
  connectMqtt();
  connectAlexa();
  createAlexaCache();
  Serial.println("[~Setup] finish");
}

void setupSerial() {
  Serial.begin(115200);
  Serial.println();
  Serial.println();
}

void setupSensors() {
  dht.begin();
  irsend.begin();
  irrecv.enableIRIn();
}

void setupMqtt() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(callbackMqtt);
}

void connectWifi() {
  if(WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[~WIFI] Connecting");
  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("[~WIFI] Connected ");
  Serial.println(WiFi.localIP());
}

void connectRedis() {
  if(wifiClientRedis.connected()) return;
  Serial.print("[~Redis] Connecting");
  while (!wifiClientRedis.connect(REDIS_ADDR, REDIS_PORT)) {
    delay(500);
    Serial.print(".");
  }
  auto connStatus = redis.authenticate(REDIS_PASSWORD);
  if (connStatus == RedisSuccess) {
    Serial.println();
    Serial.print("[~Redis] Connected ");
    Serial.println(REDIS_ADDR);
  } else {
    Serial.printf("[~Redis] Error: %d\n", (int) connStatus);
  }
}

void connectMqtt() {
  if(mqtt.connected()) return;
  Serial.print("[~MQTT] Connecting");
  while(!mqtt.connected()) {
     if(mqtt.connect(MQTT_UUID, MQTT_USER, MQTT_PASS)){
        Serial.println();
        Serial.print("[~MQTT] Connected ");
        Serial.println(MQTT_HOST);
        mqtt.subscribe(MQTT_TOPIC_RECEIVE);
     } else {
        Serial.print(".");
        delay(500);
     }
  }
}

void keepConections() {
  connectWifi();
  connectRedis();
  connectMqtt();
}

void connectAlexa() {
  alexa.createServer(true);
  alexa.setPort(80);
  alexa.enable(true);
  alexa.addDevice("Controller"); 
  alexa.onSetState([](u_char device_id, const char * device_name, bool state, u_char value) {
    String button = state ? "on" : "off";
    button = "alexa:" + button;
    Serial.printf("[~Alexa] Device (%s) state: %s\n", device_name, button.c_str());
    DynamicJsonDocument doc(2048);
    if (button.equals("alexa:on")) {
      deserializeJson(doc, ALEXA_CACHE.on);
    }
    if (button.equals("alexa:off")) {
      deserializeJson(doc, ALEXA_CACHE.off);
    }
    u_int16_t lenght = doc["lenght"];
    u_int16_t rawData[lenght];
    for (u_int16_t i = 0; i < lenght; i++) {
      rawData[i] =  doc["data"][i];
    }
    irsend.sendRaw(rawData, lenght, IR_FREQUENCY);
  });
}

void createAlexaCache() {
  Serial.println("[~Alexa] cache buttons");
  ALEXA_CACHE.on = redis.get("alexa:on");
  ALEXA_CACHE.off = redis.get("alexa:off");
}

void callbackMqtt(char *topic, byte *payload, u_int length) {
  String message;
  for(int i = 0; i < length; i++) {
     char c = (char) payload[i];
     message += c;
  }
  Serial.print("[~MQTT] recieved ");
  Serial.println(message);
  Serial.println(topic);
  
  DynamicJsonDocument doc(512);
  deserializeJson(doc, message);
  
  String type = doc["type"];
  
  if (type.equals("create")) {
    String device = topic;
    device = device.substring(14);
    String button = doc["button"];
    IR_CREATE.topic = topic;
    IR_CREATE.button = device + ":" + button;
  }

  if (type.equals("click")) {
    String device = topic;
    device = device.substring(14);
    String button = doc["button"];
    button = device + ":" + button;
    String json = redis.get(button.c_str());

    DynamicJsonDocument doc(2048);
    deserializeJson(doc, json);

    u_int16_t lenght = doc["lenght"];
    u_int16_t rawData[lenght];
    for (u_int16_t i = 0; i < lenght; i++) {
      rawData[i] =  doc["data"][i];
    }
    irsend.sendRaw(rawData, lenght, IR_FREQUENCY);
  }
}

u_long prevMilisSendStatus = 0, intervalMilisSendStatus = 1000;
u_long prevMilisKeepConnection = 0, intervalMilisKeepConnection = 10000;

void loop() {
  mqtt.loop();
  alexa.handle();
  u_long currentMillis = millis();
  if (currentMillis - prevMilisKeepConnection > intervalMilisKeepConnection) {
    prevMilisKeepConnection = currentMillis;
    keepConections();
  }
  if (currentMillis - prevMilisSendStatus > intervalMilisSendStatus) {
    prevMilisSendStatus = currentMillis;
    sendStatus();
  }
  if (!IR_CREATE.button.isEmpty()) {
    decode_results results;  
    if (irrecv.decode(&results)) {
      setRawInRedis(results);
      irrecv.resume();
      IR_CREATE.button = "";
      IR_CREATE.topic = "";
      delay(10);
    }
  }
}

void sendStatus() {
  u_int16_t lightness = analogRead(PIN_LDR);
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  // Serial.println("[~MQTT] Sending status");
  DynamicJsonDocument doc = DynamicJsonDocument(512);
  doc["humidity"] = humidity;
  doc["temperature"] = temperature;
  doc["lightness"] = lightness;
  String json;
  serializeJson(doc, json);
  mqtt.publish(MQTT_TOPIC_SEND_TEMP, json.c_str());
}

void setRawInRedis(decode_results results) {
    DynamicJsonDocument doc = DynamicJsonDocument(2048);
    JsonArray data = doc.createNestedArray("data");
    doc["lenght"] = results.rawlen;
    Serial.print("u_int16_t data[");
    Serial.print(results.rawlen, DEC);
    Serial.print("] = ");
    for (u_int8_t i = 1; i < results.rawlen; i++) {
      if (i == 1) {
        Serial.print("{");
      }
      if (i & 1) {
        Serial.print(results.rawbuf[i] * kRawTick, DEC);
        data.add(results.rawbuf[i] * kRawTick);
      } else {
        Serial.print((u_long) results.rawbuf[i] * kRawTick, DEC);
        data.add((u_long) results.rawbuf[i] * kRawTick);
      }
      if (i != (results.rawlen - 1)) {
        Serial.print(", ");
      }
      if (i == (results.rawlen - 1)) {
        Serial.print("};");
      }
    }
    Serial.println();
    
    String json;
    serializeJson(doc, json);
    redis.set(IR_CREATE.button.c_str(), json.c_str());

    doc = DynamicJsonDocument(512);
    doc["type"] = "response";
    doc["button"] = IR_CREATE.button;
    String jsonMqtt;
    serializeJson(doc, jsonMqtt);
    mqtt.publish(IR_CREATE.topic.c_str(), jsonMqtt.c_str());
}
