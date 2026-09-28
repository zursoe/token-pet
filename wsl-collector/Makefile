CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11 -I. -I../third_party/sqlite -I../third_party/cjson
SRC = collector.c tp_util_common.c tp_fs_posix.c scan_codex.c scan_kimi.c scan_opencode.c scan_claude.c \
      ../third_party/sqlite/sqlite3.c ../third_party/cjson/cJSON.c

pet-collector: $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lpthread -ldl -lm

clean:
	rm -f pet-collector

.PHONY: clean
