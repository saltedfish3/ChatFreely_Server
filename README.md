# ChatServer

即时通信服务端，支持私聊、群聊、文件上传、断点续传、在线状态同步等。

## 技术栈

C++17 / libevent / MySQL / Redis / MinIO / Nginx / libsodium / jwt-cpp

## 功能

- 用户注册、登陆、JWT鉴权、Refresh Token
- 好友申请、删除、在线状态推送
- 群聊创建、邀请、申请加入、踢人、解散
- 消息发送、同步、断点续传
- 文件上传：小文件 presigned、大文件分片、图片缩略图、视频缩略图
- 多节点：Redis pub/sub 跨节点消息推送

## 快速开始

### 编译

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```
