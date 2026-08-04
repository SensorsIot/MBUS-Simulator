# ESP-IDF project root. `components/` beside it is picked up automatically, so
# e450_core is built from the same sources the host tier compiles — FSD §2.2.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Must stay MBUS-Simulator: the build workflow reports on build/MBUS-Simulator.bin.
project(MBUS-Simulator)
