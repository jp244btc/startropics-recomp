# third_party/rcheevos.mk - included by the Makefile when present. Builds the vendored
# rcheevos library (RetroAchievements client, MIT licence, third_party/rcheevos/LICENSE)
# into the player; frontend/achievements.c uses it. Remove this file to build without it.
RC_DIR := third_party/rcheevos
RCHEEVOS_SRC := $(wildcard $(RC_DIR)/src/api/*.c) $(wildcard $(RC_DIR)/src/client/*.c) \
                $(wildcard $(RC_DIR)/src/hash/*.c) $(wildcard $(RC_DIR)/src/runtime/*.c) \
                $(wildcard $(RC_DIR)/src/util/*.c)
# no disc / zip / encrypted-image hashing: this player only ever loads one NES cartridge dump
RCHEEVOS_CFLAGS := -I$(RC_DIR)/include -I$(RC_DIR)/src -DRC_CLIENT_SUPPORTS_HASH \
                   -DRC_HASH_NO_DISC -DRC_HASH_NO_ENCRYPTED -DRC_HASH_NO_ZIP
ifeq ($(WINDOWS),1)
  RCHEEVOS_LIBS := -lwinhttp
else ifneq ($(shell pkg-config --exists libcurl 2>/dev/null && echo yes),)
  # libcurl's headers at build time only: the library itself is loaded at run time (dlopen), so the
  # program also starts on systems without it (RetroAchievements then reports that it is missing)
  RCHEEVOS_CFLAGS += -DRC_HTTP_CURL_DLOPEN $(shell pkg-config --cflags libcurl)
  RCHEEVOS_LIBS := -ldl -lpthread
else
  RCHEEVOS_LIBS := -lpthread
endif
