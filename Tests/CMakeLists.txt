find_package(Catch2 3 CONFIG REQUIRED)

add_executable(PredationTests
    CoreTests.cpp
    PlayerTests.cpp
    AnimationTests.cpp
    BodyPoseTests.cpp
    InteractionTests.cpp
    WeaponTests.cpp
    NetTests.cpp
)
pred_set_warnings(PredationTests)
target_link_libraries(PredationTests PRIVATE Predation::GameLib Catch2::Catch2WithMain)
set_target_properties(PredationTests PROPERTIES FOLDER "Tests")

include(Catch)
catch_discover_tests(PredationTests)
