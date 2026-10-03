CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -pthread -fPIC -MMD -MP

DEFINES = \
    -DVBOX \
    -DRT_OS_WINDOWS \
    -DVMSVGA3D_DIRECT3D \
    -DVBOX_VMSVGA3D_WITH_WINE_OPENGL \
    -DVBOX_WITH_VMSVGA \
    -DVBOX_WITH_VMSVGA3D \
    -DIN_RING3 \
    -DLOG_GROUP=LOG_GROUP_DEV_VMSVGA

INCLUDES_ORACLE = \
    -I. \
    -Ivbox_headers \
    -Ishim/include \
    -Ishim/include/VBox \
    -Ishim/include/iprt \
    -Ishim/include/vmsvga \
    -Ishim/include/mock \
    -Itools/include

INCLUDES_VLKN = \
    -Iinclude \
    -Iinclude/internal \
    -Ishim/include \
    -Idata \
    -Itools/include

BUILD_DIR = build
BIN_DIR = bin
LIB_DIR = lib
DATA_DIR = data

# Oracle Objects
ORACLE_OBJS = \
    $(BUILD_DIR)/DevVGA-SVGA3d-win.o \
    $(BUILD_DIR)/mock_d3d9.o \
    $(BUILD_DIR)/vbox_shim.o \
    $(BUILD_DIR)/svga3d-oracle.o

# SVGA3=VLKN Engine Objects
VLKN_OBJS = \
    $(BUILD_DIR)/vlkn_dispatch.o \
    $(BUILD_DIR)/vlkn_backend.o \
    $(BUILD_DIR)/svga3_surface.o \
    $(BUILD_DIR)/svga3_context.o \
    $(BUILD_DIR)/svga3_fifo.o \
    $(BUILD_DIR)/svga3_dx.o \
    $(BUILD_DIR)/svga3_device.o \
    $(BUILD_DIR)/svga3_shader_translator.o \
    $(BUILD_DIR)/svga3_guest_mem.o \
    $(BUILD_DIR)/qemu_vmsvga.o

ORACLE_TARGET = $(BIN_DIR)/svga3d-oracle
VLKN_LIB = $(LIB_DIR)/libsvga3_vlkn.a
VLKN_TEST_TARGET = $(BIN_DIR)/test_svga3_vlkn
REAL_VULKAN_TEST_TARGET = $(BIN_DIR)/test_real_vulkan
SHADER_TRANSLATION_TEST_TARGET = $(BIN_DIR)/test_shader_translation
TRANSLATOR_NOVULKAN_TEST_TARGET = $(BIN_DIR)/test_translator_novulkan
SHADER_TEST_TARGET = $(BIN_DIR)/test_shader_execution
GUEST_MEM_TEST_TARGET = $(BIN_DIR)/test_guest_memory
MALFORMED_INPUT_TEST_TARGET = $(BIN_DIR)/test_malformed_inputs
VERIFIED_RENDERING_TEST_TARGET = $(BIN_DIR)/test_verified_rendering
PRESENTATION_TEST_TARGET = $(BIN_DIR)/test_presentation
QEMU_TEST_TARGET = $(BIN_DIR)/test_qemu_integration
DX_TEST_TARGET = $(BIN_DIR)/test_dx_path
LIB_QEMU_SVGA3D = $(LIB_DIR)/libqemu_svga3d.so

.PHONY: all clean test test-oracle test-vlkn test-real-vulkan test-shader-translation test-shader test-guest-mem test-verified-rendering test-presentation test-qemu test-piglit harness-loop acceptance dump

all: $(BIN_DIR)/test_preload_fifo $(BIN_DIR)/test_buffer_ordering $(DX_TEST_TARGET) $(ORACLE_TARGET) $(VLKN_LIB) $(LIB_QEMU_SVGA3D) $(VLKN_TEST_TARGET) $(REAL_VULKAN_TEST_TARGET) $(SHADER_TRANSLATION_TEST_TARGET) $(TRANSLATOR_NOVULKAN_TEST_TARGET) $(SHADER_TEST_TARGET) $(GUEST_MEM_TEST_TARGET) $(VERIFIED_RENDERING_TEST_TARGET) $(PRESENTATION_TEST_TARGET) $(QEMU_TEST_TARGET) $(MALFORMED_INPUT_TEST_TARGET)

# Oracle Binary
$(ORACLE_TARGET): $(ORACLE_OBJS) | $(BIN_DIR) $(DATA_DIR)
	$(CXX) $(CXXFLAGS) $(ORACLE_OBJS) -o $@

