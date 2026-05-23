# Contract: Process Platform Abstraction

**Feature**: `003-project-creation-wizard`
**Date**: 2026-04-15

## Overview

新增平台抽象层，提供跨平台的进程启动能力。Launcher 使用此抽象启动 Editor 子进程。

## Interface

### `Platform::Process::Launch()`

```
Launch(executablePath, arguments) -> ProcessLaunchResult
```

**Parameters**:
- `executablePath`: `std::filesystem::path` - 可执行文件的绝对路径
- `arguments`: `std::vector<std::string>` - 命令行参数列表

**Returns**: `ProcessLaunchResult`

**Behavior**:
- 启动指定可执行文件作为独立子进程
- 父进程（Launcher）不等待子进程完成（异步/分离启动）
- 子进程独立运行，父进程退出后子进程继续

### `ProcessLaunchResult`

| Field | Type | Description |
|-------|------|-------------|
| success | bool | 进程是否成功创建 |
| errorMessage | string | 失败时的错误描述（仅当 success=false 时有意义） |
| processId | uint32 | 子进程 ID（仅当 success=true 时有意义，可选） |

### `Platform::Process::FindExecutable()`

```
FindExecutable(name) -> std::optional<std::filesystem::path>
```

**Parameters**:
- `name`: `std::string` - 可执行文件名（不含路径，如 "Editor.exe"）

**Behavior**:
- 在当前可执行文件的同一目录下搜索指定名称的文件
- 返回找到的绝对路径，或 `std::nullopt`

## Platform Implementations

### Windows
- 使用 `CreateProcessA` + `DETACHED_PROCESS` 标志
- 路径含空格时在命令行中自动加引号
- 参考 `ShaderCompiler.cpp` 中已有的 `CreateProcessA` 实现

### Linux/macOS
- 使用 `fork()` + `execvp()` + `setsid()`
- 父进程不 `waitpid()`（分离模式）

## Error Scenarios

| Scenario | success | errorMessage |
|----------|---------|-------------|
| 可执行文件不存在 | false | `"Editor executable not found: <path>"` |
| 权限不足 | false | `"Permission denied: <path>"` |
| 进程创建系统调用失败 | false | `"Failed to create process: <system error>"` |

## Invariants

- 抽象层位于 `Runtime/Platform/Process/` 目录下
- 头文件 `Runtime/Platform/Process/Process.h` 不暴露平台特定实现细节
- Launcher 是此抽象的唯一消费者（v1），但设计上不限于 Launcher
