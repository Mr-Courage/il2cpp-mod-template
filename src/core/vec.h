// vec.h — 极简 POD 向量（无构造/析构、无静态初始化）
// ============================================================================
// 为什么不用 std::vector：
//   本 DLL 走手动映射，映射器不跑 .CRT$XCU 里的静态构造器。
//   任何**静态存储期**的非平凡对象（全局 std::vector / 函数内 static std::string…）
//   都不会被构造，内存里是纯 0 —— 用起来就是空指针解引用。
//   所以这里只提供 malloc/realloc 支撑的 POD 容器：T 必须可 memcpy。
//
// 约束：T 里不许有指针所有权语义。存指针就当裸值存，手动管。
// ============================================================================
#pragma once

#include <cstdlib>
#include <cstring>

template <typename T>
struct Vec
{
    T  *data;
    int count;
    int cap;

    void Init()              { data = nullptr; count = 0; cap = 0; }
    void Free()              { if (data) ::free(data); data = nullptr; count = 0; cap = 0; }
    void Clear()             { count = 0; }
    bool Empty() const       { return count == 0; }
    T   *At(int i)           { return (i >= 0 && i < count) ? &data[i] : nullptr; }
    const T *At(int i) const { return (i >= 0 && i < count) ? &data[i] : nullptr; }

    // 失败（OOM）时返回 nullptr，调用方必须判空 —— 这里不抛异常（-fno-exceptions）。
    T *Push()
    {
        if (count >= cap && !Grow(cap ? cap * 2 : 16))
            return nullptr;
        T *slot = &data[count++];
        memset(slot, 0, sizeof(T));
        return slot;
    }

    bool Push(const T &v)
    {
        T *slot = Push();
        if (!slot) return false;
        *slot = v;
        return true;
    }

    // 只保证「往大了扩」时内容不丢；缩容不动已有元素。
    bool Reserve(int n)
    {
        if (n <= cap) return true;
        int nc = cap ? cap : 16;
        while (nc < n) nc *= 2;
        return Grow(nc);
    }

    // 删除第 i 个（用最后一个顶上，顺序会变）。
    void SwapRemove(int i)
    {
        if (i < 0 || i >= count) return;
        data[i] = data[count - 1];
        --count;
    }

private:
    bool Grow(int n)
    {
        void *p = ::realloc(data, (size_t)n * sizeof(T));
        if (!p) return false;
        data = (T *)p;
        cap = n;
        return true;
    }
};
