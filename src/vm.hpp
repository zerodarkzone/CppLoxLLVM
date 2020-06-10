//
// Created by juanb on 29/09/2018.
//

#ifndef CPPLOX_VM_HPP
#define CPPLOX_VM_HPP

#include <iostream>

#include "compiler.hpp"
#include "chunk.hpp"
#include "stack.hpp"
#include "hashTable.hpp"
#include "object.hpp"
#include "llvm_jit_utils.hpp"

enum class InterpretResult
{
	OK,
	COMPILE_ERROR,
	RUNTIME_ERROR,
};

extern "C"
{
	void __declspec(dllexport) callError(VM *vm, uint32_t pc);
	void __declspec(dllexport) numberError(VM *vm, uint32_t pc);
	void __declspec(dllexport) variableError(VM *vm, uint32_t pos, uint32_t pc);
	void __declspec(dllexport) arityError(VM *vm, uint32_t arity, uint32_t arg_count, uint32_t pc);
	bool __declspec(dllexport) equal(Value *a, Value *b);
	int __declspec(dllexport) concatenate(VM *vm, Value *out, Value *a, Value *b, uint32_t pc);
	void __declspec(dllexport) print(Value* val);
	void __declspec(dllexport) callNative(NativeFn fun, uint32_t argCount, Value *args, Value *out);
}

struct CallFrame
{
	ObjFunction* function;
	uint8_t* ip;
	Value* slots;
};

class VM
{
	static constexpr uint32_t FRAMES_MAX = 2048;
	static constexpr uint32_t STACK_MAX = 32 * FRAMES_MAX;
public:
	VM();
	~VM();
	InterpretResult interpret(const std::string &source);
	HashTable<std::string, Value>& globalsMap();
	std::vector<std::string>& globalNames();
	std::vector<Value>& globalValues();

	friend class Memory;

private:
	bool call(ObjFunction* function, int argCount);
	bool callValue(Value callee, int argCount);
	void defineNative(std::string_view name, NativeFn function);
	InterpretResult run();
	void prepareJit();
	InterpretResult runJitted();
	uint8_t readByte();
	uint16_t readShort();
	uint32_t readLong();
	Value readConstant();
	Value readConstantLong();
	bool isFalsey(const Value &value);
	void debugTrace();

	template <typename _Val, typename _Op>
	InterpretResult binaryOp(Value (*valFunc)(_Val), _Op op);
	template <typename _Val_a, typename _Val_b>
	void concatenate();
	template <typename ... Types>
	void runtimeError(Types ... args);

	template <typename ... Types>
	void runtimeError(uint32_t pc, Types ... args);

	std::array<CallFrame, STACK_MAX> m_frames;
	uint32_t m_frameCount = 0;
	CallFrame* m_frame = nullptr;

	FixedStack<Value, 125000> m_stack;
	HashTable<std::string, ObjString*> m_strings;
	HashTable<std::string, Value> m_globals;
	std::vector<std::string> m_globalNames;
	std::vector<Value> m_globalValues;

	Compiler m_compiler;
	Obj *m_objects = nullptr;
	std::unique_ptr<SimpleOrcJIT> m_jit;


	friend void callError(VM *vm, uint32_t pc);
	friend void numberError(VM *vm, uint32_t pc);
	friend void variableError(VM *vm, uint32_t pos, uint32_t pc);
	friend void arityError(VM *vm, uint32_t arity, uint32_t arg_count, uint32_t pc);
	friend bool equal(Value *a, Value *b);
	friend int concatenate(VM *vm, Value *out, Value *a, Value *b, uint32_t pc);
	friend void print(Value* val);

};

#include "vm.inl"

#endif //CPPLOX_VM_HPP
