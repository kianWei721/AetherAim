# 贡献指南

## 首先

感谢你想为 AetherAim 做贡献！这个项目的目标是帮助有运动障碍的玩家享受 FPS 游戏。每一个 PR、Issue、反馈都在让这件事变得更好。

## 行为准则

- 尊重残障玩家 — 这是为他们做的工具
- 讨论技术方案，不讨论"算不算外挂" — 项目立场已在 README 明确
- 新手友好 — 耐心回答问题

## 贡献方式

### 报告 Bug

在 [Issues](https://github.com/你的用户名/AetherAim/issues) 创建 Bug Report，包含：

1. **环境**: Windows 版本、AetherAim 版本
2. **复现步骤**: 越具体越好
3. **预期行为** vs **实际行为**
4. **日志**: `%APPDATA%/AetherAim/` 下的 .log 文件（如有）

### 提功能建议

创建 Feature Request，描述：

1. 这个功能解决什么**实际问题**
2. 你期望的**交互方式**
3. 是否愿意自己实现（我们也欢迎不会写代码的用户提建议）

### 提交代码

```
1. Fork 仓库
2. 创建分支: git checkout -b feature/你的功能名
3. 写代码 (参考下方代码规范)
4. 运行测试: ctest --test-dir build -C Release
5. 运行基准 (确保没有性能退化):
   build\tests\Release\benchmark.exe
6. 提交 PR (附说明)
```

### 代码规范

- **C++20** 标准
- 不使用异常 (嵌入式固件中禁用)
- 不使用 RTTI
- 注释写 **为什么**，不写 **做什么**
- 新增 `.cpp` 文件需要在对应 `CMakeLists.txt` 中注册
- 性能敏感路径（Hook 回调）禁止堆分配、IO、锁

### 测试要求

- 新增滤波器功能 → `tests/test_filters.cpp` 加用例
- 新增配置项 → `tests/test_config.cpp` 加用例
- 新增画像功能 → `tests/test_profile.cpp` 加用例

### PR 检查清单

- [ ] 代码编译通过 (Release + Debug)
- [ ] `ctest` 全部通过
- [ ] `benchmark.exe` 无性能退化
- [ ] 新功能有文档说明
- [ ] 没有引入新的编译警告 (/W4)

## 项目方向

当前优先级：

| 优先级 | 方向 |
|--------|------|
| 高 | 滤波器参数自动调优 (基于校准数据) |
| 高 | Pico 固件参数持久化 (保存到 Flash) |
| 中 | Pico 固件 OLED 显示屏支持 |
| 中 | 游戏配置文件共享平台 |
| 低 | macOS/Linux 移植 |
| 低 | 多语言 i18n |

## 联系方式

- Issues: 技术讨论和 Bug 报告
- Discussions: 使用经验分享、参数调优交流

---

*再次感谢。你写的每一行代码都在帮助一个想玩游戏的残障玩家。*
