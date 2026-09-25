#pragma once

#include <stdbool.h>

void MQTT_RPi_init(void);
void MQTT_RPi_publish_status(bool targetFound);
