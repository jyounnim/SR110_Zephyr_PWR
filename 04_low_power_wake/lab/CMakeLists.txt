cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lab04_low_power_wake_m55)

target_sources(app PRIVATE src/main.c)
