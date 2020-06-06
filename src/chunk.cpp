//
// Created by juanb on 27/09/2018.
//

#include "chunk.hpp"

Chunk::Chunk()
{
	m_code.reserve(1024);
	m_constants.reserve(255);
	m_lines.reserve(1024);
}

Chunk::~Chunk()
{
	free();
}

void Chunk::free()
{
	m_code.clear();
	m_constants.clear();
	m_lines.clear();
	m_constantMap.clear();
}

void Chunk::write(uint8_t byte, uint32_t line)
{
	m_code.push_back(byte);
	m_lines.push_back(line);
}

size_t Chunk::addConstant(const Value &value)
{
	auto index = m_constantMap[value];
	if (!index.isUndefined())
		return static_cast<size_t>(index.asNumber());
	m_constants.push_back(value);
	auto newIndex = Value::Number(m_constants.size() - 1);
	m_constantMap[value] = newIndex;
	return m_constants.size() - 1;
}