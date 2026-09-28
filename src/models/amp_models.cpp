#include "models/amp_models.h"

#include "embedded_a2_data.h"
#include "models/a2_lite.h"

static_assert(sizeof(embedded_a2::kFenderTwin65) == A2Lite::kWeights * sizeof(float), "embedded model size does not match the engine");
static_assert(sizeof(embedded_a2::kVoxAc30Chimey) == A2Lite::kWeights * sizeof(float), "embedded model size does not match the engine");
static_assert(sizeof(embedded_a2::kMarshallJcm800G5) == A2Lite::kWeights * sizeof(float), "embedded model size does not match the engine");

const char *AmpName(AmpId id) {
    switch (id) {
    case AmpId::Fender:
        return "Fender Twin65";
    case AmpId::Vox:
        return "Vox AC30 Chimey";
    case AmpId::Marshall:
        return "Marshall JCM800 G5";
    }
    return "invalid";
}

std::unique_ptr<AmpModel> CreateAmpModel(AmpId id) {
    switch (id) {
    case AmpId::Fender:
        return std::make_unique<A2Lite>(embedded_a2::kFenderTwin65);
    case AmpId::Vox:
        return std::make_unique<A2Lite>(embedded_a2::kVoxAc30Chimey);
    case AmpId::Marshall:
        return std::make_unique<A2Lite>(embedded_a2::kMarshallJcm800G5);
    }
    return nullptr;
}
