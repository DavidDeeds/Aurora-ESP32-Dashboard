# Aurora ESP32 appliance — features (firmware 0.3.0)

This file describes the **current** behaviour of the firmware in `main/`.

## Hardware and role

- **Board:** Waveshare ESP32-S3 RS485/CAN industrial module (see vendor wiki for exact variant, flash, and PSRAM).
- **Purpose:** Poll Aurora/Power-One class inverters over **RS-485** (Aurora protocol), expose a **local web UI** on the LAN, optionally upload to **PVOutput**, and retain short-term samples for charts.

## Web interface

- **Session login:** HTML form on `/` with **HttpOnly** session cookie (`aurora_sid`). Fixed username **Admin**; password is a **PBKDF2** verifier in NVS (factory default documented in install materials).
- **Home / dashboard:** After sign-in, the root page shows status, links to charts and settings, PVOutput form, admin password change, and device restart.
- **Charts:** `GET /dashboard` loads **Chart.js** from `GET /static/chart.umd.min.js` (embedded in firmware) and plots data from the samples API.
- **Branding:** `GET /favicon.ico` serves the Aurora logo PNG (used as the site favicon and elsewhere in the UI).
- **Settings pages (HTML):** Network (`/network-settings`), time & timezone (`/time-settings`), inverter (`/inverter-settings`), maintenance (`/maintenance`). All require a valid session.

## Machine-readable data (GET JSON)

Only these paths return **live device / metric JSON** for external clients:

| Path | Purpose |
|------|---------|
| `GET /api/status` | Snapshot: Wi-Fi, SNTP, inverter health, PVOutput state, heap, uptime, UI hints, firmware build metadata, etc. Requires session; returns JSON session error when not signed in. |
| `GET /api/samples` | Downsampled (and optionally “today” bounded) sample series for charts. Query parameters such as `limit`, `today=1`, and time ranges as implemented in code. Requires session. |
| `GET /api/samples/raw.csv` | Today’s **10 s** samples from the RAM ring as CSV (local timezone). Requires session. |
| `GET /api/maintenance/log.txt` | In-RAM diagnostic log (plain text). Requires session. |

Other **`/api/...`** routes are **POST** (forms, actions, tests) or return **non-data** responses (e.g. small JSON acks). Inverter clock comparison on the inverter settings page is **embedded in the HTML** (no separate clock GET API).

## POST actions (configuration and control)

All require a session unless noted:

- **`POST /api/auth/login`** — establish session (public).
- **`POST /api/auth/logout`** — clear session cookie.
- **`POST /api/config/pvoutput`** — save PVOutput credentials/options.
- **`POST /api/pvoutput/test`** — test PVOutput connectivity.
- **`POST /api/config/device`** — save timezone and related device prefs.
- **`POST /api/config/network`** — Wi-Fi, optional static IPv4, hostname, NTP; triggers restart when saved.
- **`POST /api/restart`** — schedule reboot.
- **`POST /api/auth/password`** — change admin password (invalidates current session cookie).
- **`POST /api/maintenance/log/clear`** — clear in-RAM maintenance log buffer.
- **`POST /api/maintenance/cpu-freq`** — save CPU clock (80 / 160 / 240 MHz) and restart.
- **`POST /api/maintenance/factory-reset`** — erase NVS, daylog, PHY/coredump data; reboot.
- **`POST /api/inverter/time/sync`** — set inverter clock from device time.
- **`POST /api/inverter/rs485`** — save inverter RS-485 address (1–127).
- **`POST /api/inverter/partial-reset`** — inverter partial energy reset (maintenance).

## Time and storage

- **SNTP** after Wi-Fi; timezone from NVS; optional **PCF85063 RTC** seeding and write-back when configured in build.
- **Samples:** Ring buffer in RAM with optional **LittleFS** `daylog` partition for ~2-day retention (see partition table and logs if mount fails).
- **Flash layout:** Factory app partition, `daylog` (LittleFS), coredump region — no separate SPIFFS “storage” partition in current layouts.

## PVOutput

- Configurable system ID, API key, upload interval, and related flags (see forms and NVS keys in source).
- Upload task respects connectivity and backs off on errors (details in `app_main.c` and related modules).

## Security notes (local LAN appliance)

- HTTPS is not used on the device (typical home LAN deployment).
- Session cookie is **HttpOnly** and **SameSite=Lax**; use on a trusted network.

---

## Future features (ideas and implementation notes)

1. **HTTPS / TLS on the web UI**  
   - **Needs:** mbedTLS server context, certificate storage (NVS or upload flow), RAM/CPU budget on ESP32-S3, and browser trust (self-signed install or user-provided cert).

2. **Raw CSV or file export of samples**  
   - **Needs:** New `GET` route (session-protected), streaming or chunked response, bounded time range to avoid OOM; optional SD card if added to hardware.

3. **OTA firmware updates**  
   - **Needs:** Second OTA app partition in `partitions.csv`, esp_https_ota or custom image verify, rollback strategy, and clear user warning (this project previously targeted USB-only flash).

4. **RS-485 baud / parity UI**  
   - **Needs:** NVS fields, UART reinit, validation against Aurora-supported modes, and poller restart without wedging the bus.

5. **CSRF tokens for forms**  
   - **Needs:** Per-session token in HTML hidden fields, validation on each POST, and cache policy review for pages with forms.

6. **Separate `/api/health` or public read-only status**  
   - **Needs:** Product decision: today consolidated diagnostics live under `/api/status` behind login; a public subset would require careful data minimisation (no secrets, no PII).

7. **Multi-user or OAuth**  
   - **Needs:** User store, role model, and substantially larger web stack; out of scope for current single-admin model.
