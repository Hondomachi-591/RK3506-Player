# my_player TODO

---

## 播放列表 UI

- [x] 扩展 `g_playlist` 为结构体：path / title / duration / has_video
- [x] `build_playlist()` 增加逐文件 duration 探测
- [x] 创建 playlist screen：flex 列布局 + lv_btn 项目列表
- [x] 列表项显示：序号 + 文件名 + 时长（如 `01. 歌曲名    3:42`）
- [x] 当前播放项高亮：▶ 前缀 + 不同背景色
- [x] 播放界面新增"播放列表"入口按钮
- [x] 双 screen 切换（`lv_scr_load`）
- [x] 列表点击后切歌 + 自动返回播放界面
- [x] 进入列表时自动滚动到当前曲目可见位置

---

## 播放模式

- [ ] 顺序播放（当前默认行为）
- [ ] 单曲循环
- [ ] 列表循环
- [ ] 随机播放
- [ ] 模式切换按钮 + 图标指示（🔁 / 🔂 / 🔀 / ➡）

---

## 进度条拖动（快进快退）

- [ ] 进度条改为可交互（当前只读）
- [ ] 拖动时 `av_seek_frame` 跳转到目标位置
- [ ] 刷新音视频包队列（丢弃旧包）
- [ ] 拖动时实时更新时间标签

---

## 存储热插拔

- [ ] SD 卡拔出时停止播放、清空列表
- [ ] SD 卡插入时自动重新扫描
- [ ] 优先方案：Linux inotify 监听 /mnt/sdcard 目录
- [ ] 备选方案：定时轮询 `/mnt/sdcard` 是否存在

---

## 音频文件封面显示

- [ ] 检测音频文件中嵌入的封面图（ID3v2 APIC / FLAC picture / MP4 covr）
- [ ] FFmpeg 读取附着流（attached picture stream）
- [ ] 纯音频播放时 canvas 显示封面图替代"Audio Only"文字
- [ ] 无封面时显示默认占位图

---

## OSD 信息叠加

- [ ] 切歌时短暂显示文件名 + 编码信息覆盖层（2~3 秒后消失）
- [ ] 调节音量时短暂显示音量条 OSD
- [ ] 暂停时显示暂停图标覆盖层
- [ ] LVGL 实现：一个半透明 overlay 层，带自动隐藏定时器

---

## 多语言文件名支持

- [ ] 当前 `strcasecmp` 只匹配 ASCII，需支持 UTF-8 / GBK 编码的文件名
- [ ] LVGL 显示中文文件名（需确认 freetype 字体包含中文字形）
- [ ] SD 卡常见编码：FAT32 用 GBK，ext4 用 UTF-8，需自动检测或兼容

---

## 屏幕常亮 & 待机策略

- [ ] 视频播放时禁止息屏（写 `/sys/class/graphics/fb0/blank` 或 DRM DPMS）
- [ ] 纯音频播放时允许按系统设定息屏
- [ ] 暂停超过 N 分钟进入低功耗待机
- [ ] 触摸唤醒后恢复播放

---

## 自定义播放列表管理

- [ ] 创建/删除/重命名自定义播放列表（存为 .m3u 或自定义格式）
- [ ] 支持"添加到播放列表"操作
- [ ] 播放列表文件保存在 `/userdata/playlists/`

---

## 后台播放

- [ ] 纯音频时可选关闭屏幕（关背光 + 停止 LVGL 刷新）
- [ ] 触屏唤醒恢复显示
- [ ] 选做：注册全局热键 / 硬件按键控制播放暂停

---

## 状态机重构（选做，代码拆分时再考虑）

- [ ] 定义 `player_state_t` 枚举（IDLE / PLAYING / PAUSED / SEEKING / SWITCHING / STOPPED）
- [ ] `player_set_state()` 统一状态切换入口
- [ ] 各模块通过 `player_get_state()` 获取状态，替代直接读 flag
- [ ] 状态转换合法性检查（如 SEEKING 时不允许切歌）

---

## 代码拆分（选做，main.c 超过 3000 行再考虑）

- [ ] player_engine.h/.c — FFmpeg 解封装、解码、音视频同步、ALSA 输出
- [ ] player_ui.h/.c — 播放界面 UI（canvas、进度条、按钮、音量）、帧渲染
- [ ] playlist_ui.h/.c — 播放列表 UI、曲目点击、高亮、滚动
- [ ] playlist_mgr.h/.c — 播放列表数据结构、扫描媒体、时长探测、排序
- [ ] config.h/.c — 配置读写（/userdata/player.conf）
- [ ] ui_common.h/.c — 公共 UI 工具（时间格式化、颜色常量等）
