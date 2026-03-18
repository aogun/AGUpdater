# AGUpdater 设计文档

## 1. 系统概述

AGUpdater 是一个自动更新系统，由以下组件构成：

| 组件 | 类型 | 说明 |
|------|------|------|
| ag-server | 可执行文件 | HTTP(S) 服务器，提供管理 Web 界面和客户端 REST API |
| ag-update-lib | 静态库 (.a) | C++ 更新库，供第三方程序源码集成 |
| ag-manager | 可执行文件 | 版本管理工具（GUI），查看/下载/安装版本 |
| ag-updater | 可执行文件 | 独立更新程序，执行解压覆盖操作 |

**技术栈：**
- 语言：C++11 / C
- 构建：CMake
- 编译器：Windows MinGW (MSYS2 mingw64)
- 服务器前端：HTML/CSS/JavaScript/Vue3
- 数据库：SQLite
- 通信协议：HTTPS (RESTful API)

---

## 2. 系统架构

```
┌─────────────────────────────────────────────────────┐
│                   ag-server                         │
│  ┌──────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │ Web Admin │  │ Admin API    │  │ Client API   │  │
│  │ (Vue3)   │  │ (需登录)     │  │ (HMAC校验)   │  │
│  └──────────┘  └──────┬───────┘  └──────┬────────┘  │
│                       │                 │            │
│                ┌──────┴─────────────────┴──────┐     │
│                │        业务逻辑层              │     │
│                └──────────────┬────────────────┘     │
│                        ┌─────┴─────┐                 │
│                        │  SQLite   │                 │
│                        └───────────┘                 │
└─────────────────────────────────────────────────────┘
        ▲ HTTPS                    ▲ HTTPS
        │                          │
┌───────┴───────┐          ┌───────┴──────────┐
│  Web Browser  │          │  客户端程序       │
│  (管理员)     │          │                  │
└───────────────┘          │  ┌────────────┐  │
                           │  │ag-update-lib│  │
                           │  └─────┬──────┘  │
                           │        │ 调用     │
                           │  ┌─────┴──────┐  │
                           │  │ ag-updater  │  │
                           │  └────────────┘  │
                           │                  │
                           │  ┌────────────┐  │
                           │  │ ag-manager  │──┘
                           │  └────────────┘
                           └──────────────────┘
```

---

## 3. 服务器设计

### 3.1 配置文件

服务器启动时读取配置文件 `config.json`：

```json
{
  "host": "0.0.0.0",
  "port": 8443,
  "tls": {
    "cert_file": "server.crt",
    "key_file": "server.key"
  },
  "admin": {
    "username": "admin",
    "password": "hashed_password"
  },
  "secret": "shared_hmac_secret",
  "storage_dir": "./packages",
  "db_path": "./agupdate.db",
  "max_upload_size_mb": 100
}
```

### 3.2 数据库设计

使用 SQLite，包含以下表：

**versions 表** — 版本信息

| 字段 | 类型 | 约束 | 说明 |
|------|------|------|------|
| id | INTEGER | PRIMARY KEY AUTOINCREMENT | 自增主键 |
| version | TEXT | UNIQUE NOT NULL | 语义化版本号 (如 "1.2.3") |
| description | TEXT | NOT NULL | 版本描述 |
| file_name | TEXT | NOT NULL | 存储的文件名 |
| file_size | INTEGER | NOT NULL | 文件大小 (bytes) |
| file_sha256 | TEXT | NOT NULL | 文件 SHA256 校验值 |
| download_count | INTEGER | DEFAULT 0 | 下载次数 |
| created_at | TEXT | NOT NULL | 上传时间 (ISO 8601) |

**download_logs 表** — 下载日志

| 字段 | 类型 | 约束 | 说明 |
|------|------|------|------|
| id | INTEGER | PRIMARY KEY AUTOINCREMENT | 自增主键 |
| version_id | INTEGER | FOREIGN KEY → versions.id | 版本 ID |
| device_id | TEXT | NOT NULL | 设备标识 |
| ip_address | TEXT | NOT NULL | 下载来源 IP |
| downloaded_at | TEXT | NOT NULL | 下载时间 (ISO 8601) |

