#pragma once

#include <stdbool.h>

// Initialize LED hardware and start the color cycle task
void RFID_Sensor_init(void);

// Set RGB LED color (true = on, false = off)
void rgb_set_color(bool red, bool green, bool blue);
