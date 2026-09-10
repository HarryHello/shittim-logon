# macos targets. Run from this directory (macos/).
#   make                bridge-test + app
#   make app            the Swift shell (links the bridge + spine runtime)
#   make bridge-test    spine-free pipeline verification
#   make render-still   the adapter's worked example, rebuilt for macOS
#   make clean
#
# spine-cpp is vendored into spine/ by fetch-spine.sh (never committed).

CXX      = clang++
CXXFLAGS = -std=c++17 -O2 -Wall -I../adapter -Ibridge -Ispine/include
FWORKS   = -framework ApplicationServices -framework CoreFoundation

APP       = build/ShittimMac
SPINE_SRC = spine/src/spine
SPINE_INC = spine/include
SPINE_OBJ = $(patsubst $(SPINE_SRC)/%.cpp,build/spine/%.o,$(wildcard $(SPINE_SRC)/*.cpp))
LIB       = build/libshittim.a

.PHONY: all bridge-test app render-still clean
all: bridge-test app

build:
	mkdir -p $@

build/spine/%.o: $(SPINE_SRC)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -I$(SPINE_INC) -c $< -o $@

build/shittim_bridge.o: bridge/shittim_bridge.cpp bridge/shittim_bridge.h ../adapter/image.h ../adapter/image_mac.h ../adapter/scene.h ../adapter/scenes.h ../adapter/raster.h | build
	$(CXX) $(CXXFLAGS) -c bridge/shittim_bridge.cpp -o $@

$(LIB): build/shittim_bridge.o $(SPINE_OBJ)
	ar rcs $@ $^

build/bridge-test: bridge-test.cpp ../adapter/raster.h ../adapter/scenes.h ../adapter/image.h ../adapter/image_mac.h | build
	$(CXX) $(CXXFLAGS) -Ibridge $(FWORKS) bridge-test.cpp -o $@

app: $(APP)

$(APP): app/*.swift bridge/shittim_bridge.h $(LIB) | build
	swiftc -O -import-objc-header bridge/shittim_bridge.h -o $@ app/*.swift $(LIB) -lc++

bridge-test: build/bridge-test

render-still: build/render-still

build/render-still: ../adapter/render_still.cpp ../adapter/scene.h ../adapter/raster.h ../adapter/scenes.h ../adapter/image.h ../adapter/image_mac.h $(SPINE_OBJ) | build
	$(CXX) $(CXXFLAGS) ../adapter/render_still.cpp $(SPINE_OBJ) -o $@ $(FWORKS)

clean:
	rm -rf build
