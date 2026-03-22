SHELL = /bin/bash
ifeq ($(PARAM_FILE), )
	PARAM_FILE:=../../Makefile.param
	include $(PARAM_FILE)
endif

SDIR = $(PWD)
INCS = -I$(KERNEL_INC)

TARGETS = sample_touch sample_touch_poll
ifeq ($(CONFIG_ENABLE_SDK_ASAN), y)
TARGETS = sample_touch_asan sample_touch_poll_asan
endif

EXTRA_CFLAGS = $(INCS)
EXTRA_LDFLAGS = -lm -lpthread

.PHONY : clean all
all: $(TARGETS)

$(SDIR)/%.o: $(SDIR)/%.c
	@$(CC) $(DEPFLAGS) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ -c $<
	@echo [$(notdir $(CC))] $(notdir $@)

sample_touch: $(SDIR)/sample_touch.o
	@$(CXX) -o $@ $^ $(ELFFLAGS) $(EXTRA_LDFLAGS)
	@echo -e $(BLUE)[LINK]$(END)[$(notdir $(CC))] $@

sample_touch_poll: $(SDIR)/sample_touch_poll.o
	@$(CXX) -o $@ $^ $(ELFFLAGS) $(EXTRA_LDFLAGS)
	@echo -e $(BLUE)[LINK]$(END)[$(notdir $(CC))] $@

sample_touch_asan: $(SDIR)/sample_touch.o
	@$(CXX) -o $@ $^ $(ELFFLAGS) $(EXTRA_LDFLAGS)
	@echo -e $(BLUE)[LINK]$(END)[$(notdir $(CC))] $@

sample_touch_poll_asan: $(SDIR)/sample_touch_poll.o
	@$(CXX) -o $@ $^ $(ELFFLAGS) $(EXTRA_LDFLAGS)
	@echo -e $(BLUE)[LINK]$(END)[$(notdir $(CC))] $@

clean:
	@rm -f $(SDIR)/*.o $(SDIR)/*.d $(TARGETS)

-include $(wildcard $(SDIR)/*.d)
