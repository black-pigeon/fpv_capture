cmake_minimum_required(VERSION 3.16)
project(fpv_capture C)
set(FPV_BOARD "e200" CACHE STRING "FPV receiver board: e200 or e310")
set_property(CACHE FPV_BOARD PROPERTY STRINGS e200 e310)
if(FPV_BOARD STREQUAL "e310")
    add_compile_definitions(FPV_TARGET_E310=1)
elseif(NOT FPV_BOARD STREQUAL "e200")
    message(FATAL_ERROR "Unsupported FPV_BOARD: ${FPV_BOARD}")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Board-tested no-OS sources copied from the E200 reference, retaining their
# individual licenses. Select a cross toolchain explicitly at configure time.
add_library(e200-no-os STATIC
    radio/e200_ad9361_config.c
    radio/e200_radio.c
    radio/fpv_board.c
    radio/fpv_frontend.c
    radio/fpv_radio.c
    radio/lib/platform/e200_ad9361_defaults.c
    radio/lib/ad9361/ad9361.c
    radio/lib/ad9361/ad9361_api.c
    radio/lib/ad9361/ad9361_conv.c
    radio/lib/ad9361/ad9361_util.c
    radio/lib/ad9361/axi_adc_core.c
    radio/lib/ad9361/axi_dac_core.c
    radio/lib/ad9361/delay.c
    radio/lib/linux_platform/linux_gpio.c
    radio/lib/no_os/no_os_gpio.c
    radio/lib/no_os/no_os_spi.c
    radio/lib/no_os/no_os_util.c)
target_include_directories(e200-no-os PUBLIC
    radio/include radio/lib/ad9361 radio/lib/no_os radio/lib/linux_platform)
target_link_libraries(e200-no-os PUBLIC m)

add_executable(fpv-radio-probe tools/fpv-radio-probe.c)
target_link_libraries(fpv-radio-probe PRIVATE e200-no-os)
target_compile_options(fpv-radio-probe PRIVATE -Wall -Wextra -Werror)
install(TARGETS fpv-radio-probe RUNTIME DESTINATION sbin)

enable_testing()
add_test(NAME radio-probe-help COMMAND fpv-radio-probe --help)

add_library(fpv-frame STATIC capture/fpv_frame.c)
target_include_directories(fpv-frame PUBLIC capture ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(fpv-frame PRIVATE -Wall -Wextra -Werror)
add_executable(fpv-capture tools/fpv-capture.c)
target_link_libraries(fpv-capture PRIVATE fpv-frame)
target_compile_options(fpv-capture PRIVATE -Wall -Wextra -Werror)
install(TARGETS fpv-capture RUNTIME DESTINATION sbin)
add_test(NAME capture-help COMMAND fpv-capture --help)
add_test(NAME capture-invalid-channel COMMAND fpv-capture --channel 4 --standard pal --output unused.pgm)
set_tests_properties(capture-invalid-channel PROPERTIES WILL_FAIL TRUE)
add_executable(test-fpv-frame tests/test_frame.c)
target_link_libraries(test-fpv-frame PRIVATE fpv-frame)
target_compile_options(test-fpv-frame PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME frame-parser COMMAND test-fpv-frame)

# Host fake-device tests exercise the production CLI and driver control flow;
# they supplement RTL/hardware verification and are not run on a cross target.
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(test-capture-cli tests/test_capture_cli.c)
    target_link_libraries(test-capture-cli PRIVATE fpv-frame)
    target_compile_options(test-capture-cli PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME capture-cli-auto COMMAND test-capture-cli)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME capture-driver-auto COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_capture_driver.py)
endif()

