#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

#include "display_bsp.h"
#include "lvgl_bsp.h"
#include "adc_bsp.h"
#include "config.h"

static const int SCREEN_WIDTH = 400;
static const int SCREEN_HEIGHT = 300;
static const int MAX_ROWS = 24;
static const int VISIBLE_ROWS = 5;

struct DashboardRow {
  char platform[4];
  char email[49];
  char metric1[17];
  char metric2[17];
  char reset[17];
};

DisplayPort RlcdPort(12, 11, 5, 40, 41, SCREEN_WIDTH, SCREEN_HEIGHT);
DashboardRow rows[MAX_ROWS];
int rowCount = 0;
char lastSyncText[25] = "-";
char errorText[96] = "";

time_t baseEpoch = 0;
unsigned long baseEpochMillis = 0;
unsigned long lastFetchMs = 0;
unsigned long lastPageMs = 0;
int currentPage = 0;

lv_obj_t *timeLabel = nullptr;
lv_obj_t *batteryLabel = nullptr;
lv_obj_t *footerLabel = nullptr;
lv_obj_t *errorLabel = nullptr;
lv_obj_t *rowTitleLabels[VISIBLE_ROWS];
lv_obj_t *rowDetailLabels[VISIBLE_ROWS];

static void Lvgl_FlushCallback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
  uint16_t *buffer = (uint16_t *)color_map;
  for (int y = area->y1; y <= area->y2; y++) {
    for (int x = area->x1; x <= area->x2; x++) {
      uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
      RlcdPort.RLCD_SetPixel(x, y, color);
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
  errorText[0] = '\0';
}

bool parseDashboardPayload(String payload) {
  resetRows();
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
    char *parts[6] = {0};

    if (strncmp(buffer, "META|", 5) == 0) {
      if (splitField(buffer, parts, 4)) {
        baseEpoch = (time_t)atoll(parts[1]);
        baseEpochMillis = millis();
        safeCopy(lastSyncText, sizeof(lastSyncText), parts[2]);
      }
      continue;
    }

    if (strncmp(buffer, "ROW|", 4) == 0) {
      if (rowCount >= MAX_ROWS) {
        continue;
      }
      if (splitField(buffer, parts, 6)) {
        safeCopy(rows[rowCount].platform, sizeof(rows[rowCount].platform), parts[1]);
        safeCopy(rows[rowCount].email, sizeof(rows[rowCount].email), parts[2]);
        safeCopy(rows[rowCount].metric1, sizeof(rows[rowCount].metric1), parts[3]);
        safeCopy(rows[rowCount].metric2, sizeof(rows[rowCount].metric2), parts[4]);
        safeCopy(rows[rowCount].reset, sizeof(rows[rowCount].reset), parts[5]);
        rowCount++;
      }
      continue;
    }

    if (strncmp(buffer, "ERR|", 4) == 0) {
      safeCopy(errorText, sizeof(errorText), buffer + 4);
    }
  }

  return baseEpoch > 0;
}

bool fetchDashboard() {
  if (WiFi.status() != WL_CONNECTED) {
    safeCopy(errorText, sizeof(errorText), "WiFi disconnected");
    return false;
  }

  HTTPClient http;
  http.begin(DASHBOARD_URL);
  http.setConnectTimeout(5000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    String err = http.errorToString(code);
    snprintf(errorText, sizeof(errorText), "HTTP %d %s", code, err.c_str());
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();
  if (!parseDashboardPayload(payload)) {
    safeCopy(errorText, sizeof(errorText), "Parse failed");
    return false;
  }
  errorText[0] = '\0';
  return true;
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
  }
}

