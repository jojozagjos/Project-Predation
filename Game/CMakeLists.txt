set(PRED_GAME_SOURCES
    Main.cpp
    PredationGame.h
    PredationGame.cpp
)

add_executable(ProjectPredation ${PRED_GAME_SOURCES})
pred_set_warnings(ProjectPredation)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${PRED_GAME_SOURCES})

target_link_libraries(ProjectPredation PRIVATE Predation::Engine)

set_target_properties(ProjectPredation PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    FOLDER "Game")

# Console subsystem during development so logs are visible in a terminal.
if(MSVC)
    target_link_options(ProjectPredation PRIVATE /SUBSYSTEM:CONSOLE)
endif()
