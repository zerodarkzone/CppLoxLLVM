template <typename _T>
inline Value::Value(_T value)
{
	if constexpr (std::is_same<bool, _T>::value)
	{
		m_type = ValueType::BOOL;
		m_as.boolean = value;
	}
	else if constexpr (std::is_same<double, _T>::value)
	{
		m_type = ValueType::NUMBER;
		m_as.number = value;
	}
	else if constexpr (std::is_same<Obj*, _T>::value)
	{
		m_type = ValueType::OBJ;
		m_as.obj = value;
	}
	else
	{
		m_type = ValueType::NIL;
		m_as.number = 0;
	}
}

inline Value::Value() : m_type(ValueType::UNDEFINED), m_as( 0.0 )
{}



inline bool Value::isBool() const
{
	return m_type == ValueType::BOOL;
}

inline bool Value::isNil() const
{
	return m_type == ValueType::NIL;
}

inline bool Value::isNumber() const
{
	return m_type == ValueType::NUMBER;
}

inline bool Value::isObj() const
{
	return m_type == ValueType::OBJ;
}

inline bool Value::isObjType(ObjType type) const
{
	return isObj() && objType() == type;
}

inline bool Value::isObjString() const
{
	return isObjType(ObjType::STRING);
}

inline bool Value::isObjFunction() const
{
	return isObjType(ObjType::FUNCTION);
}

inline bool Value::isObjNative() const
{
	return isObjType(ObjType::NATIVE);
}

inline bool Value::isUndefined() const
{
	return m_type == ValueType::UNDEFINED;
}

inline ValueType Value::type() const
{
	return m_type;
}

inline bool operator==(const Value &lhs, const Value &rhs)
{
	if (lhs.m_type != rhs.m_type)
		return false;

	switch (lhs.m_type) {
		case ValueType::BOOL:
			return lhs.m_as.boolean == rhs.m_as.boolean;
		case ValueType::NIL:
			return true;
		case ValueType::NUMBER:
			return lhs.m_as.number == rhs.m_as.number;
		case ValueType::OBJ:
			return lhs.m_as.obj == rhs.m_as.obj;
		case ValueType::UNDEFINED:
			return true;
	}
	return false;
}

template <class... Ts>
struct overloaded : Ts ...
{
	using Ts::operator()...;
};
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

//std::visit

size_t obj_hash(sObj* obj);

namespace std {
	template <> struct hash<Value>
	{
		size_t operator()(const Value &val) const
		{
			switch (val.type())
			{
				case ValueType::BOOL: return val.asBool() ? 3 : 5;
				case ValueType::NIL: return 7;
				case ValueType::NUMBER: return std::hash<double>()(val.asNumber());
				case ValueType::OBJ: return obj_hash(val.asObj());
				case ValueType::UNDEFINED: return 7;
			}
			return 0;
		}
	};
}
