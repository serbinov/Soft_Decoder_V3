# web_html.h is generated from web_ui.html (and version.txt) before web.c is
# compiled. This used to be a PlatformIO pre-build script; generating it from
# CMake lets the project build with plain ESP-IDF (idf.py) as well, and keeps a
# single source of truth for both build systems.

set(WEB_DIR        "${CMAKE_CURRENT_LIST_DIR}/../..")
set(WEB_UI_HTML    "${WEB_DIR}/web_ui.html")
set(WEB_VERSION    "${WEB_DIR}/version.txt")
set(WEB_GEN_SCRIPT "${WEB_DIR}/tools/gen_web_html.py")
set(WEB_HTML_HDR   "${CMAKE_CURRENT_SOURCE_DIR}/include/web_html.h")
set(EDITOR_DIST    "${WEB_DIR}/sound_editor/dist")
set(EDITOR_GEN     "${WEB_DIR}/tools/gen_sound_editor.py")
set(EDITOR_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/sound_editor_assets.c")

# CMAKE_BUILD_EARLY_EXPANSION is set while ESP-IDF only scans components for
# their requirements; running the generator during that pass is pointless and
# add_custom_command is unavailable there.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(idf_python PYTHON)
    if(NOT idf_python)
        set(idf_python "python")
    endif()

    # Generate now so the header exists before the compiler's dependency scan.
    execute_process(
        COMMAND "${idf_python}" "${WEB_GEN_SCRIPT}"
        WORKING_DIRECTORY "${WEB_DIR}"
        RESULT_VARIABLE web_gen_rc)
    if(NOT web_gen_rc EQUAL 0)
        message(FATAL_ERROR "tools/gen_web_html.py failed (exit ${web_gen_rc})")
    endif()

    # ... and regenerate on build when web_ui.html/version.txt change.
    add_custom_command(
        OUTPUT "${WEB_HTML_HDR}"
        COMMAND "${idf_python}" "${WEB_GEN_SCRIPT}"
        DEPENDS "${WEB_UI_HTML}" "${WEB_VERSION}" "${WEB_GEN_SCRIPT}"
        COMMENT "Generating web_html.h from web_ui.html"
        VERBATIM)
    set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/src/web.c"
        PROPERTIES OBJECT_DEPENDS "${WEB_HTML_HDR}")

    # The block editor is prebuilt: never download dependencies during an
    # ordinary firmware build. The production bundle and runtime ship together.
    execute_process(COMMAND "${idf_python}" "${EDITOR_GEN}"
        --dist "${EDITOR_DIST}" --output "${CMAKE_CURRENT_BINARY_DIR}"
        RESULT_VARIABLE editor_gen_rc)
    if(NOT editor_gen_rc EQUAL 0)
        message(FATAL_ERROR "Sound editor assets missing/invalid. Run the editor build in firmware/sound_editor.")
    endif()
    file(GLOB_RECURSE EDITOR_ASSETS CONFIGURE_DEPENDS "${EDITOR_DIST}/*")
    file(GLOB_RECURSE EDITOR_INPUTS CONFIGURE_DEPENDS
        "${WEB_DIR}/sound_editor/public/*" "${WEB_DIR}/sound_editor/scripts/*")
    add_custom_command(OUTPUT "${EDITOR_SOURCE}"
        COMMAND "${idf_python}" "${EDITOR_GEN}" --dist "${EDITOR_DIST}"
            --output "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS "${EDITOR_GEN}" ${EDITOR_ASSETS} ${EDITOR_INPUTS}
            "${WEB_DIR}/sound_editor/package.json"
        COMMENT "Embedding offline sound editor gzip assets"
        VERBATIM)
endif()

idf_component_register(SRCS "src/web.c" "src/web_util.c" "src/audio_pack.c" "${EDITOR_SOURCE}"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server esp_wifi esp_netif esp_event esp_timer app_update
                                 nvs_flash esp_driver_gpio audio auxio motor settings storage pinmap dcc sound)