**建表 SQL：**

```sql
CREATE TABLE IF NOT EXISTS versions (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    version       TEXT    UNIQUE NOT NULL,
    description   TEXT    NOT NULL,
    file_name     TEXT    NOT NULL,
    file_size     INTEGER NOT NULL,
    file_sha256   TEXT    NOT NULL,
    download_count INTEGER DEFAULT 0,
    created_at    TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

CREATE TABLE IF NOT EXISTS download_logs (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    version_id    INTEGER NOT NULL,
    device_id     TEXT    NOT NULL,
    ip_address    TEXT    NOT NULL,
    downloaded_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    FOREIGN KEY (version_id) REFERENCES versions(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_download_logs_version ON download_logs(version_id);
```

### 3.3 API 设计

#### 3.3.1 通用约定

- 基础路径：`/api/v1`
- 请求/响应格式：JSON（文件上传/下载除外）
- 统一响应格式：

```json
{
  "code": 0,
  "message": "success",
  "data": null
}
```

- 错误码定义：

| code | 含义 |
|------|------|
| 0 | 成功 |
| 1001 | 参数错误 |
| 1002 | 版本号已存在 |
| 1003 | 版本号不存在 |
| 1004 | 文件格式错误 |
| 1005 | 文件大小超限 |
| 2001 | 未登录/登录过期 |
| 2002 | 客户端校验失败 |
| 3001 | 服务器内部错误 |

#### 3.3.2 管理接口（需登录）

管理接口使用 Session/Cookie 或 Token 方式进行认证。

**POST /api/v1/admin/login** — 管理员登录

请求：
```json
{
  "username": "admin",
  "password": "password"
}
```

响应：
```json
{
  "code": 0,
  "message": "success",
  "data": {
    "token": "jwt_or_session_token"
  }
}
```

---

**POST /api/v1/admin/versions** — 上传新版本

请求：`multipart/form-data`
- `version`: 版本号 (string)
- `description`: 版本描述 (string)
- `file`: zip 压缩包文件

服务器处理流程：
1. 校验版本号格式（语义化版本号）
2. 检查版本号是否已存在
3. 校验文件格式（zip magic number: `50 4B 03 04`）
4. 校验文件大小 ≤ 100MB
5. 计算文件 SHA256
6. 保存文件到 `storage_dir/<version>.zip`
7. 写入数据库

响应：
```json
{
  "code": 0,
  "message": "success",
  "data": {
    "id": 1,
    "version": "1.0.0",
    "file_sha256": "abc123..."
  }
}
```

---

**GET /api/v1/admin/versions?page=1&page_size=20** — 查询版本列表

响应：
```json
{
  "code": 0,
  "message": "success",
  "data": {
    "total": 50,
    "page": 1,
    "page_size": 20,
    "items": [
      {
        "id": 1,
        "version": "1.0.0",
        "description": "初始版本",
        "file_name": "1.0.0.zip",
        "file_size": 10485760,
        "file_sha256": "abc123...",
        "download_count": 42,
        "created_at": "2026-03-01T10:00:00Z"
      }
    ]
  }
}
```

---

**PUT /api/v1/admin/versions/{version}** — 修改版本信息

请求：
```json
{
  "description": "更新后的描述"
}
```

---

**DELETE /api/v1/admin/versions/{version}** — 删除指定版本

服务器同时删除数据库记录和存储的 zip 文件。

---

**DELETE /api/v1/admin/versions** — 批量删除版本

请求：
```json
{
  "versions": ["1.0.0", "1.0.1"]
}
```

---

**GET /api/v1/admin/versions/{version}/downloads?page=1&page_size=20** — 查询版本下载统计

响应：
```json
{
  "code": 0,
  "message": "success",
  "data": {
    "total": 100,
    "page": 1,
    "page_size": 20,
    "items": [
      {
        "device_id": "DESKTOP-ABC123",
        "ip_address": "192.168.1.100",
        "downloaded_at": "2026-03-15T14:30:00Z"
      }
    ]
  }
}
```

