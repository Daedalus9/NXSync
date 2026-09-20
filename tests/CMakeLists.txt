cmake_minimum_required(VERSION 3.18)
project(nxsync_host_tests LANGUAGES C CXX)

enable_testing()

# Test assertions must run even in Release configurations.
if(MSVC)
    add_compile_options(/UNDEBUG)
else()
    add_compile_options(-UNDEBUG)
endif()

if(UNIX AND NOT APPLE)
    add_executable(persistence_failure_test persistence_failure_test.cpp
        ../source/atomic_file.cpp ../source/cloud_queue.cpp)
    target_compile_features(persistence_failure_test PRIVATE cxx_std_17)
    target_include_directories(persistence_failure_test PRIVATE ../include)
    target_link_options(persistence_failure_test PRIVATE -Wl,--wrap=rename)
    add_test(NAME persistence_failure_test COMMAND persistence_failure_test)
endif()

add_executable(installer_core_test
    installer_core_test.cpp
    ../installer/source/core.cpp)
target_compile_features(installer_core_test PRIVATE cxx_std_17)
target_include_directories(installer_core_test PRIVATE ../installer/include)
add_test(NAME installer_core_test COMMAND installer_core_test)

add_executable(atomic_file_test
    atomic_file_test.cpp
    ../source/atomic_file.cpp)
target_compile_features(atomic_file_test PRIVATE cxx_std_17)
target_include_directories(atomic_file_test PRIVATE ../include)
add_test(NAME atomic_file_test COMMAND atomic_file_test)

add_executable(remote_layout_test
    remote_layout_test.cpp
    ../source/nextcloud_paths.cpp
    ../source/remote_layout.cpp)

target_compile_features(remote_layout_test PRIVATE cxx_std_17)
target_include_directories(remote_layout_test PRIVATE ../include)

add_test(NAME remote_layout_test COMMAND remote_layout_test)

add_executable(app_config_test
    app_config_test.cpp
    credential_crypto_test_backend.cpp
    ../source/app_config.cpp
    ../source/nextcloud_paths.cpp)

target_compile_features(app_config_test PRIVATE cxx_std_17)
target_include_directories(app_config_test PRIVATE ../include)

add_test(NAME app_config_test COMMAND app_config_test)

add_executable(game_version_test
    game_version_test.cpp
    ../source/game_version.cpp)

target_compile_features(game_version_test PRIVATE cxx_std_17)
target_include_directories(game_version_test PRIVATE ../include)

add_test(NAME game_version_test COMMAND game_version_test)

add_executable(retention_test
    retention_test.cpp
    ../source/retention.cpp)

target_compile_features(retention_test PRIVATE cxx_std_17)
target_include_directories(retention_test PRIVATE ../include)

add_test(NAME retention_test COMMAND retention_test)

add_executable(display_text_test
    display_text_test.cpp
    ../source/display_text.cpp)

target_compile_features(display_text_test PRIVATE cxx_std_17)
target_include_directories(display_text_test PRIVATE ../include)

add_test(NAME display_text_test COMMAND display_text_test)

add_executable(backup_format_test
    backup_format_test.cpp
    ../source/backup_format.cpp)

target_compile_features(backup_format_test PRIVATE cxx_std_17)
target_include_directories(backup_format_test PRIVATE ../include)

add_test(NAME backup_format_test COMMAND backup_format_test)

add_executable(cloud_index_test
    cloud_index_test.cpp
    ../source/cloud_index.cpp
    ../source/remote_layout.cpp)

target_compile_features(cloud_index_test PRIVATE cxx_std_17)
target_include_directories(cloud_index_test PRIVATE ../include)

add_test(NAME cloud_index_test COMMAND cloud_index_test)

add_executable(revision_compare_test
    revision_compare_test.cpp
    ../source/revision_compare.cpp)

target_compile_features(revision_compare_test PRIVATE cxx_std_17)
target_include_directories(revision_compare_test PRIVATE ../include)

add_test(NAME revision_compare_test COMMAND revision_compare_test)

add_executable(revision_lineage_test
    revision_lineage_test.cpp
    ../source/revision_lineage.cpp
    ../source/revision_compare.cpp)
target_compile_features(revision_lineage_test PRIVATE cxx_std_17)
target_include_directories(revision_lineage_test PRIVATE ../include)
add_test(NAME revision_lineage_test COMMAND revision_lineage_test)

