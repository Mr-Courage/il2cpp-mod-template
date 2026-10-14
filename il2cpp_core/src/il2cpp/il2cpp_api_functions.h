// il2cpp_api_functions.h — IL2CPP 运行时导出函数表（平台无关）
// 通过 DO_API 宏展开为 Il2CppApi 结构体的成员声明。用法见 il2cpp_api.h。
// 每个函数指针在解析时可单独为空（版本差异），上层调用前应对其判空。

DO_API(int, il2cpp_init, (const char* domain_name));
DO_API(void, il2cpp_shutdown, ());
DO_API(Il2CppImage*, il2cpp_get_corlib, ());

DO_API(void*, il2cpp_alloc, (size_t size));
DO_API(void, il2cpp_free, (void* ptr));

// array
DO_API(Il2CppClass*, il2cpp_array_class_get, (Il2CppClass * element_class, uint32_t rank));
DO_API(uint32_t, il2cpp_array_length, (_Il2CppArray * array));
DO_API(_Il2CppArray*, il2cpp_array_new, (Il2CppClass * elementTypeInfo, il2cpp_array_size_t length));
DO_API(int, il2cpp_array_element_size, (Il2CppClass * array_class));

// assembly / image
DO_API(Il2CppImage*, il2cpp_assembly_get_image, (Il2CppAssembly * assembly));
DO_API(Il2CppAssembly*, il2cpp_image_get_assembly, (Il2CppImage * image));
DO_API(const char*, il2cpp_image_get_name, (Il2CppImage * image));
DO_API(size_t, il2cpp_image_get_class_count, (Il2CppImage * image));
DO_API(Il2CppClass*, il2cpp_image_get_class, (Il2CppImage * image, size_t index));

// class
DO_API(void, il2cpp_class_for_each, (void(*klassReportFunc)(Il2CppClass* klass, void* userData), void* userData));
DO_API(bool, il2cpp_class_is_valuetype, (Il2CppClass * klass));
DO_API(bool, il2cpp_class_is_enum, (Il2CppClass * klass));
DO_API(bool, il2cpp_class_is_interface, (Il2CppClass * klass));
DO_API(bool, il2cpp_class_is_generic, (Il2CppClass * klass));
DO_API(Il2CppClass*, il2cpp_class_from_name, (Il2CppImage * image, const char* namespaze, const char *name));
DO_API(Il2CppClass*, il2cpp_class_from_type, (Il2CppType * type));
DO_API(Il2CppClass*, il2cpp_class_from_il2cpp_type, (Il2CppType * type));
DO_API(Il2CppClass*, il2cpp_class_get_parent, (Il2CppClass * klass));
DO_API(const char*, il2cpp_class_get_name, (Il2CppClass * klass));
DO_API(char*, il2cpp_class_get_namespace, (Il2CppClass * klass));
DO_API(Il2CppImage*, il2cpp_class_get_image, (Il2CppClass * klass));
DO_API(Il2CppType*, il2cpp_class_get_type, (Il2CppClass * klass));
DO_API(Il2CppObject*, il2cpp_type_get_object, (const Il2CppType * type));
DO_API(int32_t, il2cpp_class_instance_size, (Il2CppClass * klass));
DO_API(int32_t, il2cpp_class_value_size, (Il2CppClass * klass, uint32_t * align));
DO_API(void*, il2cpp_class_get_static_field_data, (Il2CppClass * klass));
DO_API(uint32_t, il2cpp_class_get_type_token, (Il2CppClass * klass));
DO_API(FieldInfo*, il2cpp_class_get_field_from_name, (Il2CppClass * klass, const char *name));
DO_API(FieldInfo*, il2cpp_class_get_fields, (Il2CppClass * klass, void **iter));
DO_API(MethodInfo*, il2cpp_class_get_method_from_name, (Il2CppClass * klass, const char* name, int argsCount));
// ★ 枚举类的**全部**方法（含泛型定义与非泛型重载）。用法：void* it=NULL; while((m=get_methods(k,&it))){...}
// 为什么必需（2026-10-12 实测卡点）：`get_method_from_name` 只返回**第一个**同名同参数数的方法，
// 而 ABResourceManager 上 `Load<T>(string)`（泛型定义，methodPointer=NULL，直调必崩）排在
// 非泛型 `Load(string)` 前面 ⇒ 按名取只拿到泛型定义。枚举才能挑出 methodPointer 非空的那个。
DO_API(MethodInfo*, il2cpp_class_get_methods, (Il2CppClass * klass, void **iter));

// domain
DO_API(Il2CppDomain*, il2cpp_domain_get, ());
DO_API(Il2CppAssembly*, il2cpp_domain_assembly_open, (Il2CppDomain * domain, const char* name));
DO_API(Il2CppAssembly**, il2cpp_domain_get_assemblies, (Il2CppDomain * domain, size_t * size));

