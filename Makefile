# morse-code — ESP32 build + flash shortcuts (IdeaSpark, CH340, ST7789)
#
#   make upload [DEVICE=dad|emma]  regen secrets.h, compile + flash auto-detected port
#   make upload PORT=/dev/ttyUSB0
#   make upload-dad / make upload-emma   shortcuts (DEVICE fixed)
#   make build [DEVICE=dad|emma]   compile-check only (no device needed)
#   make monitor             serial log @ 115200 (Ctrl+C to exit)
#   make check               verify toolchain + serial port
#   make secrets DEVICE=dad  generate firmware/src/secrets.h from .env
#   make preview             render the UI to tools/preview/out/preview.png
#   make sim [PORT=8077]       run the ESP32 display simulator (web UI)
#   make sim-dump              headless simulator screenshots to tools/sim/out
#   make worker-dev            local Cloudflare Worker dev server (port 8787)
#   make worker-test           worker unit + integration tests
#   make worker-deploy         deploy the Worker (custom domain from MORSE_SERVER_URL)
#   make clean               remove build + preview output
#
# The relay is a Cloudflare Worker + Durable Object — see worker/README.md.
# There is no laptop server any more: `make start/stop/restart` are gone
# (deleted in 0.2.0 with server/). The sim talks to the local Worker dev
# server by default (MORSE_SERVER_URL=http://localhost:8787); run
# `make worker-dev` in another terminal first.
#
# wrangler is a worker devDependency (installed by `make setup` into
# worker/node_modules/.bin); every wrangler invocation below goes through
# `npx` from worker/ or a make target — no global install needed. (Or:
# `cd worker && npx wrangler <cmd>`, or `npm install -g wrangler` if you
# prefer bare commands.)
MORSE_SERVER_URL ?= http://localhost:8787

PORT      ?= $(firstword $(wildcard /dev/ttyUSB* /dev/ttyACM*) $(UPLOAD_PORT) /dev/ttyUSB0)
PORT_SIM  ?= 8077
DEVICE    ?= dad
FQBN      := esp32:esp32:esp32
SKETCH    := /tmp/morse-code-sketch/morse-code
BUILD_DIR := /tmp/morse-code-sketch/build
BAUD      := 115200

SOURCES := firmware/src/main.cpp firmware/src/ui.cpp firmware/src/ui.h \
           firmware/src/morse.cpp firmware/src/morse.h firmware/src/config.h \
           firmware/src/net.cpp firmware/src/net.h

.PHONY: help setup build upload upload-dad upload-emma monitor check preview sim sim-dump clean stage secrets \
        guard-not-root worker-dev worker-test worker-deploy worker-secrets worker-logs worker-login test

# Never run build/upload as root: arduino-cli resolves the esp32 core from the
# invoking user's ~/.arduino15, and root-owned staged files break later builds.
guard-not-root:
	@if [ "$$(id -u)" -eq 0 ]; then \
	  echo "Don't run make with sudo — the esp32 core is installed per-user in ~/.arduino15."; \
	  echo "Serial access needs the uucp/dialout group, not root."; \
	  exit 1; \
	fi

help:
	@echo "Targets:"
	@echo "  make setup                       npm install (worker) + copy .env.example"
	@echo "  make upload [DEVICE=dad|emma] [PORT=/dev/ttyUSB0]  regen secrets.h, compile + flash"
	@echo "  make upload-dad [PORT=...]     shortcut for DEVICE=dad"
	@echo "  make upload-emma [PORT=...]    shortcut for DEVICE=emma"
	@echo "  make build [DEVICE=dad|emma]   compile-check only (no device needed)"
	@echo "  make secrets DEVICE=dad       generate firmware/src/secrets.h from .env"
	@echo "  make monitor [PORT=...]          serial log @ 115200 baud"
	@echo "  make check                       verify arduino-cli core/libs + serial port"
	@echo "  make preview                     render UI frames to a PNG (host-side)"
	@echo "  make sim [PORT=8077]             ESP32 display simulator at http://localhost:8077/"
	@echo "                                   (talks to the local Worker; run make worker-dev first)"
	@echo "  make sim-dump                    headless simulator screenshots to tools/sim/out"
	@echo "  make worker-dev      local Worker dev server (port 8787)"
	@echo "  make worker-test     worker unit + integration tests"
	@echo "  make worker-deploy   deploy the Worker (custom domain from MORSE_SERVER_URL)"
	@echo "  make worker-secrets  push MORSE_TOKEN_DAD/EMMA from .env as Worker secrets"
	@echo "  make worker-logs     tail the deployed Worker logs"
	@echo "  make worker-login    log wrangler into your Cloudflare account"
	@echo "  make test            worker unit + integration tests"
	@echo "  make clean                       remove build + preview output"
	@echo ""
	@echo "Current PORT: $(PORT)"