#### 3.3.3 客户端接口（HMAC 校验）

所有客户端接口请求须携带 `X-Auth` 请求头，内容为身份校验 JSON 字符串（参见第 6 节）。

**GET /api/v1/client/updates?current_version=1.0.0** — 检查更新

响应（有更新）：
```json
{
  "code": 0,
  "message": "success",
  "data": {
    "has_update": true,
    "updates": [
      {
        "version": "1.1.0",
        "description": "新增功能 X",
        "file_size": 5242880,
        "file_sha256": "def456...",
        "created_at": "2026-03-10T08:00:00Z"
      },
      {
        "version": "1.2.0",
        "description": "新增功能 Y",
        "file_size": 6291456,
        "file_sha256": "ghi789...",
        "created_at": "2026-03-15T08:00:00Z"
      }
    ]
  }
}
```

响应（无更新）：
```json
{
  "code": 0,
  "message": "no update available",
  "data": {
    "has_update": false,
    "updates": []
  }
}
```

---

**GET /api/v1/client/versions?page=1&page_size=20** — 查询所有版本信息

响应格式同管理端版本列表，但不含 `download_count` 字段。

---

**GET /api/v1/client/download/{version}** — 下载指定版本文件

- 请求头须携带 `X-Auth` 身份校验
- 成功返回 zip 文件流，Content-Type: `application/octet-stream`
- 响应头包含 `Content-Disposition: attachment; filename="1.2.0.zip"`
- 下载成功后服务器更新 `download_count` 并记录下载日志
- 版本不存在返回 JSON 错误响应

### 3.4 Web 管理前端

使用 Vue3 单页应用，通过服务器静态文件托管。

**页面结构：**

| 页面 | 路径 | 功能 |
|------|------|------|
| 登录页 | /login | 用户名密码登录 |
| 版本管理 | /versions | 版本列表、上传、删除、编辑 |
| 下载统计 | /versions/:id/stats | 查看某版本的下载记录 |

---

## 4. 客户端库设计 (ag-update-lib)

### 4.1 编译选项

通过 CMake 定义以下编译选项，在 `add_subdirectory` 集成时由上层项目设置：

```cmake
set(AG_UPDATER_NAME "ag-updater" CACHE STRING "更新程序可执行文件名")
set(AG_SERVER_URL "https://localhost:8443" CACHE STRING "服务器地址")
set(AG_SECRET "default_secret" CACHE STRING "HMAC 校验密钥")
```

编译时通过 `add_definitions` 将这些值注入代码：

```cmake
add_definitions(
    -DAG_UPDATER_NAME="${AG_UPDATER_NAME}"
    -DAG_SERVER_URL="${AG_SERVER_URL}"
    -DAG_SECRET="${AG_SECRET}"
)
```

### 4.2 数据结构

```c
/* 版本信息 */
typedef struct ag_version_info {
    char version[32];         /* 版本号，如 "1.2.3" */
    char description[1024];   /* 版本描述 */
    char download_url[512];   /* 下载 URL */
    int64_t file_size;        /* 文件大小 (bytes) */
    char file_sha256[65];     /* SHA256 hex string (64 chars + '\0') */
    char created_at[32];      /* 上传时间 ISO 8601 */
} ag_version_info_t;

/* 错误码 */
typedef enum ag_error {
    AG_OK = 0,                 /* 成功 */
    AG_ERR_NETWORK = -1,       /* 网络错误 */
    AG_ERR_AUTH = -2,          /* 校验失败 */
    AG_ERR_NOT_FOUND = -3,     /* 版本不存在 */
    AG_ERR_CHECKSUM = -4,      /* SHA256 校验失败 */
    AG_ERR_IO = -5,            /* 文件 IO 错误 */
    AG_ERR_INTERNAL = -6,      /* 内部错误 */
    AG_ERR_NO_UPDATE = -7      /* 无可用更新 */
} ag_error_t;

/* 下载进度信息 */
typedef struct ag_download_progress {
    int64_t total_bytes;       /* 总字节数 */
    int64_t downloaded_bytes;  /* 已下载字节数 */
    int percent;               /* 进度百分比 0-100 */
} ag_download_progress_t;
```

