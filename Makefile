# morse-code — ESP32 build + flash shortcuts (IdeaSpark, CH340, ST7789)
#
#   make upload              compile + flash to the auto-detected port
#   make upload PORT=/dev/ttyUSB0
#   make build               compile-check only (no device needed)
#   make monitor             serial log @ 115200 (Ctrl+C to exit)
#   make check               verify toolchain + serial port
#   make secrets DEVICE=dad  generate firmware/src/secrets.h from .env
#   make preview             render the UI to tools/preview/out/preview.png
#   make sim [PORT=8077]       run the ESP32 display simulator (web UI)
#   make sim-dump              headless simulator screenshots to tools/sim/out
#   make start                 start the relay server in the background
#   make stop                  stop the relay server
#   make restart               restart the relay server
#   make clean               remove build + preview output

PORT      ?= $(firstword $(wildcard /dev/ttyUSB* /dev/ttyACM*) $(UPLOAD_PORT) /dev/ttyUSB0)
PORT_SIM  ?= 8077
RELAY_HOST ?= 0.0.0.0
RELAY_PORT ?= 8180
DEVICE    ?= dad
FQBN      := esp32:esp32:esp32
SKETCH    := /tmp/morse-code-sketch/morse-code
BUILD_DIR := /tmp/morse-code-sketch/build
BAUD      := 115200

SOURCES := firmware/src/main.cpp firmware/src/ui.cpp firmware/src/ui.h \
           firmware/src/morse.cpp firmware/src/morse.h firmware/src/config.h

.PHONY: help build upload monitor check preview sim sim-dump clean stage secrets \
        start stop restart status

help:
	@echo "Targets:"
	@echo "  make upload [PORT=/dev/ttyUSB0]  compile + flash firmware to ESP32"
	@echo "  make build                       compile-check only (no device needed)"
	@echo "  make monitor [PORT=...]          serial log @ 115200 baud"
	@echo "  make check                       verify arduino-cli core/libs + serial port"
	@echo "  make secrets DEVICE=dad          generate firmware/src/secrets.h from .env"
	@echo "  make preview                     render UI frames to a PNG (host-side)"
	@echo "  make sim [PORT=8077]             ESP32 display simulator at http://localhost:8077/"
	@echo "  make sim-dump                    headless simulator screenshots to tools/sim/out"
	@echo "  make start [RELAY_PORT=8180]     start the relay server in the background"
	@echo "  make stop                        stop the relay server"
	@echo "  make restart                     restart the relay server"
	@echo "  make clean                       remove build + preview output"
	@echo ""
	@echo "Current PORT: $(PORT)"

secrets:
	./scripts/gen-secrets.py $(DEVICE) --force

check:
	@arduino-cli version
	@arduino-cli core list | grep -q 'esp32:esp32' || \
	  (echo "Missing esp32 core: arduino-cli core install esp32:esp32"; exit 1)
	@arduino-cli lib list | grep -qi 'Adafruit ST7735 and ST7789' || \
	  (echo "Missing ST7789 library: arduino-cli lib install 'Adafruit ST7735 and ST7789 Library'"; exit 1)
	@arduino-cli lib list | grep -qi 'Adafruit GFX' || \
	  (echo "Missing GFX library: arduino-cli lib install 'Adafruit GFX Library'"; exit 1)
	@echo "PORT=$(PORT)"
	@ls -l $(PORT) || (echo "No serial port at $(PORT). Plug in via USB data cable, then: ls /dev/ttyUSB* /dev/ttyACM*"; exit 1)

# Stage the sources into a sketch folder (same layout as build-arduino-cli.sh).
$(SKETCH)/main.cpp: $(SOURCES)
	@rm -rf /tmp/morse-code-sketch
	@mkdir -p $(SKETCH)
	@cp firmware/src/main.cpp firmware/src/ui.cpp firmware/src/ui.h \
	    firmware/src/morse.cpp firmware/src/morse.h firmware/src/config.h $(SKETCH)/
	@if [ -f firmware/src/secrets.h ]; then cp firmware/src/secrets.h $(SKETCH)/; fi
	@printf '// morse-code sketch - real sources live in main.cpp / ui.cpp / morse.cpp\n' \
	    > $(SKETCH)/morse-code.ino

stage: $(SKETCH)/main.cpp

build: stage
	arduino-cli compile --fqbn $(FQBN) --output-dir $(BUILD_DIR) $(SKETCH)

upload: stage
	@if [ ! -e "$(PORT)" ]; then \
	  echo "No serial port at $(PORT). Plug in via USB data cable, then: ls /dev/ttyUSB* /dev/ttyACM*"; \
	  exit 1; \
	fi
	arduino-cli compile --fqbn $(FQBN) --output-dir $(BUILD_DIR) $(SKETCH)
	arduino-cli upload -p $(PORT) --fqbn $(FQBN) --input-dir $(BUILD_DIR) $(SKETCH)
	@echo "Flashed $(PORT). Run 'make monitor PORT=$(PORT)' to verify the boot log."

monitor:
	arduino-cli monitor -p $(PORT) -c baudrate=$(BAUD)

preview:
	./scripts/preview.sh

sim:
	./scripts/sim.sh $(PORT_SIM)

sim-dump:
	@mkdir -p tools/sim/build tools/sim/out
	c++ -std=c++17 -O2 -Wall -Wextra -pthread -Itools/preview -Ifirmware/src \
	  tools/sim/sim.cpp firmware/src/ui.cpp firmware/src/morse.cpp \
	  -o tools/sim/build/sim
	./tools/sim/build/sim --dump tools/sim/out
	@echo "Wrote tools/sim/out/00-*.ppm (170x320)"

# Relay server: see server/Makefile for the implementation.
start stop restart status:
	@$(MAKE) --no-print-directory -C server $@ HOST=$(RELAY_HOST) PORT=$(RELAY_PORT)

clean:
	rm -rf /tmp/morse-code-sketch tools/preview/build tools/preview/out tools/sim/build tools/sim/out
