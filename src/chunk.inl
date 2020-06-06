inline uint8_t Chunk::get(size_t index) const
{
	return m_code[index];
}

inline uint8_t &Chunk::get(size_t index)
{
	return m_code[index];
}

inline uint8_t* Chunk::code()
{
	return m_code.data();
}

inline Value Chunk::getConstant(size_t index) const
{
	return m_constants[index];
}

inline Value* Chunk::constants()
{
	return m_constants.data();
}

inline uint32_t Chunk::getLine(size_t index) const
{
	return m_lines[index];
}

inline size_t Chunk::size() const
{
	return m_code.size();
}

inline size_t Chunk::constantsSize() const
{
	return m_constants.size();
}

