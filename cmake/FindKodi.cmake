# FindKodi.cmake
#
# Locate the Kodi add-on development headers (kodi-dev-kit).
#
# Set KODI_INCLUDE_DIR to the directory that contains the "kodi" folder, e.g.
#   -DKODI_INCLUDE_DIR="C:/kodi-dev-kit/include"
#
# For backwards compatibility the old convention where KODI_INCLUDE_DIR points
# at the "kodi" folder itself is also accepted.
#
# Provides:
#   KODI_INCLUDE_DIRS - include directory to pass to the compiler
#   KODI_FOUND

find_path(KODI_INCLUDE_DIR
  NAMES kodi/addon-instance/PVR.h
  HINTS
    ENV KODI_INCLUDE_DIR
    ${KODI_INCLUDE_DIR}
  PATHS
    /usr/include
    /usr/local/include
    /opt/kodi/include
    "${CMAKE_SOURCE_DIR}/kodi-dev-kit/include"
    "${CMAKE_SOURCE_DIR}/../kodi-dev-kit/include"
)

if(KODI_INCLUDE_DIR AND EXISTS "${KODI_INCLUDE_DIR}/kodi/addon-instance/PVR.h")
  set(KODI_INCLUDE_DIRS "${KODI_INCLUDE_DIR}")
elseif(KODI_INCLUDE_DIR AND EXISTS "${KODI_INCLUDE_DIR}/addon-instance/PVR.h")
  # KODI_INCLUDE_DIR points at .../include/kodi
  get_filename_component(KODI_INCLUDE_DIRS "${KODI_INCLUDE_DIR}/.." ABSOLUTE)
else()
  set(KODI_INCLUDE_DIRS "")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Kodi
  REQUIRED_VARS KODI_INCLUDE_DIRS
  REASON_FAILURE_MESSAGE
    "Kodi development headers not found. Set -DKODI_INCLUDE_DIR to the kodi-dev-kit include directory (the one containing 'kodi/addon-instance/PVR.h')."
)

mark_as_advanced(KODI_INCLUDE_DIR)
