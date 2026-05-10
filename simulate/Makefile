# Baseball Tracker Simulator Build
CXX := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -O2

SDL2_CFLAGS ?= $(shell pkg-config --cflags SDL2 2>/dev/null || echo "-I$(NIX_SDL2_DEV)/include/SDL2")
SDL2_TTF_CFLAGS ?= $(shell pkg-config --cflags SDL2_ttf 2>/dev/null || echo "-I$(NIX_SDL2_TTF)/include")
SDL2_LIBS ?= $(shell pkg-config --libs SDL2 2>/dev/null || echo "-lSDL2")
SDL2_TTF_LIBS ?= $(shell pkg-config --libs SDL2_ttf 2>/dev/null || echo "-lSDL2_ttf")
GL_LIBS ?= $(shell pkg-config --libs gl 2>/dev/null || echo "-lGL")

NLOHMANN_JSON ?= $(strip $(NIX_NLOHMANN_JSON))

ifeq ($(NLOHMANN_JSON),)
ifneq ($(wildcard /usr/include/nlohmann/json.hpp),)
NLOHMANN_JSON := /usr/include
endif
endif

ifneq ($(NLOHMANN_JSON),)
EXTRA_JSON_FLAGS := -isystem $(NLOHMANN_JSON)
else
EXTRA_JSON_FLAGS :=
$(warning nlohmann/json not found — set NIX_NLOHMANN_JSON via nix develop or install libnlohmann-json-dev)
endif

HAVE_CURL := $(shell pkg-config --exists libcurl && echo yes)

ifeq ($(HAVE_CURL),yes)
CURL_CPPFLAGS := $(shell pkg-config --cflags libcurl)
CURL_LIBS := $(shell pkg-config --libs libcurl)
else
CURL_CPPFLAGS :=
CURL_LIBS :=
EXTRA_JSON_FLAGS += -DSIM_WITHOUT_CURL
$(warning libcurl missing — MLB HTTP poll disabled unless you install libcurl-dev / use nix develop)
endif

IMGUI_DIR ?= $(CURDIR)/../deps/imgui

ALL_CXXFLAGS := $(CXXFLAGS) $(EXTRA_JSON_FLAGS) $(CURL_CPPFLAGS) $(SDL2_CFLAGS) $(SDL2_TTF_CFLAGS) \
                -I$(IMGUI_DIR) -I$(IMGUI_DIR)/imgui -I$(IMGUI_DIR)/backends
ALL_LDFLAGS := $(SDL2_LIBS) $(SDL2_TTF_LIBS) $(CURL_LIBS)  -ldl -lpthread

TARGET := bin/baseball-tracker-sim

.PHONY: all clean run

all: $(TARGET)

$(TARGET): build/simulate.o build/mlb_live.o build/imgui.o build/imgui_demo.o build/imgui_draw.o \
           build/imgui_tables.o build/imgui_widgets.o build/imgui_impl_sdl2.o \
           build/imgui_impl_sdlrenderer2.o | bin
	$(CXX) $^ -o $@ $(ALL_LDFLAGS)

build/simulate.o: simulate.cpp sim_state.h mlb_live.h | build
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

build/mlb_live.o: mlb_live.cpp mlb_live.h sim_state.h | build
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

build/imgui.o: $(IMGUI_DIR)/imgui.cpp | build
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

build/imgui_demo.o: $(IMGUI_DIR)/imgui_demo.cpp | build
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

build/imgui_draw.o: $(IMGUI_DIR)/imgui_draw.cpp | build
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

build/imgui_tables.o: $(IMGUI_DIR)/imgui_tables.cpp | build
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

build/imgui_widgets.o: $(IMGUI_DIR)/imgui_widgets.cpp | build
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

build/imgui_impl_sdl2.o: $(IMGUI_DIR)/backends/imgui_impl_sdl2.cpp | build
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

build/imgui_impl_sdlrenderer2.o: $(IMGUI_DIR)/backends/imgui_impl_sdlrenderer2.cpp | build
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

bin:
	mkdir -p bin

build:
	mkdir -p build

clean:
	rm -rf build bin

run: $(TARGET)
	./$(TARGET)
