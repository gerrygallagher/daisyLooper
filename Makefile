TARGET = looper

CPP_SOURCES = looper.cpp

LIBDAISY_DIR = ../libDaisy
DAISYSP_DIR  = ../DaisySP

C_INCLUDES += -I$(DAISYSP_DIR)/Source

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile