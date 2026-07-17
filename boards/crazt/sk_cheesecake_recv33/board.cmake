# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

board_runner_args(jlink "--device=nRF52833_xxAA" "--speed=4000")
board_runner_args(nrfutil "--nrf-family=NRF52")
board_runner_args(pyocd "--target=nrf52833" "--frequency=4000000")

if(CONFIG_BUILD_OUTPUT_UF2)
  include(${ZEPHYR_BASE}/boards/common/uf2.board.cmake)
endif()

include(${ZEPHYR_BASE}/boards/common/nrfutil.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/nrfjprog.board.cmake)
include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
