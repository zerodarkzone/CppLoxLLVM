//
// Created by juanb on 27/09/2018.
//

#include <iostream>
#include <iomanip>

#include "debug.hpp"

void disassembleChunk(const Chunk &chunk, std::string name)
{
	std::cout << "== " << name << " ==" << "\n";

	for (size_t i = 0u, size = chunk.size(); i < size;)
	{
		i = disassembleInstruction(chunk, i);
	}
	std::cout.flush();
}

static size_t simpleInstruction(const std::string &name, size_t offset)
{
	std::cout << name << "\n";
	return offset + 1;
}

static size_t byteInstruction(const std::string &name, const Chunk &chunk, size_t offset) {
	uint16_t slot = chunk.get(offset + 1);
	std::cout << std::setiosflags(std::ios::left) << std::setw(16) << std::setfill(' ') << name;
	std::cout << std::resetiosflags(std::ios::left) << std::setw(4) << slot << "\n";
	return offset + 2;
}

static size_t shortInstruction(const std::string &name, const Chunk &chunk, size_t offset) {
	uint16_t slot = chunk.get(offset + 1u) |
		static_cast<uint16_t>(chunk.get(offset + 2u) << 8u);
	std::cout << std::setiosflags(std::ios::left) << std::setw(16) << std::setfill(' ') << name;
	std::cout << std::resetiosflags(std::ios::left) << std::setw(4) << slot << "\n";
	return offset + 3;
}

static size_t longInstruction(const std::string &name, const Chunk &chunk, size_t offset) {
	uint32_t slot = chunk.get(offset + 1u) |
		static_cast<uint32_t>(chunk.get(offset + 2u) << 8u) |
		static_cast<uint32_t>(chunk.get(offset + 3u) << 16u);
	std::cout << std::setiosflags(std::ios::left) << std::setw(16) << std::setfill(' ') << name;
	std::cout << std::resetiosflags(std::ios::left) << std::setw(4) << slot << "\n";
	return offset + 4;
}

static size_t jumpInstruction(const std::string &name, int sign, const Chunk &chunk, size_t offset) {
	uint16_t jump = chunk.get(offset + 1u) |
			static_cast<uint16_t>(chunk.get(offset + 2u) << 8u);
	std::cout << std::setiosflags(std::ios::left) << std::setw(16) << std::setfill(' ') << name;
	std::cout << std::resetiosflags(std::ios::left) << std::setw(4) << offset << " ";
	std::cout << "-> " << offset + 3 + sign * jump << "\n";
	return offset + 3;
}

static size_t constantInstruction(const std::string &name, const Chunk &chunk, size_t offset)
{
	uint16_t constant = chunk.get(offset + 1u);
	std::cout << std::setiosflags(std::ios::left) << std::setw(16) << std::setfill(' ') << name;
	std::cout << std::resetiosflags(std::ios::left) << std::setw(4) << constant << " '";
	std::cout << chunk.getConstant(constant) << "'" << "\n";
	return offset + 2;
}

static size_t longConstantInstruction(const std::string &name, const Chunk &chunk, size_t offset)
{
	uint32_t constant = chunk.get(offset + 1u) |
		static_cast<uint32_t>(chunk.get(offset + 2u) << 8u) |
		static_cast<uint32_t>(chunk.get(offset + 3) << 16u);
	std::cout << std::setiosflags(std::ios::left) << std::setw(16) << std::setfill(' ') << name;
	std::cout << std::resetiosflags(std::ios::left) << std::setw(4) << constant << " '";
	std::cout << chunk.getConstant(constant) << "'" << "\n";
	return offset + 4;
}

