//
// Created by juanb on 30/09/2018.
//

#ifndef CPPLOX_LOX_HPP
#define CPPLOX_LOX_HPP


#include <iostream>
#include <fstream>
#include <sstream>
#include "vm.hpp"

class Lox
{
public:
	static void repl()
	{
		std::string line;
		while (true)
		{
			std::cout << "> ";
			std::getline(std::cin, line);
			if (std::cin.eof())
			{
				std::cout << std::endl;
				break;
			}

			m_vm.interpret(line + "\n");
		}
	}

	static void runFile(const std::string &path)
	{
		std::ifstream file;
		file.open(path, std::ios::in | std::ios::binary);
		if (!file.good())
		{
			std::cerr << "Could not open or read the file \"" << path << "\"." << std::endl;
			exit(47);
		}

		std::stringstream ss;
		ss << file.rdbuf();

		InterpretResult result = m_vm.interpret(ss.str());
		file.close();

		if (result == InterpretResult::COMPILE_ERROR)
			exit(65);
		if (result == InterpretResult::RUNTIME_ERROR)
			exit(70);
	}

private:
	static VM m_vm;
};


#endif //CPPLOX_LOX_HPP
