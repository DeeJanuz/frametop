# Hand tracking, built into build/ (hands/build.sh runs this in the dev container):
#   make          ft-camd (camd/: runs on the host, so linked statically) and ft-hands (track/)
#   make tools    ft-handreplay and ft-ringplay, for recordings
#   make check    builds and runs the C++ unit tests (tests/sides_test.cpp)
# The first build fetches ncnn (NCNN_TAG) and builds it into build/ncnn, which takes a few
# minutes. NCNN=DIR uses an ncnn install already built instead.
NCNN_TAG  = 20260526
NCNN     ?= build/ncnn/install
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CXXFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-psabi
CXXFLAGS += -std=c++17 -fopenmp -I$(NCNN)/include/ncnn
LDLIBS    = $(NCNN)/lib/libncnn.a -ljsoncpp -fopenmp -lpthread
# The standalone Hand Recorder's release build passes LDFLAGS=-static (it runs on the host).
LDFLAGS  ?=

CAMD  = camd/camd.c camd/tp.c camd/xrcams.c
TRACK = track/calib.cpp track/nets.cpp track/tracker.cpp track/io.cpp track/record.cpp track/pinch.cpp track/sides.cpp
HDR   = $(wildcard track/*.h) camd/fhring.h include/fh_hands.h include/fh_gestures.h

all: build/ft-camd build/ft-hands
tools: build/ft-handreplay build/ft-ringplay

build/ft-camd: $(CAMD) camd/tp.h camd/xrcams.h camd/fhring.h
	@mkdir -p build
	$(CC) $(CFLAGS) -static -o $@ $(CAMD) -lm

build/ft-hands: track/main.cpp $(TRACK) $(HDR) $(NCNN)/lib/libncnn.a
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ track/main.cpp $(TRACK) $(LDLIBS)

build/ft-handreplay: track/replay.cpp $(TRACK) $(HDR) $(NCNN)/lib/libncnn.a
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ track/replay.cpp $(TRACK) $(LDLIBS)

build/sides-test: tests/sides_test.cpp $(TRACK) $(HDR) $(NCNN)/lib/libncnn.a
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ tests/sides_test.cpp $(TRACK) $(LDLIBS)

check: build/sides-test
	build/sides-test

build/ft-ringplay: track/ringplay.cpp track/record.h camd/fhring.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ track/ringplay.cpp

# ncnn as frame-hands built it (the models were converted and quantized for it), minus its tools
build/ncnn/install/lib/libncnn.a:
	rm -rf build/ncnn && mkdir -p build/ncnn
	git clone -q --depth 1 --branch $(NCNN_TAG) -c advice.detachedHead=false https://github.com/Tencent/ncnn.git build/ncnn/src
	cmake -S build/ncnn/src -B build/ncnn/build -G Ninja -Wno-dev -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_INSTALL_PREFIX=$(CURDIR)/build/ncnn/install -DCMAKE_INSTALL_LIBDIR=lib -DNCNN_VULKAN=OFF \
	  -DNCNN_OPENMP=ON -DNCNN_INT8=ON -DNCNN_SIMPLEOCV=ON -DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_EXAMPLES=OFF \
	  -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_BUILD_TESTS=OFF -DNCNN_PYTHON=OFF > build/ncnn/cmake.log
	cmake --build build/ncnn/build --target install > build/ncnn/build.log

clean:
	rm -f build/ft-camd build/ft-hands build/ft-handreplay build/ft-ringplay build/sides-test

.PHONY: all tools check clean
