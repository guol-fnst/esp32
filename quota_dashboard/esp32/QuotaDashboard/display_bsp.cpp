#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>

#include "display_bsp.h"

DisplayPort::DisplayPort(int mosi, int scl, int dc, int cs, int rst, int width, int height, spi_host_device_t spihost)
  : mosi_(mosi), scl_(scl), dc_(dc), cs_(cs), rst_(rst), width_(width), height_(height) {
  esp_err_t ret;
  spi_bus_config_t buscfg = {};
  int transfer = width_ * height_;
  buscfg.miso_io_num = -1;
  buscfg.mosi_io_num = mosi;
  buscfg.sclk_io_num = scl;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = transfer;
  ret = spi_bus_initialize(spihost, &buscfg, SPI_DMA_CH_AUTO);
  ESP_ERROR_CHECK(ret);

  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.dc_gpio_num = dc_;
  io_config.cs_gpio_num = cs_;
  io_config.pclk_hz = 10 * 1000 * 1000;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  io_config.spi_mode = 0;
  io_config.trans_queue_depth = 10;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spihost, &io_config, &io_handle));

  gpio_config_t gpio_conf = {};
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_OUTPUT;
  gpio_conf.pin_bit_mask = (0x1ULL << rst_);
  gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
  Set_ResetIOLevel(1);

  DisplayLen = transfer >> 3;
  DispBuffer = (uint8_t *)heap_caps_malloc(DisplayLen, MALLOC_CAP_SPIRAM);
  assert(DispBuffer);

  PixelIndexLUT = (uint16_t(*)[300])heap_caps_malloc(transfer * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  PixelBitLUT = (uint8_t(*)[300])heap_caps_malloc(transfer * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
  assert(PixelIndexLUT);
  assert(PixelBitLUT);
  if (width_ == 400) {
    InitLandscapeLUT();
  } else {
    InitPortraitLUT();
  }
}

DisplayPort::~DisplayPort() {
}

void DisplayPort::RLCD_Init() {
  RLCD_Reset();

  RLCD_SendCommand(0xD6); RLCD_SendData(0x17); RLCD_SendData(0x02);
  RLCD_SendCommand(0xD1); RLCD_SendData(0x01);
  RLCD_SendCommand(0xC0); RLCD_SendData(0x11); RLCD_SendData(0x04);
  RLCD_SendCommand(0xC1); RLCD_SendData(0x69); RLCD_SendData(0x69); RLCD_SendData(0x69); RLCD_SendData(0x69);
  RLCD_SendCommand(0xC2); RLCD_SendData(0x19); RLCD_SendData(0x19); RLCD_SendData(0x19); RLCD_SendData(0x19);
  RLCD_SendCommand(0xC4); RLCD_SendData(0x4B); RLCD_SendData(0x4B); RLCD_SendData(0x4B); RLCD_SendData(0x4B);
  RLCD_SendCommand(0xC5); RLCD_SendData(0x19); RLCD_SendData(0x19); RLCD_SendData(0x19); RLCD_SendData(0x19);
  RLCD_SendCommand(0xD8); RLCD_SendData(0x80); RLCD_SendData(0xE9);
  RLCD_SendCommand(0xB2); RLCD_SendData(0x02);

  RLCD_SendCommand(0xB3);
  RLCD_SendData(0xE5); RLCD_SendData(0xF6); RLCD_SendData(0x05); RLCD_SendData(0x46); RLCD_SendData(0x77);
  RLCD_SendData(0x77); RLCD_SendData(0x77); RLCD_SendData(0x77); RLCD_SendData(0x76); RLCD_SendData(0x45);

  RLCD_SendCommand(0xB4);
  RLCD_SendData(0x05); RLCD_SendData(0x46); RLCD_SendData(0x77); RLCD_SendData(0x77); RLCD_SendData(0x77);
  RLCD_SendData(0x77); RLCD_SendData(0x76); RLCD_SendData(0x45);

  RLCD_SendCommand(0x62); RLCD_SendData(0x32); RLCD_SendData(0x03); RLCD_SendData(0x1F);
  RLCD_SendCommand(0xB7); RLCD_SendData(0x13);
  RLCD_SendCommand(0xB0); RLCD_SendData(0x64);
  RLCD_SendCommand(0x11);
  vTaskDelay(pdMS_TO_TICKS(200));
  RLCD_SendCommand(0xC9); RLCD_SendData(0x00);
  RLCD_SendCommand(0x36); RLCD_SendData(0x48);
  RLCD_SendCommand(0x3A); RLCD_SendData(0x11);
  RLCD_SendCommand(0xB9); RLCD_SendData(0x20);
  RLCD_SendCommand(0xB8); RLCD_SendData(0x29);
  RLCD_SendCommand(0x21);

  RLCD_SendCommand(0x2A); RLCD_SendData(0x12); RLCD_SendData(0x2A);
  RLCD_SendCommand(0x2B); RLCD_SendData(0x00); RLCD_SendData(0xC7);
  RLCD_SendCommand(0x35); RLCD_SendData(0x00);
  RLCD_SendCommand(0xD0); RLCD_SendData(0xFF);
  RLCD_SendCommand(0x38);
  RLCD_SendCommand(0x29);

  RLCD_ColorClear(ColorWhite);
}

void DisplayPort::RLCD_ColorClear(uint8_t color) {
  memset(DispBuffer, color, DisplayLen);
}

void DisplayPort::RLCD_Display() {
  RLCD_SendCommand(0x2A);
  RLCD_SendData(0x12);
  RLCD_SendData(0x2A);
  RLCD_SendCommand(0x2B);
  RLCD_SendData(0x00);
  RLCD_SendData(0xC7);
  RLCD_SendCommand(0x2c);
  RLCD_Sendbuffera(DispBuffer, DisplayLen);
}

void DisplayPort::RLCD_Reset(void) {
  Set_ResetIOLevel(1);
  vTaskDelay(pdMS_TO_TICKS(50));
  Set_ResetIOLevel(0);
  vTaskDelay(pdMS_TO_TICKS(20));
  Set_ResetIOLevel(1);
  vTaskDelay(pdMS_TO_TICKS(50));
}

void DisplayPort::RLCD_SendCommand(uint8_t reg) {
  ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, reg, NULL, 0));
}

