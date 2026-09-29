using System;

namespace Lookup.Dictionary;

/// <summary>
/// 加密词典用到的两个小算法：MDict 的 fast_decrypt（异或 + 半字节交换）与 adler32。
/// 移植自 <c>js-mdict/dist/cjs/utils.js</c> 的 <c>fast_decrypt</c> / <c>mdxDecrypt</c>。
/// </summary>
internal static class MdictCrypto
{
    /// <summary>adler32（与 zlib/Node 的 zlib.adler32 同值）</summary>
    public static uint Adler32(byte[] data, int offset, int count)
    {
        const uint mod = 65521;
        uint a = 1, b = 0;
        for (var i = 0; i < count; i++)
        {
            a = (a + data[offset + i]) % mod;
            b = (b + a) % mod;
        }
        return (b << 16) | a;
    }

    /// <summary>
    /// 解密（同时也是加密，参考实现的这个函数是对合的）comp_block[offset..]，
    /// key 来自压缩块自身的第 4..8 字节，因此密文头部 8 字节必须原样保留。
    /// </summary>
    public static byte[] FastDecrypt(byte[] data, int offset, int count, byte[] key)
    {
        var result = new byte[count];
        var previous = 0x36;
        for (var i = 0; i < count; i++)
        {
            var raw = data[offset + i];
            var t = ((raw >> 4) | (raw << 4)) & 0xff;
            t = t ^ previous ^ (i & 0xff) ^ key[i % key.Length];
            previous = raw;
            result[i] = (byte)t;
        }
        return result;
    }

    /// <summary>
    /// MDict 的固定密钥派生：key = RIPEMD128(comp_block[4..8] + 95 36 00 00)，
    /// 然后对 comp_block[8..] 做 fast_decrypt，前 8 字节（压缩类型 + adler32）保持不变。
    /// </summary>
    public static byte[] MdxDecrypt(byte[] block)
    {
        return MdxDecrypt(block, 0, block.Length);
    }

    public static byte[] MdxDecrypt(byte[] block, int offset, int count)
    {
        // 至少要有 8 字节头部（压缩类型 + adler32）才能派生密钥
        if (count < 8) throw new InvalidOperationException("加密块长度不足 8 字节，无法派生密钥");

        var keyIn = new byte[8];
        keyIn[0] = block[offset + 4];
        keyIn[1] = block[offset + 5];
        keyIn[2] = block[offset + 6];
        keyIn[3] = block[offset + 7];
        keyIn[4] = 0x95;
        keyIn[5] = 0x36;
        keyIn[6] = 0x00;
        keyIn[7] = 0x00;

        var key = Ripemd128.Compute(keyIn, 0, keyIn.Length);

        var result = new byte[count];
        Buffer.BlockCopy(block, offset, result, 0, 8);
        var decrypted = FastDecrypt(block, offset + 8, count - 8, key);
        Buffer.BlockCopy(decrypted, 0, result, 8, decrypted.Length);
        return result;
    }
}
