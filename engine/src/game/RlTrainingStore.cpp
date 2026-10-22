#include <myengine/game/RlTrainingStore.h>

#include <myengine/core/AppPaths.h>
#include <myengine/core/ServiceLocator.h>

namespace myengine::game
{
    std::filesystem::path TrainingResultsPath() { return core::apppaths::UserDataDirectory() / "training.json"; }

    std::filesystem::path TrainingPacksDirectory() { return core::ServiceLocator::GetProjectContext().ResolveContentPath("assets/data/training"); }

    std::vector<rl::RlTrainingPack> LoadTrainingPacks(std::vector<std::string>* errors)
    {
        return rl::RlLoadTrainingPacks(TrainingPacksDirectory().generic_u8string(), errors);
    }

    void AttachTrainingResults(rl::RlTraining& training) { training.SetResultsFile(TrainingResultsPath().generic_u8string()); }
}
