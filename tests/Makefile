# Run from the repository root with make test.
.DEFAULT_GOAL := test
NAM_DIR = libs/NeuralAmpModelerCore
include make/models.mk
TEST_BIN = build/tests/nam_processor_test
SOURCES = tests/nam_processor_test.cpp src/audio/nam_processor.cpp
FLAGS = -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
INCLUDES = -Isrc -Itests
include tests/a2.mk

.PHONY: test
test: $(TEST_BIN) build/tests/nam_audio_test test-a2
	./$(TEST_BIN)
	./build/tests/nam_audio_test $(A2_REFERENCES)
	python3 -B tests/test_convert_a2.py

$(TEST_BIN): $(SOURCES) src/audio/nam_processor.h src/models/amp_model.h tests/Makefile
	@mkdir -p $(@D)
	$(CXX) $(FLAGS) -Ibuild/generated $(INCLUDES) $(SOURCES) -o $@

build/tests/nam_audio_test: tests/nam_audio_test.cpp tests/audio_stimulus.h tests/allocation_guard.h \
  src/audio/nam_audio.cpp src/audio/nam_audio.h src/models/a2_lite.cpp src/models/a2_lite.h src/models/amp_model.h \
  src/models/amp_models.cpp src/models/amp_models.h src/audio/nam_processor.cpp src/audio/nam_processor.h \
  $(A2_HEADER) tests/Makefile
	@mkdir -p $(@D)
	$(CXX) $(FLAGS) -Ibuild/generated $(INCLUDES) tests/nam_audio_test.cpp \
	  src/audio/nam_audio.cpp src/models/amp_models.cpp src/models/a2_lite.cpp src/audio/nam_processor.cpp -o $@
