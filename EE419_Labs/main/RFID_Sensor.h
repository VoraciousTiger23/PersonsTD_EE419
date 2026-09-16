#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Initialize LED hardware and start the color cycle task
void RFID_Sensor_init(void);

// Set RGB LED color (true = on, false = off)
void rgb_set_color(bool red, bool green, bool blue);

// Last-detected UID accessors (thread-safe)
// `buf` must have space for up to 10 bytes. On success `*len` is set.
void RFID_set_last_uid(const uint8_t *uid, size_t len);
bool RFID_get_last_uid(uint8_t *buf, size_t *len);
void RFID_clear_last_uid(void);
