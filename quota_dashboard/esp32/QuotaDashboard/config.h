#pragma once

static const char *WIFI_SSID = "fes_wan";
static const char *WIFI_PASSWORD = "Fes_wan_123$";
static const char *QUOTA_URL = "http://192.168.168.138:8765/api/quota.txt";
static const char *WEATHER_URL = "http://192.168.168.138:8765/api/weather.txt";

static const long TIMEZONE_OFFSET_SECONDS = 8L * 3600L;
static const unsigned long FETCH_INTERVAL_MS = 5UL * 60UL * 1000UL;
static const unsigned long PAGE_INTERVAL_MS = 5UL * 60UL * 1000UL;
