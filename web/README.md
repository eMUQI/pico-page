# Pico Page 网站

Astro 静态站，结构与 QuotaFrame 的 `web/` 一致。

```sh
npm ci --prefix web --ignore-scripts
npm run build --prefix web
npm run preview --prefix web
```

构建产物在 `web/dist/`。

- `src/components/HomePage.astro`：首页。
- `src/pages/`：路由。新增语言或文档页时在这里添加 Astro 或 Markdown 页面。
- `public/assets/styles.css`：样式。
- `public/assets/screens/`：界面图，来自 host 桌面版渲染，使用合成数据。

网站文案面向读者。验证状态、验收标准和开发注意事项写在根目录 README 和 `docs/` 中。
