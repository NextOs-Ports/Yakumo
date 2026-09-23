# Monster Hunter Portable 3rd HD Ver. on PortableKit. The framework builds the
# executable, the corpus in generated/ and the overlay libraries in overlays/;
# this names the game's own sources and adds what only this game needs.

# The analog camera wraps the game's camera rotation helper, and the profile
# has to hand it the generated unit that helper was compiled into. Which unit
# that is depends on how the generator partitions the code, so read it from the
# corpus instead of naming it: a new partition then finds its own unit, and a
# corpus without the helper stops the build here rather than letting the
# camera call the wrong code in game.
set(MHP3RD_CAMERA_HELPER_UNIT "")
file(GLOB MHP3RD_GENERATED CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp")
if(MHP3RD_GENERATED)
    foreach(unit IN LISTS MHP3RD_GENERATED)
        file(STRINGS "${unit}" hit REGEX "register_function\\(0x08878B70u, &recomp_unit_[0-9]+," LIMIT_COUNT 1)
        if(hit)
            string(REGEX MATCH "recomp_unit_[0-9]+" MHP3RD_CAMERA_HELPER_UNIT "${hit}")
            break()
        endif()
    endforeach()
    if(NOT MHP3RD_CAMERA_HELPER_UNIT)
        message(FATAL_ERROR "mhp3rd: no generated unit registers the camera rotation helper 0x08878B70; "
                            "regenerate the corpus from the supported executable, or update "
                            "host/camera/game_camera.cpp if the helper has moved")
    endif()
    message(STATUS "mhp3rd: camera rotation helper in ${MHP3RD_CAMERA_HELPER_UNIT}")
    # A regenerated corpus with the same file names must be read again.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MHP3RD_GENERATED})
endif()

portablekit_add_game(Yakumo
    PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    SOURCES
        host/mhp3rd_profile.cpp
        host/camera/game_aspect.cpp
        host/camera/game_camera.cpp)

# Only the profile needs the camera helper's unit; keeping it off the target
# keeps it off every other unit's command line.
if(MHP3RD_CAMERA_HELPER_UNIT)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/host/mhp3rd_profile.cpp" APPEND PROPERTY
        COMPILE_DEFINITIONS MHP3RD_CAMERA_HELPER_UNIT=${MHP3RD_CAMERA_HELPER_UNIT})
endif()

if(WIN32)
    set(MHP3RD_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/emblem.ico")
    configure_file(packaging/windows/Yakumo.rc.in Yakumo.rc @ONLY)
    target_sources(Yakumo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Yakumo.rc")
endif()

# The game's camera driver and its view shape, against stand-ins for the
# game's code: no game data.
if(PSPRECOMP_BUILD_TESTS)
    add_executable(mhp3rd_camera_tests
        tests/analog_camera_tests.cpp
        host/camera/game_camera.cpp
        "${PORTABLEKIT_ROOT}/host/camera/camera_input.cpp"
        "${PORTABLEKIT_ROOT}/host/input/bindings.cpp")
    target_compile_features(mhp3rd_camera_tests PRIVATE cxx_std_20)
    target_include_directories(mhp3rd_camera_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host" "${PORTABLEKIT_ROOT}/host")
    target_link_libraries(mhp3rd_camera_tests PRIVATE psprecomp_core)
    set_target_properties(mhp3rd_camera_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME mhp3rd_camera_tests COMMAND mhp3rd_camera_tests)

    add_executable(mhp3rd_aspect_tests
        tests/game_aspect_tests.cpp
        host/camera/game_aspect.cpp)
    target_compile_features(mhp3rd_aspect_tests PRIVATE cxx_std_20)
    target_include_directories(mhp3rd_aspect_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/host" "${PORTABLEKIT_ROOT}/host")
    target_link_libraries(mhp3rd_aspect_tests PRIVATE psprecomp_core)
    set_target_properties(mhp3rd_aspect_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME mhp3rd_aspect_tests COMMAND mhp3rd_aspect_tests)
endif()
