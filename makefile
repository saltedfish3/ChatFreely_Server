CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wno-deprecated-declarations

MINIO_ROOT = /home/saltedfish/vcpkg/installed/x64-linux

# 头文件搜索路径
INCLUDES = -I. \
           -I./global \
           -I./network \
           -I./sql \
           -I./third_party \
	   -I./thread \
	   -I./logger \
	   -I$(MINIO_ROOT)/include

# 链接库
LDFLAGS = -L$(MINIO_ROOT)/lib

# 链接库
LIBS = -lminiocpp -lcurlpp -lINIReader -linih -lcurl \
       -lpugixml -lssl -lcrypto -lz \
       -levent -lmysqlclient -lpthread -lsodium -levent_pthreads \
       -ldl

# 输出目录和目标
BUILD_DIR = build
TARGET = $(BUILD_DIR)/ChatServer

SRCS = ChatServer.cpp \
       network/ChatServerListener.cpp \
       network/ChatServerWorker.cpp \
       network/ChatServerUploadWorker.cpp \
       sql/SQL.cpp \
       sql/SQLPool.cpp \
       sql/GuardConnect.cpp \
       sql/SQLTransaction.cpp \
       global/GlobalVar.cpp \
       thread/ThreadPool.cpp \
       logger/Logger.cpp

# .cpp -> build/xxx.o
OBJS = $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(SRCS))

# 默认目标
all: $(TARGET)

# 链接
$(TARGET): $(OBJS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

$(BUILD_DIR)/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	find $(BUILD_DIR) -name "*.o" -delete

push:
	git push origin master && git push github master

commit:
	git add . && git commit
.PHONY: all clean
