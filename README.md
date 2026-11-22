# CoW Trie、并发 TrieStore 与安全内存回收实验

本实验基于 CMU BusTub `v20231227-2023fall` Project 0 改写，使用 C++17。三个任务依次学习不可变 Trie、并发读写与 CAS 乐观更新，以及经典 Epoch-Based Reclamation（EBR）。

## 1. 文件用途

| 文件                                                         | 用途                                                         |
| ------------------------------------------------------------ | ------------------------------------------------------------ |
| [include/trie.h](include/trie.h)                             | Trie 节点、值节点、`Clone`、接口和共享值包装；共享子树由 `shared_ptr` 管理 |
| [src/trie.cpp](src/trie.cpp)                                 | Task 1 的 `Get`、`Put`、`Remove` 待实现函数                  |
| [include/trie_store.h](include/trie_store.h)                 | 互斥锁版 `TrieStore`、乐观更新版 `CasTrieStore` 和保护结果的 `ValueGuard` |
| [src/trie_store.cpp](src/trie_store.cpp)                     | Task 2 两种 Store 的 `Get`、`Put`、`Remove` 待实现函数       |
| [include/epoch.h](include/epoch.h)                           | Epoch 状态、线程登记、Guard 的 RAII、观测接口和退出清理      |
| [src/epoch.cpp](src/epoch.cpp)                               | Task 3 的进入、退出、退休、推进和回收待实现函数              |
| [include/epoch_trie_store.h](include/epoch_trie_store.h)     | 提供的 EBR 使用示例：先 Pin 再读根，发布新根后 Retire 旧根   |
| [common/exception.h](common/exception.h)                     | 未实现函数使用的异常                                         |
| [common/move_blocked.h](common/move_blocked.h)、[common/tracked_value.h](common/tracked_value.h) | 暂停值的移动构造、统计析构次数，用于验证并发和生命周期       |
| [test/trie_test.cpp](test/trie_test.cpp)                     | Task 1 的完整正确性测试                                      |
| [test/trie_store_test.cpp](test/trie_store_test.cpp)         | Task 2A、2B 的完整正确性测试                                 |
| [test/epoch_test.cpp](test/epoch_test.cpp)                   | Epoch 管理器及 Trie 集成测试                                 |
| [test/trie_performance_test.cpp](test/trie_performance_test.cpp) | 最终性能验收：CoW 更新与每次完整复制 map 的对比 |
| [test/test.h](test/test.h)、[test/test_utils.h](test/test_utils.h) | 测试断言、注册、固定键集合、独立 map 对照和线程辅助工具      |

## 2. 编译与完整测试

需要支持 C++17 的编译器、CMake 3.20 或以上版本及系统线程库。最终验收包含一个 CoW 性能要求，使用 **Release** 构建。

