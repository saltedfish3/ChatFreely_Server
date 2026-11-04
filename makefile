CXX = g++
CXXFLAGS = -std=c++17 -g -O0 -Wall -Wno-deprecated-declarations

MINIO_ROOT = /home/saltedfish/vcpkg/installed/x64-linux

# 头文件搜索路径（将 src 加入，并保留你原来的子目录搜索）
INCLUDES = -Isrc \
           -Isrc/global \
           -Isrc/network \
           -Isrc/sql \
           -Isrc/thread \
           -Isrc/logger \
           -Isrc/redis \
           -Ithird_party \
           -I$(MINIO_ROOT)/include

# 链接库
LDFLAGS = -L$(MINIO_ROOT)/lib -L/usr/local/lib

# 链接库
LIBS = -lminiocpp -lcurlpp -lINIReader -linih -lcurl \
       -lpugixml -lssl -lcrypto -lz \
       -levent -lmysqlclient -lpthread -lsodium -levent_pthreads \
       -lredis++ -lhiredis \
       -ldl

# 输出目录和目标
BUILD_DIR = build
TARGET = $(BUILD_DIR)/ChatServer

# 自动搜索 src 下所有的 cpp 文件
SRCS = $(wildcard src/*.cpp) \
       $(wildcard src/network/*.cpp) \
       $(wildcard src/sql/*.cpp) \
       $(wildcard src/global/*.cpp) \
       $(wildcard src/thread/*.cpp) \
       $(wildcard src/logger/*.cpp) \
       $(wildcard src/redis/*.cpp)

# 将 src/xxx.cpp 映射为 build/xxx.o
OBJS = $(patsubst src/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))

# 默认目标
all: $(TARGET)

# 链接
$(TARGET): $(OBJS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

# 编译规则：src/xxx.cpp -> build/xxx.o
$(BUILD_DIR)/%.o: src/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

push:
	git push origin master && git push github master

commit:
	git add . && git commit

.PHONY: all clean push commit
