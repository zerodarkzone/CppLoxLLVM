//
// Created by juanb on 27/09/2018.
//

#ifndef CPPLOX_DEBUG_HPP
#define CPPLOX_DEBUG_HPP

#include <string>

#include "chunk.hpp"

void disassembleChunk(const Chunk &chunk, std::string name);
size_t disassembleInstruction(const Chunk &chunk, size_t offset);

#endif //CPPLOX_DEBUG_HPP
