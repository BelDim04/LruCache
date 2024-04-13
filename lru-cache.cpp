#include <boost/intrusive/slist.hpp>
#include <boost/intrusive/list.hpp>

#include <vector>
#include <string>

// for tests
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>

/**
    @author Dmitrii Belskii <beldim04@gmail.com>
    tg: @glowing_cone
*/

/*
    Решение представляет собой аналог bucket-based хэш-таблицы фиксированного размера,
    как следствие отсутствуют какие-либо вспомогательные аллокации в ходе использования.

    Сложность вставки записей, обновления существующих записей, поиска записей: O(1) - в среднем (в плане контейнера)
                                                                                O(len(key) + len(val)) - соответсвенно при использовании строк в качестве Ключей и Значений
    Требует от Key и Value быть Default-constructable и Copy-constructable типами
    Exception safety наследуется от std::vector

    Приложены результаты `perf record` для 1 и 4 потоков Perf test-а
    Компилировал - `g++ -g -std=c++20 -O3 lru-cache.cpp`
    Тестил с AddressSanitizer

    Все решение и тесты в одном файле - в целях экономии времени
*/

namespace Algo
{
    using namespace boost::intrusive;

    template <typename Key,
            typename Val,
            size_t LoadFactor = 4,
            typename Hash = std::hash<Key>,
            typename Equal = std::equal_to<Key>>
    class LruCache
    {
        struct Node
        {
            Key _key;
            Val _val;
            size_t _cached_hash{0};
            slist_member_hook<> _bucket_order;
            list_member_hook<> _usage_order;
        };
        using BucketHook = member_hook<Node, slist_member_hook<>, &Node::_bucket_order>;
        using BucketList = slist<Node, BucketHook, linear<true>, constant_time_size<false>, cache_last<false>>;
        using UsageHook = member_hook<Node, list_member_hook<>, &Node::_usage_order>;
        using UsageList = list<Node, UsageHook, linear<true>, constant_time_size<false>, cache_last<true>>;

        explicit LruCache(size_t capacity) : _buckets(capacity * LoadFactor), _arena(capacity)
        {
            for (auto &node : _arena)
            {
                _bucket_list.push_front(node);
                _usage_list.push_front(node);
            }
            for (auto &it : _buckets)
            {
                it = _bucket_list.end();
            }
        };

    public:
        static LruCache &get_instance(size_t arg = 1024)
        {
            static LruCache instance(arg);
            return instance;
        }

        void update(const Key &key, const Val &val)
        {
            size_t hash(_key_hash(key) % _buckets.size());
            auto it(_buckets[hash]);
            while (it != _bucket_list.end()
                && it->_cached_hash == hash)
            {
                if (_key_equal(key, it->_key))
                {
                    // Update node
                    it->_val = val;
                    // Update usage list
                    auto usage_it(UsageList::s_iterator_to(*it));
                    _usage_list.erase(usage_it);
                    _usage_list.push_back(*it);
                    return;
                }
                ++it;
            }
            // Replace lru, Update bucket list
            Node &lru(_usage_list.front());
            lru._key = key;
            lru._val = val;
            auto lru_bucket_it(BucketList::s_iterator_to(lru));
            auto next_bucket_it(std::next(lru_bucket_it));
            _buckets[lru._cached_hash] = (next_bucket_it == _bucket_list.end() 
                            || next_bucket_it->_cached_hash != lru._cached_hash)? _bucket_list.end() : next_bucket_it;
            _buckets[hash] = lru_bucket_it;
            lru._cached_hash = hash;
            // Update usage list
            _usage_list.pop_front();
            _usage_list.push_back(lru);
        }

        Val resolve(const Key &key)
        {
            size_t hash(_key_hash(key) % _buckets.size());
            auto it(_buckets[hash]);
            while (it != _bucket_list.end()
                && it->_cached_hash == hash)
            {
                if (_key_equal(key, it->_key))
                {
                    auto usage_it(UsageList::s_iterator_to(*it));
                    _usage_list.erase(usage_it);
                    _usage_list.push_back(*it);
                    return it->_val;
                }
                ++it;
            }
            return Val();
        }

    private:
        Hash _key_hash{};
        Equal _key_equal{};
        std::vector<typename BucketList::iterator> _buckets;
        std::vector<Node> _arena;
        BucketList _bucket_list;
        UsageList _usage_list;
    };
}

using DnsCache = Algo::LruCache<std::string, std::string>;

namespace Test
{
    class SpinLock
    {
    public:
        void lock()
        {
            while (_locked.exchange(true, std::memory_order::acquire))
            {
                while (_locked.load(std::memory_order::relaxed))
                {}
            }
        }

        void unlock()
        {
            _locked.store(false, std::memory_order::release);
        }
    private:
        std::atomic<bool> _locked{false};
    };

    void run(size_t id, size_t world_count, SpinLock &spinlock)
    {
        auto &lru_cache(::DnsCache::get_instance());
        size_t roundIters(2 * id);
        for (size_t i = 0; i < 1e8; ++i)
        {
            if (!(i % roundIters))
            {
                std::lock_guard guard(spinlock);
                lru_cache.update(std::to_string(id) + "round" + std::to_string(i / roundIters), std::to_string(id) + "Round" + std::to_string(i / roundIters));
            }
            else
            {
                std::lock_guard guard(spinlock);
                auto res = lru_cache.resolve(std::to_string((i + id) % world_count) + "round" + std::to_string(i / (2 * world_count)));
            }
        }
    }
}

int main()
{
    // Basic test
    bool ok(true);
    auto &lru_cache(DnsCache::get_instance(2));
    std::cout << "Basic test start\n";
    lru_cache.update("abc", "ABC");
    lru_cache.update("def", "DEF");
    ok &= (lru_cache.resolve("abc") == "ABC");
    ok &= (lru_cache.resolve("def") == "DEF");
    lru_cache.update("abc", "ABC!");
    ok &= (lru_cache.resolve("def") == "DEF");
    ok &= (lru_cache.resolve("abc") == "ABC!");
    lru_cache.update("qwe", "QWE");
    ok &= (lru_cache.resolve("abc") == "ABC!");
    ok &= (lru_cache.resolve("qwe") == "QWE");
    ok &= (lru_cache.resolve("def") == "");
    lru_cache.update("iop", "IOP");
    ok &= (lru_cache.resolve("qwe") == "QWE");
    ok &= (lru_cache.resolve("iop") == "IOP");
    ok &= (lru_cache.resolve("abc") == "");
    std::cout << (ok ? "Ok: Basic test passed" : "Error: Basic test failed") << '\n';

    // Perf test
    /*
    DnsCache::get_instance(50000);
    std::vector<std::thread> threads;
    size_t world_count(4);
    Test::SpinLock spinlock;
    std::cout << "Perf test start\n";
    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < world_count; ++i)
    {
        threads.push_back(std::thread(Test::run, i + 1, world_count, std::ref(spinlock)));
    }
    for (size_t i = 0; i < world_count; ++i)
    {
        threads[i].join();
    }
    auto finish = std::chrono::high_resolution_clock::now();
    std::cout << "Perf test finish\n";
    std::cout << "Exec time: " << std::chrono::duration_cast<std::chrono::milliseconds>(finish - start).count() << '\n';
    */
}
