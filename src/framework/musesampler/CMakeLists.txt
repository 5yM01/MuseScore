# SPDX-License-Identifier: GPL-3.0-only
# MuseScore-CLA-applies
#
# MuseScore
# Music Composition & Notation
#
# Copyright (C) 2022 MuseScore BVBA and others
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 3 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

set(MODULE musesampler)

set(MODULE_SRC
    ${CMAKE_CURRENT_LIST_DIR}/musesamplermodule.cpp
    ${CMAKE_CURRENT_LIST_DIR}/musesamplermodule.h
    ${CMAKE_CURRENT_LIST_DIR}/imusesamplerconfiguration.h

    ${CMAKE_CURRENT_LIST_DIR}/internal/musesamplerresolver.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/musesamplerresolver.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/musesamplerwrapper.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/musesamplerwrapper.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/musesamplersequencer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/musesamplersequencer.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/musesamplerconfiguration.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/musesamplerconfiguration.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/apitypes.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/libhandler.h
    )

if (MUSESAMPLER_SRC_PATH)
    add_subdirectory("ms_src")
    set(MODULE_LINK ms_src)
endif()

include(${PROJECT_SOURCE_DIR}/build/module.cmake)
