You are converting this fork of https://github.com/Qrome/marquee-scroller into an OpenHAB MQTT LED marquee.

## Hardware (do not change)
- Wemos D1 Mini / LOLIN D1 R2 & mini, ESP8266, 4MB flash, FS:1MB OTA:~1019KB.
- MAX7219 already wired: CLK=D5 (SCK), CS=D6, DIN=D7 (MOSI), VCC=5V, GND=GND.
- Keep WiFiManager, Basic Auth on the config UI, OTA, and Max72xxPanel rendering.

## Goal
OpenHAB owns all content: time text, weather, temperatures, item status.
The ESP only joins Wi-Fi, talks MQTT, and renders text (scroll / center / clock fallback).

## Remove completely
Do not compile, configure, or document these:
- OpenWeatherMap (OpenWeatherMapClient.*, API keys, city IDs, weather refresh, weather UI)
- OctoPrint (OctoPrintClient.*, printer progress, octoprint UI/routes)
- Pi-hole (PiHoleClient.*, graphs/status, pihole UI/routes)

Also remove News API if it is still in the tree (NewsApiClient.*, headlines UI). This device must not call content APIs.

Delete related web routes (`/configurenews`, `/configureoctoprint`, `/configurepihole`, weather location/city forms that exist only for OWM) and their save handlers and FS keys.

## Keep / replace
- Matrix scroll + center print
- Web config + FS settings + OTA
- MQTT client
- NTP via `configTime()` for idle clock only. No TimeZoneDB/OpenWeather “city time”.
- POSIX timezone string in settings, default `UTC0`. OpenHAB should send preformatted time in the MQTT text when it wants a specific clock line.

## MQTT contract
Default prefix: `openhab/marquee`
Default clientId: `marquee-<chipid>`
Port 1883. Empty user/pass = anonymous.

Subscribe:
- `openhab/marquee/text` and `openhab/marquee/text/set`
  plain text, or JSON `{"message":"..."}` / `{"text":"..."}`
- `openhab/marquee/clear` — clear text
- `openhab/marquee/mode` — `scroll` | `center` | `clock`
- `openhab/marquee/brightness` — 0–15
- `openhab/marquee/speed` — scroll delay ms, clamp ~10–200
- `openhab/marquee/enable` — ON/OFF/1/0/true/false

Publish:
- `openhab/marquee/status` retained `online`, LWT `offline`
- `openhab/marquee/ip` retained after connect

Display:
- New text replaces current payload, persist to FS, show immediately.
- Empty payload or `/clear` clears MQTT text.
- `scroll` = marquee; `center` = static; `clock` = local NTP `HH:MM`.
- Empty text + scroll/center → NTP clock if synced, else `OH wait`.
- Pump MQTT + `server.handleClient()` during scroll so OTA and keepalive live.
- Store up to 240 chars. Unknown glyphs → `?`. Map `°` to a drawable stand-in if needed.
- Reconnect backoff 2/5/15/30s. Broker down → last text or clock. Do not block the render loop.

## Web UI
Single page:
- Wi-Fi forget / factory reset
- MQTT: enabled, host, port, user, password (blank keeps stored), clientId, prefix
- Display: panel count, brightness, speed
- NTP: enabled, POSIX TZ
- Current text (test box optional)
- OTA, display on/off

Settings.h is defaults only. Persist with existing FS key=value. Saving must not wipe mqtt_* / panel keys. Default basic auth stays. mqtt host default empty.

## README must include OpenHAB 4/5 copy-paste

```java
Bridge mqtt:broker:mosquitto [ host="127.0.0.1", secure=false ] {
    Thing topic marquee "LED Marquee" {
        Channels:
            Type string : text       [ commandTopic="openhab/marquee/text/set" ]
            Type string : mode       [ commandTopic="openhab/marquee/mode" ]
            Type dimmer : brightness [ commandTopic="openhab/marquee/brightness" ]
            Type switch : enable     [ commandTopic="openhab/marquee/enable", on="ON", off="OFF" ]
            Type string : status     [ stateTopic="openhab/marquee/status" ]
    }
}
```

```java
String Marquee_Text       { channel="mqtt:topic:mosquitto:marquee:text" }
String Marquee_Mode       { channel="mqtt:topic:mosquitto:marquee:mode" }
Dimmer Marquee_Brightness { channel="mqtt:topic:mosquitto:marquee:brightness" }
Switch Marquee_Enable     { channel="mqtt:topic:mosquitto:marquee:enable" }
String Marquee_Status     { channel="mqtt:topic:mosquitto:marquee:status" }
```

JS rule: on item changes + every minute, `Marquee_Text.sendCommand("14:32  Out 18.4C Cloudy  In 21.2C  Garage CLOSED")`.

```
mosquitto_pub -h BROKER -t openhab/marquee/text/set -m "Living 21.4C  Garage OPEN"
mosquitto_pub -h BROKER -t openhab/marquee/mode -m center
mosquitto_pub -h BROKER -t openhab/marquee/text/set -m ""
```

State clearly that OpenWeatherMap, OctoPrint, and Pi-hole support were removed.

## Implementation order
1. Inventory `marquee.ino`, Settings.h, FS read/write, web handlers, scroll/center, loop.
2. Delete OWM, OctoPrint, Pi-hole (and News) clients, UI, settings keys, loop refresh.
3. Add MQTT settings, UI, client, LWT, topics, persist last text.
4. NTP-only time fallback.
5. Loop = Wi-Fi + MQTT + web + render mode. No weather/printer/pihole timers.
6. Confirm Arduino IDE flash: d1_mini, 4MB FS:1MB.
7. Rewrite README as OpenHAB MQTT Marquee. No secrets in git.

## Quality
- Builds on ESP8266 core 3.0.2.
- Mosquitto anonymous or user/pass.
- Survives broker restart and Wi-Fi drop.
- OTA works while scrolling.
- MQTT disabled or empty host → local NTP clock only.

Start by inventorying the repo, strip OWM/OctoPrint/Pi-hole, then add MQTT and the simplified UI.