add_executable(preflight_test
    preflight_test.cpp
    ../source/preflight.cpp
    ../source/revision_compare.cpp)

target_compile_features(preflight_test PRIVATE cxx_std_17)
target_include_directories(preflight_test PRIVATE ../include)

add_test(NAME preflight_test COMMAND preflight_test)

add_executable(preflight_protocol_test
    preflight_protocol_test.cpp
    ../source/atomic_file.cpp
    ../source/preflight_protocol.cpp)
target_compile_features(preflight_protocol_test PRIVATE cxx_std_17)
target_include_directories(preflight_protocol_test PRIVATE ../include)
add_test(NAME preflight_protocol_test COMMAND preflight_protocol_test)

add_executable(launch_protocol_test
    launch_protocol_test.cpp
    ../source/atomic_file.cpp
    ../source/launch_protocol.cpp)
target_compile_features(launch_protocol_test PRIVATE cxx_std_17)
target_include_directories(launch_protocol_test PRIVATE ../include)
add_test(NAME launch_protocol_test COMMAND launch_protocol_test)

add_executable(local_resolution_test
    local_resolution_test.cpp
    ../source/local_resolution.cpp)
target_compile_features(local_resolution_test PRIVATE cxx_std_17)
target_include_directories(local_resolution_test PRIVATE ../include)
add_test(NAME local_resolution_test COMMAND local_resolution_test)

add_executable(launch_gate_policy_test
    launch_gate_policy_test.cpp)
target_compile_features(launch_gate_policy_test PRIVATE cxx_std_17)
target_include_directories(launch_gate_policy_test PRIVATE ../include)
add_test(NAME launch_gate_policy_test COMMAND launch_gate_policy_test)

add_executable(overlay_catalog_test
    overlay_catalog_test.cpp
    ../source/overlay_catalog.cpp
    ../source/atomic_file.cpp)
target_compile_features(overlay_catalog_test PRIVATE cxx_std_17)
target_include_directories(overlay_catalog_test PRIVATE ../include)
add_test(NAME overlay_catalog_test COMMAND overlay_catalog_test)

add_executable(overlay_dependencies_test
    overlay_dependencies_test.cpp)
target_compile_features(overlay_dependencies_test PRIVATE cxx_std_17)
target_include_directories(overlay_dependencies_test PRIVATE ../include)
add_test(NAME overlay_dependencies_test COMMAND overlay_dependencies_test)

add_executable(cloud_queue_test
    cloud_queue_test.cpp
    ../source/cloud_queue.cpp
    ../source/atomic_file.cpp)

target_compile_features(cloud_queue_test PRIVATE cxx_std_17)
target_include_directories(cloud_queue_test PRIVATE ../include)

add_test(NAME cloud_queue_test COMMAND cloud_queue_test)

add_executable(cloud_worker_status_test
    cloud_worker_status_test.cpp
    ../source/cloud_worker_status.cpp
    ../source/atomic_file.cpp)
target_include_directories(cloud_worker_status_test PRIVATE ../include)
add_test(NAME cloud_worker_status_test COMMAND cloud_worker_status_test)

add_executable(cloud_worker_policy_test
    cloud_worker_policy_test.cpp)
target_compile_features(cloud_worker_policy_test PRIVATE cxx_std_17)
target_include_directories(cloud_worker_policy_test PRIVATE ../include)
add_test(NAME cloud_worker_policy_test COMMAND cloud_worker_policy_test)

add_executable(sync_engine_test
    sync_engine_test.cpp
    ../source/sync_engine.cpp
    ../source/cloud_queue.cpp
    ../source/atomic_file.cpp
    ../source/cloud_index.cpp
    ../source/remote_layout.cpp)

target_compile_features(sync_engine_test PRIVATE cxx_std_17)
target_include_directories(sync_engine_test PRIVATE ../include)

add_test(NAME sync_engine_test COMMAND sync_engine_test)

add_executable(sysmodule_state_test
    sysmodule_state_test.cpp
    ../source/sysmodule_config.cpp
    ../source/sysmodule_status.cpp
    ../source/atomic_file.cpp)

target_compile_features(sysmodule_state_test PRIVATE cxx_std_17)
target_include_directories(sysmodule_state_test PRIVATE ../include)

add_test(NAME sysmodule_state_test COMMAND sysmodule_state_test)

add_executable(storage_environment_test
    storage_environment_test.cpp
    ../source/storage_environment.cpp)
