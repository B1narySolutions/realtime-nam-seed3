#pragma once

#include "models/amp_model.h"
#include <memory>

// Values match the USB amp-selection commands.
enum class AmpId { Fender = 1, Vox = 2, Marshall = 3 };

const char *AmpName(AmpId id);
// Returns nullptr for an unknown amp. Construct only while audio is stopped.
std::unique_ptr<AmpModel> CreateAmpModel(AmpId id);
