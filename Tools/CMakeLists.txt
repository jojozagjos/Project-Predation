# The model and animation editor. It lives in Tools but links into the game executable rather than
# standing on its own: an editor that renders through a different path from the game is an editor
# whose output does not match what ships.
set(PRED_TOOLS_SOURCES
    ModelEditor/ModelEditor.h
    ModelEditor/ModelEditor.cpp
)

add_library(PredationTools STATIC ${PRED_TOOLS_SOURCES})
add_library(Predation::Tools ALIAS PredationTools)
pred_set_warnings(PredationTools)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${PRED_TOOLS_SOURCES})
target_link_libraries(PredationTools PUBLIC Predation::Engine)
set_target_properties(PredationTools PROPERTIES FOLDER "Tools")

# The lobby server. Not built with the game: it runs on a server somewhere rather than on a
# player's machine, and it is usually built there, on Linux, straight from the four source files
# (see Tools/LobbyServer/setup-linux.sh). This is here for testing it on this PC:
#   cmake --preset windows-release -DPRED_BUILD_LOBBY_SERVER=ON
option(PRED_BUILD_LOBBY_SERVER "Build the lobby server executable" OFF)
if(PRED_BUILD_LOBBY_SERVER)
    add_executable(PredationLobbyServer LobbyServer/main.cpp)
    pred_set_warnings(PredationLobbyServer)
    target_link_libraries(PredationLobbyServer PRIVATE Predation::Engine)
    set_target_properties(PredationLobbyServer PROPERTIES FOLDER "Tools")
endif()