target_compile_features(storage_environment_test PRIVATE cxx_std_17)
target_include_directories(storage_environment_test PRIVATE ../include)
add_test(NAME storage_environment_test COMMAND storage_environment_test)

add_executable(application_lifecycle_test
    application_lifecycle_test.cpp
    ../source/application_lifecycle.cpp)

target_compile_features(application_lifecycle_test PRIVATE cxx_std_17)
target_include_directories(application_lifecycle_test PRIVATE ../include)

add_test(NAME application_lifecycle_test COMMAND application_lifecycle_test)

add_executable(backup_request_queue_test
    backup_request_queue_test.cpp
    ../source/backup_request_queue.cpp
    ../source/atomic_file.cpp)

target_compile_features(backup_request_queue_test PRIVATE cxx_std_17)
target_include_directories(backup_request_queue_test PRIVATE ../include)

add_test(NAME backup_request_queue_test COMMAND backup_request_queue_test)

add_executable(backup_request_probe_test
    backup_request_probe_test.cpp
    ../source/backup_request_probe.cpp
    ../source/backup_request_queue.cpp
    ../source/atomic_file.cpp)

target_compile_features(backup_request_probe_test PRIVATE cxx_std_17)
target_include_directories(backup_request_probe_test PRIVATE ../include)

add_test(NAME backup_request_probe_test COMMAND backup_request_probe_test)

add_executable(ultrahand_notification_test
    ultrahand_notification_test.cpp
    ../source/ultrahand_notification.cpp)

target_compile_features(ultrahand_notification_test PRIVATE cxx_std_17)
target_include_directories(ultrahand_notification_test PRIVATE ../include)

add_test(NAME ultrahand_notification_test COMMAND ultrahand_notification_test)

add_executable(installer_filesystem_test installer_filesystem_test.cpp ../installer/source/filesystem.cpp)
target_compile_features(installer_filesystem_test PRIVATE cxx_std_17)
target_include_directories(installer_filesystem_test PRIVATE ../installer/include)
add_test(NAME installer_filesystem_test COMMAND installer_filesystem_test)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(CURL REQUIRED)
add_executable(nextcloud_login_test nextcloud_login_test.cpp ../source/nextcloud_login.cpp
    ../source/nextcloud_paths.cpp ../overlay/lib/libultrahand/common/cJSON.c)
target_compile_features(nextcloud_login_test PRIVATE cxx_std_17)
target_include_directories(nextcloud_login_test PRIVATE ../include ../overlay/lib/libultrahand/common)
target_link_libraries(nextcloud_login_test PRIVATE CURL::libcurl ${NXSYNC_CURL_EXTRA_LIBRARIES})
add_test(NAME nextcloud_login_test COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/nextcloud_login_test.py $<TARGET_FILE:nextcloud_login_test>)
add_executable(qr_code_test qr_code_test.cpp ../source/qr_code.cpp ../third_party/qrcodegen/qrcodegen.c)
target_compile_features(qr_code_test PRIVATE cxx_std_17)
target_include_directories(qr_code_test PRIVATE ../include ../third_party/qrcodegen)
add_test(NAME qr_code_test COMMAND qr_code_test)
add_test(NAME installer_romfs_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/installer_romfs_test.py)
add_test(NAME atmosphere_payload_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/atmosphere_payload_test.py)
add_test(NAME release_source_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/release_source_test.py)

find_path(MBEDTLS_INCLUDE_DIR mbedtls/gcm.h REQUIRED)
find_library(MBEDCRYPTO_LIBRARY mbedcrypto REQUIRED)
add_executable(credential_crypto_test credential_crypto_test.cpp ../source/credential_crypto.cpp)
target_compile_features(credential_crypto_test PRIVATE cxx_std_17)
target_include_directories(credential_crypto_test PRIVATE crypto_platform_stub ../include ${MBEDTLS_INCLUDE_DIR})
target_link_libraries(credential_crypto_test PRIVATE ${MBEDCRYPTO_LIBRARY})
add_test(NAME credential_crypto_test COMMAND credential_crypto_test)
if(UNIX)
    add_test(NAME gate_patch_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gate_patch_test.py ${CMAKE_CXX_COMPILER})
endif()

add_executable(automation_settings_test automation_settings_test.cpp ../source/storage_environment.cpp)
target_compile_features(automation_settings_test PRIVATE cxx_std_17)
target_include_directories(automation_settings_test PRIVATE ../include)
add_test(NAME automation_settings_test COMMAND automation_settings_test)
