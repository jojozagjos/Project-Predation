#include "Engine/Application.h"
#include "Game/PredationGame.h"

int main(int argc, char** argv)
{
    pred::PredationGame game;
    pred::Application app;
    return app.Run(game, argc, argv);
}
