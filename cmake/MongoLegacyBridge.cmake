# mongo-c-driver 1.x for MongoDB 3.x — built only into sqleditor-mongo3-bridge (subprocess IPC).
# Built via ExternalProject so vcpkg libbson/mongoc 2.x headers never shadow bundled libbson 1.x.

include(ExternalProject)

# Pin to 1.20.x: min wire version 3 (MongoDB 3.0+). 1.21+ requires wire 6 (MongoDB 3.6+).
set(MONGO_LEGACY_VERSION "1.20.1")
set(MONGO_LEGACY_INSTALL_DIR "${CMAKE_BINARY_DIR}/mongo-legacy-install")

ExternalProject_Add(
    mongo_c_driver_legacy_ep
    URL "https://github.com/mongodb/mongo-c-driver/archive/refs/tags/${MONGO_LEGACY_VERSION}.tar.gz"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=${MONGO_LEGACY_INSTALL_DIR}
        -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF
        -DENABLE_TESTS=OFF
        -DENABLE_EXAMPLES=OFF
        -DENABLE_MONGOC=ON
        -DENABLE_BSON=ON
        -DENABLE_SSL=OPENSSL
        -DENABLE_SASL=OFF
        -DENABLE_SNAPPY=OFF
        -DENABLE_ZSTD=OFF
        -DENABLE_ZLIB=BUNDLED
        -DENABLE_UNINSTALL=OFF
        -DBUILD_SHARED_LIBS=OFF
        -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}
    BUILD_BYPRODUCTS
        "${MONGO_LEGACY_INSTALL_DIR}/lib/libmongoc-static-1.0.a"
        "${MONGO_LEGACY_INSTALL_DIR}/lib/libbson-static-1.0.a"
    INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
)

add_executable(sqleditor-mongo3-bridge src/database/mongodb_old/mongo3_bridge_main.cpp)
add_dependencies(sqleditor-mongo3-bridge mongo_c_driver_legacy_ep)

target_include_directories(
    sqleditor-mongo3-bridge
    PRIVATE "${MONGO_LEGACY_INSTALL_DIR}/include/libbson-1.0"
            "${MONGO_LEGACY_INSTALL_DIR}/include/libmongoc-1.0"
)

find_package(ICU COMPONENTS uc i18n QUIET)
if(ICU_FOUND)
    set(_MONGO_LEGACY_ICU_LIBS ICU::uc ICU::i18n)
else()
    set(_MONGO_LEGACY_ICU_LIBS icuuc icui18n)
endif()

target_link_libraries(
    sqleditor-mongo3-bridge
    PRIVATE "${MONGO_LEGACY_INSTALL_DIR}/lib/libmongoc-static-1.0.a"
            "${MONGO_LEGACY_INSTALL_DIR}/lib/libbson-static-1.0.a"
            OpenSSL::SSL
            OpenSSL::Crypto
            nlohmann_json::nlohmann_json
            ${_MONGO_LEGACY_ICU_LIBS}
            pthread
            resolv
)
unset(_MONGO_LEGACY_ICU_LIBS)

target_compile_definitions(sqleditor-mongo3-bridge PRIVATE BSON_STATIC MONGOC_STATIC)

if(MSVC)
    target_compile_options(sqleditor-mongo3-bridge PRIVATE /wd4996)
endif()
