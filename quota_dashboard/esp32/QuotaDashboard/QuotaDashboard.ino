#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <ctype.h>

#include "display_bsp.h"
#include "lvgl_bsp.h"
#include "adc_bsp.h"
#include "config.h"

// GPIO0 is the BOOT button on most ESP32-S3 boards
static const int BUTTON_PIN = 0;

static const int SCREEN_WIDTH = 400;
static const int SCREEN_HEIGHT = 300;
static const int MAX_ROWS = 24;
static const int VISIBLE_ROWS = 5;

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
unsigned long lastClockMs = 0;
int currentPage = 0;

// Button debounce
static unsigned long buttonPressMs = 0;
static bool buttonLastState = HIGH;

lv_obj_t *timeLabel = nullptr;
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

// Per-person combined row (CX + AG merged by email prefix)
struct CombinedRow {
  char email[49];
  int  cxScore;      // CX remaining %,       -1 = no data
  int  agGemini;     // AG Gemini remaining %, -1 = no data
  int  agClaude;     // AG Claude remaining %, -1 = no data
  char cxReset[12];  // CX reset countdown e.g. "6d23h"
  char agReset[20];  // AG reset e.g. "G:2h/C:5d7h"
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
  errorText[0] = '\0';
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
    combined[i].cxScore  = -1;
    combined[i].agGemini = -1;
    combined[i].agClaude = -1;
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
      combined[idx].cxReset[0] = '\0';
      combined[idx].agReset[0] = '\0';
    }

    if (strncmp(rows[i].platform, "CX", 2) == 0) {
      int score = rowScore(rows[i]);
      combined[idx].cxScore = (score < 0 || score > 100) ? 0 : score;
      safeCopy(combined[idx].cxReset, sizeof(combined[idx].cxReset), rows[i].reset);
    } else if (strncmp(rows[i].platform, "AG", 2) == 0) {
      int gem = extractPercent(rows[i].metric1);
      int cld = extractPercent(rows[i].metric2);
      combined[idx].agGemini = (gem < 0) ? 0 : gem;
      combined[idx].agClaude = (cld < 0) ? 0 : cld;
      safeCopy(combined[idx].agReset, sizeof(combined[idx].agReset), rows[i].reset);
    }
  }

  // Sort: lowest remaining quota at top
  for (int i = 1; i < combinedCount; i++) {
    CombinedRow key = combined[i];
    int keyWorst = 101;
    if (key.cxScore  >= 0) keyWorst = min(keyWorst, key.cxScore);
    if (key.agGemini >= 0) keyWorst = min(keyWorst, key.agGemini);
    if (key.agClaude >= 0) keyWorst = min(keyWorst, key.agClaude);
    int j = i - 1;
    while (j >= 0) {
      int lw = 101;
      if (combined[j].cxScore  >= 0) lw = min(lw, combined[j].cxScore);
      if (combined[j].agGemini >= 0) lw = min(lw, combined[j].agGemini);
      if (combined[j].agClaude >= 0) lw = min(lw, combined[j].agClaude);
      if (lw <= keyWorst) break;
      combined[j + 1] = combined[j];
      j--;
    }
    combined[j + 1] = key;
  }

  return combinedCount;
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
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
#if defined(WIFI_PS_MIN_MODEM)
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
#else
    WiFi.setSleep(true);
#endif
  }
}

void createUi() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
  lv_obj_set_style_text_color(screen, lv_color_black(), 0);
  lv_obj_set_style_pad_all(screen, 0, 0);

  // ---- header row ----
  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "Quota Dashboard v6");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 4);

  timeLabel = lv_label_create(screen);
  lv_label_set_text(timeLabel, "--:--");
  lv_obj_set_style_text_font(timeLabel, &lv_font_montserrat_14, 0);
  lv_obj_align(timeLabel, LV_ALIGN_TOP_LEFT, 200, 4);

  batteryLabel = lv_label_create(screen);
  lv_label_set_text(batteryLabel, "BAT --");
  lv_obj_set_style_text_font(batteryLabel, &lv_font_montserrat_14, 0);
  lv_obj_align(batteryLabel, LV_ALIGN_TOP_RIGHT, -6, 4);

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

  // ---- footer ----
  footerLabel = lv_label_create(screen);
  lv_label_set_text(footerLabel, "OpenAI Rows 0  Page 1/1  Sync -");
  lv_obj_set_style_text_font(footerLabel, &lv_font_montserrat_14, 0);
  lv_obj_align(footerLabel, LV_ALIGN_BOTTOM_LEFT, 8, -4);
}

void refreshClockAndBattery(bool force = false) {
  if (baseEpoch <= 0) {
    return;
  }

  // Only refresh once per minute (or on force)
  if (!force && millis() - lastClockMs < 60000UL) {
    return;
  }
  lastClockMs = millis();

  time_t currentEpoch = baseEpoch + ((millis() - baseEpochMillis) / 1000UL) + TIMEZONE_OFFSET_SECONDS;
  struct tm timeinfo;
  localtime_r(&currentEpoch, &timeinfo);

  char timeBuf[16];
  strftime(timeBuf, sizeof(timeBuf), "%H:%M", &timeinfo);

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
  int activeCount = buildCombinedRows();
  int pageCount = max(1, (int)ceil((float)activeCount / (float)VISIBLE_ROWS));
  if (currentPage >= pageCount) {
    currentPage = 0;
  }
  int startIndex = currentPage * VISIBLE_ROWS;

  if (Lvgl_lock(200)) {
    lv_label_set_text(errorLabel, "");

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
        lv_label_set_text(rowDetailLabels[i], "Source offline");
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

  connectWifi();
  fetchDashboard();
  renderRows();
  refreshClockAndBattery(true);
  lastFetchMs = millis();
  lastPageMs = millis();
}

void checkButton() {
  bool state = digitalRead(BUTTON_PIN);
  // Detect falling edge (button pressed) with 50ms debounce
  if (state == LOW && buttonLastState == HIGH && millis() - buttonPressMs > 50) {
    buttonPressMs = millis();
    // Trigger immediate fetch and render
    if (fetchDashboard()) {
      renderRows();
    }
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

  if (millis() - lastFetchMs >= FETCH_INTERVAL_MS) {
    if (fetchDashboard()) {
      renderRows();
    }
    lastFetchMs = millis();
  }

  refreshClockAndBattery();
  updatePagination();

  delay(200);
}
