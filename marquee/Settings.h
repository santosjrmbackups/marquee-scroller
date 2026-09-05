/** The MIT License (MIT)
 * Copyright (c) 2018 David Payne
 * OpenHAB MQTT marquee conversion: 2026
 */

/******************************************************************************
 * Wemos D1 Mini / LOLIN D1 R2 & mini + MAX7219 daisy chain.
 * CLK -> D5 (SCK)  CS -> D6  DIN -> D7 (MOSI)  VCC -> 5V  GND -> GND
 *
 * Values here are first-boot defaults only. After that, change settings in
 * the web UI. Use Factory reset to return to these defaults.
 ******************************************************************************/

#ifndef MARQUEE_SETTINGS_H
#define MARQUEE_SETTINGS_H

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Max72xxPanel.h>
#include <PubSubClient.h>
#include <time.h>

//******************************
// Start Settings (first boot)
//******************************

const int WEBSERVER_PORT = 80;
const boolean WEBSERVER_ENABLED = true;
boolean IS_BASIC_AUTH = false;
String www_username = "admin";
String www_password = "password";

boolean ENABLE_OTA = true;
String OTA_Password = "";

const int pinCS = D6;
int numberOfHorizontalDisplays = 4;
const int numberOfVerticalDisplays = 1;
int ledRotation = 3;
boolean reverseChain = false;
boolean invertDisplay = false;
int displayIntensity = 1;
int nightIntensity = 0;
String nightStart = "";
String nightEnd = "";
int displayScrollSpeed = 25;
int scrollPauseSec = 0;
boolean scrollLeft = true;
boolean scrollLoop = true;
boolean IS_24HOUR = true;
boolean flashOnSeconds = true;

boolean MQTT_ENABLED = true;
String mqttHost = "";
int mqttPort = 1883;
String mqttUser = "";
String mqttPass = "";
String mqttClientId = "";
String mqttPrefix = "openhab/marquee";

boolean ntpEnabled = true;
String posixTZ = "UTC0";

String marqueeMessage = "";
String displayMode = "scroll";
boolean displayOn = true;

String playlist2 = "";
String playlist3 = "";

boolean httpPollEnabled = false;
String httpUrl = "";
int httpIntervalSec = 60;
String httpJsonPath = "";
String httpTemplate = "{value}";
String httpUser = "";
String httpPass = "";
String httpBearer = "";
String httpOnFail = "keep";
String ingestToken = "";

//******************************
// End Settings
//******************************

#endif
