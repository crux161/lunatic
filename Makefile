CC ?= clang
CFLAGS = -Wall -Wextra -std=c11

# Default to VULKAN_SDK if set, otherwise fall back to /usr/local
VULKAN_PREFIX ?= $(if $(VULKAN_SDK),$(VULKAN_SDK),/usr/local)

CPPFLAGS = -I$(VULKAN_PREFIX)/include
LDFLAGS = -L$(VULKAN_PREFIX)/lib -Wl,-rpath,$(VULKAN_PREFIX)/lib
LDLIBS = -lvulkan

TARGET = vk_version
SRC = vk_version.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean

