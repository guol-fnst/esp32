#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#include <esp_sleep.h>

#include "display_bsp.h"
#include "lvgl_bsp.h"
#include "adc_bsp.h"
#include "config.h"

// GPIO0 is the BOOT button on most ESP32-S3 boards
static const int BUTTON_PIN = 0;

static const int SCREEN_WIDTH = 400;
static const int SCREEN_HEIGHT = 300;
static const int MAX_ROWS = 24;
static const int VISIBLE_ROWS = 4;  // row 5 is reserved for weather
static const int WEATHER_DAYS = 5;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000UL;
static const unsigned long WIFI_RETRY_INTERVAL_MS = 15000UL;
static const unsigned long FETCH_RETRY_INTERVAL_MS = 30000UL;

// Row panel layout
static const int ROW_H = 50;              // panel height px
static const int HEADER_H = 28;          // top header bar height
static const int FOOTER_H = 22;          // bottom footer bar height
static const int ARC_SIZE = 44;          // CX arc and AG outer (Gemini) arc size
static const int ARC_INNER_SIZE = 28;    // AG inner (Claude) arc size
static const int ARC_LEFT_PAD = 4;       // panel padding before/after arcs
static const int ARC_INNER_OFS = -(ARC_LEFT_PAD + (ARC_SIZE - ARC_INNER_SIZE) / 2); // = -12, concentric
static const int TEXT_X = ARC_SIZE + ARC_LEFT_PAD + 6;  // text block start x (after CX arc)
static const int TEXT_W_COMBINED = 390 - TEXT_X - ARC_SIZE - ARC_LEFT_PAD * 2 - 4; // middle width

struct DashboardRow {
  char platform[4];
  char email[49];
  char metric1[17];
  char metric2[17];
  char reset[17];
  long sortKey;  // seconds until CX reset (9999999 = no data)
};

DisplayPort RlcdPort(12, 11, 5, 40, 41, SCREEN_WIDTH, SCREEN_HEIGHT);
DashboardRow rows[MAX_ROWS];
int rowCount = 0;
bool g_isHoliday = false;  // set by server HOLIDAY line each fetch
char lastSyncText[25] = "-";
char errorText[96] = "";

time_t baseEpoch = 0;
unsigned long baseEpochMillis = 0;
unsigned long lastFetchMs = 0;
unsigned long lastPageMs = 0;
unsigned long lastClockMs = 0;
unsigned long lastWifiAttemptMs = 0;
int currentPage = 0;
bool lastFetchOk = false;

// Button debounce
static unsigned long buttonPressMs = 0;
static bool buttonLastState = HIGH;

lv_obj_t *timeLabel = nullptr;
lv_obj_t *wifiLabel = nullptr;
lv_obj_t *batteryLabel = nullptr;
lv_obj_t *footerLabel = nullptr;
lv_obj_t *errorLabel = nullptr;
lv_obj_t *rowPanels[VISIBLE_ROWS];
lv_obj_t *rowTitleLabels[VISIBLE_ROWS];
lv_obj_t *rowDetailLabels[VISIBLE_ROWS];
lv_obj_t *rowCxArcs[VISIBLE_ROWS];
lv_obj_t *rowCxLabels[VISIBLE_ROWS];
lv_obj_t *rowAgOuterArcs[VISIBLE_ROWS];  // Gemini (outer)
lv_obj_t *rowAgInnerArcs[VISIBLE_ROWS]; // Claude (inner)

// Weather panel (bottom row)
struct WeatherDay {
  char date[6];  // "04/09"
  char temp[8];  // "25/12"
  int  code;     // WMO weather code
};
static WeatherDay g_weather[WEATHER_DAYS];
static bool g_weatherValid = false;
static int g_currentTempC = -1000;
lv_obj_t *weatherPanel   = nullptr;
lv_obj_t *wDateLabels[WEATHER_DAYS];
lv_obj_t *wInfoLabels[WEATHER_DAYS];
lv_obj_t *wTempLabels[WEATHER_DAYS];

LV_FONT_DECLARE(lv_font_weather_cjk);

const char *wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "idle";
    case WL_NO_SSID_AVAIL: return "ssid_not_found";
    case WL_SCAN_COMPLETED: return "scan_done";
    case WL_CONNECTED: return "connected";
    case WL_CONNECT_FAILED: return "connect_failed";
    case WL_CONNECTION_LOST: return "connection_lost";
    case WL_DISCONNECTED: return "disconnected";
    default: return "unknown";
  }
}

void setErrorText(const char *text) {
  safeCopy(errorText, sizeof(errorText), text);
}

