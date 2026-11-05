cmake_minimum_required(VERSION 3.16)
project(smart_office VERSION 1.0.0 LANGUAGES CXX
        DESCRIPTION "Smart Office — multi-tenant office analytics on a DEEPX DX-M1 NPU")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# ── Required everywhere ─────────────────────────────────────────────────────
find_package(DXRT REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# GStreamer is talked to directly rather than through cv::VideoCapture: the
# OpenCV on this board reports "GStreamer: NO", so CAP_GSTREAMER would silently
# fall through to FFMPEG software decode — precisely the CPU cost this
# deployment exists to avoid.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0)

# ── Optional: the HTTP/DB layer (Phase 3) ───────────────────────────────────
# Phases 0-2 (NPU pools, decoders, domain modules) deliberately do not depend
# on Drogon or libpq, so the whole vision core builds and its tests run on a
# bare host where only DXRT and OpenCV are installed. The API layer switches
# itself on when its dependencies appear (they do inside the Docker image).
find_package(Drogon QUIET)
find_package(PostgreSQL QUIET)
# libcurl carries BOTH outbound channels: Telegram's HTTPS multipart sendPhoto
# and SMTP with STARTTLS. One dependency instead of a hand-rolled TLS client and
# a hand-rolled SMTP conversation, neither of which is a good place to make a
# mistake.
find_package(CURL QUIET)
if(Drogon_FOUND AND PostgreSQL_FOUND)
    set(SO_WITH_API ON)
    message(STATUS "smart_office: API layer ENABLED (Drogon + PostgreSQL found)")
else()
    set(SO_WITH_API OFF)
    message(STATUS "smart_office: API layer DISABLED — "
                   "Drogon_FOUND=${Drogon_FOUND} PostgreSQL_FOUND=${PostgreSQL_FOUND} CURL_FOUND=${CURL_FOUND}. "
                   "Vision core and its tests still build.")
endif()

# ── so_core: everything except main() ───────────────────────────────────────
set(SO_CORE_SOURCES
    src/core/log.cpp
    src/core/config.cpp
    src/core/engine_pool.cpp
    src/core/camera_reader.cpp
    src/core/host_cpu.cpp
    src/core/stream_probe.cpp
    src/core/onvif.cpp
    src/core/postprocess_pool.cpp
    src/core/camera_worker.cpp
    src/core/preprocess.cpp
    src/vision/tensor_view.cpp
    src/vision/yolo26_pose.cpp
    src/vision/scrfd.cpp
    src/vision/arcface.cpp
    src/vision/geometry.cpp
    src/vision/kalman_box.cpp
    src/vision/tracker.cpp
    src/modules/module.cpp
    src/identity/gallery.cpp
    src/identity/face_identifier.cpp
    src/domain/attendance_rollup.cpp
    src/modules/fall.cpp
    src/modules/perimeter.cpp
    src/modules/desk_duty.cpp
    src/modules/unauthorised.cpp
    src/modules/attendance.cpp
    src/modules/out_count.cpp
    src/api/auth.cpp
)

# The database and HTTP layers are only compiled where their dependencies
# exist. Phases 0-2 (NPU pools, decoders, domain modules) deliberately do not
# depend on them, so the whole vision core and its 127 tests build and run on a
# bare host with only DXRT and OpenCV installed.
if(SO_WITH_API)
    list(APPEND SO_CORE_SOURCES
        src/db/database.cpp
        src/db/repos.cpp
        src/api/server.cpp
        src/api/controllers/auth_ctrl.cpp
        src/api/controllers/admin_ctrl.cpp
        src/api/controllers/config_ctrl.cpp
        src/api/controllers/people_ctrl.cpp
        src/api/controllers/report_ctrl.cpp
        src/api/controllers/stream_ctrl.cpp
        src/notify/outbox.cpp
        src/notify/telegram.cpp
        src/notify/email.cpp
        src/notify/snapshot.cpp
        src/app/engine.cpp
    )
endif()

add_library(so_core STATIC ${SO_CORE_SOURCES})
target_include_directories(so_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(so_core PUBLIC
    DXRT::dxrt ${OpenCV_LIBS} Threads::Threads PkgConfig::GST
    OpenSSL::Crypto OpenSSL::SSL)

# curl is wanted on its OWN terms, not as part of the API layer.
#
# Asking a camera what resolutions it supports is ONVIF over HTTP, and has
# nothing to do with Drogon or PostgreSQL. Tying it to SO_WITH_API meant the
# host build — which has no Drogon — silently compiled the "this build has no
# HTTP client" stub, so the one test that proves the SOAP exchange against a
# real camera could only ever run inside the container.
if(CURL_FOUND)
    target_compile_definitions(so_core PUBLIC SO_HAVE_CURL=1)
    target_link_libraries(so_core PUBLIC CURL::libcurl)
endif()

if(SO_WITH_API)
    target_compile_definitions(so_core PUBLIC SO_WITH_API=1)
    target_link_libraries(so_core PUBLIC Drogon::Drogon ${PostgreSQL_LIBRARIES})
    target_include_directories(so_core PUBLIC ${PostgreSQL_INCLUDE_DIRS})
endif()

# ── the binary ──────────────────────────────────────────────────────────────
add_executable(smart_office src/main.cpp)
target_link_libraries(smart_office PRIVATE so_core)

# ── tests + tools ───────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
add_subdirectory(tools)