setup:
	cp -n .env.example .env || true
	cp -n worker/.dev.vars.example worker/.dev.vars || true
	npm install --prefix worker
	@echo "setup done — wrangler $$(./worker/node_modules/.bin/wrangler --version 2>/dev/null || echo '(not found)') is available via make worker-* / cd worker && npx wrangler"
	@echo "next: fill in .env and worker/.dev.vars, then: make worker-dev"

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
# Always restages (phony): `make upload DEVICE=emma` regenerates secrets.h
# first, and the staged copy must follow even when the .cpp files are untouched.
stage:
	@rm -rf /tmp/morse-code-sketch
	@mkdir -p $(SKETCH)
	@cp firmware/src/main.cpp firmware/src/ui.cpp firmware/src/ui.h \
	    firmware/src/morse.cpp firmware/src/morse.h firmware/src/config.h \
	    firmware/src/net.cpp firmware/src/net.h $(SKETCH)/
	@if [ -f firmware/src/secrets.h ]; then cp firmware/src/secrets.h $(SKETCH)/; fi
	@printf '// morse-code sketch - real sources live in main.cpp / ui.cpp / morse.cpp\n' \
	    > $(SKETCH)/morse-code.ino

build: guard-not-root secrets stage
	@if [ ! -f firmware/src/secrets.h ]; then \
	  echo "firmware/src/secrets.h missing. Run: make secrets DEVICE=$(DEVICE)"; \
	  exit 1; \
	fi
	arduino-cli compile --fqbn $(FQBN) --output-dir $(BUILD_DIR) $(SKETCH)

upload: guard-not-root secrets stage
	@if [ ! -e "$(PORT)" ]; then \
	  echo "No serial port at $(PORT). Plug in via USB data cable, then: ls /dev/ttyUSB* /dev/ttyACM*"; \
	  exit 1; \
	fi
	arduino-cli compile --fqbn $(FQBN) --output-dir $(BUILD_DIR) $(SKETCH)
	arduino-cli upload -p $(PORT) --fqbn $(FQBN) --input-dir $(BUILD_DIR) $(SKETCH)
	@echo "Flashed $(PORT) as $(DEVICE). Run 'make monitor PORT=$(PORT)' to verify the boot log."

upload-dad:
	@$(MAKE) --no-print-directory upload DEVICE=dad PORT=$(PORT)

upload-emma:
	@$(MAKE) --no-print-directory upload DEVICE=emma PORT=$(PORT)

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

worker-dev:
	npm run dev --prefix worker

worker-test:
	npm test --prefix worker
	npm run test:workers --prefix worker

worker-deploy:
	bash scripts/worker-deploy.sh

# Push the tokens straight from .env, so the deployed secrets cannot drift
# from the values the firmware/sim use (a copy-paste slip that set both to the
# same value made every request fail with 500 config_error). Values are piped
# to wrangler on stdin and never echoed.
worker-secrets:
	@set -a; . ./.env; set +a; \
	: "$${MORSE_TOKEN_DAD:?MORSE_TOKEN_DAD is missing from .env}"; \
	: "$${MORSE_TOKEN_EMMA:?MORSE_TOKEN_EMMA is missing from .env}"; \
	if [ "$$MORSE_TOKEN_DAD" = "$$MORSE_TOKEN_EMMA" ]; then \
	  echo "worker-secrets: MORSE_TOKEN_DAD and MORSE_TOKEN_EMMA must differ in .env"; \
	  exit 1; \
	fi; \
	if [ $${#MORSE_TOKEN_DAD} -lt 16 ] || [ $${#MORSE_TOKEN_EMMA} -lt 16 ]; then \
	  echo "worker-secrets: tokens must be at least 16 characters (use: openssl rand -hex 24)"; \
	  exit 1; \
	fi; \
	cd worker && \
	printf '%s' "$$MORSE_TOKEN_DAD" | npx wrangler secret put MORSE_TOKEN_DAD && \
	printf '%s' "$$MORSE_TOKEN_EMMA" | npx wrangler secret put MORSE_TOKEN_EMMA

worker-logs:
	npm run tail --prefix worker

worker-login:
	cd worker && npx wrangler login

test:
	npm test --prefix worker
	npm run test:workers --prefix worker

clean:
	rm -rf /tmp/morse-code-sketch tools/preview/build tools/preview/out tools/sim/build tools/sim/out
