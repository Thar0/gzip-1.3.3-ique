TARGET := gzip
BUILD_DIR := build

AR := ar
CC := gcc

CFLAGS := -O2 -g -DDEBUG -ffunction-sections -fdata-sections

SRC_DIRS := $(shell find src -type d)
C_FILES := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
O_FILES := $(foreach f,$(C_FILES),$(BUILD_DIR)/$(f:.c=.o))

$(shell mkdir -p $(foreach dir,$(SRC_DIRS),$(BUILD_DIR)/$(dir)))

.PHONY: all clean

all: $(TARGET)

clean:
	$(RM) -rf $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

$(TARGET): $(O_FILES)
	$(CC) -Wl,--gc-sections -Wl,--print-gc-sections $^ -o $@
#	$(AR) rcs $@ $^
