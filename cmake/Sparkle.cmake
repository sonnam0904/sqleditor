# Sparkle integration for macOS auto-updates

set(SPARKLE_VERSION "2.9.1")
set(SPARKLE_ROOT "${CMAKE_SOURCE_DIR}/external/Sparkle")
set(SPARKLE_FRAMEWORK_PATH "${SPARKLE_ROOT}/Sparkle.framework")

if(NOT EXISTS "${SPARKLE_FRAMEWORK_PATH}")
  set(SPARKLE_ARCHIVE "${CMAKE_BINARY_DIR}/Sparkle-${SPARKLE_VERSION}.tar.xz")
  message(STATUS "Downloading Sparkle ${SPARKLE_VERSION}...")
  file(
        DOWNLOAD
        "https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz"
        "${SPARKLE_ARCHIVE}"
        SHOW_PROGRESS
    )
  file(MAKE_DIRECTORY "${SPARKLE_ROOT}")
  file(ARCHIVE_EXTRACT INPUT "${SPARKLE_ARCHIVE}" DESTINATION "${SPARKLE_ROOT}")
endif()

if(NOT DEFINED SPARKLE_ED_PUBLIC_KEY)
  set(SPARKLE_ED_PUBLIC_KEY
        ""
        CACHE STRING
        "Sparkle EdDSA public key for update verification"
    )
endif()
