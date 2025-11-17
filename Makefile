CC ?= gcc
CFLAGS ?= -O3 -march=native -std=c11 -Wall -Wextra -pedantic
CFLAGS += -Iinclude
LDFLAGS ?=
LIBS ?= -lpthread -lpng -lwebp -lavif
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := ferretptimize

TEST_OBJ := tests/test_queue.o tests/test_image_ops.o
TEST_BIN := tests/run_tests
AUTOTEST_SCRIPT := tests/autotest.sh
RUNNER := scripts/run_with_browser.sh
DEPS := $(wildcard include/*.h)
NETLIFY_DIR ?= public
NETLIFY_SITE_ID ?= $(shell [ -f .netlify/state.json ] && python3 -c "import json,sys;print(json.load(open('.netlify/state.json')).get('siteId',''))" 2>/dev/null || true)

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

$(TEST_BIN): $(TEST_OBJ) src/queue.o src/image_ops.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

tests/%.o: tests/%.c
	$(CC) $(CFLAGS) -Iinclude -c -o $@ $<

$(OBJ): $(DEPS)
$(TEST_OBJ): $(DEPS)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

run: $(BIN)
	$(RUNNER)

deploy:
	@if [ -z "$(NETLIFY_SITE_ID)" ]; then echo "NETLIFY_SITE_ID missing (set it or run netlify init)"; exit 1; fi
	netlify deploy --prod --site $(NETLIFY_SITE_ID) --dir=$(NETLIFY_DIR)

deploy-temp:
	@if [ -z "$(NETLIFY_SITE_ID)" ]; then echo "NETLIFY_SITE_ID missing (set it or run netlify init)"; exit 1; fi
	netlify deploy --site $(NETLIFY_SITE_ID) --dir=$(NETLIFY_DIR)

clean:
	rm -f $(OBJ) $(BIN) $(TEST_OBJ) $(TEST_BIN)

.PHONY: all clean test autotest install run deploy deploy-temp

test: $(TEST_BIN)
	./$(TEST_BIN)

autotest: $(BIN)
	$(AUTOTEST_SCRIPT)
