# ESP32 WiFi Config Portal

An ESP-IDF project that runs the ESP32 in simultaneous **SoftAP + Station** mode and serves a **password-protected web configuration portal** over the AP interface. Users can connect to the ESP32's own WiFi network, open a browser, and configure the ESP32's uplink (office/home) WiFi connection — no USB cable or serial terminal needed.

| Supported Targets | ESP32 | ESP32-C3 | ESP32-C6 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- |

---

## Features

- **Always-on SoftAP** — connect any phone or laptop to configure the device
- **Login page** — protected by username + password (default: `admin` / `123456`)
- **WiFi network scan** — lists nearby networks with signal strength and security type
- **One-click connect** — tap a network from the scan list, enter password, connect
- **Manual SSID entry** — type an SSID directly if it doesn't appear in the scan
- **Live connection status** — WiFi state (Connected / Connecting / Failed) + assigned IP
- **Internet availability check** — background task probes `8.8.8.8:53` every 5 seconds
- **Status auto-refresh** — page polls `/api/status` every 5 seconds via JavaScript
- **NVS persistence** — configured WiFi credentials survive power cycles
- **NAPT routing** — clients connected to the ESP32 AP can reach the internet through the STA link

---

## Default Credentials

| Setting | Value |
|---------|-------|
| AP SSID | `ESP32-Config` |
| AP Password | `esp32config` |
| Web Username | `admin` |
| Web Password | `123456` |
| Portal URL | `http://192.168.4.1/` |

---

## How to Use

### 1. Build and Flash

```bash
idf.py build flash monitor
```

### 2. Connect to the ESP32 Access Point

On your phone or laptop, connect to WiFi:
- **SSID:** `ESP32-Config`
- **Password:** `esp32config`

### 3. Open the Portal

Navigate to **http://192.168.4.1/** in a browser.
Log in with `admin` / `123456`.

### 4. Configure Office WiFi

1. Tap **Scan for Networks** to see nearby APs
2. Select the target network (or type the SSID manually)
3. Enter the password
4. Tap **Connect**

The ESP32 saves the credentials to NVS and connects. On every subsequent boot it reconnects automatically.

---

## HTTP API

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/` | Redirect to login or config |
| `GET` | `/login` | Login page |
| `POST` | `/login` | Authenticate (form: `username`, `password`) |
| `GET` | `/config` | Main configuration page (session required) |
| `GET` | `/logout` | Invalidate session |
| `POST` | `/api/scan` | Trigger WiFi scan; returns JSON array of APs |
| `POST` | `/api/connect` | Connect to SSID (form: `ssid`, `password`) |
| `GET` | `/api/status` | Returns JSON with WiFi + internet status |

### `/api/status` response example
```json
{
  "wifi": "connected",
  "ssid": "OfficeWiFi",
  "ip": "192.168.1.42",
  "internet": true
}
```

### `/api/scan` response example
```json
[
  {"ssid": "OfficeWiFi", "rssi": -55, "auth": 3},
  {"ssid": "GuestNet",   "rssi": -72, "auth": 0}
]
```
`auth: 0` = open network, `auth > 0` = secured (shown with a lock icon).

---

## Project Structure

```
softap_sta_my/
├── main/
│   ├── softap_sta.c        # All application logic + web server
│   ├── CMakeLists.txt
│   └── Kconfig.projbuild
├── CMakeLists.txt
├── sdkconfig.defaults      # Key config overrides (NAPT, HTTP header size)
└── README.md
```

---

## Configuration Notes

`sdkconfig.defaults` sets the following non-default values:

| Key | Value | Reason |
|-----|-------|--------|
| `CONFIG_LWIP_IP_FORWARD` | `y` | Enable IP forwarding for NAT router mode |
| `CONFIG_LWIP_IPV4_NAPT` | `y` | Enable NAPT (NAT) on the AP interface |
| `CONFIG_HTTPD_MAX_REQ_HDR_LEN` | `2048` | Mobile browsers send large request headers |
| `CONFIG_HTTPD_MAX_URI_LEN` | `1024` | Avoid URI truncation |

---

## License

This project is based on the Espressif `softap_sta` example (Unlicense / CC0-1.0) and is released under the same terms.
