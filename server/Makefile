# morse relay — run the FastAPI server in the background
#
#   make start              start uvicorn, wait until /health answers
#   make stop               SIGTERM, then SIGKILL after 10 s
#   make restart            stop + start
#   make status             show whether the relay is running
#
# Bind address: make start HOST=0.0.0.0 PORT=8180
# The pid file and log live in .run/ (gitignored), named after the port.

HOST    ?= 0.0.0.0
PORT    ?= 8180
UVICORN ?= .venv/bin/uvicorn
RUN_DIR := .run
PID_FILE = $(RUN_DIR)/relay-$(PORT).pid
LOG_FILE = $(RUN_DIR)/relay-$(PORT).log
HEALTH_URL = http://127.0.0.1:$(PORT)/health

# First pid listening on $(PORT), if any. The port is the source of truth: a
# pid file may be stale or belong to a relay started on another port.
PORT_PID = ss -H -ltnp 'sport = :$(PORT)' 2>/dev/null | \
           sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | head -n 1

# True when pid $1 is this relay (so we never kill an unrelated uvicorn).
IS_RELAY = tr '\0' ' ' < /proc/$(1)/cmdline 2>/dev/null | \
           grep -q 'uvicorn.*app:app'
# True when pid $1 is the relay bound to this port.
IS_ON_PORT = tr '\0' ' ' < /proc/$(1)/cmdline 2>/dev/null | \
             grep -q 'uvicorn.*app:app.*--port $(PORT)'

.PHONY: help start stop restart status

help:
	@echo "Targets:"
	@echo "  make start     start the relay on $(HOST):$(PORT) and wait for /health"
	@echo "  make stop      stop the relay on port $(PORT)"
	@echo "  make restart   stop + start"
	@echo "  make status    show pid, bind address, and health"
	@echo ""
	@echo "Override with: make start HOST=0.0.0.0 PORT=8180"

start:
	@mkdir -p $(RUN_DIR)
	@pid=$$($(PORT_PID)); \
	if [ -n "$$pid" ]; then \
	  if $(call IS_RELAY,$$pid); then \
	    echo "relay already running (pid $$pid) — $(HEALTH_URL)"; \
	    echo $$pid > $(PID_FILE); exit 0; \
	  fi; \
	  echo "port $(PORT) is already in use by pid $$pid; refusing to start"; exit 1; \
	fi; \
	rm -f $(PID_FILE); \
	if [ ! -x "$(UVICORN)" ]; then \
	  echo "Missing $(UVICORN). Run: uv sync"; exit 1; \
	fi; \
	nohup "$(UVICORN)" app:app --host $(HOST) --port $(PORT) --log-level info \
	  >> $(LOG_FILE) 2>&1 & \
	pid=$$!; echo $$pid > $(PID_FILE); \
	printf 'starting relay (pid %s) ' "$$pid"; \
	i=0; \
	while [ $$i -lt 50 ]; do \
	  if ! kill -0 "$$pid" 2>/dev/null; then \
	    echo; echo "relay exited during startup. Last log lines:"; \
	    tail -n 20 $(LOG_FILE); rm -f $(PID_FILE); exit 1; \
	  fi; \
	  if curl -fsS --max-time 1 $(HEALTH_URL) >/dev/null 2>&1; then \
	    echo "ok"; echo "listening on $(HOST):$(PORT) (log: $(LOG_FILE))"; exit 0; \
	  fi; \
	  printf '.'; sleep 0.2; i=$$((i+1)); \
	done; \
	echo; echo "relay did not answer $(HEALTH_URL) within 10 s; see $(LOG_FILE)"; \
	exit 1

stop:
	@pid=$$($(PORT_PID)); \
	if [ -z "$$pid" ]; then \
	  p=$$(cat $(PID_FILE) 2>/dev/null); \
	  if [ -n "$$p" ] && kill -0 "$$p" 2>/dev/null && $(call IS_ON_PORT,$$p); then \
	    pid=$$p; \
	  fi; \
	fi; \
	if [ -z "$$pid" ]; then \
	  echo "relay not running on port $(PORT)"; rm -f $(PID_FILE); exit 0; \
	fi; \
	if ! $(call IS_RELAY,$$pid); then \
	  echo "pid $$pid on port $(PORT) is not the relay; refusing to stop it"; exit 1; \
	fi; \
	echo "stopping relay (pid $$pid)"; \
	kill "$$pid" 2>/dev/null || true; \
	i=0; while kill -0 "$$pid" 2>/dev/null && [ $$i -lt 50 ]; do \
	  sleep 0.2; i=$$((i+1)); \
	done; \
	if kill -0 "$$pid" 2>/dev/null; then \
	  echo "still running after 10 s, sending SIGKILL"; kill -9 "$$pid" 2>/dev/null || true; \
	fi; \
	rm -f $(PID_FILE); echo "stopped"

restart:
	@$(MAKE) --no-print-directory stop
	@$(MAKE) --no-print-directory start

status:
	@pid=$$($(PORT_PID)); \
	if [ -z "$$pid" ]; then \
	  p=$$(cat $(PID_FILE) 2>/dev/null); \
	  if [ -n "$$p" ] && kill -0 "$$p" 2>/dev/null && $(call IS_ON_PORT,$$p); then \
	    pid=$$p; \
	  fi; \
	fi; \
	if [ -n "$$pid" ] && $(call IS_RELAY,$$pid); then \
	  echo "relay running (pid $$pid) on $(HOST):$(PORT)"; \
	  if curl -fsS --max-time 2 $(HEALTH_URL) >/dev/null 2>&1; then \
	    echo "health: ok"; \
	  else \
	    echo "health: not answering $(HEALTH_URL)"; exit 1; \
	  fi; \
	else \
	  echo "relay not running on port $(PORT)"; exit 1; \
	fi
