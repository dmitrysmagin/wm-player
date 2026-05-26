export TMP := C:/Users/user/AppData/Local/Temp
export TEMP := C:/Users/user/AppData/Local/Temp
CC ?= gcc
CFLAGS  = -std=c99 -Wall -Wextra -O2 -g -MMD -MP -pipe
LDFLAGS =
LIBS    =

# SDL2 not needed for WAV rendering; optionally add via `make SDL2=1`
ifdef SDL2
SDL_CONFIG := $(shell command -v sdl2-config 2>/dev/null)
ifdef SDL_CONFIG
	SDL_CFLAGS := $(shell $(SDL_CONFIG) --cflags 2>/dev/null | sed 's/-Dmain=SDL_main//')
	SDL_LIBS   := $(shell $(SDL_CONFIG) --libs 2>/dev/null | sed 's/-lSDL2main//' | sed 's/-mwindows //')
else
	SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
	SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
endif
CFLAGS += $(SDL_CFLAGS)
LIBS   += $(SDL_LIBS)
endif

SRC_DIR = src
SRCS    = $(SRC_DIR)/main.c $(SRC_DIR)/opl3.c $(SRC_DIR)/wm_loader.c $(SRC_DIR)/wm_replayer.c
OBJS    = $(SRCS:.c=.o)
DEPS    = $(OBJS:.o=.d)

TARGET  = wmplay

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)
