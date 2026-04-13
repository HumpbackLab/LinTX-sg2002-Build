SHELL = /bin/bash
ifeq ($(PARAM_FILE), )
	PARAM_FILE:=../../Makefile.param
	include $(PARAM_FILE)
endif
include ../sample.mk

SDIR = $(CURDIR)
SRCS = $(wildcard $(SDIR)/../venc/src/*.c) $(wildcard $(SDIR)/../vdec/src/*.c)
APP_SRCS = $(SDIR)/sample_vdec_bind_vo.c
VDEC_INC = ../../modules/vdec
SAMPLE_VENC_INC = $(SDIR)/../venc/include
SAMPLE_VDEC_INC = $(SDIR)/../vdec/include

INCS = -I$(MW_INC) -I$(ISP_INC) -I$(COMM_INC) -I$(SAMPLE_VDEC_INC) -I$(SAMPLE_VENC_INC) -I$(VDEC_INC) -I$(KERNEL_INC) -I../common -I$(SYS_INC)

OBJS = $(SRCS:.c=.o)
APP_OBJS = $(APP_SRCS:.c=.o)
DEPS = $(SRCS:.c=.d)
APP_DEPS = $(APP_SRCS:.c=.d)

TARGET = sample_vdec_bind_vo

PKG_CONFIG_PATH = $(MW_PATH)/pkgconfig
REQUIRES = cvi_common cvi_vdec cvi_sample cvi_ive
LIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs --define-variable=mw_dir=$(MW_PATH) $(REQUIRES))

ifeq ($(MULTI_PROCESS_SUPPORT), 1)
DEFS += -DRPC_MULTI_PROCESS
LIBS += -lnanomsg
endif

ifeq ($(CLI_DEBUG_SUPPORT), 1)
DEFS += -DCLI_DEBUG_SUPPORT
LIBS += -lcli
endif

EXTRA_CFLAGS = $(DEFS) $(INCS)
EXTRA_LDFLAGS = $(LIBS) -lini -lm -lpthread

.PHONY : clean all
all: $(TARGET)

$(COMMON_DIR)/%.o: $(COMMON_DIR)/%.c
	@$(CC) $(DEPFLAGS) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ -c $<
	@echo [$(notdir $(CC))] $(notdir $@)

$(SDIR)/%.o: $(SDIR)/%.c
	@$(CC) $(DEPFLAGS) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ -c $<
	@echo [$(notdir $(CC))] $(notdir $@)

$(TARGET): $(APP_OBJS) $(OBJS) $(MW_LIB)/libvdec.a $(MW_LIB)/libvpu.a $(MW_LIB)/libsys.a
	@$(CXX) -o $@ -Wl,--start-group $(OBJS) $(APP_OBJS) -Wl,--end-group $(ELFFLAGS) $(EXTRA_LDFLAGS)
	@echo -e $(BLUE)[LINK]$(END)[$(notdir $(CXX))] $(notdir $@)

clean:
	@rm -f $(APP_OBJS) $(OBJS) $(APP_DEPS) $(DEPS) $(TARGET)

-include $(APP_DEPS) $(DEPS)
