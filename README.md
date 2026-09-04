# OpenHAB MQTT LED Marquee

ESP8266 firmware for a MAX7219 LED matrix that **only** joins Wi-Fi, talks MQTT, and renders text.

**OpenHAB owns all content** — time strings, weather, temperatures, and item status. This device is not a content client.

## Removed from the original Qrome marquee-scroller

This fork **does not** include, compile, or configure:

- OpenWeatherMap
- OctoPrint
- Pi-hole
- News API
- TimeZoneDB / OpenWeather “city time”

There are no weather, printer, ad-block, or headline API keys.

Upstream project: [Qrome/marquee-scroller](https://github.com/Qrome/marquee-scroller) (clock/weather/news firmware). This fork is an OpenHAB MQTT display.

## Hardware

Do not change the wiring.

- Wemos D1 Mini / LOLIN D1 R2 & mini, ESP8266, **4MB flash**, **FS:1MB OTA:~1019KB**
- MAX7219 daisy chain (default 4×1, configurable 1–16)

| Display | D1 Mini     |
|---------|-------------|
| CLK     | D5 (SCK)    |
| CS      | D6          |
| DIN     | D7 (MOSI)   |
| VCC     | 5V          |
| GND     | GND         |

![Wiring](images/marquee_scroller_pins.png)

## What the ESP does

- Captive portal (WiFiManager) if it has no network
- MQTT subscribe for text, mode, brightness, speed, enable
- Scroll, center, or NTP `HH:MM` fallback
- Web UI + OTA
- Last text persisted on LittleFS (`/conf.txt`)

Idle clock uses `configTime()` and a POSIX timezone string (default `UTC0`). Send preformatted time in the MQTT text when OpenHAB should own the clock line.

## Arduino IDE flash

1. Board: **LOLIN(WEMOS) D1 R2 & mini**
2. ESP8266 core **3.0.2** (3.1.x usually works)
3. Flash size: **4MB (FS:1MB OTA:~1019KB)**
4. Open `marquee/marquee.ino`

Libraries:

- [WiFiManager](https://github.com/tzapu/WiFiManager) (tzapu, latest — not an old HTTP_HEAD fork)
- [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library)
- [Max72xxPanel](https://github.com/markruys/arduino-Max72xxPanel)
- [PubSubClient](https://github.com/knolleary/pubsubclient)

`Settings.h` is first-boot defaults only. MQTT host default is **empty** (NTP clock until you set a broker). Default web user/password: `admin` / `password` (basic auth off until you enable it). Saving the form does not wipe `mqtt_*` or panel keys.

First boot: join AP **`MARQUEE-<chipid>`**, open `http://192.168.4.1`, enter Wi-Fi. The matrix then scrolls `v4.00 IP: …`. Use that IP for the config page.

## MQTT contract

Default prefix: `openhab/marquee`  
Default clientId: `marquee-<chipid>`  
Port **1883**. Empty user/pass = anonymous.

### Subscribe

| Topic | Payload |
|-------|---------|
| `openhab/marquee/text` and `…/text/set` | Plain text, or JSON `{"message":"..."}` / `{"text":"..."}` |
| `openhab/marquee/clear` | Clears text (any payload) |
| `openhab/marquee/mode` | `scroll` \| `center` \| `clock` |
| `openhab/marquee/brightness` | `0`–`15` (values `16`–`100` mapped as percent) |
| `openhab/marquee/speed` | Scroll delay ms, clamped 10–200 |
| `openhab/marquee/enable` | `ON`/`OFF`/`1`/`0`/`true`/`false` |

### Publish

| Topic | Payload |
|-------|---------|
| `openhab/marquee/status` | Retained `online`; LWT `offline` |
| `openhab/marquee/ip` | Retained LAN IP after connect |

Display rules:

- New text replaces the current payload, is saved to flash, and is shown immediately (max 240 chars).
- Empty payload or `/clear` clears MQTT text.
- `scroll` = marquee, `center` = static, `clock` = local NTP `HH:MM`.
- Empty text with scroll/center → NTP clock if synced, else `OH wait`.
- MQTT disabled or empty host → NTP clock only.
- Broker down → last text or clock; reconnect backoff 2 / 5 / 15 / 30 s. The render loop is not blocked.
- Unknown glyphs → `?`. `°` is mapped to a drawable stand-in.
- MQTT + web + OTA are pumped while scrolling.

## OpenHAB 4 / 5 copy-paste

### Thing (`*.things`)

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

Set `host` to your Mosquitto address if it is not on the OpenHAB machine.

### Items (`*.items`)

```java
String Marquee_Text       { channel="mqtt:topic:mosquitto:marquee:text" }
String Marquee_Mode       { channel="mqtt:topic:mosquitto:marquee:mode" }
Dimmer Marquee_Brightness { channel="mqtt:topic:mosquitto:marquee:brightness" }
Switch Marquee_Enable     { channel="mqtt:topic:mosquitto:marquee:enable" }
String Marquee_Status     { channel="mqtt:topic:mosquitto:marquee:status" }
```

### JS Scripting rule

Trigger on the items that should refresh the line, **and** every minute if you want a live clock in the same string:

```javascript
Marquee_Text.sendCommand("14:32  Out 18.4C Cloudy  In 21.2C  Garage CLOSED");
```

Example: build the string from your own Number/String/Contact items (weather, temperatures, garage). Do **not** point the ESP at OpenWeatherMap.

Set mode once:

```javascript
Marquee_Mode.sendCommand("scroll");
```

## mosquitto_pub tests

```bash
mosquitto_pub -h BROKER -t openhab/marquee/text/set -m "Living 21.4C  Garage OPEN"
mosquitto_pub -h BROKER -t openhab/marquee/mode -m center
mosquitto_pub -h BROKER -t openhab/marquee/text/set -m ""
```

Anonymous broker: omit `-u` / `-P`. With auth: `-u USER -P PASS`.

JSON is also accepted:

```bash
mosquitto_pub -h BROKER -t openhab/marquee/text/set -m '{"text":"Hall 20.1C"}'
```

## Web UI

Single page at `http://<device-ip>/`:

- Wi-Fi forget / factory reset
- MQTT: enabled, host, port, user, password (blank keeps stored), clientId, prefix
- Display: panel count, brightness, speed, mode, test text
- NTP: enabled, POSIX TZ
- OTA (`/update`), display on/off
- Optional basic auth

## License

MIT. Original work by David Payne; this fork strips content APIs and adds the OpenHAB MQTT contract.
