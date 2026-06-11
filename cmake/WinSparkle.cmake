# WinSparkle integration for Windows auto-updates
#
# Auto-downloads WinSparkle if not already present in external/WinSparkle/. Uses
# the same appcast.xml format as macOS Sparkle.

set(WINSPARKLE_VERSION "0.9.2")
set(WINSPARKLE_ROOT "${CMAKE_SOURCE_DIR}/external/WinSparkle")
set(WINSPARKLE_INCLUDE_DIR "${WINSPARKLE_ROOT}/include")
set(WINSPARKLE_LIB "${WINSPARKLE_ROOT}/x64/Release/WinSparkle.lib")
set(WINSPARKLE_DLL "${WINSPARKLE_ROOT}/x64/Release/WinSparkle.dll")

if(NOT EXISTS "${WINSPARKLE_INCLUDE_DIR}/winsparkle.h")
  set(WINSPARKLE_ZIP
        "${CMAKE_BINARY_DIR}/WinSparkle-${WINSPARKLE_VERSION}.zip"
    )
  set(WINSPARKLE_URL
        "https://github.com/vslavik/winsparkle/releases/download/v${WINSPARKLE_VERSION}/WinSparkle-${WINSPARKLE_VERSION}.zip"
    )

  if(NOT EXISTS "${WINSPARKLE_ZIP}")
    message(STATUS "Downloading WinSparkle ${WINSPARKLE_VERSION}...")
    file(
            DOWNLOAD "${WINSPARKLE_URL}"
            "${WINSPARKLE_ZIP}"
            STATUS WINSPARKLE_DL_STATUS
            SHOW_PROGRESS
        )
    list(GET WINSPARKLE_DL_STATUS 0 WINSPARKLE_DL_CODE)
    if(NOT WINSPARKLE_DL_CODE EQUAL 0)
      file(REMOVE "${WINSPARKLE_ZIP}")
      list(GET WINSPARKLE_DL_STATUS 1 WINSPARKLE_DL_MSG)
      message(
                FATAL_ERROR
                "Failed to download WinSparkle: ${WINSPARKLE_DL_MSG}"
            )
    endif()
  endif()

  message(STATUS "Extracting WinSparkle to ${WINSPARKLE_ROOT}...")
  file(
        ARCHIVE_EXTRACT
        INPUT "${WINSPARKLE_ZIP}"
        DESTINATION "${CMAKE_SOURCE_DIR}/external"
    )

  # The zip extracts as WinSparkle-<version>/ — rename to WinSparkle/
  set(WINSPARKLE_EXTRACTED
        "${CMAKE_SOURCE_DIR}/external/WinSparkle-${WINSPARKLE_VERSION}"
    )
  if(EXISTS "${WINSPARKLE_EXTRACTED}" AND NOT EXISTS "${WINSPARKLE_ROOT}")
    file(RENAME "${WINSPARKLE_EXTRACTED}" "${WINSPARKLE_ROOT}")
  endif()

  if(NOT EXISTS "${WINSPARKLE_INCLUDE_DIR}/winsparkle.h")
    message(
            FATAL_ERROR
            "WinSparkle extraction failed — winsparkle.h not found at ${WINSPARKLE_INCLUDE_DIR}"
        )
  endif()

  message(STATUS "WinSparkle ${WINSPARKLE_VERSION} ready")
endif()

# Create imported target
add_library(WinSparkle SHARED IMPORTED)
set_target_properties(
    WinSparkle
    PROPERTIES
        IMPORTED_IMPLIB "${WINSPARKLE_LIB}"
        IMPORTED_LOCATION "${WINSPARKLE_DLL}"
        INTERFACE_INCLUDE_DIRECTORIES "${WINSPARKLE_INCLUDE_DIR}"
)
