# Compare the fixed-shape engine against upstream NAM on every embedded model.
# Requires A2_NAMES and A2_HEADER from make/models.mk.
A2_DIR = $(NAM_DIR)
A2_FLAGS = -std=c++20 -O1 -g -DNAM_SAMPLE_FLOAT -fsanitize=address,undefined -fno-omit-frame-pointer
A2_INCLUDES = -I$(A2_DIR) -I$(A2_DIR)/Dependencies/eigen -I$(A2_DIR)/Dependencies/nlohmann -Itests
A2_REF_SOURCES = $(addprefix $(A2_DIR)/NAM/,dsp.cpp get_dsp.cpp nam_file.cpp activations.cpp conv1d.cpp ring_buffer.cpp util.cpp wavenet/model.cpp wavenet/slimmable.cpp)
A2_REF_OBJECTS = $(patsubst $(A2_DIR)/NAM/%.cpp,build/tests/a2-ref/%.o,$(A2_REF_SOURCES))
A2_REFERENCES = $(addprefix build/tests/,$(addsuffix .f32,$(A2_NAMES)))

build/tests/a2-ref/%.o: $(A2_DIR)/NAM/%.cpp tests/a2.mk
	@mkdir -p $(@D)
	$(CXX) $(A2_FLAGS) $(A2_INCLUDES) -MMD -MP -c $< -o $@

build/tests/render_a2_reference: tests/render_a2_reference.cpp tests/audio_stimulus.h $(A2_REF_OBJECTS)
	$(CXX) $(A2_FLAGS) $(A2_INCLUDES) $< $(A2_REF_OBJECTS) -o $@

build/tests/%.f32: models/local/%.nam build/tests/render_a2_reference
	./build/tests/render_a2_reference $< $@

build/tests/a2_test: tests/a2_test.cpp tests/audio_stimulus.h tests/allocation_guard.h src/models/a2_lite.cpp src/models/a2_lite.h src/models/amp_model.h src/models/amp_models.cpp src/models/amp_models.h src/audio/nam_processor.cpp src/audio/nam_processor.h $(A2_HEADER) tests/a2.mk
	@mkdir -p $(@D)
	$(CXX) $(FLAGS) $(INCLUDES) -Ibuild/generated tests/a2_test.cpp src/models/a2_lite.cpp src/models/amp_models.cpp src/audio/nam_processor.cpp -o $@

.PHONY: test-a2
test-a2: build/tests/a2_test $(A2_REFERENCES)
	./build/tests/a2_test $(A2_REFERENCES)

-include $(A2_REF_OBJECTS:.o=.d)
