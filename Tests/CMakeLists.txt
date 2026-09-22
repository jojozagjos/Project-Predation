find_package(Catch2 3 CONFIG REQUIRED)

add_executable(PredationTests
    CoreTests.cpp
    PlayerTests.cpp
    AnimationTests.cpp
    BodyPoseTests.cpp
    ShadowTests.cpp
    InteractionTests.cpp
    WeaponTests.cpp
    AudioTests.cpp
    NetTests.cpp
    NetSessionTests.cpp
    NetUdpTests.cpp
    RelayTests.cpp
    LanDiscoveryTests.cpp
    BulletHoleTests.cpp
    PortMapperTests.cpp
    NavigationTests.cpp
    CreatureTests.cpp
    AnatomyTests.cpp
    PhysicsTests.cpp
    AssetTests.cpp
    EditorTests.cpp
)
pred_set_warnings(PredationTests)
target_link_libraries(PredationTests PRIVATE Predation::GameLib Catch2::Catch2WithMain)
set_target_properties(PredationTests PROPERTIES FOLDER "Tests")

include(Catch)
catch_discover_tests(PredationTests)
