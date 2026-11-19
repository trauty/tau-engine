#pragma once

#include "tau/defines.h"

#include <utility>
#include <vector>

namespace tau
{
    template <typename T>
    class flat_map_t
    {
      public:
        static constexpr u32_t EMPTY = 0xffffffff;
        static constexpr u32_t TOMBSTONE = 0xfffffffe;
        static constexpr u32_t INVALID = 0xffffffff;

        flat_map_t() { rehash(16); }

        void reserve(u32_t min_slots)
        {
            u32_t cap = 16;
            while (cap < min_slots) { cap *= 2; }
            if (cap > capacity) { rehash(cap); }
        }

        T& operator[](u32_t key) { return slots[find_or_insert_slot(key)].value; }

        T* find(u32_t key)
        {
            u32_t idx = locate(key);
            return idx == INVALID ? nullptr : &slots[idx].value;
        }

        const T* find(u32_t key) const
        {
            u32_t idx = locate(key);
            return idx == INVALID ? nullptr : &slots[idx].value;
        }

        bool contains(u32_t key) const { return locate(key) != INVALID; }

        bool erase(u32_t key)
        {
            u32_t idx = locate(key);
            if (idx == INVALID) { return false; }
            slots[idx].key = TOMBSTONE;
            slots[idx].value = T{};
            live_count--;
            tombstone_count++;

            return true;
        }

        void clear()
        {
            for (slot_t& s : slots)
            {
                s.key = EMPTY;
                s.value = T{};
            }

            live_count = 0;
            tombstone_count = 0;
        }

        u32_t size() const { return live_count; }
        bool empty() const { return live_count == 0; }

      private:
        struct slot_t
        {
            u32_t key = EMPTY;
            T value{};
        };

        struct iterator_t
        {
            slot_t* cur;
            slot_t* end;
            void skip_empty()
            {
                while (cur != end && (cur->key == EMPTY || cur->key == TOMBSTONE)) { cur++; }
            }
            iterator_t& operator++()
            {
                cur++;
                skip_empty();
                return *this;
            }
            bool operator!=(const iterator_t& o) const { return cur != o.cur; }
            std::pair<u32_t, T&> operator*() { return {cur->key, cur->value}; }
        };

        struct const_iterator_t
        {
            const slot_t* cur;
            const slot_t* end;
            void skip_empty()
            {
                while (cur != end && (cur->key == EMPTY || cur->key == TOMBSTONE)) { cur++; }
            }
            const_iterator_t& operator++()
            {
                cur++;
                skip_empty();
                return *this;
            }
            bool operator!=(const const_iterator_t& o) const { return cur != o.cur; }
            std::pair<u32_t, const T&> operator*() const { return {cur->key, cur->value}; }
        };

      public:
        iterator_t begin()
        {
            iterator_t it{slots.data(), slots.data() + slots.size()};
            it.skip_empty();
            return it;
        }
        iterator_t end() { return {slots.data() + slots.size(), slots.data() + slots.size()}; }

        const_iterator_t begin() const
        {
            const_iterator_t it{slots.data(), slots.data() + slots.size()};
            it.skip_empty();
            return it;
        }
        const_iterator_t end() const { return {slots.data() + slots.size(), slots.data() + slots.size()}; }

      private:
        std::vector<slot_t> slots;
        u32_t capacity = 0;
        u32_t mask = 0;
        u32_t live_count = 0;
        u32_t tombstone_count = 0;

        static u32_t hash_mix(u32_t k)
        {
            k ^= k >> 16;
            k *= 0x7feb352d;
            k ^= k >> 15;
            k *= 0x846ca68b;
            k ^= k >> 16;
            return k;
        }

        u32_t locate(u32_t key) const
        {
            if (capacity == 0) { return INVALID; }
            u32_t idx = hash_mix(key) & mask;

            for (u32_t probes = 0; probes < capacity; probes++)
            {
                const slot_t& s = slots[idx];
                if (s.key == key) { return idx; }
                if (s.key == EMPTY) { return INVALID; }

                idx = (idx + 1) & mask;
            }

            return INVALID;
        }

        u32_t find_or_insert_slot(u32_t key)
        {
            if ((live_count + tombstone_count + 1) * 10 >= capacity * 7) { rehash(capacity * 2); }

            u32_t idx = hash_mix(key) & mask;
            u32_t first_tombstone = INVALID;

            for (;;)
            {
                slot_t& s = slots[idx];
                if (s.key == key) { return idx; }

                if (s.key == EMPTY)
                {
                    u32_t target = (first_tombstone != INVALID) ? first_tombstone : idx;
                    if (target != idx) { tombstone_count--; }
                    slots[target].key = key;
                    slots[target].value = T{};
                    live_count++;

                    return target;
                }

                if (s.key == TOMBSTONE && first_tombstone == INVALID) { first_tombstone = idx; }
                idx = (idx + 1) & mask;
            }
        }

        void rehash(u32_t new_capacity)
        {
            std::vector<slot_t> old = std::move(slots);

            // resize, not assign: assign copies into every slot, which rules out move only values
            slots.clear();
            slots.resize(new_capacity);
            capacity = new_capacity;
            mask = capacity - 1;
            live_count = 0;
            tombstone_count = 0;

            for (slot_t& s : old)
            {
                if (s.key != EMPTY && s.key != TOMBSTONE)
                {
                    slots[find_or_insert_slot(s.key)].value = std::move(s.value);
                }
            }
        }
    };
} // namespace tau