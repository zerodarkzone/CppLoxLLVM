//
// Created by juanb on 30/09/2018.
//

#include "object.hpp"
#include "chunk.hpp"

sObj::sObj(ObjType type) : type(type), next(nullptr), hash(reinterpret_cast<size_t>(this))
{}

std::ostream &operator<<(std::ostream &os, const sObj &obj)
{
	switch (obj.type)
	{
		case ObjType::FUNCTION:
			os << "<fn " << static_cast<const ObjFunction*>(&obj)->name->value << ">";
			break;
		case ObjType::NATIVE:
			os << "<native fn>";
			break;
		case ObjType::STRING:
			os << static_cast<const ObjString*>(&obj)->value;
			break;
	}
	return os;
}


ObjFunction::ObjFunction() : sObj(ObjType::FUNCTION), arity(0), /*chunk(*new (&_chunk) Chunk()),*/ name(nullptr)
{}

ObjFunction::~ObjFunction() {
}

ObjNative::ObjNative(NativeFn function) : sObj(ObjType::NATIVE), function(function)
{}

ObjString::ObjString(std::string value) : sObj(ObjType::STRING), value(std::move(value))
{}

ObjString::ObjString(std::string_view value) : sObj(ObjType::STRING), value(value)
{}
