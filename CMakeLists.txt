if(CONFIG_SPACE_OLED_HUD)
  zephyr_library_named(space_oled_hud)

  zephyr_library_include_directories(
    ${APPLICATION_SOURCE_DIR}/include
  )

  zephyr_library_sources(src/space_status.c)
endif()
