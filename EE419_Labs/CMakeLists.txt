# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(EE419_Labs)

# add the X.509 certificates
target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "certs/device.cert.pem" TEXT)
target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "certs/device.private.key" TEXT)
target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "certs/AmazonRootCA1.pem" TEXT)