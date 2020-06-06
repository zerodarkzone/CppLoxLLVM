//
// Created by e10666a on 9/01/2019.
//

#ifndef HASHTABLE_HASHTABLE_HPP
#define HASHTABLE_HASHTABLE_HPP

#include <type_traits>
#include <functional>
#include <memory>
#include <tuple>
#include <iostream>
#include <stdexcept>
#include <sstream>

template <class Key, class Value, class Hash = std::hash<Key>, class KeyEqual = std::equal_to<Key>>
class HashTable final
{
public:
	using value_type = std::pair<const Key, Value>;
	using size_type = std::size_t;

private:

	struct Entry
	{
		using value_type_ = std::pair<Key, Value>;
		enum class Status : uint8_t
		{
			EMPTY,
			FULL,
			DEATH
		};
		value_type_ value;
		Status status = Status::EMPTY;
	};

public:

	template <typename ValueType>
	class ForwardIterator
	{
	public:
		using iterator_category = std::forward_iterator_tag;
		using value_type = ValueType;
		using difference_type = std::ptrdiff_t;
		using pointer = ValueType*;
		using reference = ValueType&;

		ForwardIterator(Entry *v, Entry *end) : m_ptr(v), m_end(end) {}
		~ForwardIterator() = default;


		const ForwardIterator operator++(int)
		{
			auto b_ptr = m_ptr;
			do
			{
				m_ptr++;
			}
			while(m_ptr != m_end && m_ptr->status != Entry::Status::FULL);
			return b_ptr;
		}

		ForwardIterator& operator++()
		{
			do
			{
				m_ptr++;
			}
			while(m_ptr != m_end && m_ptr->status != Entry::Status::FULL);
			return *this;
		}

		reference operator*() const
		{
			return *reinterpret_cast<HashTable::value_type *>(&(m_ptr->value));
		}

		pointer operator->() const
		{
			return reinterpret_cast<HashTable::value_type *>(&(m_ptr->value));
		}

		bool operator==(const ForwardIterator& rhs) const
		{
			return m_ptr == rhs.m_ptr;
		}
		bool operator!=(const ForwardIterator& rhs) const
		{
			return m_ptr != rhs.m_ptr;
		}

	private:
		Entry *m_ptr;
		Entry *m_end;
	};

	explicit HashTable( const size_type capacity = 8, const Hash& hash = Hash(), const KeyEqual& equal = KeyEqual())
			: m_entries(new Entry[capacity]), m_hash(hash), m_equal(equal),
			  m_capacity(capacity), m_count(0), m_tombstones(0), m_maxLoad(0.7)
	{

	}

	HashTable (const HashTable &other)
			: HashTable(other.m_capacity, other.m_hash, other.m_equal)
	{
		m_maxLoad = other.m_maxLoad;
		for (size_type i = 0u; i < other.m_capacity; ++i)
		{
			auto &t_entry = other.m_entries[i];
			if (t_entry.status != Entry::Status::FULL) continue;

			auto &d_entry = findValue(t_entry.value.first);
			d_entry = t_entry;
			d_entry.status = Entry::Status::FULL;
			m_count++;
		}
	}

	HashTable(HashTable&& other)
			: HashTable()
	{
		swap(other);
	}

	~HashTable() = default;

	HashTable &operator=(HashTable other)
	{
		swap(other);
		return *this;
	}

	HashTable &operator=(HashTable&& other) noexcept
	{
		swap(other);
		return *this;
	}

	void swap(HashTable& other)
	{
		using std::swap;

		swap(m_entries, other.m_entries);
		swap(m_hash, other.m_hash);
		swap(m_equal, other.m_equal);
		swap(m_capacity, other.m_capacity);
		swap(m_count, other.m_count);
		swap(m_tombstones, other.m_tombstones);
		swap(m_maxLoad, other.m_maxLoad);
	}

	void clear()
	{
		m_count = 0;
		m_tombstones = 0;
		m_entries.reset(new Entry[m_capacity]);
	}

	ForwardIterator<value_type> begin()
	{
		auto ptr = m_entries.get();
		auto e_ptr = m_entries.get() + m_capacity;
		while(ptr != e_ptr && ptr->status != Entry::Status::FULL)
		{
			ptr++;
		}
		return ForwardIterator<value_type>(ptr, e_ptr);
	}

	ForwardIterator<const value_type> begin() const
	{
		auto ptr = m_entries.get();
		auto e_ptr = m_entries.get() + m_capacity;
		while(ptr != e_ptr && ptr->status != Entry::Status::FULL)
		{
			ptr++;
		}
		return ForwardIterator<const value_type>(ptr, e_ptr);
	}

	ForwardIterator<value_type> end()
	{
		auto e_ptr = m_entries.get() + m_capacity;
		return ForwardIterator<value_type>(e_ptr, e_ptr);
	}

	ForwardIterator<const value_type> end() const
	{
		auto e_ptr = m_entries.get() + m_capacity;
		return ForwardIterator<const value_type>(e_ptr, e_ptr);
	}

	size_type size() const
	{
		return m_count - m_tombstones;
	}