void DisplayPort::RLCD_SendData(uint8_t data) {
  ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, -1, &data, 1));
}

void DisplayPort::RLCD_Sendbuffera(uint8_t *data, int len) {
  ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_handle, -1, data, len));
}

void DisplayPort::Set_ResetIOLevel(uint8_t level) {
  gpio_set_level((gpio_num_t)rst_, level ? 1 : 0);
}

void DisplayPort::InitLandscapeLUT() {
  for (uint16_t y = 0; y < height_; ++y) {
    for (uint16_t x = 0; x < width_; ++x) {
      uint16_t trans_x = y;
      uint16_t trans_y = width_ - 1 - x;
      uint16_t byte_x = trans_x / 4;
      uint16_t byte_y = trans_y / 2;
      PixelIndexLUT[x][y] = byte_y * (height_ / 4) + byte_x;
      PixelBitLUT[x][y] = 7 - ((trans_x % 4) * 2 + (trans_y % 2));
    }
  }
}

void DisplayPort::InitPortraitLUT() {
  for (uint16_t x = 0; x < width_; ++x) {
    for (uint16_t y = 0; y < height_; ++y) {
      uint16_t byte_x = x / 4;
      uint16_t byte_y = y / 2;
      PixelIndexLUT[x][y] = byte_y * (width_ / 4) + byte_x;
      PixelBitLUT[x][y] = 7 - ((x % 4) * 2 + (y % 2));
    }
  }
}

void DisplayPort::RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color) {
  if ((x >= width_) || (y >= height_)) {
    ESP_LOGE(TAG, "Beyond the limit : (%d,%d)", x, y);
    return;
  }
  uint32_t index = PixelIndexLUT[x][y];
  uint8_t mask = 1 << PixelBitLUT[x][y];
  if (color) {
    DispBuffer[index] |= mask;
  } else {
    DispBuffer[index] &= ~mask;
  }
}
