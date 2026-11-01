// config.h — 配置项声明 + 自动生成 UI + 落盘
// ============================================================================
// 设计取舍：不用"配置文件驱动的反射式配置"，而是**声明式**。
//   模块在编译期声明自己有哪些配置（ArkCfgItem 数组，生存期为整个进程），
//   框架负责：读 ini → 填到模块的变量里 → 在 UI 上自动画出控件 → 改动后落盘。
//
// 这样做的好处：模块只写一行声明就得到一个持久化 + 有界面的开关；
// 坏处：配置项不能运行时增删（对修改器完全够用）。
//
// ★ ptr 指向的存储**必须**是静态存储期且零初始化 —— 零初始化就是默认值，
//   配置文件不存在时行为由这里决定。用栈变量或堆变量会读到垃圾。
// ============================================================================
#pragma once

#include "core/vec.h"

enum ArkCfgType
{
    CFG_BOOL = 0,   // int*，0/1
    CFG_INT  = 1,   // int*
    CFG_FLOAT= 2,   // float*
    CFG_ENUM = 3,   // int* + names[]
};

struct ArkCfgItem
{
    const char *key;        // 唯一键（建议 "模块id.项"）
    const char *label;      // 显示名
    int         type;
    void       *ptr;        // 存储地址
    float       min, max, step;
    const char *const *names;   // CFG_ENUM 专用
    int         nameCount;
    const char *hint;       // 悬停提示，可空
};

// 注册一批配置项（可多次调，累加）。
void ArkCfgRegister(ArkCfgItem *items, int count);

// 从 ini 读入。找不到文件 = 全默认，不报错。
void ArkCfgLoad();
// 立刻落盘（Worker 卸载时要保证调一次）。
void ArkCfgSave();

// UI 侧：有改动就置脏，由 Worker 每秒检查后落盘（渲染线程不做文件 IO）。
void ArkCfgMarkDirty();
bool ArkCfgTakeDirty();     // 取走脏标记（取走即清）

// 画出全部已注册配置项（调试用，正式界面里模块自己画自己那段）。
int  ArkCfgDrawAll(const char *filterKeyPrefix);

// 画出某一组配置项，返回是否有改动。
bool ArkCfgDrawGroup(ArkCfgItem *items, int count);

// 按 key 直接读写（模块 onEnable 里偶尔需要）。
bool  ArkCfgGetBool(const char *key);
int   ArkCfgGetInt(const char *key);
float ArkCfgGetFloat(const char *key);
void  ArkCfgSetBool(const char *key, bool v);
void  ArkCfgSetInt(const char *key, int v);
void  ArkCfgSetFloat(const char *key, float v);
