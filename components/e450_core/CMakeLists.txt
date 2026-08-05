# Dual-build component — FSD §2.2.
#
# Under ESP-IDF this registers as a component; under a plain CMake host build it
# is an ordinary static library. The whole point is that the same sources compile
# both ways: the host tier (FR-ENC-12) can only exist if nothing in here reaches
# for driver/uart.h or FreeRTOS. If you find yourself needing an ESP-IDF header,
# the code belongs in main/, not here.

set(E450_CORE_SRCS
    src/crc.cpp
    src/datetime.cpp
    src/value_model.cpp
    src/telegram.cpp
    src/ebl_ci1.cpp
    src/modes.cpp)

if(ESP_PLATFORM)
  idf_component_register(SRCS ${E450_CORE_SRCS} INCLUDE_DIRS include)
  return()
endif()

add_library(e450_core ${E450_CORE_SRCS})
target_compile_features(e450_core PUBLIC cxx_std_20)
target_include_directories(e450_core PUBLIC include)
target_compile_options(e450_core PRIVATE -Wall -Wextra -Wpedantic)
