// RlTrainingStore.h
//
// Where the training of the Rocket League game (rl::RlTraining, docs/game/rl-training.md) keeps its files: the packs of exercises are data of
// the project (assets/data/training/*.json, shipped with the game), the results of the player are a file of the user (training.json in
// core::apppaths::UserDataDirectory(): %APPDATA%\myengine in development, %LOCALAPPDATA%\RocketArena in a shipping build). The game library
// knows no folders of the application, so this is the glue the game calls once when the Training screen opens.
//
//     auto packs = game::LoadTrainingPacks(&errors);            // the list of the screen
//     rl::RlTraining training(tuning);
//     game::AttachTrainingResults(training);                    // the results are read now and written after every attempt
//     training.StartPack(packs[i]);

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <rl/RlTraining.h>

namespace myengine::game
{
    // <UserData>/training.json
    std::filesystem::path TrainingResultsPath();
    // assets/data/training of the open project (the folder of the shipped game in a package)
    std::filesystem::path TrainingPacksDirectory();
    // Every pack of TrainingPacksDirectory(), by their order; the files that do not read are named in `errors`
    std::vector<rl::RlTrainingPack> LoadTrainingPacks(std::vector<std::string>* errors = nullptr);
    // Reads TrainingResultsPath() into the training and makes it write the file after every finished attempt
    void AttachTrainingResults(rl::RlTraining& training);
}
