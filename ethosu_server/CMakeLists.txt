# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Register nested Infineon ML Zephyr modules before Zephyr discovery.
list(APPEND ZEPHYR_EXTRA_MODULES
    "$ENV{ZEPHYR_BASE}/../modules/hal/infineon/ml-middleware"
    "$ENV{ZEPHYR_BASE}/../modules/hal/infineon/ml-tflite-micro"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ethosu_server LANGUAGES C CXX)

target_sources(app PRIVATE src/main.c)

# Shared mailbox layout lives next to the loader client.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../loader)

# The CM33 loader powers the U55 before releasing this core; the SRSS
# registers Cy_SysU55Enable() touches are secure-only here.
zephyr_ld_options(-Wl,--wrap=Cy_SysU55Enable)
