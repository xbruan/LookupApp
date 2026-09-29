using System;

namespace Lookup.Dictionary;

/// <summary>
/// RIPEMD-128，逐行移植 <c>js-mdict/dist/cjs/ripemd128.js</c>。
///
/// 为什么不能换成 .NET 自带的散列：MDict 的 Encrypted="2" 词典用
/// <c>RIPEMD128(压缩块[4..8] + 95 36 00 00)</c> 当密钥，只要有一个 bit 不同，
/// 解出来的密钥块就是垃圾，所以必须和参考实现逐位一致（包括它自己那套
/// 「长度按 32 位截断」的填充写法）。
/// </summary>
internal static class Ripemd128
{
    private static readonly int[] S =
    {
        11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8,   // round 1
        7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12,    // round 2
        11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5,    // round 3
        11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12,    // round 4
        8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6,    // parallel round 1
        9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11,    // parallel round 2
        9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5,    // parallel round 3
        15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8,    // parallel round 4
    };

    private static readonly int[] X =
    {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,      // round 1
        7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8,      // round 2
        3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12,      // round 3
        1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2,      // round 4
        5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12,      // parallel round 1
        6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2,      // parallel round 2
        15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13,      // parallel round 3
        8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14,      // parallel round 4
    };

    private static readonly uint[] K =
    {
        0x00000000, // FF
        0x5a827999, // GG
        0x6ed9eba1, // HH
        0x8f1bbcdc, // II
        0x50a28be6, // III
        0x5c4dd124, // HHH
        0x6d703ef3, // GGG
        0x00000000, // FFF
    };

    /// <summary>计算 RIPEMD-128，返回 16 字节摘要</summary>
    public static byte[] Compute(byte[] data, int offset, int count)
    {
        // 参考实现的填充长度算法：需要补到 56 或 120 字节（mod 64），
        // 与常规 RIPEMD 写法等价，这里照抄以便逐位对齐。
        var rem = count % 64;
        var padLen = (rem < 56 ? 56 : 120) - rem;
        var buffer = new byte[count + padLen + 8];
        Buffer.BlockCopy(data, offset, buffer, 0, count);
        buffer[count] = 0x80;

        // JS: bytes <<= 3（32 位环绕），高 32 位写成 bytes >>> 31
        var bits = unchecked(count << 3);
        WriteUInt32Le(buffer, count + padLen, unchecked((uint)bits));
        WriteUInt32Le(buffer, count + padLen + 4, (uint)bits >> 31);

        var hash = new uint[] { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };

        for (var block = 0; block < buffer.Length; block += 64)
        {
            var a = hash[0];
            var b = hash[1];
            var c = hash[2];
            var d = hash[3];
            var aa = a;
            var bb = b;
            var cc = c;
            var dd = d;

            var t = 0;
            for (; t < 64; ++t)
            {
                var r = t / 16;
                var v = unchecked(a + F(r, b, c, d) + ReadUInt32Le(buffer, block + 4 * X[16 * r + t % 16]) + K[r]);
                a = Rotl(v, S[16 * r + t % 16]);
                var tmp = d;
                d = c;
                c = b;
                b = a;
                a = tmp;
            }

            for (; t < 128; ++t)
            {
                var r = t / 16;
                var rr = (63 - t % 64) / 16;
                var v = unchecked(aa + F(rr, bb, cc, dd) + ReadUInt32Le(buffer, block + 4 * X[16 * r + t % 16]) + K[r]);
                aa = Rotl(v, S[16 * r + t % 16]);
                var tmp = dd;
                dd = cc;
                cc = bb;
                bb = aa;
                aa = tmp;
            }

            // 注意变量来源：ddd = hash[1] + cc + ddd，其中 cc 是主轮变量、ddd 是并行轮变量
            var next0 = unchecked(hash[1] + c + dd);
            var next1 = unchecked(hash[2] + d + aa);
            var next2 = unchecked(hash[3] + a + bb);
            var next3 = unchecked(hash[0] + b + cc);
            hash[0] = next0;
            hash[1] = next1;
            hash[2] = next2;
            hash[3] = next3;
        }

        // JS 返回的是 Uint32Array 的底层字节，即 4 个小端 32 位字
        var digest = new byte[16];
        for (var i = 0; i < 4; i++) WriteUInt32Le(digest, i * 4, hash[i]);
        return digest;
    }

    private static uint F(int r, uint x, uint y, uint z)
    {
        switch (r & 3)
        {
            case 0: return x ^ y ^ z;
            case 1: return (x & y) | (~x & z);
            case 2: return (x | ~y) ^ z;
            default: return (x & z) | (y & ~z);
        }
    }

    private static uint Rotl(uint x, int n)
    {
        return (x >> (32 - n)) | (x << n);
    }

    private static uint ReadUInt32Le(byte[] b, int i)
    {
        return (uint)(b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24));
    }

    private static void WriteUInt32Le(byte[] b, int i, uint v)
    {
        b[i] = (byte)v;
        b[i + 1] = (byte)(v >> 8);
        b[i + 2] = (byte)(v >> 16);
        b[i + 3] = (byte)(v >> 24);
    }
}
