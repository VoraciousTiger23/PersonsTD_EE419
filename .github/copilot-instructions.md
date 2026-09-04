# GitHub Copilot Instructions for ESP32

## 1. Target Hardware Context
* **Chip Core:** ESP32 family, including ESP32, ESP32-S2, ESP32-S3, and ESP32-C3.
* **Memory Constraints:** Strictly differentiate between internal IRAM/DRAM and external SPI PSRAM.
* **Wireless Capabilities:** Wi-Fi 4 (802.11 b/g/n) and Bluetooth 5 (LE) / Mesh.

## 2. Framework & API Constraints
* **Primary Framework:** ESP-IDF (Latest stable v6.x releases preferred).
* **Build System:** CMake + `idf.py`.
* **API Style:** Avoid bare metal or generic FreeRTOS calls if native `esp_` APIs exist.
* **Component Usage:** Rely on Espressif official components (e.g., `esp_wifi`, `esp_bt`, `nvs_flash`, `esp_peripherals`).

## 3. Memory & Performance Rules
* **PSRAM Allocation:** Use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` for large buffers (audio, images, display frames).
* **DMA Allocations:** SPI/I2S DMA buffers must use `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`. Do not put DMA buffers in PSRAM unless specifically supported by the chip version.
* **Task Stacks:** Keep default FreeRTOS task stacks low; use `xTaskCreateStatic` or allocate to `MALLOC_CAP_INTERNAL` for critical paths.
* **String Literals:** Ensure constants or lookup tables use `DRAM_ATTR` or `DRAM_STR` if required to be mapped safely.

## 4. Multi-Core & Concurrency Guidelines
* **Core Affinity:** Explicitly use `xTaskCreatePinnedToCore` instead of standard `xTaskCreate`.
* **Core Assignment:** Assign background communication/Wi-Fi to Core 0; assign heavy application logic, UI, or DSP to Core 1.
* **Thread Safety:** Protect shared resources across cores using `portMUX_TYPE` spinlocks (`taskENTER_CRITICAL(&mux)`) or FreeRTOS Mutexes (`SemaphoreHandle_t`).

## 5. Coding Standards & Error Handling
* **Error Checking:** Wrap every peripheral and system call utilizing `esp_err_t` with `ESP_ERROR_CHECK()` or local macro-based return guards.
* **Logging:** Use native logging macros: `ESP_LOGI(TAG, ...)`, `ESP_LOGE`, `ESP_LOGW`, `ESP_LOGD`, `ESP_LOGV`.
* **Tag Declaration:** Define a static constant `static const char *TAG = "MODULE_NAME";` at the top of every `.c`/`.cpp` file.
* **C++ Usage:** When writing in C++, wrap C header file includes in `extern "C" { ... }`.

## 6. Common Peripheral Rules
* **GPIO:** Use `gpio_config_t` structs for pin configuration rather than deprecated legacy macros.
* **I2C/SPI:** Always use the newer ESP-IDF driver APIs (e.g., `i2c_master_transmit`, `spi_device_transmit`).
* **Power Management:** Proactively implement light-sleep/deep-sleep configurations using `esp_sleep_enable_timer_wakeup()` where appropriate.
