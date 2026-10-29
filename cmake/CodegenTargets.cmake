if(SKATE3_REQUIRE_TITLE_UPDATE)
    set(_skate3_codegen_game_root "${SKATE3_GAME_DATA_ROOT}")
    if(TARGET skate3-title-update-codegen-inputs)
        set(_skate3_codegen_game_root "${SKATE3_TITLE_UPDATE_CODEGEN_ROOT}")
    endif()
    add_custom_target(skate3-check-title-update-inputs
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckTitleUpdateInputs.py"
            --game-root "${_skate3_codegen_game_root}"
        COMMENT "Checking game images match the TU3 recompilation"
        VERBATIM)
    if(TARGET skate3-title-update-codegen-inputs)
        add_dependencies(skate3-check-title-update-inputs skate3-title-update-codegen-inputs)
    endif()
endif()

add_custom_target(generate-skate3
    COMMAND $<TARGET_FILE:rex::rexglue> codegen
            ${SKATE3_CODEGEN_ARGS}
            "${CMAKE_CURRENT_BINARY_DIR}/manifests/skate3.toml"
    COMMAND "${CMAKE_COMMAND}"
            "-DSKATE3_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplySkate3CodegenPatches.cmake"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating recompiled code for default.xex"
    VERBATIM
)

add_custom_target(generate-eawebkit
    COMMAND $<TARGET_FILE:rex::rexglue> codegen
            ${SKATE3_CODEGEN_ARGS}
            "${CMAKE_CURRENT_BINARY_DIR}/manifests/eawebkit.toml"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating recompiled code for EAWebkit.xex"
    VERBATIM
)

if(TARGET skate3-title-update-codegen-inputs)
    add_dependencies(generate-skate3 skate3-title-update-codegen-inputs)
    add_dependencies(generate-eawebkit skate3-title-update-codegen-inputs)
endif()
if(TARGET skate3-check-title-update-inputs)
    add_dependencies(generate-skate3 skate3-check-title-update-inputs)
    add_dependencies(generate-eawebkit skate3-check-title-update-inputs)
endif()

add_dependencies(generate-skate3 generate-eawebkit)

add_custom_target(generate-all
    DEPENDS generate-skate3 generate-eawebkit
)

add_custom_target(skate3_codegen DEPENDS generate-skate3)
add_custom_target(eawebkit_codegen DEPENDS generate-eawebkit)
add_custom_target(all_codegen DEPENDS generate-all)
