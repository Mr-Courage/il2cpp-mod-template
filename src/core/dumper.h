// dumper.h — 运行时 Il2CppDumper 式导出（dump.cs + script.json）
// ============================================================================
// 解决的问题：dump.cs 是**离线产物**，游戏一更新就全错。本模块在**运行时**
// 用 il2cpp 反射导出把当前进程里的类/字段/方法全量写成 Il2CppDumper 兼容格式：
//
//   dump.cs      —— 伪 C# 声明，字段带偏移、方法带 RVA/VA 注释
//   script.json  —— ScriptMethod 数组（给 Il2CppDumper 的 IDA 脚本做批量重命名）
//
// 与离线 Il2CppDumper 的差异（都是运行时拿不到元数据导致的，写死在这里）：
//   · 没有 string literal（ScriptString 为空）
//   · 没有 generic method 表（ScriptMetadata 为 null）
//   · Offset（文件内偏移）不做 —— RVA + VA 已够 IDA 定位
//   · 枚举常量值不显示（需要 metadata 的 default_value，导出表没有）
//
// 线程模型：dump 全程只读 il2cpp **元数据**（C 字符串 + 数值），不碰 Unity
// 对象堆，跑在 Worker 线程（ArkTaskPost）。耗时几十秒（十万级方法），
// 期间 Worker 被占用，其它工具任务会排队 —— 这是预期行为，面板有进度显示。
// ============================================================================
#pragma once

// 投递 dump 任务。返回 false = 已在跑 / 类索引还没建（先在工具 tab 建过一次索引）。
bool ArkDumperStart();

// 状态：0=空闲 1=运行中 2=完成 3=失败
int         ArkDumperState();
const char *ArkDumperPhase();   // 当前阶段说明（静态字符串，随时可读）
int         ArkDumperPermille();// 进度 0-1000（按类计）

// 完成后的统计（运行中也在涨，可以当实时进度看）
int         ArkDumperClasses(); // 已写出的类数
int         ArkDumperMethods(); // 已写出的方法数
int         ArkDumperFields();  // 已写出的字段数

// 输出目录（模块目录，UTF-8，非空）。文件是 <目录>\dump.cs / <目录>\script.json。
const char *ArkDumperOutDir();