### 4.3 API 接口

```c
/**
 * 检查更新回调函数
 * @param error      错误码，AG_OK 表示成功
 * @param info       版本信息，无更新时为 NULL，调用者不得释放此指针
 * @param update_count 可用更新数量
 * @param user_data  用户自定义数据
 */
typedef void (*ag_check_callback)(
    ag_error_t error,
    const ag_version_info_t *info,
    int update_count,
    void *user_data
);

/**
 * 下载进度/完成回调函数
 * @param error      错误码，AG_OK 表示成功或下载中
 * @param progress   下载进度信息
 * @param file_path  下载完成后的文件路径，下载中或失败时为 NULL
 * @param user_data  用户自定义数据
 */
typedef void (*ag_download_callback)(
    ag_error_t error,
    const ag_download_progress_t *progress,
    const char *file_path,
    void *user_data
);

/**
 * 异步检查更新
 * 后台线程向服务器查询 current_version 之后的所有新版本。
 * 结果通过 callback 返回，callback 在后台线程中调用。
 *
 * @param app_name         程序名称
 * @param current_version  当前版本号
 * @param callback         回调函数
 * @param user_data        传递给回调的用户数据
 * @return AG_OK 表示请求已提交，其他表示参数错误
 */
ag_error_t ag_check_update(
    const char *app_name,
    const char *current_version,
    ag_check_callback callback,
    void *user_data
);

/**
 * 异步下载更新
 * 后台线程下载指定版本文件到系统临时目录。
 * 下载过程中每秒通过 callback 报告进度；
 * 下载完成后校验 SHA256，校验通过返回文件路径，校验失败删除文件并返回错误。
 *
 * @param info       版本信息（由 ag_check_update 回调获得）
 * @param callback   进度/完成回调函数
 * @param user_data  传递给回调的用户数据
 * @return AG_OK 表示请求已提交，其他表示参数错误
 */
ag_error_t ag_download_update(
    const ag_version_info_t *info,
    ag_download_callback callback,
    void *user_data
);

/**
 * 启动更新程序执行安装
 * 启动 ag-updater 可执行文件，传入 zip 文件路径和目标目录，
 * 然后当前进程应退出以允许文件覆盖。
 *
 * @param zip_path    下载的 zip 文件路径
 * @param target_dir  目标安装目录
 * @return AG_OK 表示更新程序已启动，其他表示错误
 */
ag_error_t ag_apply_update(
    const char *zip_path,
    const char *target_dir
);
```

### 4.4 库使用流程

```
第三方程序                    ag-update-lib               ag-server
    │                              │                          │
    │  ag_check_update(v1.0.0)     │                          │
    ├─────────────────────────────►│   GET /client/updates     │
    │                              ├─────────────────────────►│
    │                              │   返回更新列表            │
    │                              │◄─────────────────────────┤
    │  callback(OK, info)          │                          │
    │◄─────────────────────────────┤                          │
    │                              │                          │
    │  ag_download_update(info)    │                          │
    ├─────────────────────────────►│   GET /client/download    │
    │                              ├─────────────────────────►│
    │  callback(progress)          │   文件流                  │
    │◄─────────────────────────────┤◄─────────────────────────┤
    │  callback(OK, file_path)     │                          │
    │◄─────────────────────────────┤                          │
    │                              │                          │
    │  ag_apply_update(zip, dir)   │                          │
    ├─────────────────────────────►│                          │
    │  启动 ag-updater             │                          │
    │  进程退出                    │                          │
```

---

## 5. 更新程序设计 (ag-updater)

### 5.1 功能

独立可执行文件，由 ag-update-lib 或 ag-manager 调用，负责解压 zip 文件并覆盖更新目标目录中的文件。

### 5.2 命令行参数

```
ag-updater --zip <zip_path> --target <target_dir> [--app-name <name>] [--version <ver>]
```

| 参数 | 必填 | 说明 |
|------|------|------|
| --zip | 是 | zip 文件路径 |
| --target | 是 | 目标安装目录 |
| --app-name | 否 | 程序名称（用于定位 zip 内子目录） |
| --version | 否 | 版本号（用于定位 zip 内子目录） |

