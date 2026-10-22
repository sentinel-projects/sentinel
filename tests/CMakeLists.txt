if(TARGET sentinel_desktop_portal)
    qt_add_executable(test_portal_shortcuts desktop/test_portal_shortcuts.cpp)
    target_link_libraries(test_portal_shortcuts PRIVATE sentinel_desktop_portal Qt6::Test)
    add_test(NAME test_portal_shortcuts COMMAND $<TARGET_FILE:test_portal_shortcuts>)
    set_tests_properties(test_portal_shortcuts PROPERTIES TIMEOUT 60)
endif()

function(add_sentinel_core_test target source)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "Registered test source does not exist: ${source}")
    endif()

    qt_add_executable(${target}
        ${source}
    )

    target_link_libraries(${target}
        PRIVATE
            sentinel_core
            Qt6::Test
    )

    add_test(NAME ${target} COMMAND $<TARGET_FILE:${target}>)
endfunction()

# Measurement-only private composition; never linked into production entry points.
qt_add_executable(resource_benchmark_probe helpers/resource_benchmark_probe.cpp)
target_link_libraries(resource_benchmark_probe PRIVATE sentinel_daemon_service Qt6::Test)

# Opt-in live request probe; never registered with CTest or run by the build.
qt_add_executable(observation_request_probe EXCLUDE_FROM_ALL helpers/observation_request_probe.cpp)
target_link_libraries(observation_request_probe PRIVATE sentinel_core)
qt_add_executable(provider_certification_probe EXCLUDE_FROM_ALL helpers/provider_certification_probe.cpp)
target_link_libraries(provider_certification_probe PRIVATE sentinel_core)
qt_add_executable(audio_certification_probe EXCLUDE_FROM_ALL helpers/audio_certification_probe.cpp)
target_link_libraries(audio_certification_probe PRIVATE sentinel_core)
if(APPLE)
    set_target_properties(audio_certification_probe PROPERTIES MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/helpers/audio_certification_Info.plist.in")
endif()
qt_add_executable(settings_ui_certification_probe helpers/settings_ui_certification_probe.cpp
    ${PROJECT_SOURCE_DIR}/apps/sentinel-desktop/src/NativeCompanionAdapter.cpp
    ${PROJECT_SOURCE_DIR}/apps/sentinel-desktop/include/sentinel/desktop/NativeCompanionAdapter.h)
if(APPLE)
    target_sources(settings_ui_certification_probe PRIVATE
        ${PROJECT_SOURCE_DIR}/apps/sentinel-desktop/src/MacNativeActivation.mm
        ${PROJECT_SOURCE_DIR}/apps/sentinel-desktop/src/MacNativeNotifications.mm
        ${PROJECT_SOURCE_DIR}/apps/sentinel-desktop/include/sentinel/desktop/MacNativeNotifications.h)
    target_link_libraries(settings_ui_certification_probe PRIVATE "-framework Carbon" "-framework AppKit" "-framework UserNotifications")
    set_target_properties(settings_ui_certification_probe PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.sentinel.SettingsCertification"
        MACOSX_BUNDLE_BUNDLE_NAME "Sentinel Settings Certification")
endif()
if(WIN32)
    target_link_libraries(settings_ui_certification_probe PRIVATE user32)
endif()
target_link_libraries(settings_ui_certification_probe PRIVATE sentinel_desktop_viewmodels sentinel_daemon_service Qt6::Quick Qt6::Qml Qt6::Test)
add_test(NAME test_desktop_daemon_e2e COMMAND settings_ui_certification_probe --daemon-e2e)
set_tests_properties(test_desktop_daemon_e2e PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 75 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QSG_RENDER_LOOP=basic;QT_SCALE_FACTOR=1;QT_FONT_DPI=96")
add_test(NAME test_desktop_shell_launch COMMAND settings_ui_certification_probe --shell-smoke)
set_tests_properties(test_desktop_shell_launch PROPERTIES TIMEOUT 45 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME test_onboarding_focus COMMAND $<TARGET_FILE:settings_ui_certification_probe> --focus-smoke)
set_tests_properties(test_onboarding_focus PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME test_navigation_settings COMMAND settings_ui_certification_probe --navigation-smoke)
set_tests_properties(test_navigation_settings PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME test_ui_phase1 COMMAND settings_ui_certification_probe --phase1-smoke)
set_tests_properties(test_ui_phase1 PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 180)
add_test(NAME test_ui_phase1_failure_exit
    COMMAND ${CMAKE_COMMAND} -DPROBE=$<TARGET_FILE:settings_ui_certification_probe>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/helpers/verify_phase1_failure_exit.cmake)
set_tests_properties(test_ui_phase1_failure_exit PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 30)
add_test(NAME test_chat_surface COMMAND settings_ui_certification_probe --chat-smoke)
add_test(NAME test_models_surface COMMAND settings_ui_certification_probe --models-smoke)
add_test(NAME test_settings_surface COMMAND settings_ui_certification_probe --settings-smoke)
add_test(NAME test_tray_surface COMMAND settings_ui_certification_probe --tray-smoke)
# The tray acceptance test exercises real animation assets, including external
# build trees where executable-ancestor development discovery cannot find them.
set_tests_properties(test_tray_surface PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen;SENTINEL_CORE_WARDEN_ASSET_ROOT=${CMAKE_SOURCE_DIR}/assets/core-warden")
set_tests_properties(test_settings_surface PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(test_models_surface PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(test_chat_surface PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
file(GLOB_RECURSE CERTIFICATION_QML_FILES "${CMAKE_SOURCE_DIR}/ui/qml/*.qml" "${CMAKE_SOURCE_DIR}/ui/qml/*.js")
foreach(qml_file IN LISTS CERTIFICATION_QML_FILES)
    get_filename_component(qml_name "${qml_file}" NAME)
    set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
endforeach()
qt_add_resources(settings_ui_certification_probe "certification_qml"
    PREFIX "/qt/qml/Sentinel/Desktop" FILES ${CERTIFICATION_QML_FILES})
qt_add_resources(settings_ui_certification_probe "certification_qmldir"
    PREFIX "/qt/qml" BASE "${CMAKE_BINARY_DIR}/apps/sentinel-desktop"
    FILES "${CMAKE_BINARY_DIR}/apps/sentinel-desktop/Sentinel/Desktop/qmldir")
set_source_files_properties("${CMAKE_SOURCE_DIR}/resources/branding/tray/linux/sentinel-tray-32-black.png" PROPERTIES QT_RESOURCE_ALIAS "branding/tray.png")
set_source_files_properties("${CMAKE_SOURCE_DIR}/resources/branding/app-icon/linux/512x512/apps/sentinel.png" PROPERTIES QT_RESOURCE_ALIAS "icons/dev.sentinel.Sentinel.png")
set_source_files_properties("${CMAKE_SOURCE_DIR}/resources/branding/wordmark/sentinel-lockup.svg" PROPERTIES QT_RESOURCE_ALIAS "branding/sentinel-lockup.svg")
qt_add_resources(settings_ui_certification_probe "certification_brand"
    PREFIX "/" FILES
    "${CMAKE_SOURCE_DIR}/resources/branding/app-icon/linux/512x512/apps/sentinel.png"
    "${CMAKE_SOURCE_DIR}/resources/branding/tray/linux/sentinel-tray-32-black.png"
    "${CMAKE_SOURCE_DIR}/resources/branding/wordmark/sentinel-lockup.svg")
file(GLOB CERTIFICATION_ICON_FILES "${CMAKE_SOURCE_DIR}/resources/icons/tabler/*.svg"
    "${CMAKE_SOURCE_DIR}/resources/icons/lucide/*.svg")
qt_add_resources(settings_ui_certification_probe "certification_icons"
    PREFIX "/" BASE "${CMAKE_SOURCE_DIR}/resources" FILES ${CERTIFICATION_ICON_FILES})
set(CERTIFICATION_QM_FILES)
foreach(language en tr de es fr zh ja ar)
    set(qm_file "${CMAKE_BINARY_DIR}/apps/sentinel-desktop/sentinel_${language}.qm")
    set_source_files_properties("${qm_file}" PROPERTIES GENERATED TRUE QT_RESOURCE_ALIAS "sentinel_${language}.qm")
    list(APPEND CERTIFICATION_QM_FILES "${qm_file}")
endforeach()
qt_add_resources(settings_ui_certification_probe "certification_translations"
    PREFIX "/i18n" FILES ${CERTIFICATION_QM_FILES})
add_dependencies(settings_ui_certification_probe Sentinel_lrelease)
target_compile_definitions(settings_ui_certification_probe PRIVATE
    CERTIFICATION_DAEMON_EVIDENCE_ROOT="${CMAKE_BINARY_DIR}/desktop-daemon-verification"
    CERTIFICATION_QML_IMPORT="${CMAKE_BINARY_DIR}/apps/sentinel-desktop"
    CERTIFICATION_TRANSLATIONS="${CMAKE_BINARY_DIR}/apps/sentinel-desktop"
    CERTIFICATION_QML_WRAPPER="${CMAKE_CURRENT_SOURCE_DIR}/helpers/settings_ui_certification.qml")

add_sentinel_core_test(test_mode_manager core/test_mode_manager.cpp)
add_sentinel_core_test(test_unified_audio_lifecycle core/test_unified_audio_lifecycle.cpp)
add_sentinel_core_test(test_persistence_startup_certification core/test_persistence_startup_certification.cpp)
add_sentinel_core_test(test_chat_session core/test_chat_session.cpp)
add_sentinel_core_test(test_chat_mode_service core/test_chat_mode_service.cpp)
add_sentinel_core_test(test_chat_attachments core/test_chat_attachments.cpp)
add_sentinel_core_test(test_chat_application_controller core/test_chat_application_controller.cpp)
add_sentinel_core_test(test_conversation_session core/test_conversation_session.cpp)
add_sentinel_core_test(test_conversation_state_graph core/test_conversation_state_graph.cpp)
add_sentinel_core_test(test_context_assembly core/test_context_assembly.cpp)
add_sentinel_core_test(test_semantic_retrieval core/test_semantic_retrieval.cpp)
add_sentinel_core_test(test_local_rag_store core/test_local_rag_store.cpp)
add_sentinel_core_test(test_in_memory_conversation_store core/test_in_memory_conversation_store.cpp)
add_sentinel_core_test(test_in_memory_memory_candidate_store
                       core/test_in_memory_memory_candidate_store.cpp)
add_sentinel_core_test(test_in_memory_store core/test_in_memory_store.cpp)
add_sentinel_core_test(test_sqlite_chat_history_store core/test_sqlite_chat_history_store.cpp)
add_sentinel_core_test(test_sqlite_conversation_store core/test_sqlite_conversation_store.cpp)
add_sentinel_core_test(test_context_budget_manager core/test_context_budget_manager.cpp)
add_sentinel_core_test(test_memory_archive core/test_memory_archive.cpp)
add_sentinel_core_test(test_memory_retrieval core/test_memory_retrieval.cpp)
add_sentinel_core_test(test_memory_management core/test_memory_management.cpp)
add_sentinel_core_test(test_scoped_memory core/test_scoped_memory.cpp)
add_sentinel_core_test(test_sqlite_memory_store core/test_sqlite_memory_store.cpp)
add_sentinel_core_test(test_todo_service core/test_todo_service.cpp)
add_sentinel_core_test(test_in_memory_tool_registry core/test_in_memory_tool_registry.cpp)
add_sentinel_core_test(test_mcp_tool_provider core/test_mcp_tool_provider.cpp)
qt_add_executable(test_mcp_server helpers/test_mcp_server.cpp)
target_link_libraries(test_mcp_server PRIVATE Qt6::Core)
add_sentinel_core_test(test_mcp_integration core/test_mcp_integration.cpp)
add_dependencies(test_mcp_integration test_mcp_server)
target_compile_definitions(test_mcp_integration PRIVATE
    TEST_MCP_SERVER_PATH="$<TARGET_FILE:test_mcp_server>")
add_sentinel_core_test(test_plugin_tool_integration core/test_plugin_tool_integration.cpp)
qt_add_plugin(broker_certification_plugin SHARED CLASS_NAME BrokerCertificationPlugin
    helpers/broker_certification_plugin.cpp)
target_link_libraries(broker_certification_plugin PRIVATE sentinel_plugin_sdk Qt6::Core)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/broker-fixture")
configure_file(fixtures/plugins/broker/plugin.json "${CMAKE_CURRENT_BINARY_DIR}/broker-fixture/plugin.json" COPYONLY)
add_custom_command(TARGET broker_certification_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:broker_certification_plugin>"
        "${CMAKE_CURRENT_BINARY_DIR}/broker-fixture/$<TARGET_FILE_NAME:broker_certification_plugin>")
add_dependencies(test_plugin_tool_integration broker_certification_plugin)
target_compile_definitions(test_plugin_tool_integration PRIVATE
    TEST_BROKER_PLUGIN_DIR="${CMAKE_CURRENT_BINARY_DIR}/broker-fixture")
add_dependencies(test_plugin_tool_integration custom_agent_tool_plugin second_agent_tool_plugin)
add_dependencies(test_plugin_tool_integration stage_sample_agent_plugins)
target_compile_definitions(test_plugin_tool_integration PRIVATE
    TEST_PLUGIN_PATH="$<TARGET_FILE_DIR:custom_agent_tool_plugin>/custom-tool/$<TARGET_FILE_NAME:custom_agent_tool_plugin>")
add_sentinel_core_test(test_agent_activity_log core/test_agent_activity_log.cpp)
add_sentinel_core_test(test_agent_pipeline_result core/test_agent_pipeline_result.cpp)
add_sentinel_core_test(test_agent_runtime_context core/test_agent_runtime_context.cpp)
add_sentinel_core_test(test_agent_task_runtime core/test_agent_task_runtime.cpp)
add_sentinel_core_test(test_controlled_agent_tasks core/test_controlled_agent_tasks.cpp)
add_sentinel_core_test(test_orchestration_snapshot core/test_orchestration_snapshot.cpp)
add_sentinel_core_test(test_static_orchestration_diagnostics
                       core/test_static_orchestration_diagnostics.cpp)
add_sentinel_core_test(test_static_approval_policy core/test_static_approval_policy.cpp)
add_sentinel_core_test(test_static_sandbox_policy core/test_static_sandbox_policy.cpp)
add_sentinel_core_test(test_static_model_router core/test_static_model_router.cpp)
add_sentinel_core_test(test_static_provider_catalog core/test_static_provider_catalog.cpp)
add_sentinel_core_test(test_static_memory_catalog core/test_static_memory_catalog.cpp)
add_sentinel_core_test(test_static_agent_registry core/test_static_agent_registry.cpp)
add_sentinel_core_test(test_static_task_planner core/test_static_task_planner.cpp)
add_sentinel_core_test(test_static_runtime_capability_registry
                       core/test_static_runtime_capability_registry.cpp)
add_sentinel_core_test(test_static_runtime_permission_policy
                       core/test_static_runtime_permission_policy.cpp)
add_sentinel_core_test(test_static_runtime_safety_policy
                       core/test_static_runtime_safety_policy.cpp)
add_sentinel_core_test(test_static_runtime_pipeline core/test_static_runtime_pipeline.cpp)
add_sentinel_core_test(test_static_execution_lifecycle core/test_static_execution_lifecycle.cpp)
add_sentinel_core_test(test_runtime_integration core/test_runtime_integration.cpp)
add_sentinel_core_test(test_runtime_provider core/test_runtime_provider.cpp)
add_sentinel_core_test(test_provider_contracts core/test_provider_contracts.cpp)
add_sentinel_core_test(test_provider_registry core/test_provider_registry.cpp)
add_sentinel_core_test(test_endpoint_adapter_characterization core/test_endpoint_adapter_characterization.cpp)
add_sentinel_core_test(test_native_discovery_characterization core/test_native_discovery_characterization.cpp)
add_sentinel_core_test(test_native_provider_contract core/test_native_provider_contract.cpp)
add_sentinel_core_test(test_openai_compatible_protocol core/test_openai_compatible_protocol.cpp)
add_sentinel_core_test(test_anthropic_messages_protocol core/test_anthropic_messages_protocol.cpp)
add_sentinel_core_test(test_gemini_generate_content_protocol core/test_gemini_generate_content_protocol.cpp)
add_sentinel_core_test(test_bounded_streaming_contract core/test_bounded_streaming_contract.cpp)
add_sentinel_core_test(test_credential_store core/test_credential_store.cpp)
add_sentinel_core_test(test_provider_credentials core/test_provider_credentials.cpp)
add_sentinel_core_test(test_companion_service core/test_companion_service.cpp)
add_sentinel_core_test(test_permission_policy_service core/test_permission_policy_service.cpp)
add_sentinel_core_test(test_tool_execution_gateway core/test_tool_execution_gateway.cpp)
add_sentinel_core_test(test_tool_argument_validator core/test_tool_argument_validator.cpp)
add_sentinel_core_test(test_agent_runtime_service core/test_agent_runtime_service.cpp)
add_sentinel_core_test(test_workspace_service core/test_workspace_service.cpp)
add_sentinel_core_test(test_worktree_service core/test_worktree_service.cpp)
add_sentinel_core_test(test_skill_profile_service core/test_skill_profile_service.cpp)
add_sentinel_core_test(test_skill_extension_runtime core/test_skill_extension_runtime.cpp)
target_compile_definitions(test_skill_extension_runtime PRIVATE
    TEST_SKILL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/skills")
add_sentinel_core_test(test_model_registry core/test_model_registry.cpp)
add_sentinel_core_test(test_ollama_runtime core/test_ollama_runtime.cpp)
add_sentinel_core_test(test_local_inference core/test_local_inference.cpp)
add_sentinel_core_test(test_voice core/test_voice.cpp)
add_sentinel_core_test(test_null_tool_executor core/test_null_tool_executor.cpp)
add_sentinel_core_test(test_local_runtime core/test_local_runtime.cpp)
add_sentinel_core_test(test_local_runtime_session core/test_local_runtime_session.cpp)
add_sentinel_core_test(test_standard_path_provider core/test_standard_path_provider.cpp)
add_sentinel_core_test(test_storage_migration core/test_storage_migration.cpp)
add_sentinel_core_test(test_application_controller core/test_application_controller.cpp)
add_sentinel_core_test(test_app_settings core/test_app_settings.cpp)
add_sentinel_core_test(test_settings_storage_coordination core/test_settings_storage_coordination.cpp)
add_sentinel_core_test(test_storage_async_coordination core/test_storage_async_coordination.cpp)
set_tests_properties(test_storage_async_coordination PROPERTIES TIMEOUT 30)
add_sentinel_core_test(test_storage_admission core/test_storage_admission.cpp)
set_tests_properties(test_storage_admission PROPERTIES TIMEOUT 30)
add_sentinel_core_test(test_backup_recovery_journal core/test_backup_recovery_journal.cpp)
set_tests_properties(test_backup_recovery_journal PROPERTIES TIMEOUT 60)
add_sentinel_core_test(test_settings_recovery_runtime core/test_settings_recovery_runtime.cpp)
target_compile_definitions(test_settings_recovery_runtime PRIVATE
    CERTIFICATION_TRANSLATIONS="${CMAKE_BINARY_DIR}/apps/sentinel-desktop")
add_sentinel_core_test(test_json_settings_store core/test_json_settings_store.cpp)
add_sentinel_core_test(test_keyed_mutex core/test_keyed_mutex.cpp)
add_sentinel_core_test(test_scoped_transform core/test_scoped_transform.cpp)
add_sentinel_core_test(test_token_estimator core/test_token_estimator.cpp)
add_sentinel_core_test(test_fnv_hash core/test_fnv_hash.cpp)
add_sentinel_core_test(test_sampled_checksum core/test_sampled_checksum.cpp)
add_sentinel_core_test(test_lazy core/test_lazy.cpp)
add_sentinel_core_test(test_memo_map core/test_memo_map.cpp)
add_sentinel_core_test(test_flock_file_lock core/test_flock_file_lock.cpp)
add_sentinel_core_test(test_runtime_feature_flags core/test_runtime_feature_flags.cpp)
add_sentinel_core_test(test_json_schema_validator core/test_json_schema_validator.cpp)
add_sentinel_core_test(test_tool_name_validator core/test_tool_name_validator.cpp)
add_sentinel_core_test(test_plugin_hook_registry core/test_plugin_hook_registry.cpp)
add_sentinel_core_test(test_location_service_map core/test_location_service_map.cpp)
add_sentinel_core_test(test_session_execution_patterns core/test_session_execution_patterns.cpp)
add_sentinel_core_test(test_security_utilities core/test_security_utilities.cpp)
add_sentinel_core_test(test_config_resolver core/test_config_resolver.cpp)
add_sentinel_core_test(test_codemode_interpreter core/test_codemode_interpreter.cpp)
add_sentinel_core_test(test_background_job_persistence core/test_background_job_persistence.cpp)
add_sentinel_core_test(test_health_service core/test_health_service.cpp)
add_sentinel_core_test(test_upgrade core/test_upgrade.cpp)
add_sentinel_core_test(test_local_echo_provider core/test_local_echo_provider.cpp)
add_sentinel_core_test(test_null_agent_runtime core/test_null_agent_runtime.cpp)
add_sentinel_core_test(test_agent_loop core/test_agent_loop.cpp)
add_sentinel_core_test(test_agent_reliability_harness
                       core/test_agent_reliability_harness.cpp)
target_compile_definitions(test_agent_reliability_harness PRIVATE
    SENTINEL_ORACLE_COMPILER="${CMAKE_CXX_COMPILER}"
    SENTINEL_ORACLE_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
    SENTINEL_ORACLE_GIT="${GIT_EXECUTABLE}")
add_sentinel_core_test(test_agent_runtime core/test_agent_runtime.cpp)
add_sentinel_core_test(test_llm_agent_runtime core/test_llm_agent_runtime.cpp)
add_sentinel_core_test(test_observation_policy core/test_observation_policy.cpp)
add_sentinel_core_test(test_real_tool_executor_tools core/test_real_tool_executor_tools.cpp)
add_executable(docker_cli_fixture helpers/docker_cli_fixture.cpp)
target_compile_features(docker_cli_fixture PRIVATE cxx_std_20)
target_compile_definitions(test_real_tool_executor_tools PRIVATE
    DOCKER_CLI_FIXTURE="$<TARGET_FILE:docker_cli_fixture>")
add_dependencies(test_real_tool_executor_tools docker_cli_fixture)
add_sentinel_core_test(test_process_executor core/test_process_executor.cpp)
qt_add_executable(canonical_path_probe helpers/canonical_path_probe.cpp)
target_link_libraries(canonical_path_probe PRIVATE Qt6::Core)
target_compile_definitions(test_process_executor PRIVATE
    CANONICAL_PATH_PROBE="$<TARGET_FILE:canonical_path_probe>"
    PLUGIN_RUNTIME_HOST="$<TARGET_FILE:sentinel-plugin-host>")
add_dependencies(test_process_executor canonical_path_probe sentinel-plugin-host)
add_sentinel_core_test(test_fuzzy_editor core/test_fuzzy_editor.cpp)
add_sentinel_core_test(test_web_search_tool core/test_web_search_tool.cpp)
add_sentinel_core_test(test_plugin_manager core/test_plugin_manager.cpp)
add_sentinel_core_test(test_default_platform_services test_default_platform_services.cpp)
add_sentinel_core_test(test_desktop_shell_view_model desktop/test_desktop_shell_view_model.cpp)
add_sentinel_core_test(test_chat_view_model desktop/test_chat_view_model.cpp)
add_sentinel_core_test(test_chat_message_recovery desktop/test_chat_message_recovery.cpp)
target_link_libraries(test_chat_message_recovery PRIVATE sentinel_desktop_viewmodels)

qt_add_executable(test_daemon_ipc
    core/test_daemon_ipc.cpp
)

target_link_libraries(test_daemon_ipc
    PRIVATE
        sentinel_daemon_service
        sentinel_core
        Qt6::Test
)

add_test(NAME test_daemon_ipc COMMAND $<TARGET_FILE:test_daemon_ipc>)

qt_add_executable(test_graphics_backend
    desktop/test_graphics_backend.cpp
)

target_link_libraries(test_graphics_backend
    PRIVATE
        sentinel_desktop_startup
        Qt6::Test
)

add_test(NAME test_graphics_backend COMMAND $<TARGET_FILE:test_graphics_backend>)

qt_add_executable(test_desktop_shell_state
    desktop/test_desktop_shell_state.cpp
)

target_link_libraries(test_desktop_shell_state
    PRIVATE
        sentinel_desktop_viewmodels
        sentinel_desktop_startup
        Qt6::Test
)

add_test(NAME test_desktop_shell_state COMMAND $<TARGET_FILE:test_desktop_shell_state>)
set_tests_properties(test_desktop_shell_state PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 60)

target_link_libraries(test_desktop_shell_view_model
    PRIVATE
        sentinel_desktop_viewmodels
)

target_link_libraries(test_chat_view_model
    PRIVATE
        sentinel_desktop_viewmodels
)

# ── Integration, Performance, Benchmark & GUI Test Targets ───────────────────
add_sentinel_core_test(test_rag_e2e_pipeline integration/test_rag_e2e_pipeline.cpp)
add_sentinel_core_test(test_legacy_cli_exit_codes integration/test_legacy_cli_exit_codes.cpp)
target_compile_definitions(test_legacy_cli_exit_codes PRIVATE
    SENTINEL_CLI_BINARY="$<TARGET_FILE:sentinel-cli>")
target_include_directories(test_legacy_cli_exit_codes PRIVATE
    ${PROJECT_SOURCE_DIR}/apps/sentinel-cli)
add_dependencies(test_legacy_cli_exit_codes sentinel-cli)
set_tests_properties(test_legacy_cli_exit_codes PROPERTIES TIMEOUT 300)
add_sentinel_core_test(test_startup_time performance/test_startup_time.cpp)
add_sentinel_core_test(bench_semantic_search benchmarks/bench_semantic_search.cpp)
add_sentinel_core_test(bench_context_compression benchmarks/bench_context_compression.cpp)
add_sentinel_core_test(bench_sqlite_store benchmarks/bench_sqlite_store.cpp)
add_sentinel_core_test(bench_resource_telemetry benchmarks/bench_resource_telemetry.cpp)

find_package(Qt6 COMPONENTS QuickTest QUIET)
if(Qt6QuickTest_FOUND AND TARGET Qt6::QuickTest)
    qt_add_executable(test_gui_components
        gui/tst_main.cpp
    )
    target_link_libraries(test_gui_components
        PRIVATE
            Qt6::QuickTest
            Qt6::Quick
            Qt6::Test
    )
    # SoundManager resolves qrc:/sounds/*.wav; embed the same cues as the desktop app.
    set_source_files_properties(${PROJECT_SOURCE_DIR}/resources/sounds/notification.wav
        PROPERTIES QT_RESOURCE_ALIAS sounds/notification.wav)
    set_source_files_properties(${PROJECT_SOURCE_DIR}/resources/sounds/error.wav
        PROPERTIES QT_RESOURCE_ALIAS sounds/error.wav)
    set_source_files_properties(${PROJECT_SOURCE_DIR}/resources/sounds/success.wav
        PROPERTIES QT_RESOURCE_ALIAS sounds/success.wav)
    qt_add_resources(test_gui_components sentinel_gui_sounds
        PREFIX /
        FILES
            ${PROJECT_SOURCE_DIR}/resources/sounds/notification.wav
            ${PROJECT_SOURCE_DIR}/resources/sounds/error.wav
            ${PROJECT_SOURCE_DIR}/resources/sounds/success.wav
    )
    # Reuse the production module/resources, not copied widget implementations.
    qt_add_resources(test_gui_components gui_desktop_qml
        PREFIX "/qt/qml/Sentinel/Desktop" FILES ${CERTIFICATION_QML_FILES})
    qt_add_resources(test_gui_components gui_desktop_qmldir
        PREFIX "/qt/qml" BASE "${CMAKE_BINARY_DIR}/apps/sentinel-desktop"
        FILES "${CMAKE_BINARY_DIR}/apps/sentinel-desktop/Sentinel/Desktop/qmldir")
    qt_add_resources(test_gui_components gui_desktop_icons
        PREFIX "/" BASE "${CMAKE_SOURCE_DIR}/resources" FILES ${CERTIFICATION_ICON_FILES})
    target_compile_definitions(test_gui_components PRIVATE
        DESKTOP_QML_IMPORT="${CMAKE_BINARY_DIR}/apps/sentinel-desktop"
        UI_EVIDENCE_ROOT="${CMAKE_BINARY_DIR}/ui-verification")
    add_test(NAME test_gui_components COMMAND $<TARGET_FILE:test_gui_components> -input "${CMAKE_CURRENT_SOURCE_DIR}/gui")
    set_tests_properties(test_gui_components PROPERTIES TIMEOUT 90 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QSG_RENDER_LOOP=basic;QT_SCALE_FACTOR=1;QT_FONT_DPI=96")
endif()

add_test(NAME test_package_signing_failure
    COMMAND ${CMAKE_COMMAND}
        -DHELPER=${PROJECT_SOURCE_DIR}/cmake/SentinelSignPackages.cmake
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_package_signing.cmake)
set_tests_properties(test_package_signing_failure PROPERTIES TIMEOUT 30)

# Canonical IPC models are checked in so normal C++ builds need no generator runtime.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_development_helpers
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/development/test_build_helpers.py)
    set_tests_properties(test_development_helpers PROPERTIES TIMEOUT 60)
    add_test(NAME test_resource_benchmark_runner COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/performance/test_benchmark.py)
    set_tests_properties(test_resource_benchmark_runner PROPERTIES TIMEOUT 30)
    add_test(NAME test_verification_desktop_daemon COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verification/test_desktop_daemon.py)
    set_tests_properties(test_verification_desktop_daemon PROPERTIES TIMEOUT 30)
    add_test(NAME test_verification_registry
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/verification/test_registry.py)
    add_test(NAME test_verification_desktop_ui
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/verification/test_desktop_ui.py)
    set_tests_properties(test_verification_desktop_ui PROPERTIES TIMEOUT 30)
    add_test(NAME test_verification_e2e_collector
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/verification/test_e2e.py)
    set_tests_properties(test_verification_e2e_collector PROPERTIES TIMEOUT 30)
    add_test(NAME test_verification_inventory
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/verification/validate.py --format json)
    add_test(NAME test_packaging_contract
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/packaging/test_packaging.py)
    set_tests_properties(test_packaging_contract PROPERTIES TIMEOUT 120)
    add_test(NAME test_branding_sources
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/branding/check.py)
    add_test(NAME test_desktop_generated_contract
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/ipc/generate_desktop.py --check)
    add_test(NAME test_ipc_generated_contract
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/ipc/generate.py --check)
endif()

qt_add_executable(test_desktop_ipc desktop/test_desktop_ipc.cpp)
target_compile_definitions(test_desktop_ipc PRIVATE TEST_MCP_SERVER_PATH="$<TARGET_FILE:test_mcp_server>")
qt_add_resources(test_desktop_ipc model_catalog_test PREFIX "/catalog-test"
    BASE "${CMAKE_SOURCE_DIR}/ui/qml/pages/models"
    FILES "${CMAKE_SOURCE_DIR}/ui/qml/pages/models/ModelCatalog.js")
target_link_libraries(test_desktop_ipc PRIVATE sentinel_desktop_viewmodels sentinel_desktop_ipc sentinel_daemon_service sentinel_core Qt6::Test Qt6::Qml)
add_test(NAME test_desktop_ipc COMMAND $<TARGET_FILE:test_desktop_ipc>)

qt_add_executable(test_provider_agent_e2e desktop/test_provider_agent_e2e.cpp)
target_link_libraries(test_provider_agent_e2e PRIVATE sentinel_desktop_ipc sentinel_daemon_service sentinel_core Qt6::Test)
add_test(NAME test_provider_agent_e2e COMMAND $<TARGET_FILE:test_provider_agent_e2e>)
set_tests_properties(test_provider_agent_e2e PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)

qt_add_executable(test_agent_reliability_ipc desktop/test_agent_reliability_ipc.cpp)
target_link_libraries(test_agent_reliability_ipc PRIVATE
    sentinel_desktop_viewmodels sentinel_desktop_ipc sentinel_daemon_service sentinel_core Qt6::Test)
add_test(NAME test_agent_reliability_ipc COMMAND $<TARGET_FILE:test_agent_reliability_ipc>)
set_tests_properties(test_agent_reliability_ipc PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
target_compile_definitions(test_agent_reliability_harness PRIVATE
    SENTINEL_RELIABILITY_IPC_FIXTURE="$<TARGET_FILE:test_agent_reliability_ipc>")
add_dependencies(test_agent_reliability_harness test_agent_reliability_ipc)


qt_add_executable(test_native_integration
    desktop/test_native_integration.cpp
    ${PROJECT_SOURCE_DIR}/apps/sentinel-desktop/src/NativeCompanionAdapter.cpp
    ${PROJECT_SOURCE_DIR}/apps/sentinel-desktop/include/sentinel/desktop/NativeCompanionAdapter.h
)
target_link_libraries(test_native_integration PRIVATE sentinel_desktop_viewmodels sentinel_core Qt6::Test Qt6::Widgets Qt6::Quick)
if(APPLE)
    target_sources(test_native_integration PRIVATE ${PROJECT_SOURCE_DIR}/apps/sentinel-desktop/src/MacNativeActivation.mm ${PROJECT_SOURCE_DIR}/apps/sentinel-desktop/src/MacNativeNotifications.mm ${PROJECT_SOURCE_DIR}/apps/sentinel-desktop/include/sentinel/desktop/MacNativeNotifications.h)
    target_link_libraries(test_native_integration PRIVATE "-framework Carbon" "-framework AppKit" "-framework UserNotifications")
elseif(WIN32)
    target_link_libraries(test_native_integration PRIVATE user32)
endif()
add_test(NAME test_native_integration COMMAND $<TARGET_FILE:test_native_integration>)
set_tests_properties(test_native_integration PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_sentinel_core_test(test_core_warden desktop/test_core_warden.cpp)
target_link_libraries(test_core_warden PRIVATE sentinel_desktop_viewmodels Qt6::Quick)
target_compile_definitions(test_core_warden PRIVATE WARDEN_TEST_ROOT="${PROJECT_SOURCE_DIR}/assets/core-warden")
set_tests_properties(test_core_warden PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_sentinel_core_test(test_extensions_hub desktop/test_extensions_hub.cpp)
target_link_libraries(test_extensions_hub PRIVATE sentinel_desktop_viewmodels)

add_test(NAME test_extensions_surface COMMAND $<TARGET_FILE:settings_ui_certification_probe> --extensions-smoke)
set_tests_properties(test_extensions_surface PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# P6-03: base control and overlay dialog accessibility contracts (offscreen).
add_test(NAME test_desktop_accessibility COMMAND $<TARGET_FILE:settings_ui_certification_probe> --accessibility-smoke)
set_tests_properties(test_desktop_accessibility PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

# Local explicit-root contract; no live provider or stored credentials are used.
add_executable(test_provider_certification_root
    core/test_provider_certification_root.cpp
    ${PROJECT_SOURCE_DIR}/core/src/security/ExternalDirectoryGate.cpp)
target_include_directories(test_provider_certification_root PRIVATE ${PROJECT_SOURCE_DIR}/core/include)
target_link_libraries(test_provider_certification_root PRIVATE Qt6::Core)
add_test(NAME test_provider_certification_root COMMAND test_provider_certification_root)

add_sentinel_core_test(test_model_lifecycle_ownership core/test_model_lifecycle_ownership.cpp)
add_sentinel_core_test(test_resource_lease_reference_contract core/test_resource_lease_reference_contract.cpp)

# P3-02: real helper reconciliation with synthetic IO; no model/process starts.
qt_add_executable(test_local_model_runtime desktop/test_local_model_runtime.cpp)
target_link_libraries(test_local_model_runtime PRIVATE sentinel_daemon_service Qt6::Test)
add_test(NAME test_local_model_runtime COMMAND $<TARGET_FILE:test_local_model_runtime>)
set_tests_properties(test_local_model_runtime PROPERTIES TIMEOUT 30)

add_sentinel_core_test(test_vendor_cloud_contract core/test_vendor_cloud_contract.cpp)

add_sentinel_core_test(test_endpoint_security_capability core/test_endpoint_security_capability.cpp)

# P4-01: existing telemetry seam and existing execution-budget contracts only.
add_sentinel_core_test(test_resource_telemetry_contract core/test_resource_telemetry_contract.cpp)
add_sentinel_core_test(test_execution_budget_policy core/test_execution_budget_policy.cpp)

# P4-02: native resource telemetry value contracts and sampling lifecycle.
add_sentinel_core_test(test_native_resource_telemetry core/test_native_resource_telemetry.cpp)
add_sentinel_core_test(test_resource_monitor_sampling core/test_resource_monitor_sampling.cpp)

# P4-03: adaptive resource policy foundation and deterministic contracts.
add_sentinel_core_test(test_resource_policy core/test_resource_policy.cpp)

# P4-03.1: independent adversarial fixtures for the same policy contract.
add_sentinel_core_test(test_resource_policy_review core/test_resource_policy_review.cpp)

# P4-04: read-only telemetry → policy-input adapter translation.
add_sentinel_core_test(test_resource_policy_adapter core/test_resource_policy_adapter.cpp)

# P4-05: non-production calibration fixtures for proposed profiles.
add_sentinel_core_test(test_resource_policy_calibration core/test_resource_policy_calibration.cpp)

# P4-05: disabled-by-default observational shadow evaluation.
add_sentinel_core_test(test_resource_policy_shadow core/test_resource_policy_shadow.cpp)
