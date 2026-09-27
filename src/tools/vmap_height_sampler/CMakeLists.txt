# Copyright (C) 2008-2018 TrinityCore <https://www.trinitycore.org/>
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#
# Standalone diagnostic CLI: VMap-aware ground height lookup without a full
# worldserver process. See backlog_brief_2026-09-23_npc_hoehen_restfaelle.md.

set(PRIVATE_SOURCES VMapHeightSampler.cpp)

add_executable(vmapheightsampler ${PRIVATE_SOURCES})

target_link_libraries(vmapheightsampler
  PRIVATE
    trinity-core-interface
  PUBLIC
    common)

set_target_properties(vmapheightsampler
    PROPERTIES
      FOLDER
        "tools")

if( UNIX )
  install(TARGETS vmapheightsampler DESTINATION bin)
elseif( WIN32 )
  install(TARGETS vmapheightsampler DESTINATION "${CMAKE_INSTALL_PREFIX}")
endif()
