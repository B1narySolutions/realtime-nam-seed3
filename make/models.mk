# Shared firmware/host model preparation. Keep the original .nam as the source.
# models/amps.json lists the models in AmpId order.
A2_MANIFEST = models/amps.json
A2_NAMES := $(shell python3 scripts/convert_a2.py names)
A2_MODELS = $(addprefix models/local/,$(addsuffix .nam,$(A2_NAMES)))
A2_HEADER = build/generated/embedded_a2_data.h

$(A2_MODELS):
	@echo 'Missing required amp models. Run bash scripts/download_models.sh first.' >&2
	@exit 1

$(A2_HEADER): $(A2_MODELS) $(A2_MANIFEST) scripts/convert_a2.py
	python3 scripts/convert_a2.py header $@
