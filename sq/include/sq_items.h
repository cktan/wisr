/*
 * sq_items.h -- a node's list: PostgreSQL's List *, as a field.
 *
 * The nodes it holds are in an array cut from the arena of the tree
 * being built, as PostgreSQL's List is an array in its memory
 * context, so a list has no destructor and goes with its tree. It
 * reads as a std::vector of nodes does. It is a value: a copy is a
 * copy of the array, made in the tree being built; a move takes the
 * array. Whatever changes a list, a copy included, runs in a parse,
 * or with a tree named current (src/tree.h).
 */
#ifndef SQ_ITEMS_H
#define SQ_ITEMS_H

#include <cstddef>
#include <cstdint>

struct sq_node_t;

struct sq_items_t {
    sq_items_t() = default;
    sq_items_t(const sq_items_t& other) { append(other); }
    sq_items_t& operator=(const sq_items_t& other) {
        if (this != &other) {
            m_size = 0;
            append(other);
        }
        return *this;
    }
    sq_items_t(sq_items_t&& other) noexcept
        : m_items(other.m_items), m_size(other.m_size), m_cap(other.m_cap) {
        other.m_items = nullptr;
        other.m_size = other.m_cap = 0;
    }
    sq_items_t& operator=(sq_items_t&& other) noexcept {
        if (this != &other) {
            m_items = other.m_items;
            m_size = other.m_size;
            m_cap = other.m_cap;
            other.m_items = nullptr;
            other.m_size = other.m_cap = 0;
        }
        return *this;
    }

    size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    sq_node_t*& operator[](size_t i) { return m_items[i]; }
    sq_node_t* operator[](size_t i) const { return m_items[i]; }
    sq_node_t** data() { return m_items; }
    sq_node_t* const* data() const { return m_items; }
    sq_node_t** begin() { return m_items; }
    sq_node_t** end() { return m_items + m_size; }
    sq_node_t* const* begin() const { return m_items; }
    sq_node_t* const* end() const { return m_items + m_size; }
    sq_node_t*& front() { return m_items[0]; }
    sq_node_t*& back() { return m_items[m_size - 1]; }

    /* Add node at the end. */
    void push_back(sq_node_t* node);

    /* Add node at the front. */
    void push_front(sq_node_t* node);

    /* Add other's nodes at the end; other may be this list. */
    void append(const sq_items_t& other);

    /* Keep the first n nodes, n no more than size(). */
    void truncate(size_t n) { m_size = static_cast<uint32_t>(n); }

    /* Empty the list; its array stays for what is added next. */
    void clear() { m_size = 0; }

private:
    /* Room for n nodes in all. */
    void reserve(size_t n);

    sq_node_t** m_items = nullptr;
    uint32_t m_size = 0;
    uint32_t m_cap = 0;
};

#endif /* SQ_ITEMS_H */
