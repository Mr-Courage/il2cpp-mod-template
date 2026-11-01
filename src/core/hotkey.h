// hotkey.h — 全局热键注册 + 冲突检测
// ============================================================================
// ★ 用 GetAsyncKeyState（系统级键状态），**不用** GetKeyState / GetKeyboardState。
//   后两个读的是**调用线程**的输入队列，而我们的轮询跑在渲染线程（Present 钩子里）
//   —— 那个线程从来没被投递过键盘消息，永远是"什么都没按"。
//
// 轮询放在渲染线程（UiSetFrameHook 里，面板关着也跑）。
// ============================================================================
#pragma once

typedef void (*ArkHotkeyFn)();

// 注册一个热键。vk 传 0 表示默认不绑定（用户自己设）。
// 返回槽位 id（>=0），-1 = 表满。
int ArkHotkeyRegister(const char *id, const char *name, int defaultVk, ArkHotkeyFn fn);

// 每帧轮询（渲染线程）。边沿触发 —— 按住不放只触发一次。
void ArkHotkeyPoll();

int  ArkHotkeyGetVk(const char *id);
// 设置键位。已有别的热键占着这个 vk 时返回 false（冲突）。
bool ArkHotkeySetVk(const char *id, int vk);

int  ArkHotkeyCount();
const char *ArkHotkeyIdAt(int i);
const char *ArkHotkeyNameAt(int i);
int  ArkHotkeyVkAt(int i);

// 把 vk 写成可读名字（"F1" / "Ctrl" / "A"）。返回 buf。
const char *ArkHotkeyKeyName(int vk, char *buf, int cap);

// 记录"当前按下的是哪个键"（给"按键绑定"UI 用）。返回 vk，0 = 还没按。
// 调用一次就消耗掉这次捕获结果。
int ArkHotkeyCapture();
void ArkHotkeyBeginCapture(int slot);
