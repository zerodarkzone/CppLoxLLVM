#include "vm.hpp"

template<typename _T, typename... _Types>
sObj* Memory::createObject(VM *vm, _Types... args)
{
	auto obj = new _T(args...);
	obj->next = vm->m_objects;
	vm->m_objects = obj;
	return obj;
}

inline ObjFunction* Memory::createFunction(VM *vm)
{
	auto obj_func = createObject<ObjFunction>(vm);
	return static_cast<ObjFunction*>(obj_func);
}

inline ObjNative* Memory::createNative(VM *vm, NativeFn function)
{
	auto obj_native = createObject<ObjNative>(vm, function);
	return static_cast<ObjNative*>(obj_native);
}

inline ObjString* Memory::createString(VM *vm, const std::string &str)
{
	auto interned = vm->m_strings[str];
	if (interned)
		return static_cast<ObjString*>(interned);

	auto obj_str = createObject<ObjString>(vm, str);
	vm->m_strings[str] = obj_str;
	return static_cast<ObjString*>(obj_str);
}

inline ObjString* Memory::createString(VM *vm, std::string_view str)
{
	return createString(vm, std::string(str));
}