	bool empty() const
	{
		return !m_count;
	}

	Value& at(const Key &key)
	{
		auto &t_entry = findValue(key);
		if (t_entry.status == Entry::Status::FULL)
			return t_entry.value.second;
		throw std::out_of_range("Key not found");
	}

	const Value& at(const Key &key) const
	{
		const auto &t_entry = findValue(key);
		if (t_entry.status == Entry::Status::FULL)
			return t_entry.value.second;
		throw std::out_of_range("Key not found");
	}

	Value& operator[]( const Key& key )
	{
		return insert(std::make_pair(key, Value())).first->second;
	}

	Value& operator[]( Key&& key )
	{
		return insert(std::make_pair(std::move(key), Value())).first->second;
	}

	std::pair<ForwardIterator<value_type>, bool> insert(const value_type &value)
	{
		if (m_count + 1 > m_capacity * m_maxLoad)
		{
			increaseCapacity();
		}
		auto &t_entry = findValue(value.first);
		if (t_entry.status != Entry::Status::FULL)
		{
			t_entry.status = Entry::Status::FULL;
			t_entry.value.first = value.first;
			t_entry.value.second = value.second;
			m_count++;
			return std::make_pair(ForwardIterator<value_type>(&t_entry, m_entries.get() + m_capacity), true);
		}
		return std::make_pair(ForwardIterator<value_type>(&t_entry, m_entries.get() + m_capacity), false);
	}

	std::pair<ForwardIterator<value_type>, bool> insert(value_type &&value)
	{
		if (m_count + 1 > m_capacity * m_maxLoad)
		{
			increaseCapacity();
		}
		auto &t_entry = findValue(value.first);
		if (t_entry.status != Entry::Status::FULL)
		{
			t_entry.status = Entry::Status::FULL;
			t_entry.value.first = std::move(value.first);
			t_entry.value.second = std::move(value.second);
			m_count++;
			return std::make_pair(ForwardIterator<value_type>(&t_entry, m_entries.get() + m_capacity), true);
		}
		return std::make_pair(ForwardIterator<value_type>(&t_entry, m_entries.get() + m_capacity), false);
	}

	size_type erase(const Key &key)
	{
		auto &t_entry = findValue(key);
		if (t_entry.status == Entry::Status::EMPTY)
			return 0;

		t_entry.status = Entry::Status::DEATH;
		m_tombstones++;
		return 1;
	}

	// friends
	friend void swap(HashTable &first, HashTable &second)
	{
		first.swap(second);
	}

private:
	Entry& findValue(const Key &key) const
	{
		auto index = m_hash(key) & (m_capacity - 1); // Modulo
		Entry *tombstone = nullptr;
		while (true)
		{
			auto &t_entry = m_entries[index];
			if (t_entry.status != Entry::Status::FULL)
			{
				if (t_entry.status == Entry::Status::EMPTY) // Found empty
				{
					return tombstone ? *tombstone : t_entry;
				}
				else // Found tombstone
				{
					if (!tombstone)
						tombstone = &t_entry;
				}
			}
			else if (m_equal(key, t_entry.value.first))
			{
				return t_entry; // Found Key
			}
			index = (index + 1) & (m_capacity - 1); // Modulo
		}
	}

	void increaseCapacity()
	{
		auto o_capacity = m_capacity;
		m_capacity *= 2;
		auto old_entries = m_entries.release();
		m_entries.reset(new Entry[m_capacity]);
		m_count = 0;
		m_tombstones = 0;

		for (size_type i = 0u; i < o_capacity; ++i)
		{
			auto &t_entry = old_entries[i];
			if (t_entry.status != Entry::Status::FULL) continue;

			auto &d_entry = findValue(t_entry.value.first);
			d_entry = std::move(t_entry);
			d_entry.status = Entry::Status::FULL;
			m_count++;
		}

		delete[] old_entries;
	}

	std::unique_ptr<Entry[]> m_entries;
	Hash m_hash;
	KeyEqual m_equal;
	size_type m_capacity;
	size_type m_count;
	size_type m_tombstones;
	float m_maxLoad;
};

template <class Key, class Value, class Hash, class KeyEqual>
inline bool operator==(const HashTable<Key, Value, Hash, KeyEqual> &lhs, const HashTable<Key, Value, Hash, KeyEqual> &rhs)
{
	return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

template <class Key, class Value, class Hash, class KeyEqual>
inline bool operator!=(const HashTable<Key, Value, Hash, KeyEqual> &lhs, const HashTable<Key, Value, Hash, KeyEqual> &rhs)
{
	return !(lhs == rhs);
}

template <typename Key, typename Val>
inline std::ostream & operator << (std::ostream &out, const HashTable<Key, Val> &hashTable)
{
	std::stringstream ss;

	for (const auto &[key, value]: hashTable)
	{
		ss << key << ": " << value << ", ";
	}

	auto str = ss.str();
	out << "{" << str.substr(0, str.length() - (!str.empty() ? 2 : 0)) << "}";
	return out;
}

#endif //HASHTABLE_HASHTABLE_HPP
