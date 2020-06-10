//
// Created by juanb on 30/09/2018.
//

#include "memory.hpp"
#include "vm.hpp"
#include "object.hpp"

void Memory::freeObjects(VM *vm)
{
	auto object = vm->m_objects;
	while (object)
	{
		auto next = object->next;
		destroyObject(object);
		object = next;
	}

}

void Memory::destroyObject(sObj *obj)
{
	switch (obj->type)
	{
		case ObjType::FUNCTION: {
			auto function = static_cast<ObjFunction *>(obj);
			function->function = nullptr;
			delete function;
			break;
		}
		case ObjType::NATIVE: {
			delete static_cast<ObjNative *>(obj);
			break;
		}
		case ObjType::STRING: {
			delete static_cast<ObjString *>(obj);
			break;
		}
	}
}

