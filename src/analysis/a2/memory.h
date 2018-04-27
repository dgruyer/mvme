#ifndef __A2_MEMORY_H__
#define __A2_MEMORY_H__

#include <cassert>
#include <cstdlib>
#include <exception>
#include <memory>
#include <numeric>
#include <type_traits>
#include <vector>

#include "util/typedefs.h"

namespace memory
{

template<typename T>
inline bool is_aligned(const T *ptr, size_t alignment = alignof(T))
{
    return (((uintptr_t)ptr) % alignment) == 0;
}

namespace detail
{

template<typename T>
struct destroy_only_deleter
{
    void operator()(T *ptr)
    {
        //fprintf(stderr, "%s %p\n", __PRETTY_FUNCTION__, ptr);
        ptr->~T();
    }
};

} // namespace detail

class Arena
{
    public:
        explicit Arena(size_t segmentSize)
            : m_segmentSize(segmentSize)
            , m_currentSegmentIndex(0)
        {
            addSegment(m_segmentSize);
        }

        ~Arena()
        {
            destroyObjects();
        }

        // can't copy
        Arena(const Arena &other) = delete;
        Arena &operator=(const Arena &other) = delete;

        // can move
        Arena(Arena &&other) = default;
        Arena &operator=(Arena &&other) = default;

        /** Total space used. */
        inline size_t used() const
        {
            return std::accumulate(
                m_segments.begin(), m_segments.end(),
                static_cast<size_t>(0),
                [](size_t sum, const Segment &seg) { return sum + seg.used(); });
        }

        /** Sum of all segment sizes. */
        inline size_t size() const
        {
            return std::accumulate(
                m_segments.begin(), m_segments.end(),
                static_cast<size_t>(0),
                [](size_t sum, const Segment &seg) { return sum + seg.size; });
        }

        /** Destroys objects created via pushObject() and clears all segments.
         * Does not deallocate segments. */
        inline void reset()
        {
            destroyObjects();

            for (auto &seg: m_segments)
            {
                seg.reset();
            }

            m_currentSegmentIndex = 0;
        }

        /** Push size bytes into the arena. */
        inline void *pushSize(size_t size, size_t align = 1)
        {
            return pushSize_impl(size, align);
        }

        /** IMPORTANT: Use for POD types only! It doesn't do construction nor
         * destruction. */
        template<typename T>
        T *pushStruct(size_t align = alignof(T))
        {
            static_assert(std::is_trivial<T>::value, "T must be a trivial type");

            return reinterpret_cast<T *>(pushSize(sizeof(T), align));
        }

        /** IMPORTANT: Use for arrays of POD types only! It doesn't do
         * construction nor destruction. */
        template<typename T>
        T *pushArray(size_t size, size_t align = alignof(T))
        {
            static_assert(std::is_trivial<T>::value, "T must be a trivial type");

            return reinterpret_cast<T *>(pushSize(size * sizeof(T), align));
        }

        /** Performs pushStruct<T>() and copies the passed in value into the
         * arena. */
        template<typename T>
        T *push(const T &t, size_t align = alignof(T))
        {
            T *result = pushStruct<T>(align);
            *result = t;
            return result;
        }

        /* Construct an object of type T inside the arena. The object will be
         * properly deconstructed on resetting or destroying the arena. */
        template<typename T>
        T *pushObject(size_t align = alignof(T))
        {
            /* Get memory and construct the object using placement new. */
            void *mem = pushSize(sizeof(T), align);
            T *result = new (mem) T;

            /* Now push a lambda calling the object destructor onto the
             * deleters vector.
             * To achieve exception safety a unique_ptr with a custom deleter
             * that only runs the destructor is used to temporarily hold the
             * object pointer. If the vector operation throws the unique_ptr
             * will properly destroy the object. Otherwise the deleter lambda
             * has been stored and thus the unique pointer may release() its
             * pointee. Note that in case of an exception the space for T has
             * already been allocated inside the arena and will not be
             * reclaimed. */
            std::unique_ptr<T, detail::destroy_only_deleter<T>> guard_ptr(result);

            m_deleters.emplace_back([result] () {
                //fprintf(stderr, "%s %p\n", __PRETTY_FUNCTION__, result);
                result->~T();
            });

            /* emplace_back() did not throw. It's safe to release the guard now. */
            guard_ptr.release();

            return result;
        }

