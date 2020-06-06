#include <iostream>
#include "Lox.hpp"
#include "scanner.hpp"
#include "chunk.hpp"

int main(int argc, const char **argv)
{
	std::cout << sizeof(Chunk) << std::endl;
	std::cout << alignof(Chunk) << std::endl;

	if (argc == 1)
	{
		Lox::repl();
	}
	else if (argc == 2)
	{
		Lox::runFile(argv[1]);
	}
	else
	{
		std::cerr << "Usage: CppLox [path]" << std::endl;
		exit(64);
	}

	return 0;
}