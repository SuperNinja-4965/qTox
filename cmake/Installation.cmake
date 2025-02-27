# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright © 2019 by The qTox Project Contributors
# Copyright © 2024-2025 The TokTok team.

################################################################################
#
# :: Installation
#
################################################################################

if(APPLE)
  set_target_properties(${PROJECT_NAME} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/macos/Info.plist")

  set(BUNDLE_PATH "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.app")

  install(FILES img/icons/qtox.icns DESTINATION ${BUNDLE_PATH}/Contents/Resources/)
  install(FILES img/icons/qtox_profile.icns DESTINATION ${BUNDLE_PATH}/Contents/Resources/)

  install(CODE "
  message(STATUS \"Creating dmg image\")
  execute_process(COMMAND ${CMAKE_SOURCE_DIR}/macos/createdmg ${CMAKE_SOURCE_DIR} ${BUNDLE_PATH})
  " COMPONENT Runtime
  )
else()
  include(GNUInstallDirs)
  # follow the xdg-desktop specification
  install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  install(FILES "res/io.github.TokTok.qTox.appdata.xml" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/metainfo")
  install(FILES "io.github.TokTok.qTox.desktop" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")

  # Install application icons according to the XDG spec
  set(ICON_SIZES 14 16 22 24 32 36 48 64 72 96 128 192 256 512)
  foreach(size ${ICON_SIZES})
    set(path_from "img/icons/${size}x${size}/qtox.png")
    set(path_to "share/icons/hicolor/${size}x${size}/apps/")
    install(FILES ${path_from} DESTINATION ${path_to})
  endforeach(size)

  # process the icon, compress if enabled
  set(SVG_SRC "${CMAKE_SOURCE_DIR}/img/icons/qtox.svg")
  if(${SVGZ_ICON})
    set(SVG_GZIP "${CMAKE_BINARY_DIR}/qtox.svgz")
    install(CODE "
    execute_process(COMMAND gzip -S z INPUT_FILE ${SVG_SRC} OUTPUT_FILE ${SVG_GZIP})
    " COMPONENT Runtime)
    set(SVG_DEST "${SVG_GZIP}")
  else()
    set(SVG_DEST "${SVG_SRC}")
  endif()
  install(FILES "${SVG_DEST}" DESTINATION "share/icons/hicolor/scalable/apps")
endif()
