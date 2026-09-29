#include "models/amp_models.h"

#include "embedded_a2_data.h"
#include "models/a2_lite.h"
#include <iterator>

// models/amps.json must list every AmpId exactly once, in order, and every
// model must have the engine's weight count.
static constexpr bool ModelsMatchAmpIds() {
    // The right number of models...
    if (std::size(embedded_a2::kModels) != kAmpCount)
        return false;

    // ...each in its AmpId slot and with the weight count the engine expects.
    for (int i = 0; i < kAmpCount; ++i) {
        const auto &model = embedded_a2::kModels[i];
        if (model.id != static_cast<AmpId>(i + 1) || model.weight_count != A2Lite::kWeights)
            return false;
    }
    return true;
}
static_assert(ModelsMatchAmpIds(), "models/amps.json does not match AmpId or the engine");

// Looks up an amp's embedded model (AmpIds start at 1); nullptr if invalid.
static const embedded_a2::Model *FindModel(AmpId id) {
    const int index = static_cast<int>(id) - 1;
    return index >= 0 && index < kAmpCount ? &embedded_a2::kModels[index] : nullptr;
}

const char *AmpName(AmpId id) {
    const auto *model = FindModel(id);
    return model ? model->name : "invalid";
}

std::unique_ptr<AmpModel> CreateAmpModel(AmpId id) {
    const auto *model = FindModel(id);
    return model ? std::make_unique<A2Lite>(model->weights) : nullptr;
}
