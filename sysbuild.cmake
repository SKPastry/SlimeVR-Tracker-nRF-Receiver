set(partition_overlay_dir ${CMAKE_CURRENT_LIST_DIR}/dts/partitions)
set(mcuboot_overlay_dir ${CMAKE_CURRENT_LIST_DIR}/sysbuild)
set(partition_overlay)
set(use_uf2_partition_layout FALSE)

if(DEFINED EXTRA_DTC_OVERLAY_FILE)
  list(APPEND ${DEFAULT_IMAGE}_EXTRA_DTC_OVERLAY_FILE ${EXTRA_DTC_OVERLAY_FILE})
endif()

# CONFIG_* may be supplied directly, but application Kconfig normally has not
# run yet in sysbuild, so also inspect the board target and its defconfig.
if(DEFINED CONFIG_BUILD_OUTPUT_UF2 AND CONFIG_BUILD_OUTPUT_UF2)
  set(use_uf2_partition_layout TRUE)
endif()

if((DEFINED SB_CONFIG_BOARD AND
    SB_CONFIG_BOARD MATCHES "(^|[_/-])uf2($|[_/-])") OR
   (DEFINED SB_CONFIG_BOARD_QUALIFIERS AND
    SB_CONFIG_BOARD_QUALIFIERS MATCHES "(^|[_/-])uf2($|[_/-])"))
  set(use_uf2_partition_layout TRUE)
endif()

set(board_defconfig_dirs)

if(DEFINED BOARD_DIRECTORIES)
  list(APPEND board_defconfig_dirs ${BOARD_DIRECTORIES})
elseif(DEFINED BOARD_DIR)
  list(APPEND board_defconfig_dirs ${BOARD_DIR})
