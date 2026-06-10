CXX = g++
CXXFLAGS = -std=c++17 -Wall

# 头文件搜索路径
INCLUDES = -I. \
           -I./global \
           -I./network \
           -I./sql \
           -I./third_party \
	   -I./thread \
	   -I./logger

# 链接库
LIBS = -levent -lmysqlclient -lpthread -lsodium

# 输出目录和目标
BUILD_DIR = build
TARGET = $(BUILD_DIR)/ChatServer

# 收集所有 .cpp 源文件
SRCS = ChatServer.cpp \
       network/ChatServerListener.cpp \
       network/ChatServerWorker.cpp \
       sql/SQL.cpp \
       sql/SQLPool.cpp \
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
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LIBS)

# 编译每个 .cpp，保留子目录结构
$(BUILD_DIR)/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

push:
	git push origin master && git push github master

commit:
	git add . && git commit
.PHONY: all clean