### 5.3 执行流程

```
1. 解析命令行参数
2. 验证 zip 文件存在且可读
3. 打开 zip 文件，定位根目录 "<app_name><version>/"
4. 遍历 zip 内文件：
   a. 跳过更新程序自身（ag-updater.exe）
   b. 将文件解压到 target_dir，覆盖已有文件
   c. 保持目录结构
5. 删除临时 zip 文件
6. 退出，返回 0 表示成功，非 0 表示失败
```

### 5.4 注意事项

- 更新程序不能覆盖自身，需跳过自身文件名
- 需要处理文件被占用的情况（等待重试或提示用户关闭相关程序）
- 解压过程中若失败，应尽量保证原文件不被破坏（可先解压到临时目录再整体移动）

---

## 6. 版本管理工具设计 (ag-manager)

### 6.1 功能

GUI 工具，提供界面供用户浏览服务器版本并选择下载安装。

### 6.2 界面设计

```
┌──────────────────────────────────────────────────┐
│  AGUpdater 版本管理                               │
├──────────────────────────────────────────────────┤
│  ┌────────────────────────────────────────────┐  │
│  │ 版本列表                              刷新  │  │
│  ├──────┬──────────┬──────────┬────────┬──────┤  │
│  │ 版本  │ 描述     │ 大小     │ 日期   │ 操作 │  │
│  ├──────┼──────────┼──────────┼────────┼──────┤  │
│  │1.2.0 │功能更新Y │ 6.0 MB  │03-15   │[下载]│  │
│  │1.1.0 │功能更新X │ 5.0 MB  │03-10   │[下载]│  │
│  │1.0.0 │初始版本  │ 4.5 MB  │03-01   │[下载]│  │
│  └──────┴──────────┴──────────┴────────┴──────┘  │
│                                                  │
│  ┌────────────────────────────────────────────┐  │
│  │ 版本详情 / 下载进度                         │  │
│  │                                            │  │
│  │ 版本: 1.2.0                                │  │
│  │ 描述: 功能更新Y                             │  │
│  │ SHA256: ghi789...                          │  │
│  │                                            │  │
│  │ [████████████████░░░░] 80%  4.8/6.0 MB     │  │
│  └────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────┘
```

### 6.3 功能流程

1. 启动时调用 `GET /api/v1/client/versions` 获取版本列表
2. 用户点击"下载"按钮，调用 `ag_download_update` 开始下载
3. 下载过程中显示进度条，每秒更新
4. 下载完成后提示用户是否立即安装
5. 用户确认安装后调用 `ag_apply_update` 启动更新程序

---

## 7. 身份校验设计

### 7.1 校验流程

```
客户端                                      服务器
  │                                           │
  │  1. 获取 device_id (机器名)               │
  │  2. 获取当前 timestamp (秒)               │
  │  3. 生成 16 字符 nonce                    │
  │  4. 计算 sign = HMAC-SHA256(              │
  │       key=secret,                         │
  │       msg=device_id+timestamp+nonce)      │
  │  5. 构建 JSON                              │
  │                                           │
  │  X-Auth: {"device_id":"...",              │
  │           "timestamp":...,                │
  │           "nonce":"...",                  │
  │           "sign":"..."}                   │
  ├──────────────────────────────────────────►│
  │                                           │  6. 检查 timestamp 是否在 ±10 分钟内
  │                                           │  7. 检查 nonce 是否已使用（最近 100 个）
  │                                           │  8. 用同样方式计算 HMAC，比较 sign
  │                                           │  9. 校验通过则处理请求
  │                          响应              │
  │◄──────────────────────────────────────────┤
```

### 7.2 HMAC 计算

- 算法：HMAC-SHA256
- 密钥：共享 secret
- 消息：`device_id` + `timestamp`(字符串) + `nonce` 拼接
- 输出：hex 编码的 HMAC 值

### 7.3 服务器端校验规则