$(BUILD_DIR)/DevVGA-SVGA3d-win.o: DevVGA-SVGA3d-win.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(DEFINES) $(INCLUDES_ORACLE) -c $< -o $@

$(BUILD_DIR)/mock_d3d9.o: shim/src/mock_d3d9.cpp shim/include/mock/mock_d3d9.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(DEFINES) $(INCLUDES_ORACLE) -c $< -o $@

$(BUILD_DIR)/vbox_shim.o: shim/src/vbox_shim.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(DEFINES) $(INCLUDES_ORACLE) -c $< -o $@

$(BUILD_DIR)/svga3d-oracle.o: tools/svga3d-oracle.cpp tools/include/svga3d_tables.h shim/include/mock/mock_d3d9.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(DEFINES) $(INCLUDES_ORACLE) -c $< -o $@

# SVGA3=VLKN Library
$(VLKN_LIB): $(VLKN_OBJS) | $(LIB_DIR)
	ar rcs $@ $(VLKN_OBJS)

$(BUILD_DIR)/vlkn_dispatch.o: src/vlkn_dispatch.cpp include/internal/vlkn_dispatch.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) -c $< -o $@

$(BUILD_DIR)/vlkn_backend.o: src/vlkn_backend.cpp include/internal/vlkn_backend.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) -c $< -o $@

$(BUILD_DIR)/svga3_surface.o: src/svga3_surface.cpp include/internal/svga3_surface.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) -c $< -o $@

$(BUILD_DIR)/svga3_context.o: src/svga3_context.cpp include/internal/svga3_context.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) -c $< -o $@

$(BUILD_DIR)/svga3_fifo.o: src/svga3_fifo.cpp include/internal/svga3_device.h include/internal/svga3_dx.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) -c $< -o $@

$(BUILD_DIR)/svga3_dx.o: src/svga3_dx.cpp include/internal/svga3_dx.h include/internal/svga3_device.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) -c $< -o $@

$(BUILD_DIR)/svga3_device.o: src/svga3_device.cpp include/internal/svga3_device.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) -c $< -o $@

$(BUILD_DIR)/svga3_shader_translator.o: src/svga3_shader_translator.cpp include/internal/svga3_shader_translator.h include/internal/svga3_spirv_builder.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) -c $< -o $@

$(BUILD_DIR)/svga3_guest_mem.o: src/svga3_guest_mem.cpp include/internal/svga3_guest_mem.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) -c $< -o $@

$(BUILD_DIR)/qemu_vmsvga.o: src/qemu_vmsvga.cpp include/qemu_vmsvga.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) -c $< -o $@

# SVGA3=VLKN Test Suite
$(VLKN_TEST_TARGET): tests/test_svga3_vlkn.cpp $(VLKN_LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) $< -L$(LIB_DIR) -lsvga3_vlkn -ldl -o $@

$(REAL_VULKAN_TEST_TARGET): tests/test_real_vulkan.cpp $(VLKN_LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) $< -L$(LIB_DIR) -lsvga3_vlkn -ldl -o $@

$(SHADER_TRANSLATION_TEST_TARGET): tests/test_shader_translation.cpp $(VLKN_LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) $< -L$(LIB_DIR) -lsvga3_vlkn -ldl -o $@

$(TRANSLATOR_NOVULKAN_TEST_TARGET): tests/test_translator_novulkan.cpp $(VLKN_LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) $< -L$(LIB_DIR) -lsvga3_vlkn -ldl -o $@

$(SHADER_TEST_TARGET): tests/test_shader_execution.cpp $(VLKN_LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) $< -L$(LIB_DIR) -lsvga3_vlkn -ldl -o $@

$(GUEST_MEM_TEST_TARGET): tests/test_guest_memory.cpp $(VLKN_LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) $< -L$(LIB_DIR) -lsvga3_vlkn -ldl -o $@

$(MALFORMED_INPUT_TEST_TARGET): tests/test_malformed_inputs.cpp $(VLKN_LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) $< -L$(LIB_DIR) -lsvga3_vlkn -ldl -o $@

$(VERIFIED_RENDERING_TEST_TARGET): tests/test_verified_rendering.cpp $(VLKN_LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) $< -L$(LIB_DIR) -lsvga3_vlkn -ldl -o $@

$(PRESENTATION_TEST_TARGET): tests/test_presentation.cpp $(VLKN_LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) $< -L$(LIB_DIR) -lsvga3_vlkn -ldl -o $@

$(QEMU_TEST_TARGET): tests/test_qemu_integration.cpp $(VLKN_LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) $< -L$(LIB_DIR) -lsvga3_vlkn -ldl -o $@

