/** The MIT License (MIT)
 * Copyright (c) 2018 David Payne
 * OpenHAB MQTT marquee conversion: 2026
 */

/******************************************************************************
 * Wemos D1 Mini / LOLIN D1 R2 & mini + MAX7219 daisy chain.
 * CLK -> D5 (SCK)  CS -> D6  DIN -> D7 (MOSI)  VCC -> 5V  GND -> GND
 *
 * Values here are first-boot defaults only. After that, change settings in
 * the web UI. Use Reset Settings to return to these defaults.
 ******************************************************************************/

#ifndef MARQUEE_SETTINGS_H
#define MARQUEE_SETTINGS_H

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
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

// MAX7219
const int pinCS = D6;
int numberOfHorizontalDisplays = 4;  // 1–16, persisted; changing it reboots
const int numberOfVerticalDisplays = 1;
int ledRotation = 3;  // 3 = 90° CCW (typical 4-in-1 FC-16)
int displayIntensity = 1;     // 0–15
int displayScrollSpeed = 25;  // ms per pixel, clamped 10–200

boolean MQTT_ENABLED = true;
String mqttHost = "";          // empty = MQTT off (NTP clock only)
int mqttPort = 1883;
String mqttUser = "";
String mqttPass = "";
String mqttClientId = "";      // empty = marquee-<chipid>
String mqttPrefix = "openhab/marquee";

boolean ntpEnabled = true;
String posixTZ = "UTC0";       // POSIX TZ; OpenHAB should send preformatted time in MQTT text

String marqueeMessage = "";    // last MQTT / test text, max 240
String displayMode = "scroll"; // scroll | center | clock
boolean displayOn = true;

//******************************
// End Settings
//******************************

#endif
