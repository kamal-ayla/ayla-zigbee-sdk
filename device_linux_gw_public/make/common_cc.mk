#
# Copyright 2011-2018 Ayla Networks, Inc.  All rights reserved.
#
# Use of the accompanying software is permitted only in accordance
# with and subject to the terms of the Software License Agreement
# with Ayla Networks, Inc., a copy of which can be obtained from
# Ayla Networks, Inc.
#

.PHONY: $(EXEC) $(LIB)

#
# Rule to build executable
#
ifdef EXEC
$(EXEC): $(BUILD)/$(EXEC)

$(BUILD)/$(EXEC): $(LIBDEPS) $(OBJS) $(CSTYLES) | $(BUILD)
	@echo Linking $(EXEC); $(CC) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)
endif
#
# Rule to make static and shared libraries
#
ifdef LIB
LIB_A := $(filter %.a,$(LIB))
LIB_SO := $(filter %.so \
	%.so.$(MAJOR_VERSION) \
	%.so.$(MAJOR_VERSION).$(MINOR_VERSION) \
	%.so.$(MAJOR_VERSION).$(MINOR_VERSION).$(MAINTENANCE_VERSION),$(LIB))
LIB_BUILD :=

ifneq ($(LIB_A),)
LIB_BUILD += $(BUILD)/$(LIB_A)
$(BUILD)/$(LIB_A): $(CSTYLES) $(OBJS) | $(BUILD)
	$(AR) r $@ $(OBJS)
endif

ifneq ($(LIB_SO),)
LIB_BUILD += $(BUILD)/$(LIB_SO)
$(BUILD)/$(LIB_SO): $(CSTYLES) $(OBJS) | $(BUILD)
	@echo CC $@; $(CC) -shared -fPIC -o $@ $(OBJS) -lc
endif

ifeq ($(LIB_BUILD),)
$(error Unsupported LIB type: $(LIB) ***)
endif

$(LIB): $(LIB_BUILD)
endif
#
# Rule to make dependencies files
#
$(BUILD)/%.d: %.c Makefile
	@(mkdir -p $(dir $@); $(CC) -MM $(CPPFLAGS) $(CFLAGS) $< | \
        	sed 's,\($*\)\.o[ :]*,$(BUILD)/\1.o $@: ,g' > $@) || rm -f $@

-include $(DEPS)

#
# Object file rules
#
$(BUILD)/%.o: %.c Makefile
	@echo CC $< ; mkdir -p $(dir $@) ; $(CC) -c $(CFLAGS) -o $@ $<

#
# Style check rules
#
$(BUILD)/%.cs: %.c
	$(CSTYLE) --strict --terse --summary-file --no-tree -f $< \
	&& mkdir -p $(dir $@) && touch $@

$(BUILD)/%.hcs: %.h
	$(CSTYLE) --strict --terse --summary-file --no-tree -f $< \
	&& mkdir -p $(dir $@) && touch $@

#
# Generate build directory
#
$(BUILD):
	@mkdir -p $@

.PHONY: tags.list clean
#
# Generate tag list
#
tags.list:
	find $(TAGS_DIRS) -name '*.[hc]' -print > tags.list

tags: tags.list
	ctags -L tags.list

clean:
	rm -f $(OBJS) $(DEPS) $(CSTYLES) \
		tags tags.list cscope.out
	cd $(BUILD); rm -f $(EXEC) $(LIB)
