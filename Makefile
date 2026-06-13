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

SDL_LIBS    := $(shell pkg-config --cflags --libs sdl2 2>/dev/null)
ifeq ($(SDL_LIBS),)
  SDL_LIBS  := -I/c/Users/user/msys64/ucrt64/include/SDL2 -L/c/Users/user/msys64/ucrt64/lib -lSDL2main -lSDL2
endif

ALL_CFLAGS  := $(BASE_CFLAGS) $(filter -I% -D%,$(SDL_LIBS))
ALL_LDFLAGS := $(filter -L% -l%,$(SDL_LIBS)) -lm

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
