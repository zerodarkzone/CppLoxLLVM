//
// Created by juanb on 29/09/2018.
//

#ifndef CPPLOX_STACK_HPP
#define CPPLOX_STACK_HPP

#include <cstddef>
#include <ostream>
#include <array>
#include <vector>

template <typename _T>
class Stack
{
public:
	explicit Stack(size_t capacity = 256)
	{
		m_container.reserve(capacity);
		reset();
	}

	void reset()
	{
		m_container.clear();
	}

	size_t size() const
	{
		return m_container.size();
	}

	size_t capacity() const
	{
		return m_container.capacity();
	}

	_T &top()
	{
		return m_container.back();
	}

	const _T &top() const
	{
		return m_container.back();
	}

	_T &peek(size_t offset)
	{
		return *(m_container.end() - (offset + 1u));
	}

	const _T &peek(size_t offset) const
	{
		return *(m_container.end() - (offset + 1u));
	}

	_T &get(size_t index)
	{
		return m_container[index];
	}

	const _T &get(size_t index) const
	{
		return m_container[index];
	}

	void push(_T &&value)
	{
		m_container.push_back(std::move(value));
	}

	void push(const _T &value)
	{
		m_container.push_back(value);
	}

	_T &pop()
	{
		_T &temp = m_container.back();
		m_container.pop_back();
		return temp;
	}

	friend std::ostream &operator<<(std::ostream &os, const Stack &stack)
	{
		for (const auto &value: stack.m_container)
			os << "[ " << value << " ]";
		return os;
	}

private:
	std::vector<_T> m_container;
};

template <typename _T, size_t t_size>
class FixedStack
{
public:
	FixedStack()
	{
		reset();
	}

	void reset()
	{
		m_top = m_container.begin();
	}

	size_t size() const
	{
		return m_top - m_container.begin();
	}

	size_t capacity() const
	{
		return t_size;
	}

	_T &top()
	{
		return peek(0);
	}

	const _T &top() const
	{
		return peek(0);
	}

	_T &peek(size_t offset)
	{
		return *(m_top - (offset + 1));
	}

	const _T &peek(size_t offset) const
	{
		return *(m_top - (offset + 1));
	}

	_T &get(size_t index)
	{
		return m_container[index];
	}

	const _T &get(size_t index) const
	{
		return m_container[index];
	}

	typename std::array<_T, t_size>::iterator & getTop()
	{
		return m_top;
	}

	void push(_T &&value)
	{
		*m_top = std::move(value);
		++m_top;
	}

	void push(const _T &value)
	{
		*m_top = value;
		++m_top;
	}

	_T &pop()
	{
		--m_top;
		return *m_top;
	}

	friend std::ostream &operator<<(std::ostream &os, const FixedStack &stack)
	{
		for (auto it = stack.m_container.begin(); it != stack.m_top; ++it)
			os << "[ " << *it << " ]";
		return os;
	}

private:
	std::array<_T, t_size> m_container;
	typename std::array<_T, t_size>::iterator m_top;
};

#endif //CPPLOX_STACK_HPP
