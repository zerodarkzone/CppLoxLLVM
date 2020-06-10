//
// Created by juanb on 30/09/2018.
//

#ifndef CPPLOX_MEMORY_HPP
#define CPPLOX_MEMORY_HPP

#include "object.hpp"

struct sObj;
class VM;

class Memory
{
public:
	template <typename _T, typename ... _Types>
	static _T* createObject(VM *vm, _Types ... args);
	static ObjString* createString(VM *vm, const std::string &str);
	static ObjString* createString(VM *vm, std::string_view str);
	static ObjFunction* createFunction(VM *vm);
	static ObjNative* createNative(VM *vm, NativeFn function);
	static void freeObjects(VM *vm);
private:
	static void destroyObject(sObj *obj);
};

#include "memory.inl"

using Mem = Memory;

#endif //CPPLOX_MEMORY_HPP
