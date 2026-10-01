# Run from the repository root with make test.
.DEFAULT_GOAL := test
NAM_DIR = libs/NeuralAmpModelerCore
include make/models.mk
TEST_BIN = build/tests/nam_processor_test
CAB_TEST_BIN = build/tests/cab_filter_test
SOURCES = tests/nam_processor_test.cpp src/audio/nam_processor.cpp
FLAGS = -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
INCLUDES = -Isrc -Itests
include tests/a2.mk

# The amp models are downloaded locally and never committed. Without them,
# run only the tests that do not need them.
MISSING_MODELS = $(filter-out $(wildcard $(A2_MODELS)),$(A2_MODELS))

.PHONY: test
ifeq ($(MISSING_MODELS),)
test: $(TEST_BIN) $(CAB_TEST_BIN) build/tests/nam_audio_test test-a2
	./$(TEST_BIN)
	./$(CAB_TEST_BIN)
	./build/tests/nam_audio_test $(A2_REFERENCES)
	python3 -B tests/test_convert_a2.py
else
test: $(TEST_BIN) $(CAB_TEST_BIN)
	./$(TEST_BIN)
	./$(CAB_TEST_BIN)
	@echo 'Skipped A2-Lite and converter tests; missing $(MISSING_MODELS).' >&2
	@echo 'Run make download-models to enable them.' >&2
endif

$(TEST_BIN): $(SOURCES) src/audio/nam_processor.h src/models/amp_model.h tests/Makefile
	@mkdir -p $(@D)
	$(CXX) $(FLAGS) -Ibuild/generated $(INCLUDES) $(SOURCES) -o $@

$(CAB_TEST_BIN): tests/cab_filter_test.cpp tests/audio_stimulus.h tests/allocation_guard.h src/audio/cab_filter.cpp src/audio/cab_filter.h tests/Makefile
	@mkdir -p $(@D)
	$(CXX) $(FLAGS) $(INCLUDES) tests/cab_filter_test.cpp src/audio/cab_filter.cpp -o $@

build/tests/nam_audio_test: tests/nam_audio_test.cpp tests/audio_stimulus.h tests/allocation_guard.h \
  src/audio/nam_audio.cpp src/audio/nam_audio.h src/audio/cab_filter.cpp src/audio/cab_filter.h src/models/a2_lite.cpp src/models/a2_lite.h src/models/amp_model.h \
  src/models/amp_models.cpp src/models/amp_models.h src/audio/nam_processor.cpp src/audio/nam_processor.h \
  $(A2_HEADER) tests/Makefile
	@mkdir -p $(@D)
	$(CXX) $(FLAGS) -Ibuild/generated $(INCLUDES) tests/nam_audio_test.cpp \
	  src/audio/nam_audio.cpp src/audio/cab_filter.cpp src/models/amp_models.cpp src/models/a2_lite.cpp src/audio/nam_processor.cpp -o $@