在 `trie/` 根目录执行。日常调试使用 **Debug**，只运行正确性测试（排除最终性能验收）：

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-debug --config Debug --parallel 4
ctest --test-dir build-debug -C Debug -LE "^performance$" --output-on-failure
```

最终验收使用 **Release**，运行全部测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

两个构建目录可以同时保留，后文的 `build` 均指 Release 构建目录。Visual Studio 等多配置生成器通过 `--config` 和 `-C` 选择配置；Makefile、普通 Ninja 等单配置生成器通过 `CMAKE_BUILD_TYPE` 选择配置。

默认运行全部 **8 个测试组**，所有用例统一纳入验收。全部任务完成后应得到：

```text
100% tests passed, 0 tests failed out of 8
```

每次修改代码后都要重新编译。未完成任务前相应测试失败是正常现象，可以先运行公共自检或已经完成的任务。

## 3. Task 1：CoW Trie

在 `src/trie.cpp` 完成三个函数。`trie.h` 已提供将普通值包装成 `shared_ptr<T>` 的入口，只需实现一个实际插入算法。

| 函数                 | 要求                                                         |
| -------------------- | ------------------------------------------------------------ |
| `Get<T>(key)`        | 沿完整 key 查找；不存在、没有值或类型不匹配时返回 `nullptr`  |
| `Put<T>(key, value)` | 复制修改路径、共享其他分支，返回新 Trie；支持覆盖值和改变类型 |
| `Remove(key)`        | 删除目标值，保留子键，剪去无值且无子节点的路径；删空后根为 `nullptr` |

本任务包含 **32 个正确性用例**，覆盖基本操作、全部 256 种字节、长键与大值、类型转换、多分支历史快照、固定种子的 map 随机对照、异常和对象生命周期。

`GetDecisionMatrix` 明确检查空树、空键、路径首段/中段/末段不存在、路径终点无值、键超出叶子、类型不匹配、不可复制值及字符串视图边界。

`PutNodeTransitionMatrix` 包含 14 行场景：空树插入空键/非空键、首段/中段/末段缺失、无值节点转为值节点、沿有值叶子或内部节点继续插入、叶子/内部节点同类型及跨类型覆盖、根节点转换。每行同时检查旧快照、完整树形和未改动值的地址稳定性。

`RemoveDecisionMatrix` 包含 16 行场景，并检查剩余节点恰好对应剩余键的全部前缀，避免漏剪枝或过度删除：

| 删除场景                                     | 必须满足的结果                     |
| -------------------------------------------- | ---------------------------------- |
| 空树；键的首段、中段、末段不存在；键超出叶子 | 返回原根，不新增或重建节点         |
| 路径终点存在但无值，包括无值根               | 保持原树和子键                     |
| 目标有值且仍有子节点，包括根节点             | 只删除值，保留所有子键             |
| 目标无子节点但存在兄弟节点                   | 删除目标叶子，保留兄弟             |
| 删除后无值祖先也变空                         | 连续向上剪枝                       |
| 向上遇到有值祖先                             | 停止剪枝，保留祖先值               |
| 向上遇到仍有其他分支的祖先                   | 停止剪枝，保留其他分支             |
| 最后一个非空键或最后一个空键被删除           | 根为 `nullptr`                     |
| 重复删除、二进制键                           | 重复删除不改变根；严格按键长度处理 |

以上场景都检查旧快照仍有效、未删除值保持共享；另外的随机用例验证多次操作组合。

## 4. Task 2：并发 TrieStore 与 CAS

依赖 Task 1，不依赖 Task 3。在 `src/trie_store.cpp` 完成两种 Store 的三个操作。

| 任务与函数                       | 要求                                                         |
| -------------------------------- | ------------------------------------------------------------ |
| Task 2A：`TrieStore::Get`        | 在根锁内取得快照，解锁后查询，用 `ValueGuard` 保护结果       |
| Task 2A：`TrieStore::Put/Remove` | 用写锁串行化完整更新，在根锁外构造新版本，再短暂持根锁发布   |
| Task 2B：`CasTrieStore::Get`     | 原子取得根快照，在快照上查询并返回 Guard                     |
| Task 2B：`CasTrieStore::Put`     | 值只准备一次；根据当前快照构造候选版本，用 CAS 发布，冲突后基于最新快照重建并重试 |
| Task 2B：`CasTrieStore::Remove`  | 用 CAS 发布删除结果，冲突后重新执行删除，保留其他写者已完成的更新 |

旧 Guard 必须一直保护其读取的旧值，并允许跨线程传递；写者构造新值或读者长期持有 Guard 时，其他允许并发的操作仍能推进。不可复制值不能在重试时被重复移动，异常不能改变已发布版本或留下未释放的锁。

本任务包含 **49 个正确性用例**：互斥锁版 22 个，CAS 版 27 个。除基本并发外，还覆盖共享前缀、多写者、热点覆盖与删除、Guard 生命周期、构造异常和 CAS 冲突；还专门检查并发删除不存在的键/无值前缀不会丢失插入，以及反复删除空键或前缀值不会破坏并发插入的不可复制子键。

每种 Store 另有 100 轮小规模并发历史检查：每轮 3 个线程共执行 12 次操作，检查是否存在满足真实先后关系、查询结果和最终状态的合法串行顺序。有限测试通过不能替代对并发正确性的推导。

## 5. Task 3：经典 EBR

在 `src/epoch.cpp` 完成五个函数；管理器可以独立测试，Trie 集成需要 Task 1，不依赖 Task 2。

| 函数                | 要求                                                         |
| ------------------- | ------------------------------------------------------------ |
| `Enter()`           | 相当于 Pin；首次进入登记线程的 `local_epoch`，嵌套进入增加深度并保留最早 epoch，返回 Guard |
| `Exit(thread_id)`   | 由 Guard 自动调用；按传入的登记线程 ID 减少嵌套深度，降到 0 后删除登记 |
| `Retire(callback)`  | 记录当前全局 epoch 和回调；不推进 epoch，不立即执行回调      |
| `TryAdvanceEpoch()` | 所有活跃线程的 `local_epoch` 均等于当前全局 epoch 时推进一轮；否则返回 `false` |
| `TryReclaim()`      | 尝试推进一次，再移出满足 `global_epoch - retired_epoch >= 2` 的条目，在锁外执行回调并返回数量 |

本实验的 EBR 元数据由短时互斥锁保护，不要求无锁线程注册或 epoch 回绕。计数到达 64 位上限时停止推进。`ActiveReaders()` 返回存活 Guard 数量，包含嵌套进入。

完整测试包含 **27 个正确性用例**：管理器 21 个、Trie 集成 6 个，全部纳入现有 `epoch` 和 `epoch_trie_store` 测试组。

| 覆盖场景                               | 验收重点                                                     |
| -------------------------------------- | ------------------------------------------------------------ |
| 空队列、连续回收、单轮推进             | 无对象时返回 0；一次调用最多推进一轮，旧读者仍能阻止推进     |
| 两轮等待、多个 epoch 批次混合          | 只回收已安全的对象；推进受阻时仍处理已有安全条目，每个对象恰好回收一次 |
| 嵌套、乱序销毁、Guard 跨线程移交       | 最早 epoch 保持到最后一个 Guard 退出；跨线程销毁归还原登记线程的 Pin |
| Guard 在不同管理器间移动赋值、自移动   | 先释放原登记，管理器状态互不干扰，自移动不误退出             |
| 回调内再次退休并调用回收               | 不死锁、不重复执行当前回调，新退休对象仍需等待两轮           |
| 暂停回调时另一个线程继续操作           | Enter、Retire、TryReclaim 仍能完成；新退休条目不丢失，多个回收者不重复回收 |
| 长读者造成 4096 个对象积压             | 读者存活时不提前回收；退出后分两轮清空，并逐个核对回收次数   |
| 独立的读者、退休者和回收者并发         | 没有提前回收，返回的回收总数与逐对象计数一致                 |
| ReadGuard 移动、跨线程读取、覆盖与删除 | 旧版本持续受保护，直到实际持有者释放 Guard                   |
| 不同 epoch 的读者分阶段退出、共享子树  | 安全的旧根及时回收，共享值一直存活到最后一个拥有它的版本被释放 |
| 析构清理                               | 所有操作与 Guard 已结束后，管理器清理剩余退休对象            |

关键交错通过 `promise/future` 固定操作顺序，不依靠睡眠碰巧触发。带等待的用例会先释放暂停点再报告失败；死锁仍由 CTest 的超时兜底。跨线程移交 Guard 必须使用同步手段，管理器必须比全部 Guard 活得更久。

所有 Guard 退出且不再新增退休对象后，连续调用两次 `TryReclaim()` 应能清空退休列表。共享子树仍由 `shared_ptr` 管理。

## 6. 按任务测试与通过标准

先按第 2 节构建，然后选择需要验收的任务。按任务运行时只检查正确性，性能统一在最后验收：

```bash
# 公共自检。
ctest --test-dir build -C Release -L '^provided$' --output-on-failure

