TARGET    := wmplay
SRC_DIR   := src
SRCS      := $(SRC_DIR)/main.c $(SRC_DIR)/opl3.c $(SRC_DIR)/wm_loader.c \
             $(SRC_DIR)/wm_replayer.c $(SRC_DIR)/sdl_audio.c
OBJS      := $(SRCS:.c=.o)

BUILD    ?= debug

.DEFAULT_GOAL := all

ifeq ($(BUILD),release)
  BASE_CFLAGS := -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
else
  BASE_CFLAGS := -std=c99 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
endif

SDL_CONFIG  := sdl2-config
SDL_CFLAGS  := $(shell $(SDL_CONFIG) --cflags 2>/dev/null)
SDL_LDFLAGS := $(shell $(SDL_CONFIG) --libs 2>/dev/null)

ALL_CFLAGS  := $(BASE_CFLAGS) $(SDL_CFLAGS)
ALL_LDFLAGS := $(SDL_LDFLAGS) -lm

ifeq ($(OS),Windows_NT)
  ALL_LDFLAGS += -mconsole
endif

BUILD_TMP := $(CURDIR)/.build_tmp
TMP_ENV   := TMP=$(BUILD_TMP) TEMP=$(BUILD_TMP) TMPDIR=$(BUILD_TMP)

$(BUILD_TMP):
	mkdir -p $(BUILD_TMP)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) | $(BUILD_TMP)
	$(TMP_ENV) $(CC) $(ALL_CFLAGS) -o $@ $^ $(ALL_LDFLAGS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_TMP)
	$(TMP_ENV) $(CC) $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)
	rm -rf $(BUILD_TMP)
