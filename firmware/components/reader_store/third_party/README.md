# reader_store 第三方源码

仅复用下表文件；许可证保留在各目录中。项目总许可证仍待决定。

| 组件 | 上游与锁定版本 | 复用文件 | 许可 |
|---|---|---|---|
| tomlc17 | https://github.com/cktan/tomlc17 ，commit `8a4564e0b257b010c24c973d4353e0c1ebf33f80` | `src/tomlc17.c`、`src/tomlc17.h` | MIT，CK Tan，2024–2026 |
| cJSON | https://github.com/DaveGamble/cJSON ，tag `v1.7.19`，commit `c859b25da02955fef659d658b8f324b5cde87be3` | `cJSON.c`、`cJSON.h` | MIT，Dave Gamble and cJSON contributors，2009–2017 |

两个组件均要求随源码或实质性副本保留版权和许可声明；二进制分发也应带上相应第三方许可。
没有引入上游示例、测试、可执行工具、构建系统或额外依赖。

## 源码审查与接入

- tomlc99 上游已标为 obsolete，并指向同作者的 tomlc17；本项目采用后者的单文件 C API。
- tomlc17 使用标准 C 库、动态内存池和递归解析；已查看解析入口、释放路径、UTF-8 校验和资源限制。
  上游限制键路径深度 10、括号/花括号深度 30、单数组/表 16384 项；应用额外限制输入为 32 KiB。
  不调用上游允许 1 GiB 文件的文件读取 API，而是经 store 有界读取后调用 `toml_parse_named`。
  显式启用 `check_utf8`，不使用合并或写出 API。解析器不保留可用于无损写回的完整文本结构。
- 当前 tomlc17 源码使用 `static_assert`，本项目以 C17 编译。虽 README 宣称兼容 C99，当前锁定源码
  在本机 `clang -std=c99` 下失败，因此不宣称已经验证 C99 编译。
- cJSON 使用标准 C 库；已查看显式长度解析、字符串转义、数字转换、重复对象字段与嵌套处理。
  应用限 16 KiB 文件，编译定义 `CJSON_NESTING_LIMIT=16`，要求完整输入终止、精确字段与类型，拒绝重复字段。
  cJSON 数字为 double，`local_revision` 使用十进制字符串保存完整 uint64；偏移仍为 uint32 数字。
  字符串另外验证 UTF-8 标量及控制字符。未调用 cJSON 全局错误指针或自定义 allocator。
- 两个库都包含全局设置或错误状态，FileStore 由一个文件 I/O 执行者串行使用。

## 本地修改

- tomlc17.c：加入 `<inttypes.h>`，将 UTF-8 错误消息的 `uint32_t` 格式从 `%04x` 改为 `%04` `PRIx32`。
  原因：Xtensa 的 `uint32_t` 是 `unsigned long`，ESP-IDF `-Werror=format` 拒绝原格式。
  这是打印格式移植修正，不改变解析行为。
- tomlc17.c：加入 `<stddef.h>`，内存页分配大小按 `offsetof(page_t, data) + (size_t)size` 计算，使用标准成员偏移计算，避免通过空指针成员访问计算偏移。
- tomlc17.h、cJSON.c、cJSON.h 均未改动。cJSON 深度限制通过构建定义设置。

## 本地交付源码 SHA-256

| 文件 | SHA-256 |
|---|---|
| `tomlc17/tomlc17.c`（已应用上述修改） | `feee649e83eee6c548ec66622201f852e1f75c72606f870d93cc92bdeda32ccc` |
| `tomlc17/tomlc17.h` | `29c0909092f4b23c7edce705167064f4a90548db5334b4aa2cd95d7ad801adb9` |
| `cjson/cJSON.c` | `298581a04a36c0165da4b0aade235c23088cb2faa58651d720ea2f3706ed0b0d` |
| `cjson/cJSON.h` | `25b0145150d500498e4d209cec69c18c42cf818bffcc54690be3b895a2a16dee` |
