#line 1 "D:\\codex\\quota_dashboard\\esp32\\QuotaDashboard\\adc_bsp.h"
#ifndef ADC_BSP_H
#define ADC_BSP_H

#include <Arduino.h>

void Adc_PortInit(void);
float Adc_GetBatteryVoltage(int *data);
uint8_t Adc_GetBatteryLevel(void);

#endif
