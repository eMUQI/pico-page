# BookmarkRibbon

宽 24 px、下端有 12 px 缺口的书签标记：3 px `ink` 空心表示可添加，实心表示本页已有书签。

依据 `firmware/components/reader_app/app_impl.h` 的 `bookmark_ribbon()` 手写的静态示意。

## 规则

- 在工具页顶栏中位于 x 584 起的 100 px 书签格内，左侧以 2 px `rule` 线分隔，下方以 `caption` 写「加书签」或「删书签」。整个书签格点按切换书签。
- 实心表示当前可见页有书签，空心表示没有。
- 正文页上的书签标记与顶栏书签格中心对齐。
- 调用方提供中心 x、顶部 y、高度和是否实心。
