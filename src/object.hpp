//
// Created by juanb on 30/09/2018.
//

#ifndef CPPLOX_OBJECT_HPP
#define CPPLOX_OBJECT_HPP

#include <string>
#include <ostream>
#include <iostream>
#include <memory>
#include <type_traits>

#include "objType.hpp"
#include "chunk.hpp"

struct Value;

struct sObj
{
	sObj *next;
	size_t hash;
	ObjType type;

	explicit sObj(ObjType type);

	friend std::ostream &operator<<(std::ostream &os, const sObj &obj);
};

struct ObjString;

using JitFn = std::int32_t (*)(void* vm, Value* globals, Value* stack, std::int32_t* stack_top);
struct ObjFunction : sObj
{
	uint32_t arity{0};
	ObjString* name{nullptr};
	JitFn function{nullptr};
	Chunk chunk;

	ObjFunction();
	~ObjFunction();

private:
	//std::aligned_storage<120, 8>::type _chunk;
};

using NativeFn = Value (*)(int argCount, Value* args);

struct ObjNative : sObj
{
	NativeFn function;

	explicit ObjNative(NativeFn function);
};

struct ObjString : sObj
{
	std::string value;

	explicit ObjString(std::string value);
	explicit ObjString(std::string_view value);
};

#endif //CPPLOX_OBJECT_HPP