# Task 1：CoW Trie 正确性。
ctest --test-dir build -C Release -L '^task1$' --output-on-failure

# Task 2：两种 Store 的并发正确性。
ctest --test-dir build -C Release -L '^task2$' --output-on-failure

# Task 3：Epoch 管理器和 Trie 集成。
ctest --test-dir build -C Release -L '^task3$' --output-on-failure
```

| 验收范围 | `-L` 标签 | 预期测试组数 | 用例数 |
| --- | --- | --- | --- |
| 公共支持代码 | `^provided$` | 2 | 4 |
| Task 1 | `^task1$` | 1 | 32 个正确性 |
| Task 2 | `^task2$` | 2 | 49 个正确性 |
| 只测 Task 2A | `^mutex$` | 1 | 22 个正确性 |
| 只测 Task 2B | `^cas$` | 1 | 27 个正确性 |
| Task 3 | `^task3$` | 2 | 27 个正确性 |
| 最终性能验收 | `^performance$` | 1 | 1 个 CoW 性能对比 |
| 全量验收 | 不加标签 | 8 | 上述全部正确性用例及一个性能对比 |

通过要求：重新构建成功；选中的测试组数量正确；对应任务的正确性用例全部通过；不出现崩溃、超时或内存错误。最终验收还需通过第 7 节的 CoW 性能对比。运行了 0 个测试、只通过单例或只通过性能测试，都不能替代完整任务验收。

定位问题时可以查看详细输出或单独执行用例：

```bash
ctest --test-dir build -C Release -L '^task2$' -V
./build/trie_test TrieTest.GetDecisionMatrix
./build/trie_test TrieTest.PutNodeTransitionMatrix
./build/trie_test TrieTest.RemoveDecisionMatrix
./build/trie_test TrieTest.RandomBranchesAgainstMapOracle
./build/trie_store_test CasStoreTest.SmallHistoriesAreLinearizable
./build/epoch_test EpochTest.PausedCallbackAllowsOtherThreadsToPinRetireAndCollect
./build/epoch_test EpochTrieStoreTest.StaggeredReadersReleaseVersionsAndSharedValuesInStages
```

## 7. 最终性能验收：CoW 更新与整表复制

全部任务完成后，统一验收一个性能指标：**CoW 更新吞吐至少为每次完整复制 `std::map` 的 16 倍**。这项对比使用 Task 1 的 Trie 实现，单独运行一个测试组、一个用例：

```bash
ctest --test-dir build -C Release -L '^performance$' -V
```

固定工作负载：初始 4096 个键，每批 768 次更新，其中 3/4 为写入、1/4 为删除。两种实现都保留 8 个历史快照；CoW 复制修改路径，对照实现每次更新前完整复制 map。

输出中的 `throughput_ratio = 完整复制每操作耗时 / CoW 每操作耗时`，达到 **16.0** 即通过。运行结果同时输出两种实现的 `ns/op`，用于解释路径复制与整表复制的成本差异。

先预热，再交替顺序运行 5 轮，取每轮相对吞吐的中位数。每次采样重复固定工作负载，累计至少 20 ms 的计时工作或达到 64 批上限。初始建树和每批结束后的最终清理不计时；更新过程中的复制与回收计入。测试校验操作结果，并使用校验和避免计算被优化掉。

性能验收由 CTest 串行执行。使用 Release 构建，不使用 Sanitizer，并在空闲机器上运行。Debug 构建会明确报错。临界失败应在相同 Release 构建下复测，并保留实际比例。

## 8. 内存检查

使用支持 AddressSanitizer、UndefinedBehaviorSanitizer 的 GCC/Clang 时，可在独立构建目录检查悬挂指针、重复释放和未定义行为。例如检查 Task 1、2 的正确性：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build build-asan --parallel 4
ctest --test-dir build-asan -L '^task[12]$' --output-on-failure
```

按任务标签运行 Sanitizer 检查时只执行正确性用例；最终性能验收仍需使用 Release 构建运行。检测工具报告的内存错误也需要修复。

## 9. 来源

接口与部分测试源自 [CMU BusTub v20231227-2023fall](https://github.com/cmu-db/bustub/tree/v20231227-2023fall)，许可见 `LICENSE`。其余测试为本实验编写，包含随机模型对照、并发历史检查和相对性能验收。

EBR 参考：[Crossbeam Epoch 协议与延迟回收](https://docs.rs/crossbeam-epoch/latest/crossbeam_epoch/)。