1. **时间戳校验**：`|server_time - timestamp| ≤ 600`（10 分钟）
2. **Nonce 校验**：维护最近 100 个已使用 nonce 的集合（内存中），拒绝重复 nonce
3. **签名校验**：使用相同 secret 和算法重新计算 HMAC，与客户端 sign 比较
4. 三项校验全部通过才处理请求，任一失败返回 code 2002

---

## 8. 版本号规范

### 8.1 格式

语义化版本号 (Semantic Versioning)：`MAJOR.MINOR.PATCH`

- `MAJOR` — 重大更新，不兼容的 API 变更
- `MINOR` — 功能更新，向下兼容
- `PATCH` — 修复 bug，向下兼容

示例：`1.0.0`, `2.1.3`, `10.0.1`

### 8.2 版本比较

按 MAJOR → MINOR → PATCH 依次比较数值大小：

```
1.0.0 < 1.0.1 < 1.1.0 < 2.0.0
```

服务器在查询"指定版本之后的更新"时，按此规则排序并筛选所有比 `current_version` 更大的版本。

### 8.3 版本号正则校验

```
^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$
```

---

## 9. 项目目录结构

```
AGUpdater/
├── CMakeLists.txt              # 顶层 CMake
├── design/
│   ├── requirement.md          # 需求文档
│   └── design.md               # 设计文档（本文件）
├── server/
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp            # 服务器入口
│   │   ├── http_server.h/cpp   # HTTP(S) 服务器
│   │   ├── admin_api.h/cpp     # 管理接口
│   │   ├── client_api.h/cpp    # 客户端接口
│   │   ├── auth.h/cpp          # 认证与 HMAC 校验
│   │   ├── database.h/cpp      # SQLite 数据库操作
│   │   ├── config.h/cpp        # 配置文件解析
│   │   └── version_util.h/cpp  # 版本号解析与比较
│   └── web/                    # Vue3 前端资源
│       ├── index.html
│       └── ...
├── client/
│   ├── CMakeLists.txt
│   ├── lib/
│   │   ├── CMakeLists.txt      # 静态库构建
│   │   ├── include/
│   │   │   └── ag_updater.h    # 公开头文件
│   │   └── src/
│   │       ├── ag_updater.cpp  # 库实现
│   │       ├── http_client.h/cpp  # HTTPS 请求
│   │       ├── auth.h/cpp      # HMAC 校验生成
│   │       └── version_util.h/cpp
│   ├── manager/
│   │   ├── CMakeLists.txt      # 版本管理工具构建
│   │   └── src/
│   │       └── main.cpp
│   └── updater/
│       ├── CMakeLists.txt      # 更新程序构建
│       └── src/
│           └── main.cpp
└── third_party/                # 第三方依赖
    ├── sqlite3/
    ├── minizip/                # zip 解压
    └── ...
```

---

## 10. 第三方依赖

| 库 | 用途 | 使用方 |
|----|------|--------|
| SQLite3 | 数据库 | 服务器 |
| OpenSSL / mbedTLS | HTTPS, HMAC-SHA256, SHA256 | 服务器 + 客户端 |
| minizip / miniz | zip 解压 | 更新程序 |
| nlohmann/json 或 cJSON | JSON 解析 | 服务器 + 客户端 |
| cpp-httplib 或同类 | HTTP(S) 服务器/客户端 | 服务器 + 客户端 |

---

## 11. 第三方程序集成示例

```cmake
# 第三方程序的 CMakeLists.txt
set(AG_UPDATER_NAME "my-updater")
set(AG_SERVER_URL "https://update.example.com:8443")
set(AG_SECRET "my_shared_secret")

add_subdirectory(path/to/AGUpdater/client)

target_link_libraries(my_app PRIVATE ag-update-lib)
```

```cpp
// 第三方程序中调用
#include "ag_updater.h"

void on_check_result(ag_error_t err, const ag_version_info_t *info,
                     int count, void *user_data) {
    if (err == AG_OK && info != NULL) {
        printf("发现新版本: %s\n", info->version);
        // 可调用 ag_download_update 下载
    }
}

int main() {
    ag_check_update("MyApp", "1.0.0", on_check_result, NULL);
    // ... 程序继续运行
}
```
