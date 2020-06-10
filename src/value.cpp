//
// Created by juanb on 6/06/2020.
//

#include "object.hpp"
#include "value.hpp"

size_t obj_hash(sObj* obj)
{
	return obj->hash;
}

Value Value::Bool(bool value)
{
	return Value(value);
}

Value Value::Nil()
{
	return Value(nullptr);
}

Value Value::Number(double value)
{
	return Value(value);
}

Value Value::Object(Obj *value)
{
	return Value(value);
}

Value Value::Undefined()
{
	return Value();
}

bool Value::asBool() const
{
	return m_as.boolean;
}

double Value::asNumber() const
{
	return m_as.number;
}

Obj *Value::asObj() const
{
	return m_as.obj;
}

ObjString *Value::asObjString() const
{
	return static_cast<ObjString*>(asObj());
}

std::string_view Value::asString() const
{
	return asObjString()->value;
}

ObjFunction *Value::asObjFunction() const
{
	return static_cast<ObjFunction*>(asObj());
}

ObjNative *Value::asObjNative() const
{
	return static_cast<ObjNative*>(asObj());
}

ObjType Value::objType() const
{
	return asObj()->type;
}

std::ostream &operator<<(std::ostream &os, const Value &value)
{
	switch (value.m_type)
	{
		case ValueType::BOOL:
			os << std::boolalpha << value.m_as.boolean;
			break;
		case ValueType::NIL:
			os << "nil";
			break;
		case ValueType::NUMBER:
			os << value.m_as.number;
			break;
		case ValueType::OBJ:
			os << *value.m_as.obj;
			break;
		default:
			os << "undefined";
			break;
	}

	return os;
}