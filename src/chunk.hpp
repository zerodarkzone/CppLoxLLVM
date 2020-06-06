//
// Created by juanb on 27/09/2018.
//

#ifndef CPPLOX_CHUNK_HPP
#define CPPLOX_CHUNK_HPP

#include <cstdint>
#include <vector>

#include "hashTable.hpp"
#include "value.hpp"

namespace OpCode
{
	enum : uint8_t
	{
#define OPCODE(name) name,
#include "op_codes.hpp"
#undef OPCODE
	};
}

class Chunk final
{
public:
	Chunk();
	~Chunk();
	void free();
	void write(uint8_t byte, uint32_t line);
	size_t addConstant(const Value &value);

	inline uint8_t get(size_t index) const;
	inline uint8_t &get(size_t index);
	inline uint8_t* code();
	inline Value getConstant(size_t index) const;
	inline Value* constants();
	inline uint32_t getLine(size_t index) const;

	inline size_t size() const;
	inline size_t constantsSize() const;


private:
	std::vector<uint8_t> m_code;
	std::vector<Value> m_constants;
	std::vector<uint32_t> m_lines;
	HashTable<Value, Value> m_constantMap;
};

#include "chunk.inl"

#endif //CPPLOX_CHUNK_HPP