else()
  file(GLOB board_defconfig_dirs
    LIST_DIRECTORIES true
    ${CMAKE_CURRENT_LIST_DIR}/boards/*/${SB_CONFIG_BOARD}
  )
endif()

set(board_qualifiers_normalized)

if(DEFINED SB_CONFIG_BOARD_QUALIFIERS AND
   NOT SB_CONFIG_BOARD_QUALIFIERS STREQUAL "")
  string(REPLACE "/" "_" board_qualifiers_normalized
         ${SB_CONFIG_BOARD_QUALIFIERS})
endif()

set(board_defconfig_candidates)

foreach(board_defconfig_dir ${board_defconfig_dirs})
  set(found_specific_board_defconfig FALSE)

  if(NOT board_qualifiers_normalized STREQUAL "")
    set(specific_board_defconfig
      ${board_defconfig_dir}/${SB_CONFIG_BOARD}_${board_qualifiers_normalized}_defconfig
    )

    if(EXISTS ${specific_board_defconfig})
      list(APPEND board_defconfig_candidates ${specific_board_defconfig})
      set(found_specific_board_defconfig TRUE)
    endif()
  endif()

  if(NOT found_specific_board_defconfig)
    list(APPEND board_defconfig_candidates
      ${board_defconfig_dir}/${SB_CONFIG_BOARD}_defconfig
    )
  endif()
endforeach()

list(REMOVE_DUPLICATES board_defconfig_candidates)

foreach(board_defconfig_candidate ${board_defconfig_candidates})
  if(EXISTS ${board_defconfig_candidate})
    file(STRINGS ${board_defconfig_candidate} board_defconfig_uf2
      REGEX "^[ \t]*CONFIG_BUILD_OUTPUT_UF2[ \t]*=[ \t]*y[ \t]*$"
    )

    if(board_defconfig_uf2)
      set(use_uf2_partition_layout TRUE)
      break()
    endif()
  endif()
endforeach()

if(SB_CONFIG_BOOTLOADER_MCUBOOT AND SB_CONFIG_BOARD STREQUAL "holyiot_21017")
  message(FATAL_ERROR
          "holyiot_21017 uses its existing nRF5/OpenDFU boot path; MCUboot is not supported")
elseif(SB_CONFIG_BOOTLOADER_MCUBOOT AND SB_CONFIG_BOARD STREQUAL "xiao_ble")
  set(partition_overlay ${partition_overlay_dir}/nrf52840_xiao_mcuboot.overlay)
  list(APPEND mcuboot_EXTRA_DTC_OVERLAY_FILE ${partition_overlay})
elseif(SB_CONFIG_BOOTLOADER_MCUBOOT AND
       SB_CONFIG_BOARD STREQUAL "nrf52840dongle")
  set(partition_overlay ${partition_overlay_dir}/nrf52840_dongle_mcuboot.overlay)
  list(APPEND mcuboot_EXTRA_DTC_OVERLAY_FILE ${partition_overlay})
elseif(SB_CONFIG_BOOTLOADER_MCUBOOT AND SB_CONFIG_SOC_NRF52840)
  set(partition_overlay ${partition_overlay_dir}/nrf52840_mcuboot.overlay)
  list(APPEND mcuboot_EXTRA_DTC_OVERLAY_FILE ${partition_overlay})
elseif(SB_CONFIG_BOOTLOADER_MCUBOOT AND SB_CONFIG_SOC_NRF52833)
  set(partition_overlay ${partition_overlay_dir}/nrf52833_mcuboot.overlay)
  list(APPEND mcuboot_EXTRA_DTC_OVERLAY_FILE ${partition_overlay})
elseif(SB_CONFIG_BOARD STREQUAL "xiao_ble")
  set(partition_overlay ${partition_overlay_dir}/nrf52840_xiao.overlay)
elseif(SB_CONFIG_BOARD MATCHES "^nrf52840dongle$|^holyiot_21017$")
  set(partition_overlay ${partition_overlay_dir}/nrf52840_dongle.overlay)
elseif(use_uf2_partition_layout AND SB_CONFIG_SOC_NRF52833)
  set(partition_overlay ${partition_overlay_dir}/nrf52833_uf2.overlay)
elseif(use_uf2_partition_layout AND SB_CONFIG_SOC_NRF52840)
  set(partition_overlay ${partition_overlay_dir}/nrf52840_uf2.overlay)
elseif(SB_CONFIG_SOC_NRF52833)
  set(partition_overlay ${partition_overlay_dir}/nrf52833_direct.overlay)
elseif(SB_CONFIG_SOC_NRF52840)
  set(partition_overlay ${partition_overlay_dir}/nrf52840_direct.overlay)
endif()

if(SB_CONFIG_BOOTLOADER_MCUBOOT)
  set_property(TARGET mcuboot APPEND PROPERTY _EP_CMAKE_ARGS
               -DBOARD_DEFCONFIG:FILEPATH=${mcuboot_overlay_dir}/mcuboot_board_defconfig)
  list(APPEND mcuboot_EXTRA_DTC_OVERLAY_FILE
       ${mcuboot_overlay_dir}/mcuboot.overlay
       ${mcuboot_overlay_dir}/mcuboot_boot_mode.overlay)
  list(APPEND ${DEFAULT_IMAGE}_EXTRA_DTC_OVERLAY_FILE
       ${mcuboot_overlay_dir}/mcuboot_boot_mode.overlay)
  list(APPEND ${DEFAULT_IMAGE}_EXTRA_CONF_FILE
       ${CMAKE_CURRENT_LIST_DIR}/boards/mcuboot.conf)
  list(APPEND mcuboot_EXTRA_CONF_FILE
       ${mcuboot_overlay_dir}/mcuboot_usb_legacy.conf)
  list(REMOVE_DUPLICATES mcuboot_EXTRA_CONF_FILE)
  set(mcuboot_EXTRA_CONF_FILE
      ${mcuboot_EXTRA_CONF_FILE}
      CACHE INTERNAL "")
  list(REMOVE_DUPLICATES mcuboot_EXTRA_DTC_OVERLAY_FILE)
  set(mcuboot_EXTRA_DTC_OVERLAY_FILE
      ${mcuboot_EXTRA_DTC_OVERLAY_FILE}
      CACHE INTERNAL "")
endif()

if(partition_overlay)
  list(APPEND ${DEFAULT_IMAGE}_EXTRA_DTC_OVERLAY_FILE ${partition_overlay})
  set(${DEFAULT_IMAGE}_EXTRA_DTC_OVERLAY_FILE
      ${${DEFAULT_IMAGE}_EXTRA_DTC_OVERLAY_FILE}
      CACHE INTERNAL "")
endif()

if(DEFINED ${DEFAULT_IMAGE}_EXTRA_CONF_FILE)
  list(REMOVE_DUPLICATES ${DEFAULT_IMAGE}_EXTRA_CONF_FILE)
  set(${DEFAULT_IMAGE}_EXTRA_CONF_FILE
      ${${DEFAULT_IMAGE}_EXTRA_CONF_FILE}
      CACHE INTERNAL "")
endif()