void createUi() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
  lv_obj_set_style_text_color(screen, lv_color_black(), 0);
  lv_obj_set_style_pad_all(screen, 0, 0);

  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "Quota Dashboard");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 8);

  timeLabel = lv_label_create(screen);
  lv_label_set_text(timeLabel, "Waiting for sync");
  lv_obj_set_style_text_font(timeLabel, &lv_font_montserrat_14, 0);
  lv_obj_align(timeLabel, LV_ALIGN_TOP_LEFT, 10, 32);

  batteryLabel = lv_label_create(screen);
  lv_label_set_text(batteryLabel, "BAT --");
  lv_obj_set_style_text_font(batteryLabel, &lv_font_montserrat_14, 0);
  lv_obj_align(batteryLabel, LV_ALIGN_TOP_RIGHT, -10, 12);

  errorLabel = lv_label_create(screen);
  lv_label_set_text(errorLabel, "Start quota_server.py on your PC");
  lv_obj_set_width(errorLabel, 380);
  lv_obj_set_style_text_color(errorLabel, lv_color_black(), 0);
  lv_obj_set_style_text_font(errorLabel, &lv_font_montserrat_14, 0);
  lv_obj_align(errorLabel, LV_ALIGN_TOP_LEFT, 10, 56);

  for (int i = 0; i < VISIBLE_ROWS; i++) {
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_size(panel, 380, 38);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 10, 84 + (i * 40));

    rowTitleLabels[i] = lv_label_create(panel);
    lv_label_set_text(rowTitleLabels[i], "");
    lv_obj_set_width(rowTitleLabels[i], 370);
    lv_obj_set_style_text_font(rowTitleLabels[i], &lv_font_montserrat_14, 0);
    lv_obj_align(rowTitleLabels[i], LV_ALIGN_TOP_LEFT, 2, 0);

    rowDetailLabels[i] = lv_label_create(panel);
    lv_label_set_text(rowDetailLabels[i], "");
    lv_obj_set_width(rowDetailLabels[i], 370);
    lv_obj_set_style_text_font(rowDetailLabels[i], &lv_font_montserrat_14, 0);
    lv_obj_align(rowDetailLabels[i], LV_ALIGN_TOP_LEFT, 2, 18);
  }

  footerLabel = lv_label_create(screen);
  lv_label_set_text(footerLabel, "Rows 0  Page 1/1  Sync -");
  lv_obj_set_style_text_font(footerLabel, &lv_font_montserrat_14, 0);
  lv_obj_align(footerLabel, LV_ALIGN_BOTTOM_LEFT, 10, -10);
}

void refreshClockAndBattery() {
  if (baseEpoch <= 0) {
    return;
  }

  time_t currentEpoch = baseEpoch + ((millis() - baseEpochMillis) / 1000UL) + TIMEZONE_OFFSET_SECONDS;
  struct tm timeinfo;
  localtime_r(&currentEpoch, &timeinfo);

  char timeBuf[32];
  strftime(timeBuf, sizeof(timeBuf), "%m-%d %H:%M:%S", &timeinfo);

  char battBuf[32];
  float voltage = Adc_GetBatteryVoltage(NULL);
  snprintf(battBuf, sizeof(battBuf), "BAT %u%% %.2fV", Adc_GetBatteryLevel(), voltage);

  if (Lvgl_lock(50)) {
    lv_label_set_text(timeLabel, timeBuf);
    lv_label_set_text(batteryLabel, battBuf);
    Lvgl_unlock();
  }
}

void renderRows() {
  int pageCount = max(1, (int)ceil((float)rowCount / (float)VISIBLE_ROWS));
  if (currentPage >= pageCount) {
    currentPage = 0;
  }
  int startIndex = currentPage * VISIBLE_ROWS;

  if (Lvgl_lock(200)) {
    lv_label_set_text(errorLabel, errorText[0] ? errorText : "");

    for (int i = 0; i < VISIBLE_ROWS; i++) {
      int index = startIndex + i;
      if (index < rowCount) {
        char title[80];
        char detail[96];
        snprintf(title, sizeof(title), "[%s] %s", rows[index].platform, rows[index].email);
        snprintf(detail, sizeof(detail), "%s  %s  R:%s", rows[index].metric1, rows[index].metric2, rows[index].reset);
        lv_label_set_text(rowTitleLabels[i], title);
        lv_label_set_text(rowDetailLabels[i], detail);
      } else if (rowCount == 0 && i == 0) {
        lv_label_set_text(rowTitleLabels[i], "No quota data yet");
        lv_label_set_text(rowDetailLabels[i], "Run quota_server.py, then wait for next sync");
      } else {
        lv_label_set_text(rowTitleLabels[i], "");
        lv_label_set_text(rowDetailLabels[i], "");
      }
    }

    char footer[96];
    snprintf(footer, sizeof(footer), "Rows %d  Page %d/%d  Sync %s", rowCount, currentPage + 1, pageCount, lastSyncText);
    lv_label_set_text(footerLabel, footer);
    Lvgl_unlock();
  }
}

void updatePagination() {
  int pageCount = max(1, (int)ceil((float)rowCount / (float)VISIBLE_ROWS));
  if (pageCount <= 1) {
    currentPage = 0;
    return;
  }
  if (millis() - lastPageMs >= PAGE_INTERVAL_MS) {
    currentPage = (currentPage + 1) % pageCount;
    lastPageMs = millis();
    renderRows();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Adc_PortInit();
  RlcdPort.RLCD_Init();
  Lvgl_PortInit(SCREEN_WIDTH, SCREEN_HEIGHT, Lvgl_FlushCallback);
  createUi();

  connectWifi();
  fetchDashboard();
  renderRows();
  refreshClockAndBattery();
  lastFetchMs = millis();
  lastPageMs = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  if (millis() - lastFetchMs >= FETCH_INTERVAL_MS) {
    if (fetchDashboard()) {
      renderRows();
    }
    lastFetchMs = millis();
  }

  refreshClockAndBattery();
  updatePagination();
  delay(250);
}
