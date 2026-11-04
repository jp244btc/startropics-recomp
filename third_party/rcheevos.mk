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
# (its HTTP goes through frontend/http.c, set up by the Makefile)
ifneq ($(WINDOWS),1)
  RCHEEVOS_LIBS := -lpthread
endif
