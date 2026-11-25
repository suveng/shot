# Shot

原生 GTK4 截图标注工具（C，无解释器）。单窗口：框选 → 标注 → 回车复制并保存。

## 构建

```bash
meson setup build --prefix=/usr
meson compile -C build
sudo meson install -C build
```

依赖：`libgtk-4-dev`、`meson`、`build-essential`。

## 运行

```bash
shot            # 抓屏并进入标注界面
shot --smoke    # 自检：自动截图 + 画一条箭头 + 保存（1.2 秒后自动提交）
shot --test-dbl # 自检：验证双击判定（选区外/第一次/第二次）
```

快捷键：**双击选区 = 复制+保存** · `Esc` 取消 · `Enter`/`Ctrl+C` 复制+保存 · `Ctrl+S` 只保存 · `Ctrl+Z` 撤销 · `1..5` 箭头/方框/画笔/文字/马赛克 · `c` 换色

保存路径：`<Pictures>/截图/<软件名>/截图 YYYY-MM-DD HH-MM-SS.png`，可用
`~/.config/shot/config.ini` 覆盖：

```ini
[shot]
base=~/Pictures/Ubuntu
```

## GNOME Wayland 的两个坑（已在代码/工程里处理）

1. **portal 抓屏的 app-id**：GUI 应用的 app-id 由 systemd scope 推导。直接跑会得到
   `Only the focused app is allowed to show a system access dialog`。本地安装用包装脚本
   `systemd-run --user --scope --unit=app-io.github.suveng.shot` 固定 app-id，并预先授权：

   ```bash
   gdbus call --session --dest org.freedesktop.impl.portal.PermissionStore \
     --object-path /org/freedesktop/impl/portal/PermissionStore \
     --method org.freedesktop.impl.portal.PermissionStore.SetPermission \
     'screenshot' true 'screenshot' 'io.github.suveng.shot' "['yes']"
   ```

2. **焦点应用名**：Wayland 下应用拿不到焦点窗口，需配套 GNOME Shell 扩展
   `shot-focus@suveng`（写入 `$XDG_RUNTIME_DIR/shot-focus`），安装后需重新登录。

## 发布到 Ubuntu App Center

App Center 的应用来源是 Snap Store，因此走 snapcraft。

```bash
sudo snap install snapcraft --classic
snapcraft login                        # 需要 Ubuntu One 账号
snapcraft register shot-screenshot     # 名称需全局唯一
snapcraft pack                         # 产出 shot-screenshot_0.1.0_amd64.snap
snapcraft upload --release=edge shot-screenshot_0.1.0_amd64.snap
```

上传后在 <https://dashboard.snapcraft.io> 填写商店信息（截图、分类、图标、许可证），
提交后进入人工审核；通过后 App Center 即可搜索安装。

`snap/snapcraft.yaml` 已使用 `extensions: [gnome]`、`confinement: strict`、
`adopt-info`（从 AppStream 元数据取版本/描述），符合官方桌面应用打包指南。

## 目录

```
src/shot.c                    全部逻辑（抓屏、绘制、标注、保存、剪贴板）
data/*.desktop.in             桌面入口
data/*.metainfo.xml           AppStream 元数据（商店展示 + adopt-info）
data/icons/.../*.svg          可缩放图标
snap/snapcraft.yaml           Snap 打包
```

## 商店截图（离屏渲染，不碰真实屏幕）

不要用"拍真实桌面"的方式做商店素材（会带隐私/版权画面）。用 Xvfb 离屏跑 `--demo-bg`：

```bash
# 1. 造一张干净的背景图（cairo + Pango 画文档），或直接用任意图片
# 2. 起一个 1920×1080 的虚拟屏，在里面跑 demo 模式（不调 portal，纯本地渲染）
Xvfb :99 -screen 0 1920x1080x24 -nolisten tcp &
DISPLAY=:99 GDK_BACKEND=x11 GSK_RENDERER=cairo ./build/shot --demo-bg bg.png &
sleep 4 && DISPLAY=:99 import -window root store-screenshot-01.png
pkill -x Xvfb
```

`--demo-bg FILE` 用给定图片代替 portal 抓屏，并自动摆好选区与两个标注，
用于演示/商店素材，不读取真实屏幕内容。
