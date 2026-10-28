# Unicode 查词字符分类

`lookup_ranges.inc` 提取 [Unicode 16.0.0 的 UnicodeData.txt](https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt) 中的字母（L）、组合标记（M）和数字（N）类别，合并相邻码点区间。它供查词范围调整判断有效文字，不改变现有拉丁词切分或自动选词范围，也不实现字素簇切分。

来源文件 SHA-256：`ff58e5823bd095166564a006e47d111130813dcf8bf234ef79fa51a870edb48f`。上游是版本化数据发行，没有 Git commit；生成脚本验证该摘要，固件构建只编译已生成的范围表，不需要 Python Unicode 数据库或网络。表含 819 个区间，每区间两个 32 位码点，共 6,552 字节只读数据；查询采用二分查找，无动态分配。

从仓库根目录执行下列命令重新生成或核对；参数指向下载的固定版本文件：

```sh
python3 firmware/components/reader_dictionary/third_party/unicode/generate.py /path/to/UnicodeData.txt
python3 firmware/components/reader_dictionary/third_party/unicode/generate.py /path/to/UnicodeData.txt --check
```

数据遵循 Unicode-3.0，完整许可保存在 [LICENSE](LICENSE)，取自 [Unicode 官方许可](https://www.unicode.org/license.txt)。源码或二进制分发的配套资料须保留版权与许可通知；未复制第三方运行时代码。