        inline size_t segmentCount() const
        {
            return m_segments.size();
        }

    private:
        struct Segment
        {
            inline size_t free() const
            {
                return (mem.get() + size) - reinterpret_cast<u8 *>(cur);
            }

            inline size_t used() const
            {
                return size - free();
            }

            void reset()
            {
                cur = mem.get();
            }

            std::unique_ptr<u8[]> mem;
            void *cur;
            size_t size;
        };

        Segment &currentSegment()
        {
            assert(m_currentSegmentIndex < m_segments.size());

            return m_segments[m_currentSegmentIndex];
        }

        const Segment &currentSegment() const
        {
            assert(m_currentSegmentIndex < m_segments.size());

            return m_segments[m_currentSegmentIndex];
        }

        void addSegment(size_t size)
        {
            Segment segment = {};
            segment.mem     = std::unique_ptr<u8[]>{ new u8[size] };
            segment.cur     = segment.mem.get();
            segment.size    = size;

            m_segments.emplace_back(std::move(segment));

            //fprintf(stderr, "%s: added segment of size %u, segmentCount=%u\n",
            //        __PRETTY_FUNCTION__, (u32)size, (u32)segmentCount());
        }

        inline void destroyObjects()
        {
            /* Destroy objects in reverse construction order. */
            for (auto it = m_deleters.rbegin();
                 it != m_deleters.rend();
                 it++)
            {
                (*it)();
            }

            m_deleters.clear();
        }

        /*
         * Check each segment from the current one to the last. If the std::align()
         * call succeeds use that segment and return the pointer.
         *
         * Otherwise add a new segment that's large enough to handle the
         * requested size including alignment. Now the std::align() call must
         * succeed.
         *
         * If the system runs OOM the call to addSegment() will throw a
         * bad_alloc and we're done.
         */

        inline void *pushSize_impl(size_t size, size_t align)
        {
            //fprintf(stderr, "%s: size=%lu, align=%lu\n",
            //        __FUNCTION__, (u64)size, (u64)align);

            assert(m_currentSegmentIndex < segmentCount());

            for (; m_currentSegmentIndex < segmentCount(); m_currentSegmentIndex++)
            {
                auto &seg = m_segments[m_currentSegmentIndex];
                size_t space = seg.free();

                if (std::align(align, size, seg.cur, space))
                {
                    void *result = seg.cur;
                    seg.cur = reinterpret_cast<u8 *>(seg.cur) + size;
                    assert(is_aligned(result, align));
                    return result;
                }
            }

            assert(m_currentSegmentIndex == segmentCount());

            // Point to the last valid segment to stay consistent in case addSegment() throws
            m_currentSegmentIndex--;

            // This amount should guarantee that std::align() succeeds.
            size_t sizeNeeded = size + align;

            // this can throw bad_alloc
            addSegment(sizeNeeded > m_segmentSize ? sizeNeeded : m_segmentSize);

            m_currentSegmentIndex++;

            auto &seg = currentSegment();
            size_t space = seg.free();

            if (std::align(align, size, seg.cur, space))
            {
                void *result = seg.cur;
                seg.cur = reinterpret_cast<u8 *>(seg.cur) + size;
                assert(is_aligned(result, align));
                return result;
            }

            assert(false);
            return nullptr;
        }

        using Deleter = std::function<void ()>;

        std::vector<Deleter> m_deleters;
        std::vector<Segment> m_segments;
        size_t m_segmentSize;
        size_t m_currentSegmentIndex;
};

} // namespace memory

#endif /* __A2_MEMORY_H__ */