size_t disassembleInstruction(const Chunk &chunk, size_t offset)
{
	std::cout << std::setw(4) << std::setfill('0') << offset << " ";
	if (offset > 0 && chunk.getLine(offset) == chunk.getLine(offset - 1))
	{
		std::cout << "   | ";
	}
	else
	{
		std::cout << std::setw(4) << std::setfill(' ') << chunk.getLine(offset) << " ";
	}

	auto instruction = chunk.get(offset);
	switch (instruction)
	{
		case OpCode::CONSTANT:
			return constantInstruction("OP_CONSTANT", chunk, offset);
		case OpCode::CONSTANT_LONG:
			return longConstantInstruction("OP_CONSTANT_LONG", chunk, offset);
		case OpCode::NIL:
			return simpleInstruction("OP_NIL", offset);
		case OpCode::TRUE:
			return simpleInstruction("OP_TRUE", offset);
		case OpCode::FALSE:
			return simpleInstruction("OP_FALSE", offset);
		case OpCode::POP:
			return simpleInstruction("OP_POP", offset);
		case OpCode::DUP:
			return simpleInstruction("OP_DUP", offset);
		case OpCode::GET_LOCAL:
			return byteInstruction("OP_GET_LOCAL", chunk, offset);
		case OpCode::GET_LOCAL_SHORT:
			return shortInstruction("OP_GET_LOCAL_SHORT", chunk, offset);
		case OpCode::SET_LOCAL:
			return byteInstruction("OP_SET_LOCAL", chunk, offset);
		case OpCode::SET_LOCAL_SHORT:
			return shortInstruction("OP_SET_LOCAL_SHORT", chunk, offset);
		case OpCode::GET_GLOBAL:
			return byteInstruction("OP_GET_GLOBAL", chunk, offset);
		case OpCode::GET_GLOBAL_LONG:
			return longInstruction("OP_GET_GLOBAL_LONG", chunk, offset);
		case OpCode::DEFINE_GLOBAL:
			return byteInstruction("OP_DEFINE_GLOBAL", chunk, offset);
		case OpCode::DEFINE_GLOBAL_LONG:
			return longInstruction("OP_DEFINE_GLOBAL_LONG", chunk, offset);
		case OpCode::SET_GLOBAL:
			return byteInstruction("OP_SET_GLOBAL", chunk, offset);
		case OpCode::SET_GLOBAL_LONG:
			return longInstruction("OP_SET_GLOBAL_LONG", chunk, offset);
		case OpCode::EQUAL:
			return simpleInstruction("OP_EQUAL", offset);
		case OpCode::GREATER:
			return simpleInstruction("OP_GREATER", offset);
		case OpCode::LESS:
			return simpleInstruction("OP_LESS", offset);
		case OpCode::ADD:
			return simpleInstruction("OP_ADD", offset);
		case OpCode::SUBTRACT:
			return simpleInstruction("OP_SUBTRACT", offset);
		case OpCode::MULTIPLY:
			return simpleInstruction("OP_MULTIPLY", offset);
		case OpCode::DIVIDE:
			return simpleInstruction("OP_DIVIDE", offset);
		case OpCode::MODULO:
			return simpleInstruction("OP_MODULO", offset);
		case OpCode::NOT:
			return simpleInstruction("OP_NOT", offset);
		case OpCode::NEGATE:
			return simpleInstruction("OP_NEGATE", offset);
		case OpCode::PRINT:
			return simpleInstruction("OP_PRINT", offset);
		case OpCode::JUMP:
			return jumpInstruction("OP_JUMP", 1, chunk, offset);
		case OpCode::JUMP_IF_FALSE:
			return jumpInstruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
		case OpCode::JUMP_IF_TRUE:
			return jumpInstruction("OP_JUMP_IF_TRUE", 1, chunk, offset);
		case OpCode::JUMP_BACK:
			return jumpInstruction("OP_JUMP_BACK", -1, chunk, offset);
		case OpCode::CALL:
			return byteInstruction("OP_CALL", chunk, offset);
		case OpCode::RETURN:
			return simpleInstruction("OP_RETURN", offset);
		default:
			std::cout << "Unknown opcode " << instruction << "\n";
			return offset + 1;
	}
}
