//
// Created by e10666a on 22/08/2019.
//

#ifndef CPPLOX_NATIVEFUNCTIONS_HPP
#define CPPLOX_NATIVEFUNCTIONS_HPP

#include <chrono>

#include "value.hpp"

inline Value clockNative(int argCount, Value *args)
{
	using namespace std::chrono;
	auto now = std::chrono::system_clock::now();

	time_t tnow = std::chrono::system_clock::to_time_t(now);
	tm *date = std::localtime(&tnow);
	date->tm_hour = 0;
	date->tm_min = 0;
	date->tm_sec = 0;
	auto midnight = std::chrono::system_clock::from_time_t(std::mktime(date));
	return Value::Number(duration<double>(duration_cast<milliseconds>(now - midnight)).count());
}

#endif //CPPLOX_NATIVEFUNCTIONS_HPP
