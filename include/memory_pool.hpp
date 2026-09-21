#pragma once
#include <cstddef>
#include <vector>
#include <cassert>
#include <new>
#include <type_traits>

template <typename T, std::size_t Capacity>
class MemoryPool {
    public:
        MemoryPool() {
            free_list_.reserve(Capacity);
            for (std::size_t i = 0; i < Capacity; ++i) {
                free_list_.push_back(i);
            }
        }

        template <typename... Args>
        T* allocate(Args&&... args) {
            assert(!free_list_.empty() && "Memory pool exhausted");
            if (free_list_.empty()) {
                return nullptr;
            }
            std::size_t idx = free_list_.back();
            free_list_.pop_back();
            T* slot = slot_ptr(idx);
            return ::new (slot) T(std::forward<Args>(args)...);
        }

        void release(T* ptr) {
            ptr->~T();
            std::size_t idx = static_cast<std::size_t>(
                reinterpret_cast<Storage*>(ptr) - storage_.data()
            );
            free_list_.push_back(idx);
        }

        std::size_t capacity() const {
            return Capacity;
        }
        std::size_t available() const {
            return free_list_.size();
        }
    
    private:
        using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;
        T* slot_ptr(std::size_t idx) {
            return reinterpret_cast<T*>(&storage_[idx]);
        }
        std::vector<Storage> storage_ = std::vector<Storage>(Capacity);
        std::vector<std::size_t> free_list_;
};