$(LIB_QEMU_SVGA3D): src/qemu_svga3d_preload.cpp $(VLKN_LIB) | $(LIB_DIR)
	$(CXX) $(CXXFLAGS) -shared -I. $(INCLUDES_VLKN) $< -L$(LIB_DIR) -Wl,--whole-archive -lsvga3_vlkn -Wl,--no-whole-archive -static-libstdc++ -static-libgcc -ldl -lpthread -o $@

test-real-vulkan: $(REAL_VULKAN_TEST_TARGET)
	./$(REAL_VULKAN_TEST_TARGET)

test-shader-translation: $(SHADER_TRANSLATION_TEST_TARGET)
	./$(SHADER_TRANSLATION_TEST_TARGET)

test-translator-novulkan: $(TRANSLATOR_NOVULKAN_TEST_TARGET)
	./$(TRANSLATOR_NOVULKAN_TEST_TARGET)

test-shader: $(SHADER_TEST_TARGET)
	./$(SHADER_TEST_TARGET)

test-guest-mem: $(GUEST_MEM_TEST_TARGET)
	./$(GUEST_MEM_TEST_TARGET)

test-verified-rendering: $(VERIFIED_RENDERING_TEST_TARGET)
	./$(VERIFIED_RENDERING_TEST_TARGET)

test-presentation: $(PRESENTATION_TEST_TARGET)
	./$(PRESENTATION_TEST_TARGET)

test-qemu: $(QEMU_TEST_TARGET)
	./$(QEMU_TEST_TARGET)

# External guest OpenGL tests; independent of the local Vulkan harness.
# Example: make test-piglit PIGLIT_ARGS="--ssh user@guest"
test-piglit:
	python3 scripts/run_piglit.py $(PIGLIT_ARGS)

# Tight feedback loop: build -> ICD-free translator suite -> lavapipe suites ->
# per-iteration summary. Stops on NEW regressions vs the previous iteration.
# Env: HARNESS_ICD (Vulkan ICD JSON), SPIRV_TOOLS_DIR (spirv-val/spirv-dis),
# HARNESS_TIMEOUT (per-suite seconds). Extra args via HARNESS_LOOP_ARGS, e.g.
#   make harness-loop HARNESS_LOOP_ARGS="--iterations 5 --fail-fast"
harness-loop:
	@chmod +x scripts/harness_loop.sh
	./scripts/harness_loop.sh $(HARNESS_LOOP_ARGS)

acceptance: all
	@chmod +x scripts/run_acceptance_suite.sh
	./scripts/run_acceptance_suite.sh

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(LIB_DIR):
	mkdir -p $(LIB_DIR)

$(DATA_DIR):
	mkdir -p $(DATA_DIR)

test: $(ORACLE_TARGET) $(VLKN_TEST_TARGET) $(BIN_DIR)/test_preload_fifo $(DX_TEST_TARGET)
	@echo "=== Running Oracle Reference Verification ==="
	./$(ORACLE_TARGET) --test
	@echo "\n=== Running SVGA3=VLKN Product Verification ==="
	./$(VLKN_TEST_TARGET)
	./$(BIN_DIR)/test_preload_fifo
	./$(DX_TEST_TARGET)

test-oracle: $(ORACLE_TARGET)
	./$(ORACLE_TARGET) --test

test-vlkn: $(VLKN_TEST_TARGET)
	./$(VLKN_TEST_TARGET)

dump: $(ORACLE_TARGET) | $(DATA_DIR)
	./$(ORACLE_TARGET) --dump-json $(DATA_DIR)/svga3d_reference.json
	./$(ORACLE_TARGET) --dump-header $(DATA_DIR)/svga3d_reference.h

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) $(LIB_DIR)

# Track transitive headers to prevent stale objects after interface changes.
-include $(wildcard $(BUILD_DIR)/*.d)

$(BIN_DIR)/test_buffer_ordering: tests/test_buffer_ordering.cpp $(VLKN_LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) $< -L$(LIB_DIR) -lsvga3_vlkn -ldl -o $@

$(DX_TEST_TARGET): tests/test_dx_path.cpp $(VLKN_LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) $< -L$(LIB_DIR) -lsvga3_vlkn -ldl -o $@

$(BIN_DIR)/test_preload_fifo: tests/test_preload_fifo.cpp src/qemu_svga3d_preload.cpp $(VLKN_LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES_VLKN) $< -L$(LIB_DIR) -lsvga3_vlkn -ldl -o $@
