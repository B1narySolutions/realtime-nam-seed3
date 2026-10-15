#pragma once

#include "models/amp_model.h"
#include <memory>

// Values match the USB amp-selection commands. models/amps.json lists the
// models in this order; the build fails if they disagree.
enum class AmpId { Fender = 1, Vox = 2, Marshall = 3, Princeton = 4 };
inline constexpr int kAmpCount = 4;

// Display name for the serial status line, or "invalid" for an unknown amp.
const char *AmpName(AmpId id);
// Whether the amp's capture already includes a mic'd speaker cabinet, so
// the cabinet EQ must not be applied on top. False for an unknown amp.
bool AmpIncludesCabinet(AmpId id);
// Returns nullptr for an unknown amp. Construct only while audio is stopped.
std::unique_ptr<AmpModel> CreateAmpModel(AmpId id);