void setErrorTextf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(errorText, sizeof(errorText), fmt, args);
  va_end(args);
}

const char* wmoText(int code) {
  if (code <= 1)  return "\u6674";           // 晴
  if (code == 2)  return "\u591a\u4e91";    // 多云
  if (code == 3)  return "\u9634";           // 阴
  if (code <= 48) return "\u96fe";           // 雾
  if (code <= 57) return "\u5c0f\u96e8";   // 小雨 (drizzle)
  if (code <= 61) return "\u5c0f\u96e8";   // 小雨
  if (code <= 63) return "\u4e2d\u96e8";   // 中雨
  if (code <= 67) return "\u5927\u96e8";   // 大雨
  if (code <= 71) return "\u5c0f\u96ea";   // 小雪
  if (code <= 73) return "\u4e2d\u96ea";   // 中雪
  if (code <= 77) return "\u5927\u96ea";   // 大雪
  if (code <= 82) return "\u9635\u96e8";   // 阵雨
  if (code <= 86) return "\u9635\u96ea";   // 阵雪
  if (code <= 99) return "\u96f7\u96e8";   // 雷雨
  return "---";
}
struct CombinedRow {
  char email[49];
  int  cxScore;      // CX remaining %,       -1 = no data
  int  agGemini;     // AG Gemini remaining %, -1 = no data
  int  agClaude;     // AG Claude remaining %, -1 = no data
  char cxReset[12];  // CX reset countdown e.g. "6d23h"
  char agReset[20];  // AG reset e.g. "G:2h/C:5d7h"
  long cxResetSec;   // seconds until CX reset; 9999999 = no CX data
};
CombinedRow combined[MAX_ROWS];
int combinedCount = 0;

void extractEmailPrefix(const char *email, char *dst, size_t dstSize) {
  if (!dst || dstSize == 0) {
    return;
  }
  dst[0] = '\0';
  if (!email) {
    return;
  }

  const char *at = strchr(email, '@');
  size_t copyLen = at ? (size_t)(at - email) : strlen(email);
  if (copyLen >= dstSize) {
    copyLen = dstSize - 1;
  }
  memcpy(dst, email, copyLen);
  dst[copyLen] = '\0';
}

static void Lvgl_FlushCallback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
  uint16_t *buffer = (uint16_t *)color_map;
  for (int y = area->y1; y <= area->y2; y++) {
    for (int x = area->x1; x <= area->x2; x++) {
      uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
      int tx = SCREEN_WIDTH - 1 - x;
      int ty = SCREEN_HEIGHT - 1 - y;
      RlcdPort.RLCD_SetPixel(tx, ty, color);
      buffer++;
    }
  }
  RlcdPort.RLCD_Display();
  lv_disp_flush_ready(drv);
}

bool splitField(char *source, char **parts, int maxParts) {
  int count = 0;
  char *cursor = source;
  while (count < maxParts) {
    parts[count++] = cursor;
    char *sep = strchr(cursor, '|');
    if (!sep) {
      break;
    }
    *sep = '\0';
    cursor = sep + 1;
  }
  return count == maxParts;
}

