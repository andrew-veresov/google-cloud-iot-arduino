/******************************************************************************
 * Copyright 2019 Google
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/
#include "CloudIoTCoreMqtt.h"

// Forward global callback declarations
String getJwt();
void setupCert();
Client* setupNetwork(bool);
void messageReceived(String &topic, String &payload);


///////////////////////////////
// MQTT common functions
///////////////////////////////
CloudIoTCoreMqtt::CloudIoTCoreMqtt(
    MQTTClient *_mqttClient, Client *_netClient, CloudIoTCoreDevice *_device){
  this->mqttClient = _mqttClient;
  this->netClient = _netClient;
  this->device = _device;
}

void CloudIoTCoreMqtt::loop() {
  bool needToReconnect = false;
  if (millis() > device->getExpMillis()) {
    // reconnect
    Serial.println(F("Reconnecting before JWT expiration"));
    iat = 0; // Force JWT regeneration
	  if (mqttClient->connected()) mqttClient->disconnect();
    needToReconnect = true;
  }
  if (!mqttClient->connected() && abs(this->__last_retry_at - millis()) >= this->__backoff__) {
    // reconnect
    Serial.println(F("Reconnecting with back-off"));
    needToReconnect = true;
    iat = 0; // Force JWT regeneration
  }
  if (needToReconnect) {
    mqttConnect(); // TODO: should we skip closing connection
  }
  this->mqttClient->loop();
}

void CloudIoTCoreMqtt::increaseBackoff() {
    // See https://cloud.google.com/iot/docs/how-tos/exponential-backoff
    if (this->__backoff__ < this->__minbackoff__) {
      this->__backoff__ = this->__minbackoff__;
    }
    this->__backoff__ = (this->__backoff__ * this->__factor__) + random(this->__jitter__);
    if (this->__backoff__ > this->__max_backoff__) {
      this->__backoff__ = this->__max_backoff__;
    }
    Serial.printf("Back-off: %ims\n", this->__backoff__);
}

void CloudIoTCoreMqtt::mqttConnect(bool skip) {
  Serial.print(F("Connecting MQTT... "));
  if (iat == 0) {
    jwt = getJwt(); // Regenerate JWT using device function
  }
  this->__last_retry_at = millis();
  bool result =
      this->mqttClient->connect(
          device->getClientId().c_str(),
          "unused",
          jwt.c_str(),
          false);

  Serial.println(mqttClient->connected() ? "connected" : "not connected");

  if (this->mqttClient->lastError() != LWMQTT_SUCCESS && !result){
    // TODO: refactorme
    // Inform the client why it could not connect and help debugging.
    Serial.println(F("Settings incorrect or missing a cyper for SSL"));
    logError();
    logReturnCode();
    logConfiguration(false);
    increaseBackoff();
  } else {
      // We're now connected
      Serial.println(F("Library connected!"));
      this->__backoff__ = this->__minbackoff__;
  }

  // Set QoS to 1 (ack) for configuration messages
  this->mqttClient->subscribe(device->getConfigTopic(), 1);
  // QoS 0 (no ack) for commands
  this->mqttClient->subscribe(device->getCommandsTopic(), 0);

  onConnect();
}

void CloudIoTCoreMqtt::mqttConnectAsync(bool skip) {
  Serial.print(F("Connecting MQTT async [NOT COMPLETE IMPLEMENTATION]..."));
  if (iat == 0) {
    jwt = getJwt(); // Regenerate JWT using device function
  }
  this->__last_retry_at = millis();

  bool result =
      this->mqttClient->connect(
          device->getClientId().c_str(),
          "unused",
          jwt.c_str(),
          skip);

  if (this->mqttClient->lastError() != LWMQTT_SUCCESS && result == true){
    // TODO: refactorme
    // Inform the client why it could not connect and help debugging.
    logError();
    logReturnCode();
    logConfiguration(false);

    increaseBackoff();
  } else {
    Serial.println(mqttClient->connected() ? "connected" : "not connected");
    if (!mqttClient->connected()) {
      Serial.println("No internet or Settings incorrect or missing a cyper for SSL");
      //mqttClient->disconnect();
      logConfiguration(false);
      Serial.println("\naborting mqtt connection attempt, lets rety later...\tLibrary not connected!");

      increaseBackoff();
    } else {
      // We're now connected
      Serial.println("\nLibrary connected!");
      this->__backoff__ = this->__minbackoff__;
    }
  }


  // Set QoS to 1 (ack) for configuration messages
  this->mqttClient->subscribe(device->getConfigTopic(), 1);
  // QoS 0 (no ack) for commands
  this->mqttClient->subscribe(device->getCommandsTopic(), 0);

  onConnect();
}

void CloudIoTCoreMqtt::startMQTT() {
  int port = CLOUD_IOT_CORE_MQTT_PORT;
  if (this->use443Port) {
    port = CLOUD_IOT_CORE_HTTP_PORT;
  }
  if (this->useLts) {
    this->mqttClient->begin(CLOUD_IOT_CORE_MQTT_HOST_LTS, port, *netClient);
  } else {
    this->mqttClient->begin(CLOUD_IOT_CORE_MQTT_HOST, port, *netClient);
  }
  this->mqttClient->onMessage(messageReceived);
}

bool CloudIoTCoreMqtt::publishTelemetry(String data) {
  return this->mqttClient->publish(device->getEventsTopic(), data);
}

bool CloudIoTCoreMqtt::publishTelemetry(String data, int qos) {
  return this->mqttClient->publish(device->getEventsTopic(), data, false, qos);
}

bool CloudIoTCoreMqtt::publishTelemetry(const char* data, int length) {
  return this->mqttClient->publish(device->getEventsTopic().c_str(), data, length);
}

bool CloudIoTCoreMqtt::publishTelemetry(String subtopic, String data) {
  return this->mqttClient->publish(device->getEventsTopic() + subtopic, data);
}

bool CloudIoTCoreMqtt::publishTelemetry(String subtopic, String data, int qos) {
  return this->mqttClient->publish(device->getEventsTopic() + subtopic, data, false, qos);
}

bool CloudIoTCoreMqtt::publishTelemetry(String subtopic, const char* data, int length) {
  return this->mqttClient->publish(String(device->getEventsTopic() + subtopic).c_str(), data, length);
}

// Helper that just sends default sensor
bool CloudIoTCoreMqtt::publishState(String data) {
  return this->mqttClient->publish(device->getStateTopic(), data);
}

bool CloudIoTCoreMqtt::publishState(const char* data, int length) {
  return this->mqttClient->publish(device->getStateTopic().c_str(), data, length);
}

void CloudIoTCoreMqtt::logError() {
  Serial.println(this->mqttClient->lastError());
  switch(this->mqttClient->lastError()) {
    case (LWMQTT_BUFFER_TOO_SHORT):
      Serial.println("LWMQTT_BUFFER_TOO_SHORT");
      break;
    case (LWMQTT_VARNUM_OVERFLOW):
      Serial.println("LWMQTT_VARNUM_OVERFLOW");
      break;
    case (LWMQTT_NETWORK_FAILED_CONNECT):
      Serial.println("LWMQTT_NETWORK_FAILED_CONNECT");
      break;
    case (LWMQTT_NETWORK_TIMEOUT):
      Serial.println("LWMQTT_NETWORK_TIMEOUT");
      break;
    case (LWMQTT_NETWORK_FAILED_READ):
      Serial.println("LWMQTT_NETWORK_FAILED_READ");
      break;
    case (LWMQTT_NETWORK_FAILED_WRITE):
      Serial.println("LWMQTT_NETWORK_FAILED_WRITE");
      break;
    case (LWMQTT_REMAINING_LENGTH_OVERFLOW):
      Serial.println("LWMQTT_REMAINING_LENGTH_OVERFLOW");
      break;
    case (LWMQTT_REMAINING_LENGTH_MISMATCH):
      Serial.println("LWMQTT_REMAINING_LENGTH_MISMATCH");
      break;
    case (LWMQTT_MISSING_OR_WRONG_PACKET):
      Serial.println("LWMQTT_MISSING_OR_WRONG_PACKET");
      break;
    case (LWMQTT_CONNECTION_DENIED):
      Serial.println("LWMQTT_CONNECTION_DENIED");
      break;
    case (LWMQTT_FAILED_SUBSCRIPTION):
      Serial.println("LWMQTT_FAILED_SUBSCRIPTION");
      break;
    case (LWMQTT_SUBACK_ARRAY_OVERFLOW):
      Serial.println("LWMQTT_SUBACK_ARRAY_OVERFLOW");
      break;
    case (LWMQTT_PONG_TIMEOUT):
      Serial.println("LWMQTT_PONG_TIMEOUT");
      break;
    default:
      Serial.println("This error code should never be reached.");
      break;
  }
}

void CloudIoTCoreMqtt::logConfiguration(bool showJWT) {
  Serial.println("Connect with " + String(CLOUD_IOT_CORE_MQTT_HOST_LTS) +
      ":" + String(CLOUD_IOT_CORE_MQTT_PORT));
  Serial.println("ClientId: " + device->getClientId());
  if (showJWT) {
    Serial.println("JWT: " + getJwt());
  }
}

void CloudIoTCoreMqtt::logReturnCode() {
  Serial.println(this->mqttClient->returnCode());
  switch(this->mqttClient->returnCode()) {
    case (LWMQTT_CONNECTION_ACCEPTED):
      Serial.println("OK");
      break;
    case (LWMQTT_UNACCEPTABLE_PROTOCOL):
      Serial.println("LWMQTT_UNACCEPTABLE_PROTOCOLL");
      break;
    case (LWMQTT_IDENTIFIER_REJECTED):
      Serial.println("LWMQTT_IDENTIFIER_REJECTED");
      break;
    case (LWMQTT_SERVER_UNAVAILABLE):
      Serial.println("LWMQTT_SERVER_UNAVAILABLE");
      break;
    case (LWMQTT_BAD_USERNAME_OR_PASSWORD):
      Serial.println("LWMQTT_BAD_USERNAME_OR_PASSWORD");
      iat = 0; // Force JWT regeneration
      break;
    case (LWMQTT_NOT_AUTHORIZED):
      Serial.println("LWMQTT_NOT_AUTHORIZED");
      iat = 0; // Force JWT regeneration
      break;
    case (LWMQTT_UNKNOWN_RETURN_CODE):
      Serial.println("LWMQTT_UNKNOWN_RETURN_CODE");
      break;
    default:
      Serial.println("This return code should never be reached.");
      break;
  }
}

void CloudIoTCoreMqtt::onConnect() {
  if (logConnect) {
    publishState("connected");
    publishTelemetry("/events", device->getDeviceId() + String("-connected"));
  }
}

void CloudIoTCoreMqtt::setLogConnect(boolean enabled) {
  this->logConnect = enabled;
}

void CloudIoTCoreMqtt::setUseLts(boolean enabled) {
  this->useLts = enabled;
}

void CloudIoTCoreMqtt::setUse443Port(boolean enabled) {
  this->use443Port = enabled;
}
