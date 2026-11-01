// gcobj.cpp — GC 活体枚举 + gchandle 钉住
// ============================================================================
// ★ 手动映射约束：本文件不得出现任何静态存储期的非平凡对象（std::string/vector）。
//   全局状态一律 POD。
// ============================================================================
#include "core/gcobj.h"
#include "core/guard.h"
#include "core/log.h"

#include "il2cpp/il2cpp_runtime.h"

namespace ir = il2cpp::runtime;

// ================================================================ 可用性探测
// 三代 liveness API：
//   NEW = allocate_struct/finalize（Unity >= 2021.2，需要 stop/start_gc_world）
//   OLD = calculation_begin/end   （更早的 Unity，两个回调参数）
// 用"哪个解析到了走哪个"，不靠版本号字符串 —— 版本串在不同 Unity 分支上格式不一，
// 解析出来的结论还不如直接看函数指针权威。
enum { LIV_NONE = 0, LIV_NEW = 1, LIV_OLD = 2 };

static volatile LONG g_livMode = -1;   // -1 = 未探测

static int DetectLiveness()
{
    const LONG cached = InterlockedCompareExchange(&g_livMode, -1, -1);
    if (cached >= 0)
        return (int)cached;

    Il2CppApi &api = ir::Api();
    int mode = LIV_NONE;

    if (api.il2cpp_unity_liveness_allocate_struct && api.il2cpp_unity_liveness_calculation_from_statics &&
        api.il2cpp_unity_liveness_finalize && api.il2cpp_unity_liveness_free_struct &&
        api.il2cpp_stop_gc_world && api.il2cpp_start_gc_world)
    {
        mode = LIV_NEW;
    }
    else if (api.il2cpp_unity_liveness_calculation_begin &&
             api.il2cpp_unity_liveness_calculation_from_statics &&
             api.il2cpp_unity_liveness_calculation_end)
    {
        mode = LIV_OLD;
    }

    if (mode == LIV_NEW)
        Log("[gc] 对象枚举可用（新版 liveness：allocate_struct + stop_gc_world）");
    else if (mode == LIV_OLD)
        Log("[gc] 对象枚举可用（旧版 liveness：calculation_begin/end）");
    else
        Log("[gc] ★对象枚举不可用：liveness 导出缺失。"
            "「对象查找」整页会是空的，其余功能不受影响。");

    InterlockedExchange(&g_livMode, mode);
    return mode;
}

bool ArkGcAvailable()
{
    return DetectLiveness() != LIV_NONE;
}

// ================================================================ 枚举
namespace
{
    // 回调的 userdata。★ 必须是 POD 且活到 end/finalize 之后。
    struct CollectCtx
    {
        Il2CppObject **out;
        int            max;
        int            count;
        int            overflow;
    };

    void CollectCallback(Il2CppObject **arr, int size, void *ud)
    {
        CollectCtx *c = (CollectCtx *)ud;
        if (!c || !arr || size <= 0)
            return;
        for (int i = 0; i < size; ++i)
        {
            if (c->count >= c->max)
            {
                c->overflow = 1;
                return;
            }
            c->out[c->count++] = arr[i];
        }
    }

    // liveness 内部要一片可增长缓冲，语义同 realloc：size==0 表示释放。
    // ★★ 这里踩过一个必崩的坑（2026-11-01 实锤）：判据写成 `if (!ptr || size == 0) return nullptr`
    //    就把**首次分配**也当成了释放。liveness 建构时是 reallocate(NULL, n) 拿对象数组，
    //    返回 NULL ⇒ object_array 为空 ⇒ 收集到第 2 个对象时写 object_array[1] ⇒ 写地址 0x8 ⇒ AV。
    //    正确判据只有一条：`ptr != nullptr && size == 0` 才是释放。
    void *LivenessRealloc(void *ptr, size_t size, void *ud)
    {
        (void)ud;
        Il2CppApi &api = ir::Api();
        if (size == 0)
        {
            if (ptr != nullptr && api.il2cpp_free)
                api.il2cpp_free(ptr);
            return nullptr;
        }
        if (!api.il2cpp_alloc)
        {
            ArkLogThrottled(0x67637265, "[gc] il2cpp_alloc 未解析，liveness 缓冲无法分配");
            return nullptr;
        }
        return api.il2cpp_alloc(size);
    }

    void OnWorldChanged() {}
} // namespace

