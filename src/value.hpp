//
// Created by juanb on 27/09/2018.
//

#ifndef CPPLOX_VALUE_HPP
#define CPPLOX_VALUE_HPP

#include <variant>
#include <ostream>

#include "objType.hpp"

struct sObj;
struct ObjString;
struct ObjFunction;
struct ObjNative;

enum class ValueType : uint8_t
{
	BOOL,
	NIL,
	NUMBER,
	OBJ,
	UNDEFINED,
};

using Obj = sObj;

class Value
{
public:

	Value();

	static Value Bool(bool value);
	static Value Nil();
	static Value Number(double value);
	static Value Object(Obj *value);
	static Value Undefined();

	bool asBool() const;
	double asNumber() const;
	Obj *asObj() const;
	ObjString *asObjString() const;
	std::string_view asString() const;
	ObjFunction *asObjFunction() const;
	ObjNative *asObjNative() const;

	bool isBool() const;
	bool isNil() const;
	bool isNumber() const;
	bool isObj() const;
	bool isObjType(ObjType type) const;
	bool isObjString() const;
	bool isObjFunction() const;
	bool isObjNative() const;
	bool isUndefined() const;
	ValueType type() const;
	ObjType objType() const;

	friend std::ostream &operator<<(std::ostream &os, const Value &value);
	friend bool operator==(const Value &lhs, const Value &rhs);

private:
	template <typename _T>
	explicit Value(_T value);
	ValueType m_type;
	union As{
		bool boolean;
		double number;
		Obj* obj;

		As() = default;
		explicit As(bool boolean) : boolean(boolean) {}
		explicit As(double number) : number(number) {}
		explicit As(Obj* obj) : obj(obj) {}
	} m_as{};
};

#include "value.inl"

#endif //CPPLOX_VALUE_HPP
