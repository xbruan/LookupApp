using System;
using System.Collections.Generic;

namespace Lookup.Dictionary;

/// <summary>
/// 极简 LRU 缓存，用来避免每次查询都重新解压同一个词块/记录块。
///
/// 词典里有 20 万词条、上千个块，全读进内存既慢又占内存（big.mdx 完整测试约 11MB，
/// 更大的词典会到几百 MB），所以只保留最近用到的若干块。
/// </summary>
internal sealed class LruCache<TKey, TValue>
{
    private readonly int _capacity;
    private readonly Dictionary<TKey, LinkedListNode<KeyValuePair<TKey, TValue>>> _map;
    private readonly LinkedList<KeyValuePair<TKey, TValue>> _order = new LinkedList<KeyValuePair<TKey, TValue>>();
    // UI 线程与后台线程都可能来查词，缓存本身必须能并发访问。
    // 工厂（解压）也在锁内执行：宁可串行也不要让同一块被并行解压两遍。
    private readonly object _gate = new object();

    internal LruCache(int capacity, IEqualityComparer<TKey> comparer = null)
    {
        if (capacity <= 0) throw new ArgumentOutOfRangeException(nameof(capacity));
        _capacity = capacity;
        _map = new Dictionary<TKey, LinkedListNode<KeyValuePair<TKey, TValue>>>(comparer ?? EqualityComparer<TKey>.Default);
    }

    internal int Count
    {
        get
        {
            lock (_gate) return _map.Count;
        }
    }

    internal TValue GetOrAdd(TKey key, Func<TKey, TValue> factory)
    {
        lock (_gate)
        {
            if (_map.TryGetValue(key, out var node))
            {
                _order.Remove(node);
                _order.AddFirst(node);
                return node.Value.Value;
            }

            var value = factory(key);
            var added = _order.AddFirst(new KeyValuePair<TKey, TValue>(key, value));
            _map[key] = added;

            while (_map.Count > _capacity)
            {
                var last = _order.Last;
                _order.RemoveLast();
                _map.Remove(last.Value.Key);
            }
            return value;
        }
    }

    internal void Clear()
    {
        lock (_gate)
        {
            _map.Clear();
            _order.Clear();
        }
    }
}
