# Figures for presentations: run from the repository root with make viz.
# Needs the downloaded models and NAM Core from make install.
.DEFAULT_GOAL := viz
NAM_DIR = libs/NeuralAmpModelerCore
include make/models.mk

# Optimized, unsanitized builds: the figures render tens of seconds of audio.
VIZ_FLAGS = -std=c++20 -O2 -DNAM_SAMPLE_FLOAT
VIZ_INCLUDES = -Isrc -I$(NAM_DIR) -I$(NAM_DIR)/Dependencies/eigen -I$(NAM_DIR)/Dependencies/nlohmann -Ibuild/generated
VIZ_REF_SOURCES = $(addprefix $(NAM_DIR)/NAM/,dsp.cpp get_dsp.cpp nam_file.cpp activations.cpp conv1d.cpp ring_buffer.cpp util.cpp wavenet/model.cpp wavenet/slimmable.cpp)
VIZ_REF_OBJECTS = $(patsubst $(NAM_DIR)/NAM/%.cpp,build/viz/ref/%.o,$(VIZ_REF_SOURCES))
VIZ_ENGINE_SOURCES = src/models/a2_lite.cpp src/models/amp_models.cpp src/audio/cab_filter.cpp
VIZ_RENDER = build/viz/render
VENV = .venv
VENV_STAMP = $(VENV)/.requirements-installed

build/viz/ref/%.o: $(NAM_DIR)/NAM/%.cpp viz/viz.mk
	@mkdir -p $(@D)
	$(CXX) $(VIZ_FLAGS) $(VIZ_INCLUDES) -MMD -MP -c $< -o $@

$(VIZ_RENDER): viz/render.cpp $(VIZ_ENGINE_SOURCES) src/audio/cab_filter.h src/models/a2_lite.h src/models/amp_model.h src/models/amp_models.h $(A2_HEADER) $(VIZ_REF_OBJECTS) viz/viz.mk
	@mkdir -p $(@D)
	$(CXX) $(VIZ_FLAGS) $(VIZ_INCLUDES) viz/render.cpp $(VIZ_ENGINE_SOURCES) $(VIZ_REF_OBJECTS) -o $@

# The virtual environment is reinstalled whenever requirements.txt changes.
$(VENV_STAMP): viz/requirements.txt
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install --quiet -r viz/requirements.txt
	@touch $@

.PHONY: viz venv
venv: $(VENV_STAMP)

viz: $(VIZ_RENDER) $(VENV_STAMP) $(A2_MODELS)
	$(VENV)/bin/python -B viz/figures.py

-include $(VIZ_REF_OBJECTS:.o=.d)
