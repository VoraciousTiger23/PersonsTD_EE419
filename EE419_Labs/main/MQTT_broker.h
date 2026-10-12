#pragma once

#include <stdbool.h>

void MQTT_broker_init(void);
void MQTT_broker_publish_status(bool targetFound);
