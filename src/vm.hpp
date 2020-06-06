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


enum class InterpretResult
{
	OK,
	COMPILE_ERROR,
	RUNTIME_ERROR,
};

extern "C"
{
	void numberError(VM *vm, int32_t pc);
	void variableError(VM *vm, uint32_t pos);
	bool equal(Value *a, Value *b);
	int concatenate(VM *vm, Value *a, Value *b, Value *res);
	void print(Value *val);
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
	void runtimeError(int pc, Types ... args);

	std::array<CallFrame, STACK_MAX> m_frames;
	uint32_t m_frameCount = 0;
	CallFrame* m_frame = nullptr;

	FixedStack<Value, 125000> m_stack;
	HashTable<std::string, sObj*> m_strings;
	HashTable<std::string, Value> m_globals;
	std::vector<std::string> m_globalNames;
	std::vector<Value> m_globalValues;

	Compiler m_compiler;
	sObj *m_objects = nullptr;


	friend void numberError(VM *vm, int32_t pc);
	friend void variableError(VM *vm, uint32_t pos);
	friend bool equal(Value *a, Value *b);
	friend int concatenate(VM *vm, Value *a, Value *b, Value *res);
	friend void print(Value *val);

};

#include "vm.inl"

#endif //CPPLOX_VM_HPP
