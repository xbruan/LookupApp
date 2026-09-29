using System;

namespace Lookup.Dictionary;

/// <summary>
/// LZO1X 解压，逐行移植 <c>js-mdict/dist/cjs/lzo1x.js</c>（minilzo-js 的 JS 版）。
///
/// 为什么要有它：MDict 的 <c>CompressionType="LZO"</c> 词典（v1.2 时代以及部分
/// 手机端工具生成的 v2.0 词典）用 LZO1X 而不是 zlib 压缩词块，.NET 没有内置实现。
///
/// 与原版的差异只有两点，都不影响输出：
/// 1. 原版在 <c>out32</c>/<c>buf32</c> 上做了 4 字节对齐的批量拷贝，触发条件是
///    <c>op % 4 == mPos % 4</c> 且都已对齐，此时源与目标不可能重叠，逐字节拷贝结果相同；
/// 2. 越界读在原版里得到 <c>undefined</c>，这里统一当作 0（只会出现在损坏的数据里）。
/// </summary>
internal static class Lzo1x
{
    /// <summary>扩容步长，与参考实现原型上的 blockSize 一致（状态里传进来的 blockSize 其实没被用）</summary>
    private const int BlockSize = 4096;

    private const int MaxOutputSize = 64 * 1024 * 1024;

    private const int Ok = 0;
    private const int EofFound = -999;

    /// <summary>解压一段 LZO1X 数据流，返回实际解出的字节</summary>
    public static byte[] Decompress(byte[] input)
    {
        if (input == null) throw new ArgumentNullException(nameof(input));
        return new State(input).Run();
    }

    private sealed class State
    {
        private readonly byte[] _buf;
        private byte[] _out;
        private int _capacity;
        private int _t;
        private int _ip;
        private int _op;
        private int _mPos;

        internal State(byte[] input)
        {
            _buf = input;
            // 参考实现：初始容量 = 输入长度补齐到 4096 的整数倍（刚好整除时再多给一块）
            var size = input.Length + (BlockSize - (input.Length % BlockSize));
            _out = new byte[size];
            _capacity = size;
        }

        /// <summary>越界读按参考实现的语义处理（负下标 → 0，尾部之后 → 0）</summary>
        private int Buf(int index)
        {
            return (uint)index < (uint)_buf.Length ? _buf[index] : 0;
        }

        private void ExtendBuffer()
        {
            var next = new byte[_capacity + BlockSize];
            Buffer.BlockCopy(_out, 0, next, 0, _op);
            _out = next;
            _capacity = next.Length;
        }

        private void Ensure(int needed)
        {
            if (needed > _capacity)
            {
                if (needed > MaxOutputSize) throw new InvalidOperationException("LZO 解压输出超过 64MB，输入数据可能已损坏");
                while (needed > _capacity) ExtendBuffer();
            }
        }

        internal byte[] Run()
        {
            _t = 0;
            _ip = 0;
            _op = 0;
            _mPos = 0;

            var skipToFirstLiteral = false;
            if (Buf(_ip) > 17)
            {
                _t = Buf(_ip++) - 17;
                if (_t < 4)
                {
                    MatchNext();
                    if (Match() != Ok) return Finish();
                }
                else
                {
                    CopyFromBuf();
                    skipToFirstLiteral = true;
                }
            }

            for (;;)
            {
                if (!skipToFirstLiteral)
                {
                    _t = Buf(_ip++);
                    if (_t >= 16)
                    {
                        if (Match() != Ok) return Finish();
                        continue;
                    }
                    if (_t == 0)
                    {
                        while (Buf(_ip) == 0)
                        {
                            _t += 255;
                            _ip++;
                        }
                        _t += 15 + Buf(_ip++);
                    }
                    _t += 3;
                    CopyFromBuf();
                }
                else
                {
                    skipToFirstLiteral = false;
                }

                _t = Buf(_ip++);
                if (_t < 16)
                {
                    _mPos = _op - (1 + 0x0800);
                    _mPos -= _t >> 2;
                    _mPos -= Buf(_ip++) << 2;
                    Ensure(_op + 3);
                    _out[_op++] = _out[_mPos++];
                    _out[_op++] = _out[_mPos++];
                    _out[_op++] = _out[_mPos];
                    if (MatchDone() == 0) continue;
                    MatchNext();
                }

                if (Match() != Ok) return Finish();
            }
        }

        private int Match()
        {
            for (;;)
            {
                if (_t >= 64)
                {
                    _mPos = _op - 1;
                    _mPos -= (_t >> 2) & 7;
                    _mPos -= Buf(_ip++) << 3;
                    _t = (_t >> 5) - 1;
                    CopyMatch();
                    if (MatchDone() == 0) break;
                    MatchNext();
                    continue;
                }

                if (_t >= 32)
                {
                    _t &= 31;
                    if (_t == 0)
                    {
                        while (Buf(_ip) == 0)
                        {
                            _t += 255;
                            _ip++;
                        }
                        _t += 31 + Buf(_ip++);
                    }
                    _mPos = _op - 1;
                    _mPos -= (Buf(_ip) >> 2) + (Buf(_ip + 1) << 6);
                    _ip += 2;
                }
                else if (_t >= 16)
                {
                    _mPos = _op;
                    _mPos -= (_t & 8) << 11;
                    _t &= 7;
                    if (_t == 0)
                    {
                        while (Buf(_ip) == 0)
                        {
                            _t += 255;
                            _ip++;
                        }
                        _t += 7 + Buf(_ip++);
                    }
                    _mPos -= (Buf(_ip) >> 2) + (Buf(_ip + 1) << 6);
                    _ip += 2;
                    if (_mPos == _op) return EofFound;
                    _mPos -= 0x4000;
                }
                else
                {
                    _mPos = _op - 1;
                    _mPos -= _t >> 2;
                    _mPos -= Buf(_ip++) << 2;
                    Ensure(_op + 2);
                    _out[_op++] = _out[_mPos++];
                    _out[_op++] = _out[_mPos];
                    if (MatchDone() == 0) break;
                    MatchNext();
                    continue;
                }

                CopyMatch();
                if (MatchDone() == 0) break;
                MatchNext();
            }
            return Ok;
        }

        private void MatchNext()
        {
            Ensure(_op + 3);
            _out[_op++] = (byte)Buf(_ip++);
            if (_t > 1)
            {
                _out[_op++] = (byte)Buf(_ip++);
                if (_t > 2) _out[_op++] = (byte)Buf(_ip++);
            }
            _t = Buf(_ip++);
        }

        private int MatchDone()
        {
            _t = Buf(_ip - 2) & 3;
            return _t;
        }

        private void CopyMatch()
        {
            _t += 2;
            Ensure(_op + _t);
            do
            {
                _out[_op++] = _out[_mPos++];
            }
            while (--_t > 0);
        }

        private void CopyFromBuf()
        {
            Ensure(_op + _t);
            do
            {
                _out[_op++] = (byte)Buf(_ip++);
            }
            while (--_t > 0);
        }

        /// <summary>EOF：参考实现会把输出截断到已写出的长度</summary>
        private byte[] Finish()
        {
            var result = new byte[_op];
            Buffer.BlockCopy(_out, 0, result, 0, _op);
            return result;
        }
    }
}
