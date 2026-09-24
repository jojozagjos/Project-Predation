set(PRED_GAME_SOURCES
    PredationGame.h
    PredationGame.cpp
    PredationGameCreatures.cpp
    PredationGameItems.cpp
    PredationGameSounds.cpp

    Player/PlayerTypes.h
    Player/PlayerConfig.cpp
    Player/PlayerController.h
    Player/PlayerController.cpp
    Player/Ragdoll.h
    Player/Ragdoll.cpp
    Player/PlayerBody.h
    Player/PlayerBody.cpp

    Interaction/Interactable.h
    Interaction/InteractionSystem.h
    Interaction/InteractionSystem.cpp

    Items/ItemDatabase.h
    Items/ItemDatabase.cpp
    Items/ItemAppearance.h
    Items/ItemAppearance.cpp
    Items/ItemIcons.h
    Items/ItemIcons.cpp
    Items/Inventory.h
    Items/Inventory.cpp

    Weapons/WeaponTypes.h
    Weapons/WeaponAppearance.h
    Weapons/WeaponAppearance.cpp
    Weapons/WeaponDatabase.h
    Weapons/WeaponDatabase.cpp
    Weapons/WeaponSystem.h
    Weapons/WeaponSystem.cpp
    Weapons/ShotResolver.h
    Weapons/ShotResolver.cpp
    Creature/Noise.h
    Creature/CreatureTraits.h
    Creature/CreatureTraits.cpp
    Creature/CreatureBrain.h
    Creature/CreatureBrain.cpp
    Creature/CreatureTactics.cpp
    Creature/VoiceMemory.h
    Creature/VoiceMemory.cpp
    Creature/CreatureAnatomy.h
    Creature/CreatureAnatomy.cpp
    Creature/CreatureSkin.h
    Creature/CreatureSkin.cpp
    Creature/CreatureRig.h
    Creature/CreatureRig.cpp
    Creature/CreatureRagdoll.h
    Creature/CreatureRagdoll.cpp
    Creature/Creature.h
    Creature/Creature.cpp
    Weapons/BulletHole.h
    Weapons/BulletHole.cpp

    Net/Protocol.h
    Net/Protocol.cpp
    Net/Prediction.h
    Net/Prediction.cpp
    Net/NetSession.h
    Net/NetSession.cpp

    World/TestMap.h
    World/TestMap.cpp
    World/MapBuilder.h
    World/LabMap.h
    World/LabMap.cpp
    World/HiveMesh.h
    World/LevelLights.h
    World/LevelLights.cpp
    World/HiveMesh.cpp
    World/WorldObjects.h
    World/WorldObjects.cpp
)

# Gameplay lives in a library rather than directly in the executable so the tests can link it.
# Movement, and later the creature AI, are exactly the systems most worth testing headlessly.
add_library(PredationGameLib STATIC ${PRED_GAME_SOURCES})
add_library(Predation::GameLib ALIAS PredationGameLib)
pred_set_warnings(PredationGameLib)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${PRED_GAME_SOURCES})
target_link_libraries(PredationGameLib PUBLIC Predation::Engine Predation::Tools)
set_target_properties(PredationGameLib PROPERTIES FOLDER "Game")

add_executable(ProjectPredation Main.cpp)
pred_set_warnings(ProjectPredation)
target_link_libraries(ProjectPredation PRIVATE Predation::GameLib)

set_target_properties(ProjectPredation PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    FOLDER "Game")

# Console subsystem during development so logs are visible in a terminal.
if(MSVC)
    target_link_options(ProjectPredation PRIVATE /SUBSYSTEM:CONSOLE)
endif()
