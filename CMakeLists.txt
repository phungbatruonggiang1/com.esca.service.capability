cmake_minimum_required(VERSION 3.16)
project(esca-capability VERSION 0.1.0 LANGUAGES CXX)

# The AI capability provider for the ESCA board:
#   libesca-capability-core     manifest, device manager, discovery, CoAP, monitors
#   esca-capabilityd            the D-Bus service, vn.sparc.esca.Capability1
#   libesca-capability-client   what applications link
#   esca-capctl                 the service from a terminal

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
find_package(nlohmann_json_schema_validator REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
pkg_check_modules(SDBUSCPP REQUIRED IMPORTED_TARGET sdbus-c++)
pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)
pkg_check_modules(COAP REQUIRED IMPORTED_TARGET libcoap-3)

set(PUBLIC_INCLUDE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# --- the parts, static: only the service links them ---------------------------
add_library(esca-capability-core STATIC
    src/manifest/device_manifest.cpp
    src/core/logger.cpp
    src/core/device_manager.cpp
    src/core/discovery_manager.cpp
    src/core/connection_manager.cpp
    src/core/network_monitor.cpp
    src/core/performance_monitor.cpp)
target_include_directories(esca-capability-core PUBLIC src/manifest src/core)
target_link_libraries(esca-capability-core
    PUBLIC nlohmann_json::nlohmann_json Threads::Threads
    PRIVATE nlohmann_json_schema_validator::validator OpenSSL::Crypto PkgConfig::AVAHI PkgConfig::COAP)

# --- esca-capabilityd -----------------------------------------------------------
add_executable(esca-capabilityd
    src/service/main.cpp
    src/service/config.cpp
    src/service/capability_manager.cpp)
target_include_directories(esca-capabilityd PRIVATE include src/service)
target_link_libraries(esca-capabilityd PRIVATE esca-capability-core PkgConfig::SDBUSCPP)

# --- libesca-capability-client ----------------------------------------------------
add_library(esca-capability-client SHARED src/client/client.cpp)
target_include_directories(esca-capability-client PUBLIC ${PUBLIC_INCLUDE})
target_link_libraries(esca-capability-client
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE PkgConfig::SDBUSCPP)
set_target_properties(esca-capability-client PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)

# --- esca-capctl --------------------------------------------------------------------
add_executable(esca-capctl src/tools/capctl.cpp)
target_link_libraries(esca-capctl PRIVATE esca-capability-client)

# --- tests: manifest and device manager, runnable on the board ---------------------
add_executable(esca-capability-test test/manager_test.cpp)
target_include_directories(esca-capability-test PRIVATE test)
target_link_libraries(esca-capability-test PRIVATE esca-capability-core)

install(TARGETS esca-capability-client LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS esca-capabilityd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(TARGETS esca-capctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS esca-capability-test RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/esca-capability)
install(DIRECTORY include/esca DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES data/vn.sparc.esca.Capability1.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d)
install(FILES data/esca-capabilityd.service DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
install(FILES data/capability.conf DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/esca)
install(FILES data/device.json DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/esca/capability)
