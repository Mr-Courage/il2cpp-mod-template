// hotkey.cpp — 全局热键
#include "core/hotkey.h"
#include "core/log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

#define ARK_HOTKEY_MAX 32

struct ArkHotkey
{
    const char *id;
    const char *name;
    int         vk;
    ArkHotkeyFn fn;
    int         wasDown;      // 上一帧的按下状态（边沿检测）
};

static ArkHotkey g_hk[ARK_HOTKEY_MAX];
static int       g_hkCount;
static volatile LONG g_captureSlot = -1;
static volatile LONG g_captured;

int ArkHotkeyRegister(const char *id, const char *name, int defaultVk, ArkHotkeyFn fn)
{
    if (!id || !fn) return -1;
    if (g_hkCount >= ARK_HOTKEY_MAX)
    {
        Log("[hotkey] 表满（%d），%s 未注册", ARK_HOTKEY_MAX, id);
        return -1;
    }

    // 冲突检测：默认键位被别人占了就不绑，让用户自己改
    int vk = defaultVk;
    if (vk)
    {
        for (int i = 0; i < g_hkCount; ++i)
            if (g_hk[i].vk == vk)
            {
                Log("[hotkey] %s 的默认键位与 %s 冲突，先不绑定", id, g_hk[i].id);
                vk = 0;
                break;
            }
    }

    const int slot = g_hkCount++;
    g_hk[slot].id = id;
    g_hk[slot].name = name ? name : id;
    g_hk[slot].vk = vk;
    g_hk[slot].fn = fn;
    g_hk[slot].wasDown = 0;
    Log("[hotkey] 注册 %s（%s）vk=%d", id, g_hk[slot].name, vk);
    return slot;
}

void ArkHotkeyPoll()
{
    // 捕获模式下，扫描全部按键，第一个"刚按下"的就是用户想绑的键。
    if (InterlockedCompareExchange(&g_captureSlot, 0, 0) >= 0)
    {
        for (int vk = 8; vk < 255; ++vk)
        {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
            if (GetAsyncKeyState(vk) & 0x8000)
            {
                InterlockedExchange(&g_captured, vk);
                InterlockedExchange(&g_captureSlot, -1);
                break;
            }
        }
        return;   // 捕获期间不触发热键，免得"按 F1 想绑定，结果把功能开了"
    }

    for (int i = 0; i < g_hkCount; ++i)
    {
        ArkHotkey &h = g_hk[i];
        if (!h.vk) { h.wasDown = 0; continue; }

        const bool down = (GetAsyncKeyState(h.vk) & 0x8000) != 0;
        if (down && !h.wasDown && h.fn)
            h.fn();
        h.wasDown = down ? 1 : 0;
    }
}

int ArkHotkeyGetVk(const char *id)
{
    if (!id) return 0;
    for (int i = 0; i < g_hkCount; ++i)
        if (strcmp(g_hk[i].id, id) == 0) return g_hk[i].vk;
    return 0;
}

bool ArkHotkeySetVk(const char *id, int vk)
{
    if (!id) return false;
    int slot = -1;
    for (int i = 0; i < g_hkCount; ++i)
        if (strcmp(g_hk[i].id, id) == 0) { slot = i; break; }
    if (slot < 0) return false;

    if (vk)
    {
        for (int i = 0; i < g_hkCount; ++i)
        {
            if (i != slot && g_hk[i].vk == vk)
            {
                Log("[hotkey] %s ← %d 与 %s 冲突，未设置", id, vk, g_hk[i].id);
                return false;
            }
        }
    }
    g_hk[slot].vk = vk;
    g_hk[slot].wasDown = 0;
    return true;
}

int ArkHotkeyCount() { return g_hkCount; }
const char *ArkHotkeyIdAt(int i)   { return (i >= 0 && i < g_hkCount) ? g_hk[i].id : nullptr; }
const char *ArkHotkeyNameAt(int i) { return (i >= 0 && i < g_hkCount) ? g_hk[i].name : nullptr; }
int ArkHotkeyVkAt(int i)           { return (i >= 0 && i < g_hkCount) ? g_hk[i].vk : 0; }

const char *ArkHotkeyKeyName(int vk, char *buf, int cap)
{
    if (!buf || cap <= 0) return "";
    if (!vk) { strcpy_s(buf, cap, "(未绑定)"); return buf; }

    // 先试特殊键
    switch (vk)
    {
    case VK_F1: case VK_F2: case VK_F3: case VK_F4: case VK_F5: case VK_F6:
    case VK_F7: case VK_F8: case VK_F9: case VK_F10: case VK_F11: case VK_F12:
        _snprintf_s(buf, cap, _TRUNCATE, "F%d", vk - VK_F1 + 1);
        return buf;
    case VK_INSERT:   strcpy_s(buf, cap, "Insert"); return buf;
    case VK_DELETE:   strcpy_s(buf, cap, "Delete"); return buf;
    case VK_HOME:     strcpy_s(buf, cap, "Home"); return buf;
    case VK_END:      strcpy_s(buf, cap, "End"); return buf;
    case VK_PRIOR:    strcpy_s(buf, cap, "PgUp"); return buf;
    case VK_NEXT:     strcpy_s(buf, cap, "PgDn"); return buf;
    case VK_SPACE:    strcpy_s(buf, cap, "Space"); return buf;
    case VK_TAB:      strcpy_s(buf, cap, "Tab"); return buf;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: strcpy_s(buf, cap, "Shift"); return buf;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: strcpy_s(buf, cap, "Ctrl"); return buf;
    case VK_MENU: case VK_LMENU: case VK_RMENU: strcpy_s(buf, cap, "Alt"); return buf;
    default: break;
    }

    // 字母数字：用 MapVirtualKey 拿布局相关字符
    const UINT ch = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_CHAR);
    if (ch >= 32 && ch < 127)
    {
        buf[0] = (char)(ch & 0x7F);
        buf[1] = 0;
        return buf;
    }

    _snprintf_s(buf, cap, _TRUNCATE, "VK%02X", vk);
    return buf;
}

void ArkHotkeyBeginCapture(int slot)
{
    InterlockedExchange(&g_captured, 0);
    InterlockedExchange(&g_captureSlot, slot >= 0 ? slot : 0);
}

int ArkHotkeyCapture()
{
    return (int)InterlockedExchange(&g_captured, 0);
}