void safeCopy(char *dst, size_t dstSize, const char *src) {
  if (!dst || dstSize == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}

void resetRows() {
  rowCount = 0;
}

int extractPercent(const char *text) {
  if (!text) {
    return -1;
  }

  // Find the '%' sign and parse the number immediately before it.
  const char *pct = strchr(text, '%');
  if (!pct || pct == text) {
    return -1;
  }

  // Walk backwards past any spaces
  const char *end = pct - 1;
  while (end > text && *end == ' ') {
    end--;
  }
  if (!isdigit((unsigned char)*end)) {
    return -1;
  }

  // Walk backwards to find start of the number
  const char *start = end;
  while (start > text && isdigit((unsigned char)*(start - 1))) {
    start--;
  }

  int value = 0;
  while (start <= end) {
    value = value * 10 + (*start - '0');
    start++;
  }

  if (value < 0) {
    value = 0;
  }
  if (value > 100) {
    value = 100;
  }
  return value;
}

int rowScore(const DashboardRow &row) {
  int p1 = extractPercent(row.metric1);
  int p2 = extractPercent(row.metric2);
  if (p1 < 0 && p2 < 0) {
    return 101;
  }
  if (p1 < 0) {
    return p2;
  }
  if (p2 < 0) {
    return p1;
  }
  return min(p1, p2);
}

// Merge raw rows by email prefix: each person gets one CombinedRow with CX + AG data.
int buildCombinedRows() {
  combinedCount = 0;
  memset(combined, 0, sizeof(combined));
  for (int i = 0; i < MAX_ROWS; i++) {
    combined[i].cxScore    = -1;
    combined[i].agGemini   = -1;
    combined[i].agClaude   = -1;
    combined[i].cxResetSec = 9999999L;
  }

  for (int i = 0; i < rowCount; i++) {
    char prefix[49];
    extractEmailPrefix(rows[i].email, prefix, sizeof(prefix));

    int idx = -1;
    for (int j = 0; j < combinedCount; j++) {
      if (strcasecmp(combined[j].email, prefix) == 0) { idx = j; break; }
    }
    if (idx < 0) {
      if (combinedCount >= MAX_ROWS) continue;
      idx = combinedCount++;
      safeCopy(combined[idx].email, sizeof(combined[idx].email), prefix);
      combined[idx].cxScore  = -1;
      combined[idx].agGemini = -1;
      combined[idx].agClaude = -1;
      combined[idx].cxReset[0]  = '\0';
      combined[idx].agReset[0]   = '\0';
      combined[idx].cxResetSec   = 9999999L;
    }

    if (strncmp(rows[i].platform, "CX", 2) == 0) {
      int score = rowScore(rows[i]);
      combined[idx].cxScore    = (score < 0 || score > 100) ? 0 : score;
      combined[idx].cxResetSec = rows[i].sortKey;
      safeCopy(combined[idx].cxReset, sizeof(combined[idx].cxReset), rows[i].reset);
    } else if (strncmp(rows[i].platform, "AG", 2) == 0) {
      int gem = extractPercent(rows[i].metric1);
      int cld = extractPercent(rows[i].metric2);
      combined[idx].agGemini = (gem < 0) ? 0 : gem;
      combined[idx].agClaude = (cld < 0) ? 0 : cld;
      safeCopy(combined[idx].agReset, sizeof(combined[idx].agReset), rows[i].reset);
    }
  }

  // Sort: soonest CX reset first (ascending cxResetSec); no-CX users sort to end
  for (int i = 1; i < combinedCount; i++) {
    CombinedRow key = combined[i];
    int j = i - 1;
    while (j >= 0 && combined[j].cxResetSec > key.cxResetSec) {
      combined[j + 1] = combined[j];
      j--;
    }
    combined[j + 1] = key;
  }

  return combinedCount;
}

bool parseDashboardPayload(String payload) {
  resetRows();
  g_isHoliday = false;  // assume workday; server overrides if today is a holiday
  int start = 0;
  while (start < payload.length()) {
    int end = payload.indexOf('\n', start);
    if (end < 0) {
      end = payload.length();
    }
    String line = payload.substring(start, end);
    line.trim();
    start = end + 1;
    if (line.length() == 0) {
      continue;
    }

    char buffer[192];
    safeCopy(buffer, sizeof(buffer), line.c_str());
    char *parts[7] = {0};

    if (strncmp(buffer, "META|", 5) == 0) {
      if (splitField(buffer, parts, 4)) {
        baseEpoch = (time_t)atoll(parts[1]);
        baseEpochMillis = millis();
        safeCopy(lastSyncText, sizeof(lastSyncText), parts[2]);
      }
      continue;
    }

    if (strncmp(buffer, "HOLIDAY|", 8) == 0) {
      g_isHoliday = (buffer[8] == '1');
      continue;
    }

    if (strncmp(buffer, "ROW|", 4) == 0) {
      if (rowCount >= MAX_ROWS) {
        continue;
      }
      if (splitField(buffer, parts, 7)) {
        safeCopy(rows[rowCount].platform, sizeof(rows[rowCount].platform), parts[1]);
        safeCopy(rows[rowCount].email, sizeof(rows[rowCount].email), parts[2]);
        safeCopy(rows[rowCount].metric1, sizeof(rows[rowCount].metric1), parts[3]);
        safeCopy(rows[rowCount].metric2, sizeof(rows[rowCount].metric2), parts[4]);
        safeCopy(rows[rowCount].reset, sizeof(rows[rowCount].reset), parts[5]);
        rows[rowCount].sortKey = atol(parts[6]);
        rowCount++;
      }
      continue;
    }

    if (strncmp(buffer, "ERR|", 4) == 0) {
      safeCopy(errorText, sizeof(errorText), buffer + 4);
    }

    if (strncmp(buffer, "WEATHER|", 8) == 0) {
      // New format: WEATHER|curTemp|MM/DD|tmax/tmin|code|... (5 days)
      // Legacy format: WEATHER|MM/DD|tmax/tmin|code|... (5 days)
      char *wpNew[2 + WEATHER_DAYS * 3] = {0};
      char *wpOld[1 + WEATHER_DAYS * 3] = {0};
      if (splitField(buffer, wpNew, 2 + WEATHER_DAYS * 3)) {
        bool ok = true;
        g_currentTempC = atoi(wpNew[1]);
        for (int i = 0; i < WEATHER_DAYS; i++) {
          int b = 2 + i * 3;
          if (!wpNew[b] || !wpNew[b + 1] || !wpNew[b + 2]) { ok = false; break; }
          safeCopy(g_weather[i].date, sizeof(g_weather[i].date), wpNew[b]);
          safeCopy(g_weather[i].temp, sizeof(g_weather[i].temp), wpNew[b + 1]);
          g_weather[i].code = atoi(wpNew[b + 2]);
        }
        if (ok) g_weatherValid = true;
      } else if (splitField(buffer, wpOld, 1 + WEATHER_DAYS * 3)) {
        bool ok = true;
        g_currentTempC = -1000;
        for (int i = 0; i < WEATHER_DAYS; i++) {
          int b = 1 + i * 3;
          if (!wpOld[b] || !wpOld[b + 1] || !wpOld[b + 2]) { ok = false; break; }
          safeCopy(g_weather[i].date, sizeof(g_weather[i].date), wpOld[b]);
          safeCopy(g_weather[i].temp, sizeof(g_weather[i].temp), wpOld[b + 1]);
          g_weather[i].code = atoi(wpOld[b + 2]);
        }
        if (ok) g_weatherValid = true;
      }
    }
  }

  return baseEpoch > 0;
}

bool fetchDashboard() {
  if (WiFi.status() != WL_CONNECTED) {
    setErrorTextf("WiFi %s", wifiStatusName(WiFi.status()));
    Serial.printf("[fetch] WiFi unavailable status=%s\n", wifiStatusName(WiFi.status()));
    return false;
  }

  HTTPClient http;
  http.begin(DASHBOARD_URL);
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    String err = http.errorToString(code);
    setErrorTextf("HTTP %d %s", code, err.c_str());
    Serial.printf("[fetch] GET failed code=%d err=%s\n", code, err.c_str());
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();
  if (!parseDashboardPayload(payload)) {
    setErrorText("Parse failed");
    Serial.println("[fetch] Parse failed");
    return false;
  }
  errorText[0] = '\0';
  Serial.printf("[fetch] OK rows=%d weather=%d\n", rowCount, g_weatherValid ? 1 : 0);
  return true;
}

bool connectWifi(bool force = false) {
  unsigned long nowMs = millis();
  if (!force && WiFi.status() == WL_CONNECTED) {
    return true;
  }
  if (!force && nowMs - lastWifiAttemptMs < WIFI_RETRY_INTERVAL_MS) {
    return WiFi.status() == WL_CONNECTED;
  }
  lastWifiAttemptMs = nowMs;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, true);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
#if defined(WIFI_PS_MIN_MODEM)
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
#else
    WiFi.setSleep(true);
#endif
    errorText[0] = '\0';
    Serial.printf("[wifi] connected ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }

  setErrorTextf("WiFi %s", wifiStatusName(WiFi.status()));
  Serial.printf("[wifi] connect failed status=%s ssid=%s\n", wifiStatusName(WiFi.status()), WIFI_SSID);
  return false;
}

void createUi() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
  lv_obj_set_style_text_color(screen, lv_color_black(), 0);
  lv_obj_set_style_pad_all(screen, 0, 0);

  // ---- header row ----
  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "Quota Dashboard v9");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 5);

  timeLabel = lv_label_create(screen);
  lv_label_set_text(timeLabel, "--:--");
  lv_obj_set_style_text_font(timeLabel, &lv_font_montserrat_12, 0);
  lv_obj_align(timeLabel, LV_ALIGN_TOP_MID, -38, 5);

  wifiLabel = lv_label_create(screen);
  lv_label_set_text(wifiLabel, "WiFi --");
  lv_obj_set_style_text_font(wifiLabel, &lv_font_montserrat_12, 0);
  lv_obj_align(wifiLabel, LV_ALIGN_TOP_MID, 42, 5);

  batteryLabel = lv_label_create(screen);
  lv_label_set_text(batteryLabel, "BAT --");
  lv_obj_set_style_text_font(batteryLabel, &lv_font_montserrat_12, 0);
  lv_obj_align(batteryLabel, LV_ALIGN_TOP_RIGHT, -6, 5);

  errorLabel = lv_label_create(screen);
  lv_label_set_text(errorLabel, "");
  lv_obj_set_width(errorLabel, 380);
  lv_obj_set_style_text_color(errorLabel, lv_color_black(), 0);
  lv_obj_set_style_text_font(errorLabel, &lv_font_montserrat_14, 0);
  // Positioned at top-left; only visible when errorText is non-empty
  lv_obj_align(errorLabel, LV_ALIGN_TOP_LEFT, 10, 4);

  // ---- 5 row panels ----
  for (int i = 0; i < VISIBLE_ROWS; i++) {
    int panelY = HEADER_H + i * ROW_H;

    lv_obj_t *panel = lv_obj_create(screen);
    rowPanels[i] = panel;
    lv_obj_set_size(panel, 390, ROW_H - 2);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 3, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 5, panelY);

    // --- CX arc (LEFT side) ---
    lv_obj_t *cxArc = lv_arc_create(panel);
    rowCxArcs[i] = cxArc;
    lv_obj_set_size(cxArc, ARC_SIZE, ARC_SIZE);
    lv_arc_set_bg_angles(cxArc, 135, 45);
    lv_arc_set_angles(cxArc, 135, 135);
    lv_arc_set_range(cxArc, 0, 100);
    lv_arc_set_value(cxArc, 0);
    lv_obj_remove_style(cxArc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(cxArc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(cxArc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(cxArc, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_arc_width(cxArc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(cxArc, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_align(cxArc, LV_ALIGN_LEFT_MID, ARC_LEFT_PAD, 0);

    lv_obj_t *cxLbl = lv_label_create(panel);
    rowCxLabels[i] = cxLbl;
    lv_label_set_text(cxLbl, "--");
    lv_obj_set_width(cxLbl, ARC_SIZE);
    lv_obj_set_style_text_align(cxLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(cxLbl, &lv_font_montserrat_14, 0);
    lv_obj_align(cxLbl, LV_ALIGN_LEFT_MID, ARC_LEFT_PAD, 0);

    // --- AG outer arc = Gemini (RIGHT side) ---
    lv_obj_t *agOuterArc = lv_arc_create(panel);
    rowAgOuterArcs[i] = agOuterArc;
    lv_obj_set_size(agOuterArc, ARC_SIZE, ARC_SIZE);
    lv_arc_set_bg_angles(agOuterArc, 135, 45);
    lv_arc_set_angles(agOuterArc, 135, 135);
    lv_arc_set_range(agOuterArc, 0, 100);
    lv_arc_set_value(agOuterArc, 0);
    lv_obj_remove_style(agOuterArc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(agOuterArc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(agOuterArc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(agOuterArc, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_arc_width(agOuterArc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(agOuterArc, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_align(agOuterArc, LV_ALIGN_RIGHT_MID, -ARC_LEFT_PAD, 0);

    // --- AG inner arc = Claude (concentric inside Gemini) ---
    lv_obj_t *agInnerArc = lv_arc_create(panel);
    rowAgInnerArcs[i] = agInnerArc;
    lv_obj_set_size(agInnerArc, ARC_INNER_SIZE, ARC_INNER_SIZE);
    lv_arc_set_bg_angles(agInnerArc, 135, 45);
    lv_arc_set_angles(agInnerArc, 135, 135);
    lv_arc_set_range(agInnerArc, 0, 100);
    lv_arc_set_value(agInnerArc, 0);
    lv_obj_remove_style(agInnerArc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(agInnerArc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(agInnerArc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(agInnerArc, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_arc_width(agInnerArc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(agInnerArc, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_align(agInnerArc, LV_ALIGN_RIGHT_MID, ARC_INNER_OFS, 0);

    // --- middle text block ---
    // Line 1: email prefix
    lv_obj_t *titleLbl = lv_label_create(panel);
    rowTitleLabels[i] = titleLbl;
    lv_label_set_text(titleLbl, "");
    lv_obj_set_width(titleLbl, TEXT_W_COMBINED);
    lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_14, 0);
    lv_obj_align(titleLbl, LV_ALIGN_TOP_LEFT, TEXT_X, 4);

    // Line 2: CX reset | AG reset
    lv_obj_t *detailLbl = lv_label_create(panel);
    rowDetailLabels[i] = detailLbl;
    lv_label_set_text(detailLbl, "");
    lv_obj_set_width(detailLbl, TEXT_W_COMBINED);
    lv_label_set_long_mode(detailLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(detailLbl, &lv_font_montserrat_14, 0);
    lv_obj_align(detailLbl, LV_ALIGN_TOP_LEFT, TEXT_X, 24);
  }

  // ---- weather panel (below data rows) ----
  {
    int wpY = HEADER_H + VISIBLE_ROWS * ROW_H;
    lv_obj_t *wp = lv_obj_create(screen);
    weatherPanel = wp;
    lv_obj_set_size(wp, 390, ROW_H - 2);
    lv_obj_set_style_bg_color(wp, lv_color_white(), 0);
    lv_obj_set_style_border_color(wp, lv_color_black(), 0);
    lv_obj_set_style_border_width(wp, 1, 0);
    lv_obj_set_style_radius(wp, 3, 0);
    lv_obj_set_style_pad_all(wp, 0, 0);
    lv_obj_align(wp, LV_ALIGN_TOP_LEFT, 5, wpY);

    static const int COL_W = 76;
    for (int i = 0; i < WEATHER_DAYS; i++) {
      int cx = 6 + i * COL_W;
      lv_obj_t *dl = lv_label_create(wp);
      wDateLabels[i] = dl;
      lv_label_set_text(dl, "--/--");
      lv_obj_set_style_text_font(dl, &lv_font_montserrat_12, 0);
      lv_obj_set_width(dl, COL_W - 4);
      lv_label_set_long_mode(dl, LV_LABEL_LONG_CLIP);
      lv_obj_align(dl, LV_ALIGN_TOP_LEFT, cx, 2);

      lv_obj_t *il = lv_label_create(wp);
      wInfoLabels[i] = il;
      lv_label_set_text(il, "---");
      lv_obj_set_style_text_font(il, &lv_font_weather_cjk, 0);
      lv_obj_set_width(il, COL_W - 4);
      lv_label_set_long_mode(il, LV_LABEL_LONG_CLIP);
      lv_obj_align(il, LV_ALIGN_TOP_LEFT, cx, 14);

      lv_obj_t *tl = lv_label_create(wp);
      wTempLabels[i] = tl;
      lv_label_set_text(tl, "--/--");
      lv_obj_set_style_text_font(tl, &lv_font_montserrat_12, 0);
      lv_obj_set_width(tl, COL_W - 4);
      lv_label_set_long_mode(tl, LV_LABEL_LONG_CLIP);
      lv_obj_align(tl, LV_ALIGN_TOP_LEFT, cx, 32);
    }
  }

  // ---- footer ----
  footerLabel = lv_label_create(screen);
  lv_label_set_text(footerLabel, "OpenAI Rows 0  Page 1/1  Sync -");
  lv_obj_set_style_text_font(footerLabel, &lv_font_montserrat_14, 0);
  lv_obj_align(footerLabel, LV_ALIGN_BOTTOM_LEFT, 8, -4);
}

void updateWeatherPanel() {
  if (!g_weatherValid) return;
  if (!Lvgl_lock(200)) return;
  lv_obj_clear_flag(weatherPanel, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < WEATHER_DAYS; i++) {
    lv_label_set_text(wDateLabels[i], g_weather[i].date);
    char info[16];
    if (i == 0 && g_currentTempC > -1000) {
      snprintf(info, sizeof(info), "%s %dC", wmoText(g_weather[i].code), g_currentTempC);
    } else {
      snprintf(info, sizeof(info), "%s", wmoText(g_weather[i].code));
    }
    lv_label_set_text(wInfoLabels[i], info);
    lv_label_set_text(wTempLabels[i], g_weather[i].temp);
  }
  Lvgl_unlock();
}

// ---------- Sleep / schedule helpers ----------

static time_t nowUtcEpoch() {
  return baseEpoch + (millis() - baseEpochMillis) / 1000UL;
}

static void toLocalTm(time_t utcEpoch, struct tm *out) {
  time_t localEpoch = utcEpoch + TIMEZONE_OFFSET_SECONDS;
  gmtime_r(&localEpoch, out);
}

// True if current local time is Mon-Fri 09:00-19:59
bool isWorkHours() {
  if (baseEpoch <= 0) return true;  // no time sync: stay active
  time_t nowUtc = nowUtcEpoch();
  struct tm t;
  toLocalTm(nowUtc, &t);
  if (t.tm_wday == 0 || t.tm_wday == 6) return false;  // Sun / Sat
  return (t.tm_hour >= 9 && t.tm_hour < 20);
}

// Returns UTC epoch of next weekday 09:00 in configured local timezone.
time_t nextWorkWakeEpoch() {
  time_t nowUtc = nowUtcEpoch();
  struct tm t;
  toLocalTm(nowUtc, &t);

  int daysAhead = 0;
  for (; daysAhead < 8; daysAhead++) {
    int wday = (t.tm_wday + daysAhead) % 7;
    bool weekday = (wday >= 1 && wday <= 5);
    if (!weekday) continue;
    if (daysAhead == 0 && t.tm_hour >= 9) continue;
    break;
  }
  if (daysAhead >= 8) {
    daysAhead = 1;  // fallback
  }

  time_t nowLocal = nowUtc + TIMEZONE_OFFSET_SECONDS;
  int secOfDay = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
  time_t localMidnight = nowLocal - secOfDay;
  time_t wakeLocal = localMidnight + (time_t)daysAhead * 86400 + 9 * 3600;
  return wakeLocal - TIMEZONE_OFFSET_SECONDS;
}

void showSleepScreen() {
  time_t wakeEpoch = nextWorkWakeEpoch();
  struct tm w;
  toLocalTm(wakeEpoch, &w);
  char wakeStr[20];
  strftime(wakeStr, sizeof(wakeStr), "%m/%d %H:%M", &w);

  if (Lvgl_lock(1000)) {
    for (int i = 0; i < VISIBLE_ROWS; i++) {
      lv_obj_add_flag(rowPanels[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(timeLabel, "--:--");
    lv_label_set_text(wifiLabel, "");
    lv_label_set_text(batteryLabel, "");
    lv_label_set_text(footerLabel, "");
    char msg[52];
    snprintf(msg, sizeof(msg), "SLEEPING\nWake: %s", wakeStr);
    lv_obj_set_style_text_align(errorLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(errorLabel, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(errorLabel, msg);
    Lvgl_unlock();
  }
  delay(4000);  // let e-ink finish refreshing before power-down
}

void enterOffHoursMode() {
  showSleepScreen();       // display retention: e-ink shows this while CPU sleeps
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200);

  // Light sleep in 5-minute intervals until work hours resume.
  // Light sleep preserves PSRAM, so LVGL state remains intact.
  while (g_isHoliday || !isWorkHours()) {
    esp_sleep_enable_timer_wakeup(5ULL * 60ULL * 1000000ULL);  // 5 min
    esp_light_sleep_start();  // CPU halts here; millis() continues via RTC
    // woke up due to timer — loop to re-check time
  }

  // Work hours resumed — restore screen and reconnect
  if (Lvgl_lock(1000)) {
    lv_obj_set_style_text_align(errorLabel, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(errorLabel, LV_ALIGN_TOP_LEFT, 10, 4);
    lv_label_set_text(errorLabel, "");
    for (int i = 0; i < VISIBLE_ROWS; i++) {
      lv_obj_clear_flag(rowPanels[i], LV_OBJ_FLAG_HIDDEN);
    }
    Lvgl_unlock();
  }
  connectWifi();
  g_isHoliday = false;  // stale flag; next fetchDashboard will set it correctly
}

// ----------------------------------------------

void refreshClockAndBattery(bool force = false) {
  // Only refresh once per minute (or on force)
  if (!force && millis() - lastClockMs < 60000UL) {
    return;
  }
  lastClockMs = millis();

  char timeBuf[16];
  safeCopy(timeBuf, sizeof(timeBuf), "--:--");
  if (baseEpoch > 0) {
    time_t currentEpoch = nowUtcEpoch();
    struct tm timeinfo;
    toLocalTm(currentEpoch, &timeinfo);
    strftime(timeBuf, sizeof(timeBuf), "%H:%M", &timeinfo);
  }

  char wifiBuf[16];
  snprintf(wifiBuf, sizeof(wifiBuf), "WiFi %s", WiFi.status() == WL_CONNECTED ? "OK" : wifiStatusName(WiFi.status()));

  char battBuf[32];
  snprintf(battBuf, sizeof(battBuf), "BAT %u%%", Adc_GetBatteryLevel());

  if (Lvgl_lock(50)) {
    lv_label_set_text(timeLabel, timeBuf);
    lv_label_set_text(wifiLabel, wifiBuf);
    lv_label_set_text(batteryLabel, battBuf);
    lv_label_set_text(errorLabel, errorText[0] ? errorText : "");
    Lvgl_unlock();
  }

  // Check work schedule once per minute; light sleep if outside hours or holiday
  if (g_isHoliday || !isWorkHours()) {
    enterOffHoursMode();
  }
}

void renderRows() {
  int activeCount = buildCombinedRows();
  int pageCount = max(1, (int)ceil((float)activeCount / (float)VISIBLE_ROWS));
  if (currentPage >= pageCount) {
    currentPage = 0;
  }
  int startIndex = currentPage * VISIBLE_ROWS;

  if (Lvgl_lock(200)) {
    lv_label_set_text(errorLabel, errorText[0] ? errorText : "");

    for (int i = 0; i < VISIBLE_ROWS; i++) {
      int index = startIndex + i;
      if (index < activeCount) {
        const CombinedRow &cr = combined[index];

        // CX arc (left): remaining %
        char cxText[8];
        if (cr.cxScore >= 0) {
          snprintf(cxText, sizeof(cxText), "%d", cr.cxScore);
          lv_arc_set_value(rowCxArcs[i], cr.cxScore);
        } else {
          safeCopy(cxText, sizeof(cxText), "--");
          lv_arc_set_value(rowCxArcs[i], 0);
        }
        lv_label_set_text(rowCxLabels[i], cxText);

        // AG concentric arcs (right): outer=Gemini, inner=Claude
        lv_arc_set_value(rowAgOuterArcs[i], cr.agGemini >= 0 ? cr.agGemini : 0);
        lv_arc_set_value(rowAgInnerArcs[i], cr.agClaude >= 0 ? cr.agClaude : 0);

        // Title
        lv_label_set_text(rowTitleLabels[i], cr.email);

        // Detail: CX reset and AG reset (agReset already contains "G:Xh/C:Ydh")
        char detail[40];
        bool hasCx = cr.cxReset[0] != '\0';
        bool hasAg = cr.agReset[0] != '\0';
        if (hasCx && hasAg) {
          snprintf(detail, sizeof(detail), "CX:%s %s", cr.cxReset, cr.agReset);
        } else if (hasCx) {
          snprintf(detail, sizeof(detail), "CX:%s", cr.cxReset);
        } else if (hasAg) {
          safeCopy(detail, sizeof(detail), cr.agReset);
        } else {
          safeCopy(detail, sizeof(detail), "-");
        }
        lv_label_set_text(rowDetailLabels[i], detail);

      } else if (activeCount == 0 && i == 0) {
        lv_label_set_text(rowTitleLabels[i], "No data");
        lv_label_set_text(rowDetailLabels[i], errorText[0] ? errorText : "Source offline");
        lv_label_set_text(rowCxLabels[i], "--");
        lv_arc_set_value(rowCxArcs[i], 0);
        lv_arc_set_value(rowAgOuterArcs[i], 0);
        lv_arc_set_value(rowAgInnerArcs[i], 0);
      } else {
        lv_label_set_text(rowTitleLabels[i], "");
        lv_label_set_text(rowDetailLabels[i], "");
        lv_label_set_text(rowCxLabels[i], "");
        lv_arc_set_value(rowCxArcs[i], 0);
        lv_arc_set_value(rowAgOuterArcs[i], 0);
        lv_arc_set_value(rowAgInnerArcs[i], 0);
      }
    }

    char footer[80];
    snprintf(footer, sizeof(footer), "CX | AG  %d users  P%d/%d  %s",
             activeCount, currentPage + 1, pageCount, lastSyncText);
    lv_label_set_text(footerLabel, footer);
    Lvgl_unlock();
  }
  updateWeatherPanel();
}

void updatePagination() {
  int activeCount = buildCombinedRows();
  int pageCount = max(1, (int)ceil((float)activeCount / (float)VISIBLE_ROWS));
  if (millis() - lastPageMs >= PAGE_INTERVAL_MS) {
    currentPage = (pageCount > 1) ? (currentPage + 1) % pageCount : 0;
    lastPageMs = millis();
    renderRows();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // BOOT button (GPIO0) as manual refresh trigger
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Adc_PortInit();
  RlcdPort.RLCD_Init();
  Lvgl_PortInit(SCREEN_WIDTH, SCREEN_HEIGHT, Lvgl_FlushCallback);
  createUi();

  connectWifi(true);
  lastFetchOk = fetchDashboard();
  renderRows();
  refreshClockAndBattery(true);  // check work hours after first render
  lastFetchMs = millis();
  lastPageMs = millis();
}

void checkButton() {
  bool state = digitalRead(BUTTON_PIN);
  // Detect falling edge (button pressed) with 50ms debounce
  if (state == LOW && buttonLastState == HIGH && millis() - buttonPressMs > 50) {
    buttonPressMs = millis();
    // Trigger immediate fetch and render
    lastFetchOk = fetchDashboard();
    renderRows();
    lastFetchMs = millis();
    refreshClockAndBattery(true);
  }
  buttonLastState = state;
}

void loop() {
  checkButton();

  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  unsigned long fetchInterval = lastFetchOk ? FETCH_INTERVAL_MS : FETCH_RETRY_INTERVAL_MS;
  if (millis() - lastFetchMs >= fetchInterval) {
    lastFetchOk = fetchDashboard();
    renderRows();
    lastFetchMs = millis();
  }

  refreshClockAndBattery();
  updatePagination();

  delay(200);
}
