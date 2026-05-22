# BL618 WiFi Speed Test

A Zephyr sample that starts a WiFi AP and serves a browser-based speed test UI
over HTTP.

## What it does

1. Brings up a WiFi AP (`BL618_SpeedTest` / `SpeedTest42`)
2. Assigns itself `10.15.84.1` and runs a DHCPv4 server
3. Serves a single-page speed test at `http://10.15.84.1/`
4. Exposes `/api/download` (GET, 2 MB) and `/api/upload` (POST) endpoints

## Build

```bash
./build.sh
```

Or manually:

```bash
west build -p -b bl618g0 .
```

## Flash

```bash
west flash
```
