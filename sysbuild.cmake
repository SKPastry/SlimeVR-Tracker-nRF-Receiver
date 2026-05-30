set(pm_static_dir ${CMAKE_CURRENT_LIST_DIR}/pm_static)

set(pm_static_override_candidates)
set(use_uf2_partition_layout FALSE)

if(DEFINED SB_CONFIG_BOARD_QUALIFIERS AND NOT SB_CONFIG_BOARD_QUALIFIERS STREQUAL "")
  string(REPLACE "/" "_" pm_static_board_qualifiers ${SB_CONFIG_BOARD_QUALIFIERS})
  list(APPEND pm_static_override_candidates
    ${pm_static_dir}/pm_static_${SB_CONFIG_BOARD}_${pm_static_board_qualifiers}.yml
    ${pm_static_dir}/${SB_CONFIG_BOARD}_${pm_static_board_qualifiers}.yml
  )
endif()

list(APPEND pm_static_override_candidates
  ${pm_static_dir}/pm_static_${SB_CONFIG_BOARD}.yml
  ${pm_static_dir}/${SB_CONFIG_BOARD}.yml
)

foreach(pm_static_override_candidate ${pm_static_override_candidates})
  if(EXISTS ${pm_static_override_candidate})
    set(PM_STATIC_YML_FILE ${pm_static_override_candidate} CACHE INTERNAL "")
    break()
  endif()
endforeach()

if(DEFINED CONFIG_BUILD_OUTPUT_UF2 AND CONFIG_BUILD_OUTPUT_UF2)
  set(use_uf2_partition_layout TRUE)
endif()

if((DEFINED SB_CONFIG_BOARD AND SB_CONFIG_BOARD MATCHES "(^|[_/-])uf2($|[_/-])") OR
   (DEFINED SB_CONFIG_BOARD_QUALIFIERS AND SB_CONFIG_BOARD_QUALIFIERS MATCHES "(^|[_/-])uf2($|[_/-])"))
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

set(board_defconfig_candidates)

foreach(board_defconfig_dir ${board_defconfig_dirs})
  set(found_specific_board_defconfig FALSE)

  if(DEFINED pm_static_board_qualifiers)
    set(specific_board_defconfig
      ${board_defconfig_dir}/${SB_CONFIG_BOARD}_${pm_static_board_qualifiers}_defconfig
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

if(DEFINED PM_STATIC_YML_FILE)
  return()
elseif(SB_CONFIG_BOARD STREQUAL "xiao_ble")
  set(PM_STATIC_YML_FILE ${pm_static_dir}/nrf52840_xiao.yml CACHE INTERNAL "")
elseif(SB_CONFIG_BOARD MATCHES "^nrf52840dongle$|^holyiot_21017$")
  set(PM_STATIC_YML_FILE ${pm_static_dir}/nrf52840_dongle.yml CACHE INTERNAL "")
elseif(use_uf2_partition_layout)
  if(SB_CONFIG_SOC_NRF52833)
    set(PM_STATIC_YML_FILE ${pm_static_dir}/nrf52833_uf2.yml CACHE INTERNAL "")
  elseif(SB_CONFIG_SOC_NRF52840)
    set(PM_STATIC_YML_FILE ${pm_static_dir}/nrf52840_uf2.yml CACHE INTERNAL "")
  endif()
endif()
