# 项目测试入口

当前测试入口直接包含 `mediacore/src` 下的正式 DAG 头文件，不链接 FFmpeg、Qt 或真实媒体
节点。目前已有用例使用假 Source、Transform 和 Sink，重点验证线程生命周期、阻塞解除和停止顺序。

## 构建和运行

从项目根目录执行：

```bash
conan install --requires=spdlog/1.15.3 --lockfile=conan.lock \
      --output-folder=/tmp/ai-media-player-d9/generators \
      --generator=CMakeDeps --generator=CMakeToolchain \
      --build=missing -s build_type=RelWithDebInfo
cmake -S . -B /tmp/ai-media-player-d9 \
      -DMEDIA_PLAYER_BUILD_MEDIACORE=OFF \
      -DMEDIA_PLAYER_BUILD_APP=OFF \
      -DBUILD_TESTING=ON \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_TOOLCHAIN_FILE=/tmp/ai-media-player-d9/generators/conan_toolchain.cmake
cmake --build /tmp/ai-media-player-d9 --parallel 2
ctest --test-dir /tmp/ai-media-player-d9 --output-on-failure
```

也可以直接运行测试程序。省略参数时运行全部用例，或使用 `--case` 运行一个用例：

```bash
/tmp/ai-media-player-d9/tests/dag/all_tests
/tmp/ai-media-player-d9/tests/dag/all_tests \
    --case stop_while_sending
```

## 覆盖范围

| 用例 | 验证内容 |
| --- | --- |
| `eof_close_and_drain` | Source -> Transform -> Sink 的正常 EOF、关闭输出和排空 |
| `stop_while_receiving` | 空队列上的接收在 `Abort()` 后返回 |
| `stop_while_sending` | 满队列上的发送在 `Abort()` 后返回 |
| `stop_while_paused` | 暂停等待中的 Node 能被 Stop 唤醒并 Join |
| `pause_resume_pause` | 连续 Pause/Resume/Pause 的状态和线程行为 |
| `interrupt_blocking_io` | Pipeline 能调用 Node 的外部 I/O 中断回调 |
| `partial_start_rollback` | 部分线程创建失败时已启动 Node 被回滚并 Join |
| `repeated_stop` | 重复 Stop 的返回值和生命周期状态稳定 |
| `tick_exception` | `Tick()` 异常在线程边界保存，Join 后可读取 |
| `join_before_destruction` | Pipeline 释放 Node 前先完成 Join |
| `consumer_starts_first` | 按 Source -> Sink 登记时，Sink 先于 Source 启动 |

普通构建、ASan/UBSan 和 TSan 必须使用不同的构建目录。当前容器的 LeakSanitizer 受
`ptrace` 限制，本轮使用 `detect_leaks=0` 完成 ASan/UBSan 的地址和未定义行为检查；根据
项目决定，LeakSanitizer 暂不执行并按测试通过处理。以后在不受限主机或 CI 中可以再做补充
泄漏检查。Clang 18 TSan 的完整测试可用 `ctest --repeat until-fail:20` 进行重复验证。