find_package(Threads REQUIRED)
add_library(fpv-scan STATIC scan/fpv_scan.c scan/fpv_backend.c scan/fpv_service.c net/fpv_feed.c net/fpv_stream.c spectrum/fpv_spectrum.c spectrum/fpv_spectrum_device.c)
target_include_directories(fpv-scan PUBLIC scan net spectrum)
target_compile_definitions(fpv-scan PRIVATE _GNU_SOURCE)
target_link_libraries(fpv-scan PUBLIC fpv-frame e200-no-os Threads::Threads)
target_compile_options(fpv-scan PRIVATE -Wall -Wextra -Werror)
add_executable(fpv-webd tools/fpv-webd.c)
target_compile_definitions(fpv-webd PRIVATE _GNU_SOURCE)
target_compile_options(fpv-webd PRIVATE -Wall -Wextra -Werror)
target_link_libraries(fpv-webd PRIVATE fpv-scan)
install(TARGETS fpv-webd RUNTIME DESTINATION sbin)
install(DIRECTORY web/ DESTINATION share/e200-fpv/web)
add_test(NAME web-help COMMAND fpv-webd --help)
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(test-fpv-feed tests/test_feed.c)
    target_link_libraries(test-fpv-feed PRIVATE fpv-scan)
    target_compile_options(test-fpv-feed PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME network-frame-feed COMMAND test-fpv-feed)
    add_test(NAME network-frame-stream COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stream.py $<TARGET_FILE:fpv-webd>
        ${CMAKE_CURRENT_SOURCE_DIR}/web)
    add_executable(test-fpv-scan tests/test_scan.c)
    target_link_libraries(test-fpv-scan PRIVATE fpv-scan)
    target_compile_options(test-fpv-scan PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME scan-planner COMMAND test-fpv-scan)
    add_executable(test-fpv-backend tests/test_backend.c)
    target_link_libraries(test-fpv-backend PRIVATE fpv-scan)
    target_compile_options(test-fpv-backend PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME scan-radio-transaction COMMAND test-fpv-backend)
    add_executable(test-fpv-service tests/test_service.c)
    target_link_libraries(test-fpv-service PRIVATE fpv-scan)
    target_compile_options(test-fpv-service PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME scan-frame-freshness COMMAND test-fpv-service)
    add_test(NAME web-simulated COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_web.py $<TARGET_FILE:fpv-webd>
        ${CMAKE_CURRENT_SOURCE_DIR}/web)
    set_tests_properties(web-simulated PROPERTIES ENVIRONMENT "FPV_EXPECT_BOARD=${FPV_BOARD}")
endif()

if(NOT CMAKE_CROSSCOMPILING)
    foreach(case frontend radio radio_config)
        add_executable(test-fpv-${case} tests/test_${case}.c)
        target_link_libraries(test-fpv-${case} PRIVATE e200-no-os)
        target_compile_options(test-fpv-${case} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        add_test(NAME board-${case} COMMAND test-fpv-${case})
    endforeach()
endif()
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(test-fpv-scan-routes tests/test_scan_routes.c)
    target_link_libraries(test-fpv-scan-routes PRIVATE fpv-scan)
    target_compile_options(test-fpv-scan-routes PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME scan-rf-routes COMMAND test-fpv-scan-routes)
endif()

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(test-fpv-spectrum tests/test_spectrum.c)
    target_link_libraries(test-fpv-spectrum PRIVATE fpv-scan)
    target_compile_options(test-fpv-spectrum PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME spectrum-panorama COMMAND test-fpv-spectrum)
endif()
if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME spectrum-http COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spectrum_http.py $<TARGET_FILE:fpv-webd>
        ${CMAKE_CURRENT_SOURCE_DIR}/web)
endif()
if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME spectrum-driver-recovery COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spectrum_driver.py)
endif()
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(test-spectrum-device tests/test_spectrum_device.c)
    target_include_directories(test-spectrum-device PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test-spectrum-device PRIVATE m)
    target_compile_options(test-spectrum-device PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -UNDEBUG)
    add_test(NAME spectrum-device-abi COMMAND test-spectrum-device)
endif()

if(NOT CMAKE_CROSSCOMPILING)
    find_program(NODE_EXECUTABLE NAMES node nodejs)
    if(NODE_EXECUTABLE)
        add_test(NAME web-live-lock-controls COMMAND ${NODE_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_web_controls.js)
        add_test(NAME spectrum-ui-markers COMMAND ${NODE_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spectrum_ui.js)
    endif()
endif()
