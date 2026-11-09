CXX = g++
MODE = debug

ifeq ($(MODE),release)
CXXFLAGS = -std=c++17 -02 -DNDEBUG -Wall -Wno-deprecated-declarations
else
CXXFLAGS = -std=c++17 -g -O0 -Wall -Wno-deprecated-declarations
endif

CXXFLAGS += -MMD -MP -pthread

VCPKG_ROOT = $(HOME)/vcpkg
VCPKG_TRIPLET = x64-linux
VCPKG_PREFIX = $(CURDIR)/vcpkg_installed/$(VCPKG_TRIPLET)

# 若修改览 VCPKG_TRIPLET 或 安装位置，需要同步修改此处
RPATH_FROM_BUILD = ../vcpkg_installed/$(VCPKG_TRIPLET)/lib

MYSQL_LIB = $(if $(wildcard $(VCPKG_PREFIX)/lib/libmysqlclient.*),mysqlclient,mysql)

# 头文件搜索路径（将 src 加入，并保留你原来的子目录搜索）
INCLUDES = -Isrc \
           -Isrc/global \
           -Isrc/network \
           -Isrc/sql \
           -Isrc/thread \
           -Isrc/logger \
           -Isrc/redis \
           -Ithird_party \
           -I$(VCPKG_PREFIX)/include

# 链接库
LDFLAGS = -L$(VCPKG_PREFIX)/lib

# 链接库
LIBS = -lminiocpp -lcurlpp -lINIReader -linih -lcurl \
       -lpugixml -levent -levent_pthreads -lsodium \
       -lredis++ -lhiredis \
       -l$(MYSQL_LIB) \
       -lssl -lcrypto -lz -lzstd \
       -lpthread -ldl

# 输出目录和目标
BUILD_DIR = build
TARGET = $(BUILD_DIR)/ChatServer

# 自动搜索 src 下所有的 cpp 文件
SRCS = $(shell find src -name '*.cpp')

# 将 src/xxx.cpp 映射为 build/xxx.o
OBJS = $(patsubst src/%.cpp, $(BUILD_DIR)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

# 默认目标
all: $(TARGET)

# 链接
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

# 编译规则：src/xxx.cpp -> build/xxx.o
$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

deps:
	$(VCPKG_ROOT)/vcpkg install --triplet $(VCPKG_TRIPLET) \
		--x-install-root=$(CURDIR)/vcpkg_installed

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean all

push:
	git push origin master && git push github master

commit:
	git add . && git commit

-include $(DEPS)

.PHONY: all clean rebuild deps push commit