int ArkGcEnumObjects(Il2CppClass *klass, Il2CppObject **out, int maxCount, int superClasses)
{
    if (!klass || !out || maxCount <= 0)
        return -3;

    const int mode = DetectLiveness();
    if (mode == LIV_NONE)
        return -1;

    if (!ir::Domain())
        return -2;

    Il2CppApi &api = ir::Api();
    CollectCtx ctx;
    ctx.out = out;
    ctx.max = maxCount;
    ctx.count = 0;
    ctx.overflow = 0;

    // superClasses：Unity 的 filter 语义就是"这个类**及其子类**"。
    // 传 0 时拿不到派生类实例（例如按 Character 筛会漏掉 Enemy）。
    // 当前两个 liveness 变体的第 2 参都是 max_object_count，子类开关藏在
    // 内部实现里 —— 所以这里统一按"含子类"用，不做区分，日志里说明。
    (void)superClasses;

    if (mode == LIV_NEW)
    {
        // ★ 停世界期间绝对不能调托管代码（runtime_invoke / value_box / object_new）。
        //   这里只收集裸指针，恢复世界之后再由调用方按需钉住。
        // ★ 每一步单独设"最近动作"：崩在 stop_gc_world 和崩在 finalize 是完全不同的病，
        //   汇总成一个 "obj.enum" 等于把四个可能的原因糊成一个，白丢信息。
        ArkGuardSetAction("gc.stopworld");
        api.il2cpp_stop_gc_world();

        ArkGuardSetAction("gc.alloc");
        void *state = api.il2cpp_unity_liveness_allocate_struct(klass, 0, CollectCallback, &ctx,
                                                               LivenessRealloc);
        if (state)
        {
            // 只走 from_statics：它从**静态区根**出发遍历整个 GC 图，能覆盖到场景里
            // 绝大多数常驻对象（管理器、关卡、单位表都是静态/单例可达的）。
            // ★ 不要额外调 calculation_from_root(nullptr)：那个函数会拿 root 当对象解引用，
            //   传空指针进去就是一次确定的 AV，且发生在 GC 世界停住的状态下，后果被放大。
            ArkGuardSetAction("gc.collect");
            api.il2cpp_unity_liveness_calculation_from_statics(state);

            ArkGuardSetAction("gc.finalize");
            api.il2cpp_unity_liveness_finalize(state);
        }

        ArkGuardSetAction("gc.startworld");
        api.il2cpp_start_gc_world();

        // ★ free_struct 必须在 start_gc_world **之后**（旧项目 el2cpp_tool 的顺序，实测可用）。
        //   它的析构里会碰 GC 内部结构；在"世界已停"的状态下释放，等于把 GC 自己的锁
        //   和内部数据在托管线程全部挂起的时刻拆掉。
        if (state)
        {
            ArkGuardSetAction("gc.freestruct");
            api.il2cpp_unity_liveness_free_struct(state);
        }

        if (!state)
        {
            Log("[gc] liveness_allocate_struct 返回空：这个类枚举不出实例");
            return -1;
        }
    }
    else // LIV_OLD
    {
        void *state = api.il2cpp_unity_liveness_calculation_begin(klass, 0, CollectCallback, &ctx,
                                                                  OnWorldChanged, OnWorldChanged);
        if (!state)
            return -1;
        api.il2cpp_unity_liveness_calculation_from_statics(state);
        api.il2cpp_unity_liveness_calculation_end(state);
    }

    if (ctx.overflow)
        ArkLogThrottled(0x67636F31, "[gc] 枚举结果超过上限（%d），只显示前 %d 个", maxCount, maxCount);

    return ctx.count;
}

// ================================================================ 钉住
// il2cpp_gchandle_new 的返回值 0 是**合法句柄**，但我们用 0 当"失败"。
// 所以对外句柄 = 内部句柄 + 1，对内存类比一致，调用方不需要知道。
unsigned int ArkGcPin(Il2CppObject *obj)
{
    if (!obj)
        return 0;
    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_gchandle_new)
        return 0;
    const uint32_t h = api.il2cpp_gchandle_new(obj, false);
    return h + 1;
}

void ArkGcUnpin(unsigned int h)
{
    if (!h)
        return;
    Il2CppApi &api = ir::Api();
    if (api.il2cpp_gchandle_free)
        api.il2cpp_gchandle_free(h - 1);
}

Il2CppObject *ArkGcTarget(unsigned int h)
{
    if (!h)
        return nullptr;
    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_gchandle_get_target)
        return nullptr;
    return api.il2cpp_gchandle_get_target(h - 1);
}
