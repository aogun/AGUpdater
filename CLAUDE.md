# AGCapturer - Project Rules

## 工作流程

- 每次修改完代码后，必须同步更新 `design/` 目录下对应的设计文档，反映所有变更
- 每次提交时，将 `src/version.h` 中的 `APP_VERSION_PATCH` 加 1
- 更新完成后将所有改动提交并推送到 GitHub

## 平台要求
- 项目仅支持windows平台，必须采用 Windows MinGW (MSYS2 mingw64) 编译
- 每次提交后查看 GitHub Actions 的构建状态，确保构建成功
