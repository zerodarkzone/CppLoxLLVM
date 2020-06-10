#include<sstream>

#include "memory.hpp"
#include "debug.hpp"

inline uint8_t VM::readByte()
{
	return *m_frame->ip++;
}

inline uint16_t VM::readShort()
{
	m_frame->ip += 2;
	return static_cast<uint16_t>(*(m_frame->ip - 2)) |
			static_cast<uint16_t>(*(m_frame->ip - 1) << 8u);
}

inline uint32_t VM::readLong()
{
	return static_cast<uint32_t>(readByte()) |
		static_cast<uint32_t>(readByte() << 8u) |
		static_cast<uint32_t>(readByte() << 16u);
}

inline Value VM::readConstant()
{
	return m_frame->function->chunk.getConstant(readByte());
}

inline Value VM::readConstantLong()
{
	return m_frame->function->chunk.getConstant(readLong());
}

inline bool VM::isFalsey(const Value &value)
{
	return value.isNil() || (value.isBool() && !value.asBool());
}

inline void VM::debugTrace()
{
    std::cout << "          " << m_stack << std::endl;
    disassembleInstruction(m_frame->function->chunk, (m_frame->ip - m_frame->function->chunk.code()));
}

template <typename _Val, typename _Op>
inline InterpretResult VM::binaryOp(Value (*valFunc)(_Val), _Op op)
{
	auto b = m_stack.pop();
	auto a = m_stack.top();

	if (!a.isNumber() || !b.isNumber())
	{
		runtimeError("Operands must be numbers.");
		return InterpretResult::RUNTIME_ERROR;
	}
	m_stack.top() = valFunc(op(a.asNumber(), b.asNumber()));
	return InterpretResult::OK;
}

template <typename _Val_a, typename _Val_b>
inline void VM::concatenate()
{
	if constexpr (std::is_same<std::string, _Val_a>::value)
	{
		if constexpr (std::is_same<std::string, _Val_b>::value)
		{
			auto b = m_stack.pop().asObjString()->value;
			auto a = m_stack.top().asObjString()->value;
			m_stack.top() = Value::Object(Memory::createString(this, a + b));
		}
		else if constexpr (std::is_same<double, _Val_b>::value)
		{
			auto b = m_stack.pop().asNumber();
			auto a = m_stack.top().asObjString()->value;
			char buff[1024];
			std::sprintf(buff, "%g", b);
			m_stack.top() = Value::Object(Memory::createString(this, a + std::string(buff)));
		}
	}
	else if constexpr (std::is_same<double, _Val_a>::value)
	{
		if constexpr (std::is_same<std::string, _Val_b>::value)
		{
			auto b = m_stack.pop().asObjString()->value;
			auto a = m_stack.top().asNumber();
			char buff[1024];
			std::sprintf(buff, "%g", a);
			m_stack.top() = Value::Object(Memory::createString(this, std::string(buff) + b));
		}
		else if constexpr (std::is_same<double, _Val_b>::value)
		{
			auto b = m_stack.pop().asNumber();
			auto a = m_stack.top().asNumber();
			m_stack.top() = Value::Number(a + b);
		}
	}
}

template <class... Types>
inline void VM::runtimeError(Types ... args)
{
	auto argList = {args...};
	for (auto &&arg: argList)
	{
		std::cerr << arg;
	}
	std::cerr << std::endl;
	auto index = static_cast<size_t>(m_frame->ip - m_frame->function->chunk.code());
	std::cerr << "[line " << m_frame->function->chunk.getLine(index) << "]" << std::endl;
	m_stack.reset();
}

template <class... Types>
inline void VM::runtimeError(uint32_t pc, Types ... args)
{
	auto argList = {args...};
	for (auto &&arg: argList)
	{
		std::cerr << arg;
	}
	std::cerr << std::endl;
	auto index = pc;
	std::cerr << "[line " << m_frame->function->chunk.getLine(index) << "]" << std::endl;
	m_stack.reset();
}
