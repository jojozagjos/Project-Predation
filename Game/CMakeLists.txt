set(PRED_GAME_SOURCES
    PredationGame.h
    PredationGame.cpp
    PredationGameShared.h
    PredationGameShared.cpp
    PredationGameCommands.cpp
    PredationGameNet.cpp
    PredationGameMenus.cpp
    PredationGameHud.cpp
    PredationGameVoice.cpp
    PredationGameCombat.cpp
    PredationGameEditor.cpp
    PredationGameAudio.cpp
    PredationGameCreatures.cpp
    PredationGameItems.cpp
    PredationGameSounds.cpp
    PredationGameDrone.cpp
    PredationGameMission.cpp
    PredationGameLifts.cpp
    PredationGameCinematics.cpp
    PredationGameShip.cpp
    PredationGameBriefing.cpp
    PredationGameLoadout.cpp
    PredationGameDevices.cpp
    PredationGameCampaign.cpp
    PredationGameMap.cpp
    PredationGameSpace.cpp
    PredationGameMapView.cpp
    PredationGameTitle.cpp

    Campaign/Universe.h
    Campaign/Universe.cpp
    Campaign/Campaign.h
    Campaign/Campaign.cpp
    Campaign/CampaignStore.h
    Campaign/CampaignStore.cpp
    Campaign/Travel.h
    Campaign/Travel.cpp
    Campaign/SystemMap.h
    Campaign/SystemMap.cpp
    Mission/Mission.h
    Mission/Mission.cpp
    Mission/MissionProps.h
    Mission/MissionProps.cpp
    Mission/Objectives.h
    Mission/Objectives.cpp
    Mission/SiteNames.h
    Mission/SiteNames.cpp
    Mission/Briefing.cpp

    Cinematic/Cinematic.h
    Cinematic/Cinematic.cpp
    Cinematic/CinematicPlayer.h
    Cinematic/CinematicPlayer.cpp
    Cinematic/CinematicEditor.h
    Cinematic/CinematicEditor.cpp

    Player/PlayerTypes.h
    Player/PlayerConfig.cpp
    Player/PlayerController.h
    Player/PlayerController.cpp
    Player/Ragdoll.h
    Player/Ragdoll.cpp
    Player/SupportDrone.h
    Player/SupportDrone.cpp
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
    Items/Loadout.cpp

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
    Creature/TacticLearner.h
    Creature/TacticLearner.cpp
    Creature/CreatureTuning.h
    Creature/CreatureTuning.cpp
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
    World/FacilityLayout.h
    World/FacilityLayout.cpp
    World/FacilityMap.h
    World/FacilityMap.cpp
    World/FacilityFurniture.h
    World/FacilityFurniture.cpp
    World/FacilityOutside.h
    World/FacilityOutside.cpp
    World/SitePlan.h
    World/SitePlan.cpp
    World/SiteTerrain.cpp
    World/SiteMap.h
    World/SiteMap.cpp
    World/SiteScatter.cpp
    World/Landmark.h
    World/Landmark.cpp
    World/Vents.h
    World/Vents.cpp
    World/Atmospherics.h
    World/Atmospherics.cpp
    World/Signs.h
    World/Signs.cpp
    World/Foliage.cpp
    World/Cargo.h
    World/Cargo.cpp
    World/Surfaces.h
    World/Surfaces.cpp
    World/Weather.cpp
    World/Footprints.h
    World/Footprints.cpp
    World/Particles.cpp
    World/Splashes.h
    World/Splashes.cpp
    World/ShipMap.h
    World/ShipMap.cpp
    World/KestrelStation.cpp
    World/Outpost.cpp
    World/Vehicles.h
    World/Vehicles.cpp
    World/Lifts.h
    World/Lifts.cpp
    World/Lettering.h
    World/Lettering.cpp
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
pred_precompile(PredationGameLib)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${PRED_GAME_SOURCES})
target_link_libraries(PredationGameLib PUBLIC Predation::Engine Predation::Tools)
set_target_properties(PredationGameLib PROPERTIES FOLDER "Game")

add_executable(ProjectPredation Main.cpp)
pred_set_warnings(ProjectPredation)
target_link_libraries(ProjectPredation PRIVATE Predation::GameLib)

set_target_properties(ProjectPredation PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    FOLDER "Game")

# A window of its own, no console behind it: what it says goes to its log (%APPDATA%\CIRRA\ProjectPredation\Logs) and the
# in-game console. Still started at main(); run from a terminal with its output redirected, that output is still written.
if(MSVC)
    target_link_options(ProjectPredation PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()
