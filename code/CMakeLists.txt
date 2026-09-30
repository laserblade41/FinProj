cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(smartwatch)

target_sources(app PRIVATE
    src/main.c
    src/rtc/rtc.c
    src/pmic/pmic.c
    src/sensor/sensor.c
    src/touch/touch.c
    src/display/display.c
    src/display/watchface.c
    src/display/test_screen.c
    src/display/module_screen.c
    src/expansion/expansion.c
    src/expansion/expansion_transport.c
    src/expansion/expansion_mock.c
    src/storage/storage.c
    src/ble/ble.c
    src/buttons/buttons.c
)

target_include_directories(app PRIVATE src)
