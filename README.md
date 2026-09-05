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
- Optional HTTP JSON poll (Prometheus, Grafana proxy, OpenHAB REST, any JSON)
- Optional `POST /ingest` webhook (Grafana Alerting, curl, Node-RED)
- Scroll / center / NTP clock, playlist of extra lines
- Web UI + OTA + mDNS `http://marquee.local`
- Last text persisted on LittleFS (`/conf.txt`)

Idle clock uses `configTime()` and a POSIX timezone string (default `UTC0`). Send preformatted time in MQTT or HTTP text when OpenHAB/Grafana should own the clock line.

The ESP does **not** speak Grafana’s dashboard query API. It pulls a small JSON/text URL or accepts a webhook. Compose full sentences in OpenHAB, Grafana alert titles, or a tiny HTTP endpoint.

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

First boot: join AP **`MARQUEE-<chipid>`**, open `http://192.168.4.1`, enter Wi-Fi. The matrix then scrolls `v4.10 IP: …`. Then use `http://marquee.local` or the LAN IP.

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
- MQTT disabled or empty host → HTTP line and/or NTP clock.
- Broker down → last text or clock; reconnect backoff 2 / 5 / 15 / 30 s. The render loop is not blocked.
- Unknown glyphs → `?`. `°` is mapped to a drawable stand-in.
- MQTT + web + OTA + mDNS are pumped while scrolling.
- Display line = MQTT text + HTTP poll text + playlist lines 2 and 3 (empty parts skipped).

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

`http://<device-ip>/` or `http://marquee.local`

Live header: current line, MQTT/HTTP status, last error, RSSI, day/night brightness, next poll.

- **Identify panels** — lights each MAX7219 module in order (module 1 first)
- Display: rotation 0–3, reverse chain, invert, day/night brightness + schedule, speed, pause after scroll, direction, loop vs once-then-clock, mode, 12/24h, colon blink, MQTT text, playlist lines
- MQTT (unchanged contract)
- HTTP / Grafana poll + ingest token
- NTP, OTA, forget Wi-Fi, factory reset, optional basic auth

Saving does not wipe `mqtt_*`, HTTP, or panel keys. Blank passwords keep the stored value.

## HTTP poll (universal JSON)

LAN **HTTP only** (no TLS on this chip). Interval 15–300 s. Response capped at 3 KB.

| Field | Example |
|-------|---------|
| URL | `http://prometheus:9090/api/v1/query?query=sensor_temp` |
| JSON path | `data.result.0.value.1` (comma-separate up to 3 paths) |
| Template | `Out {value}C` (`{value}`, `{value2}`, `{value3}`) |
| On failure | keep last / clear / show `HTTP err` |

Prometheus (simplest Grafana-adjacent source):

```
http://PROMETHEUS:9090/api/v1/query?query=weather_temp_c
JSON path: data.result.0.value.1
Template: Out {value}C
```

Grafana as a datasource **proxy** (same Prometheus JSON shape). Use an HTTP Grafana URL on the LAN, service account token as Bearer:

```
http://GRAFANA:3000/api/datasources/proxy/DS_ID/api/v1/query?query=weather_temp_c
JSON path: data.result.0.value.1
```

`DS_ID` is the numeric datasource id (Grafana → Connections → your Prometheus → URL bar id). Do not use `/api/ds/query` dashboard frames; they are too large for the ESP.

OpenHAB REST example: poll an item JSON and path `state`, template `{value}`.

## Grafana webhook (push)

Grafana **Alerting → Contact point → Webhook**:

```
http://192.168.1.216/ingest
```

If you set an ingest token in the UI:

```
http://192.168.1.216/ingest?token=YOURTOKEN
```

or header `Authorization: Bearer YOURTOKEN`.

The firmware reads JSON `text`, then `message`, then `title` (Grafana alert title). Keep the contact-point message short.

```bash
curl -X POST http://192.168.1.216/ingest \
  -H "Content-Type: application/json" \
  -d "{\"text\":\"14:32  Out 18.4C  Garage CLOSED\"}"
```

Prefer OpenHAB MQTT for the always-on line; use Grafana alerts for exceptions.

## License

MIT. Original work by David Payne; this fork strips content APIs and adds the OpenHAB MQTT contract.