// field
DO_API(const char*, il2cpp_field_get_name, (FieldInfo * field));
DO_API(Il2CppClass*, il2cpp_field_get_parent, (FieldInfo * field));
DO_API(size_t, il2cpp_field_get_offset, (FieldInfo * field));
DO_API(Il2CppType*, il2cpp_field_get_type, (FieldInfo * field));
DO_API(void, il2cpp_field_get_value, (Il2CppObject * obj, FieldInfo * field, void *value));
DO_API(Il2CppObject*, il2cpp_field_get_value_object, (FieldInfo * field, Il2CppObject * obj));
DO_API(void, il2cpp_field_set_value, (Il2CppObject * obj, FieldInfo * field, void *value));
DO_API(void, il2cpp_field_static_get_value, (FieldInfo * field, void *value));
DO_API(void, il2cpp_field_static_set_value, (FieldInfo * field, void *value));
DO_API(void, il2cpp_field_set_value_object, (Il2CppObject * instance, FieldInfo * field, Il2CppObject * value));
DO_API(bool, il2cpp_field_is_literal, (FieldInfo * field));

// gchandle
DO_API(uint32_t, il2cpp_gchandle_new, (Il2CppObject * obj, bool pinned));
DO_API(Il2CppObject*, il2cpp_gchandle_get_target, (uint32_t gchandle));
DO_API(void, il2cpp_gchandle_free, (uint32_t gchandle));

// method
DO_API(Il2CppType*, il2cpp_method_get_return_type, (MethodInfo * method));
DO_API(Il2CppClass*, il2cpp_method_get_declaring_type, (MethodInfo * method));
DO_API(const char*, il2cpp_method_get_name, (MethodInfo * method));
DO_API(bool, il2cpp_method_is_instance, (MethodInfo * method));
DO_API(uint32_t, il2cpp_method_get_param_count, (MethodInfo * method));

// property
DO_API(const char*, il2cpp_property_get_name, (PropertyInfo * prop));
DO_API(MethodInfo*, il2cpp_property_get_get_method, (PropertyInfo * prop));
// [2026-10-10 新增] 取属性（PropertyInfo）。读 C# 属性（public T Foo { get; }）时
// 需要三步：class_get_property_from_name -> property_get_get_method -> runtime_invoke。
// 不走属性就只能去啃同名私有字段偏移，脆弱且易碎（热更改名/加字段就失效）。
// 顺带用 invoke 而不是直调getter RVA：invoke 负责线程附加与异常处理。
DO_API(PropertyInfo*, il2cpp_class_get_property_from_name, (Il2CppClass * klass, const char * name));
DO_API(MethodInfo*, il2cpp_property_get_set_method, (PropertyInfo * prop));

// object
DO_API(Il2CppClass*, il2cpp_object_get_class, (Il2CppObject * obj));
DO_API(MethodInfo*, il2cpp_object_get_virtual_method, (Il2CppObject * obj, MethodInfo * method));
DO_API(Il2CppObject*, il2cpp_object_new, (Il2CppClass * klass));
DO_API(void*, il2cpp_object_unbox, (Il2CppObject * obj));
DO_API(Il2CppObject*, il2cpp_value_box, (Il2CppClass * klass, void* data));

// runtime invoke
DO_API(Il2CppObject*, il2cpp_runtime_invoke, (MethodInfo * method, void *obj, void **params, Il2CppException **exc));
DO_API(Il2CppObject*, il2cpp_runtime_invoke_convert_args, (MethodInfo * method, void *obj, Il2CppObject **params, int paramCount, Il2CppException **exc));
DO_API(void, il2cpp_runtime_class_init, (Il2CppClass * klass));
DO_API(void, il2cpp_runtime_object_init, (Il2CppObject * obj));

// string
DO_API(int32_t, il2cpp_string_length, (Il2CppString * str));
DO_API(Il2CppChar*, il2cpp_string_chars, (Il2CppString * str));
DO_API(Il2CppString*, il2cpp_string_new, (const char* str));
DO_API(Il2CppString*, il2cpp_string_new_utf16, (const Il2CppChar * text, int32_t len));

// thread
DO_API(Il2CppThread*, il2cpp_thread_current, ());
DO_API(Il2CppThread*, il2cpp_thread_attach, (Il2CppDomain * domain));
DO_API(void, il2cpp_thread_detach, (Il2CppThread * thread));

// type
DO_API(int, il2cpp_type_get_type, (Il2CppType * type));
DO_API(Il2CppClass*, il2cpp_type_get_class_or_element_class, (Il2CppType * type));
DO_API(const char*, il2cpp_type_get_name, (Il2CppType * type));

// exception
DO_API(void, il2cpp_format_exception, (Il2CppException * ex, char* message, int message_size));
DO_API(void, il2cpp_format_stack_trace, (Il2CppException * ex, char* output, int output_size));