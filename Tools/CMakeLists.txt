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
