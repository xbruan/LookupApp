namespace Lookup.Host
{
    using System;
    using System.Collections.Generic;
    using System.Globalization;
    using System.IO;
    using System.Speech.Synthesis;
    using System.Text;
    using Lookup.Interop;

    /// <summary>
    /// 系统离线语音（Windows 上的 SAPI，经 .NET 的 `System.Speech`）+ 合成结果的暂存 + 「朗读」这条路的编排。
    /// 壳只做两件**平台能力**的事：探测本机装了哪些音色、照着内核给的答案出声 —— 「走哪一层、哪个语种配哪个嗓子、语速、能不能点、不能点时哪句人话」**一个字都不在这里**（全是 `dsh_speech_plan` 的活）。
    /// ⚠️ 响度补偿那两个键（`doubaoLoudnessEn/Zh`）**今天没有消费者**（在线的音量在「产字节」那一侧施加）—— 别以为它们已经生效了。
    /// </summary>
    internal static class Speech
    {
        /// <summary>本机的一个离线音色（= 递进内核的那个 JSON 里的四项）</summary>
        internal sealed class Voice
        {
            internal string Id;
            internal string Name;
            internal string Language; // 主代码："en" / "zh"
            internal string Culture;  // 区域标记："en-US" / "zh-CN"
        }

        private static readonly object Gate = new object();
        private static List<Voice> _voices;
        private static string _voicesMessage;

        /// <summary>
        /// 探测本机音色（**只探一次**，之后走缓存 —— 音色不会在程序运行中变）。
        ///
        /// ⚠️ 「探不到」与「本机没有音色」是两件事：语音引擎没装 / 服务被禁时 `System.Speech` 会抛，
        ///    那时 `message` 是一句人话、`list` 是空的；内核正是按这个分界说两句不同的话（`voices_json` 传 null 还是 `[]`）。
        /// </summary>
        internal static List<Voice> Probe(out string message)
        {
            lock (Gate)
            {
                if (_voices != null)
                {
                    message = _voicesMessage;
                    return _voices;
                }
                var list = new List<Voice>();
                string why = null;
                try
                {
                    using (var synth = new SpeechSynthesizer())
                    {
                        foreach (var installed in synth.GetInstalledVoices())
                        {
                            if (!installed.Enabled) continue;
                            var info = installed.VoiceInfo;
                            var v = new Voice
                            {
                                Id = info.Name,
                                Name = info.Name,
                                Language = (info.Culture != null)
                                    ? info.Culture.TwoLetterISOLanguageName
                                    : "",
                                Culture = (info.Culture != null) ? info.Culture.Name : ""
                            };
                            if (!string.IsNullOrEmpty(v.Id)) list.Add(v);
                        }
                    }
                }
                catch (Exception err)
                {
                    why = "问不到本机的离线音色（" + Explain(err) + "）";
                }
                _voices = list;
                _voicesMessage = why;
                message = why;
                return list;
            }
        }

        /// <summary>
        /// 本机音色表 → **内核要的那个 JSON**（`dsh_speech_plan` 的 `voices_json`）。
        ///
        /// ⚠️ 探不到时返回 **null**（不是 `[]`）—— 两句话的分界见 `Probe`。
        /// </summary>
        internal static string VoicesJson()
        {
            string why;
            var list = Probe(out why);
            if (why != null) return null;
            var sb = new StringBuilder();
            sb.Append('[');
            for (var i = 0; i < list.Count; i++)
            {
                if (i > 0) sb.Append(',');
                sb.Append("{\"id\":").Append(Quote(list[i].Id));
                sb.Append(",\"name\":").Append(Quote(list[i].Name));
                sb.Append(",\"language\":").Append(Quote(list[i].Language));
                sb.Append(",\"culture\":").Append(Quote(list[i].Culture));
                sb.Append('}');
            }
            sb.Append(']');
            return sb.ToString();
        }

        /// <summary>
        /// 内核挑中的那把嗓子，它的**区域标记**（`zh-CN` / `en-US`）。
        ///
        /// 为什么由这一层查、而不是让内核给：区域标记是**平台事实**（别的系统未必这么写），内核不该知道；
        /// 查不到（理论上不该发生）就退回空串 —— `Synthesize` 见空串走纯文本那条路。
        /// </summary>
        internal static string CultureOf(string voiceId)
        {
            if (string.IsNullOrEmpty(voiceId)) return "";
            string why;
            var list = Probe(out why);
            for (var i = 0; i < list.Count; i++)
            {
                if (string.Equals(list[i].Id, voiceId, StringComparison.Ordinal)) return list[i].Culture;
            }
            return "";
        }

        /// <summary>
        /// 豆包音色 id → **界面上的说法**（`Dacey` / `Vivi`；认不出来就是 id 本身）。
        ///
        /// ★ 必须**问内核**（`dsh_speech_speaker_label`）：那张表住在内核里，壳里再抄一份就是「同一个东西两个来源」（那个坑）—— 迟早一处改了另一处没改。
        /// ⚠️ 问不到时**回 id 本身**、不回空串：空串是「这里什么都没配」，而这一格是「这把嗓子叫什么」，不是同一件事。
        /// </summary>
        internal static string SpeakerLabel(string speakerId)
        {
            if (string.IsNullOrEmpty(speakerId)) return "";
            try
            {
                string label;
                Dsh.SpeechSpeakerLabel(speakerId, out label);
                return string.IsNullOrEmpty(label) ? speakerId : label;
            }
            catch (Exception)
            {
                /* 内核这条路断了也不许把名字变成空 —— 那会让界面显示成「没配音色」 */
                return speakerId;
            }
        }

        private static string Quote(string s)
        {
            if (s == null) return "\"\"";
            var sb = new StringBuilder(s.Length + 2);
            sb.Append('"');
            foreach (var c in s)
            {
                if (c == '"' || c == '\\') sb.Append('\\').Append(c);
                else if (c < 0x20) sb.Append(' '); // 控制字符：音色名里不会有，但别造出坏 JSON
                else sb.Append(c);
            }
            sb.Append('"');
            return sb.ToString();
        }

        /// <summary>把引擎的英文报错翻成用户能照做的话</summary>
        private static string Explain(Exception err)
        {
            var message = (err != null && err.Message != null) ? err.Message : "";
            if (message.IndexOf("No voice installed", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                return "本机没有可用的语音引擎（Windows 的「时间和语言 → 语音」里可以装）";
            }
            if (message.IndexOf("Access is denied", StringComparison.OrdinalIgnoreCase) >= 0 ||
                message.IndexOf("access", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                return "访问语音引擎被拒（可能是权限，或者语音服务被禁用了）：" + message;
            }
            return message;
        }

        /* ── 合成 ────────────────────────────────────────────────────────── */

        /// <summary>
        /// 合成**一段**文本 → **16bit PCM 的 WAV 字节**（在内存里，不落临时文件）。
        ///
        /// 语速 `Rate` 这里必须再夹一遍（超出 -10..10 SAPI 直接抛，内核给的值本就在里头）；**先走 SSML**、真失败才退回纯文本，不许不报错地换一种念法；产出不足 45 字节 = 没有音频数据，如实报错。
        /// ⚠️ `languageTag` 为空时**走纯文本那条路** —— 不许给中文嗓子挂一个 `en-US` 的 `xml:lang`（那是在猜，猜错会让它按英文念）。
        /// </summary>
        internal static byte[] Synthesize(string text, string voiceName, string languageTag, int rate,
                                          out string why)
        {
            why = null;
            try
            {
                using (var synth = new SpeechSynthesizer())
                {
                    if (!string.IsNullOrEmpty(voiceName)) synth.SelectVoice(voiceName);
                    synth.Rate = Math.Max(-10, Math.Min(10, rate));
                    var tag = languageTag ?? "";
                    if (tag.Length == 0)
                    {
                        var plain = new MemoryStream();
                        synth.SetOutputToWaveStream(plain);
                        synth.Speak(text);
                        return Finish(plain, out why);
                    }
                    var buffer = new MemoryStream();
                    synth.SetOutputToWaveStream(buffer);
                    var ssml = "<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"" +
                               EscapeXml(tag) + "\">" + EscapeXml(text) + "</speak>";
                    try
                    {
                        synth.SpeakSsml(ssml);
                    }
                    catch (Exception ssmlError)
                    {
                        /* SSML 失败才退回纯文本。⚠️ 这条兜底**必须留着**：不同引擎对 `xml:lang` /
                           SSML 版本的容忍度不一样，写死走 SSML 会让某些机器上一个字都念不出来。 */
                        Console.Error.WriteLine("[speech] SSML 合成失败，退回纯文本：" + ssmlError.Message);
                        buffer = new MemoryStream();
                        synth.SetOutputToWaveStream(buffer);
                        synth.Speak(text);
                    }
                    return Finish(buffer, out why);
                }
            }
            catch (Exception err)
            {
                why = Explain(err);
                return null;
            }
        }

        private static byte[] Finish(MemoryStream buffer, out string why)
        {
            why = null;
            var bytes = buffer.ToArray();
            if (bytes.Length <= 44)
            {
                why = "语音引擎没有产生音频数据。";
                return null;
            }
            return bytes;
        }

        private static string EscapeXml(string s)
        {
            if (s == null) return "";
            return s.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;")
                    .Replace("\"", "&quot;").Replace("'", "&apos;");
        }

        /* ── 切好的几段怎么变成一段音频 ───────────────────────────────────── */

        /*
         * 为什么要接：内核把长文本**切段**是产品约定（一段最多 300 个字符、
         * 断点优先句末 —— 见 `dsh_speech_split`），这套规则在参考实现里住在界面
         * （`main.ts` 的 `splitForSpeech`），搬进内核之后**必须有人在念的时候照它办**。
         *
         * 接成**一段**而不是给页面一个播放列表：页面上就一个复用的 `<audio>`，
         * "第 2 段失败了怎么办"这类判断不该长在视图层（那又是一条业务规则）。
         */

        /// <summary>
        /// 把内核切好的几段依次合成、接成**一段** WAV。
        ///
        /// 一段一段合成（而不是一个合成器连着念几遍）是因为：`SetOutputToWaveStream`
        /// 下一次 `Speak` 会不会**再写一个 RIFF 头**是实现细节，不能赌；
        /// 每段各出一个自包含的 WAV、再由我们按 `data` 块拼起来，结果是可验证的。
        /// </summary>
        internal static byte[] SynthesizeAll(IList<string> chunks, string voiceName,
                                             string languageTag, int rate, out string why)
        {
            why = null;
            var wavs = new List<byte[]>(chunks.Count);
            for (var i = 0; i < chunks.Count; i++)
            {
                string one;
                var wav = Synthesize(chunks[i], voiceName, languageTag, rate, out one);
                if (wav == null)
                {
                    why = (chunks.Count > 1)
                        ? "第 " + (i + 1).ToString(CultureInfo.InvariantCulture) + " 段念不出来：" + one
                        : one;
                    return null;
                }
                wavs.Add(wav);
            }
            if (wavs.Count == 0)
            {
                why = "内核这次没给出要念的段。";
                return null;
            }
            if (wavs.Count == 1) return wavs[0];
            return ConcatWav(wavs, out why);
        }

        /// <summary>
        /// 把同一把嗓子、同一语速合成的几段 WAV 接成一段：**头只留第一份**，
        /// `data` 块顺次相接，并把两处长度写对（RIFF 总长、data 块长）。
        ///
        /// 这里老老实实走一遍 RIFF 的分块（`id` + 4 字节长度 + 内容，奇数长度要补一个填充字节），
        /// 而不是假定"头就是 44 字节" —— 合成器多写一个 `LIST` 块并不稀奇。
        /// 要求各段的 `fmt ` 块**逐字节相同**（同一把嗓子必然相同）；不同就如实报错，
        /// 不去硬接（接出来是个坏文件，播放器只会给一句看不懂的错）。
        /// </summary>
        internal static byte[] ConcatWav(IList<byte[]> wavs, out string why)
        {
            why = null;
            long total = 0;
            var dataStarts = new int[wavs.Count];
            var dataSizes = new int[wavs.Count];
            byte[] fmt = null;
            for (var i = 0; i < wavs.Count; i++)
            {
                var w = wavs[i];
                if (w == null || w.Length < 12 || w[0] != 'R' || w[1] != 'I' || w[2] != 'F' || w[3] != 'F')
                {
                    why = "第 " + (i + 1).ToString(CultureInfo.InvariantCulture) + " 段不是 WAV。";
                    return null;
                }
                var at = 12;
                var found = false;
                while (at + 8 <= w.Length)
                {
                    var size = (int)(w[at + 4] | (w[at + 5] << 8) | (w[at + 6] << 16) | (w[at + 7] << 24));
                    var body = at + 8;
                    if (size < 0 || body + size > w.Length) break; // 长度不对：不猜，跳出
                    var id = Encoding.ASCII.GetString(w, at, 4);
                    if (id == "fmt ")
                    {
                        var here = new byte[size];
                        Array.Copy(w, body, here, 0, size);
                        if (fmt == null) fmt = here;
                        else if (!Same(fmt, here))
                        {
                            why = "分段的音频格式不一致（不该发生）—— 不硬接。";
                            return null;
                        }
                    }
                    else if (id == "data")
                    {
                        dataStarts[i] = body;
                        dataSizes[i] = size;
                        total += size;
                        found = true;
                    }
                    at = body + size + ((size % 2) == 1 ? 1 : 0);
                }
                if (!found)
                {
                    why = "第 " + (i + 1).ToString(CultureInfo.InvariantCulture) + " 段里没有 data 块。";
                    return null;
                }
            }
            if (fmt == null)
            {
                why = "第一段里没有 fmt 块（不是标准 WAV）。";
                return null;
            }
            if (total > int.MaxValue - 1024)
            {
                why = "接起来太长了。";
                return null;
            }

            /*
             * ⚠️ `head` 是**第一段里 data 那块内容开始的位置**（= data 头之后那一个字节）——
             *    不是"内容开始 + 8"。第一版写成 `dataStarts[0] + 8`，于是输出比它自己
             *    声明的长 8 个字节：多出来的是 8 个零，接在 data 头后面、真数据前面。
             *    症状极隐蔽：字节数、状态码、`RIFF` 头全"看着对"，只有**声明长度与真实
             *    长度差 8** —— 那条"自洽的 WAV"检查标准当场把它照了出来。
             */
            var head = dataStarts[0];                    // 第一段的头 + data 块自己那 8 字节
            var outBytes = new byte[head + (int)total];
            Array.Copy(wavs[0], 0, outBytes, 0, head);
            PutInt(outBytes, 4, head + (int)total - 8);   // RIFF 总长 = 文件长 - 8
            PutInt(outBytes, dataStarts[0] - 4, (int)total); // data 块长
            var cursor = head;
            for (var i = 0; i < wavs.Count; i++)
            {
                Array.Copy(wavs[i], dataStarts[i], outBytes, cursor, dataSizes[i]);
                cursor += dataSizes[i];
            }
            return outBytes;
        }

        private static bool Same(byte[] a, byte[] b)
        {
            if (a.Length != b.Length) return false;
            for (var i = 0; i < a.Length; i++)
            {
                if (a[i] != b[i]) return false;
            }
            return true;
        }

        private static void PutInt(byte[] buf, int at, int value)
        {
            buf[at] = (byte)(value & 0xFF);
            buf[at + 1] = (byte)((value >> 8) & 0xFF);
            buf[at + 2] = (byte)((value >> 16) & 0xFF);
            buf[at + 3] = (byte)((value >> 24) & 0xFF);
        }

        /* ── 暂存：合成的音频放在哪，页面从哪儿取 ─────────────────────────── */

        /*
         * 为什么是"内存 + 一条路由"而不是"把字节发给页面"：
         *   · 桥那条通道的纪律是**字节不过桥**（见 `web/src/shared/bridge.ts` 顶上三条），
         *     音频一律走 `https://…` 由宿主发；
         *   · 于是合成结果要有个地方放：这里是一个**上界固定**的内存字典
         *     （`MaxEntries` 条，先进先出淘汰），页面拿到的是一段 URL。
         *
         * ⚠️ 键是**内容寻址**的（音色 + 语速 + 语言 + 文本的哈希）：同一段话再点一次
         *    直接命中，不必重新合成。这正是参考实现 `AudioCache` 的思路 ——
         *    差别只在那一版还落盘（见下面"还没做"那一条）。
         */
        private const int MaxEntries = 64;

        private static readonly Dictionary<string, byte[]> Cache = new Dictionary<string, byte[]>();
        private static readonly Queue<string> Order = new Queue<string>();
        /// <summary>
        /// 每个缓存键的 MIME —— 因为**两条路产出的格式不一样**：系统离线那条是 WAV，
        /// 豆包在线那条是 **MP3**。少了这张表，`/__speech__/` 那条路由只能一律回 `audio/wav`，
        /// 而浏览器拿到一段 mp3 却被告知是 wav 时，表现是"能播但时长/波形乱"（很难查）。
        /// </summary>
        private static readonly Dictionary<string, string> Mime = new Dictionary<string, string>();

        /// <summary>算一段音频的缓存键（**纯函数**：同一个请求永远同一个键）
        ///
        /// ⚠️ `gainDb` 是**键的一部分**（参考实现的 `SpeechCacheKey` 里就有 `GainMath.DbToken`）：
        ///    同一个词、同一个嗓子、两个不同的音量是**两份音频** —— 少了这一格，
        ///    用户拖完滑块再点发音会拿到上一次那份（"改了没反应"）。
        /// </summary>
        internal static string CacheKey(string voiceName, int rate, string languageTag, double gainDb,
                                        string text)
        {
            using (var sha = System.Security.Cryptography.SHA256.Create())
            {
                var payload = Encoding.UTF8.GetBytes(
                    "sys-v1|" + (voiceName ?? "") + "|" + rate.ToString(CultureInfo.InvariantCulture) +
                    "|" + (languageTag ?? "") + "|" +
                    gainDb.ToString("0.0", CultureInfo.InvariantCulture) + "|" + (text ?? ""));
                var hash = sha.ComputeHash(payload);
                var sb = new StringBuilder(hash.Length * 2);
                foreach (var b in hash) sb.Append(b.ToString("x2", CultureInfo.InvariantCulture));
                return sb.ToString();
            }
        }

        /// <summary>暂存一段音频，回它的键（满了就淘汰最早那条）</summary>
        internal static string Store(string key, byte[] audio, string mime = "audio/wav")
        {
            lock (Gate)
            {
                if (Cache.ContainsKey(key)) return key;
                while (Order.Count >= MaxEntries)
                {
                    var dropped = Order.Dequeue();
                    Cache.Remove(dropped);
                    Mime.Remove(dropped);
                }
                Cache[key] = audio;
                Mime[key] = string.IsNullOrEmpty(mime) ? "audio/wav" : mime;
                Order.Enqueue(key);
                return key;
            }
        }

        /// <summary>按一段任意载荷算缓存键（内容寻址：同样的载荷永远同样的键）</summary>
        private static string HashKey(string payload)
        {
            using (var sha = System.Security.Cryptography.SHA256.Create())
            {
                var hash = sha.ComputeHash(Encoding.UTF8.GetBytes(payload ?? ""));
                var sb = new StringBuilder(hash.Length * 2);
                foreach (var b in hash) sb.Append(b.ToString("x2", CultureInfo.InvariantCulture));
                return sb.ToString();
            }
        }
        /// <summary>某个缓存键的 MIME（`__speech__` 那条路由据此给 Content-Type）</summary>
        internal static string MimeOf(string key)
        {
            lock (Gate)
            {
                string mime;
                return Mime.TryGetValue(key ?? "", out mime) ? mime : "audio/wav";
            }
        }

        /// <summary>取一段暂存的音频（`__speech__` 那条路由用）</summary>
        internal static bool TryGet(string key, out byte[] wav)
        {
            lock (Gate)
            {
                return Cache.TryGetValue(key ?? "", out wav);
            }
        }

        /// <summary>
        /// 把这块内存缓存清空（参考实现的 `speech:clearCache`）。
        ///
        /// ⚠️ 这是**壳里的平台动作**，不是内核的事：缓存的是"本机这把嗓子合成出来的字节"，
        ///    与平台绑死，换平台时它跟着换（见：**磁盘**缓存才是内核的活，
        ///    内核现在只有那条 64MB 的常量、没有实现）。
        /// </summary>
        internal static void ClearCache()
        {
            lock (Gate)
            {
                Cache.Clear();
                Order.Clear();
                Mime.Clear();
            }
        }

        /* ── 对外的两条（Dispatch 调）─────────────────────────────────────── */

        /// <summary>
        /// `speech.plan`：**壳把探测到的音色表注入**，其余全交给内核。
        ///
        /// ⚠️ 注入是**无条件覆盖**：页面不可能比壳更清楚本机装了什么嗓子，
        ///    所以那个入参不让页面填（填了也不作数）。
        /// </summary>
        internal static string Plan(IntPtr engine, string text, string dictId, string overridesJson = null)
        {
            string json;
            Dsh.SpeechPlan(engine, text ?? "", dictId, VoicesJson(), overridesJson, out json);
            return json;
        }

        /// <summary>
        /// `speech.speak`：**照内核规划出来的那条路出声**，回一段可以直接塞给
        /// `&lt;audio src&gt;` 的地址。
        ///
        /// 形状与参考实现的 `SpeakResult` 对齐（`{ok,url,mime,source,file,voiceId,voiceName,
        /// language,bytes,cached,why}`）；`ok=false` 时 `url` 为空，`reason` 是
        /// **内核那句话**（不许自己编一句"这段放不了"）。
        ///
        /// 这一版能走通的是三层：
        ///   · `dict`   → 交给 `/__sound__/` 那条路由（里面会 `audio_prepare`，`.spx` 也在那儿解）；
        ///   · `system` → 本机 SAPI 合成（`SynthesizeAll`），暂存后给一条 `__speech__` 地址；
        ///   · `online` → 豆包 HTTP（`dsh_speech_online_plan` → `TranslateHttp.Post` →
        ///                `dsh_speech_online_accept`，带内容缓存）。
        ///   （2026-09-24 更正：这条注释原来说 online「这一版没做」—— 那是 当时的说法，
        ///   后来实现补上了、注释忘了删，别按它去找"还没接"的那条路。）
        /// </summary>
        internal static string Speak(IntPtr engine, string text, string dictId,
                                     string overridesJson = null)
        {
            OnlinePending pending;
            var immediate = BeginSpeak(engine, text, dictId, overridesJson, out pending);
            if (pending == null) return immediate;
            /* 同步那条路（诊断 / 单元验收）：就地发 —— 它们没有界面要保活 */
            return FinishSpeak(engine, pending, PostOnline(pending));
        }

        /// <summary>
        /// 在线那一段的**待办**：plan 已算好、缓存没命中，就等一次 HTTP（2026-09-24 加）。
        ///
        /// 为什么要有这个类：`Speak` 原来是一条同步长函数，把 HTTP 夹在中间 ——
        /// 而它跑在**UI 线程**上（桥的 `WebMessageReceived`），于是最长 8 秒界面不动。
        /// 现在拆成 `BeginSpeak`（UI：内核 plan + 缓存）→ HTTP（**线程池**）→
        /// `FinishSpeak`（UI：内核 accept + 存缓存），这个对象就是中间那一步要带的东西。
        /// </summary>
        internal sealed class OnlinePending
        {
            internal string PlanJson;
            internal string CacheKey;
            internal string SpeakerId;
            internal string SpeakerName;
            internal string Language;
            internal string Why;
            internal string Label;
        }

        /**
         * **第一段**（必须在 UI 线程上叫）：能当场定的直接给结果 —— 词典自带录音、
         * 系统离线合成、在线缓存命中、"这次用不了"的那几种都在这里出结果；
         * **需要联网**时回 `null` 并给出 `pending`，调用方发完 HTTP 再调 `FinishSpeak`。
         *
         * ⚠️ 拆的时候有一条纪律：**结果 JSON 的形状一个字都不许变**（界面与 断言都认它），
         *    所以这里只是把原来的段落搬走，没有重排任何字段。
         */
        internal static string BeginSpeak(IntPtr engine, string text, string dictId,
                                          string overridesJson, out OnlinePending pending)
        {
            pending = null;
            var plan = Plan(engine, text, dictId, overridesJson);
            var source = VirtualHost.JsonString(plan, "source") ?? "none";
            var language = VirtualHost.JsonString(plan, "language") ?? "";
            var why = VirtualHost.JsonString(plan, "why") ?? "";
            var disabled = VirtualHost.JsonString(plan, "disabledReason") ?? "";
            var trimmed = (text ?? "").Trim();

            if (trimmed.Length == 0 || source == "none")
            {
                return Result(false, "", "", "none", "", "", "", language, 0, 0, false,
                              disabled.Length > 0 ? disabled : "这次没有可用的音源。", why);
            }

            if (source == "dict")
            {
                var key = VirtualHost.JsonString(plan, "audioKey") ?? "";
                /*
                 * ⚠️ 这段录音住在**哪一本**里，是内核给的 `chosen.dictId` ——
                 *    壳**不许**拿自己那一侧的入参去顶（入参为空时内核走的是"当前词典"，
                 *    壳根本不知道当前是哪本）。地址也必须拼在**词典资源域**上：
                 *    `/__sound__/` 那条路由属于 `*.dictres.invalid`，不在外壳站点上。
                 */
                var owner = VirtualHost.JsonString(plan, "dictId") ?? "";
                if (key.Length == 0 || owner.Length == 0)
                {
                    return Result(false, "", "", "dict", "", "", "", language, 0, 0, false,
                                  "规划说走词典自带录音，却没给键名或词典（内核的答案不完整）。", why);
                }
                /*
                 * ⚠️ 地址拼在**外壳站点**上（`lookup.invalid/__sound__/<词典 id>/<键名>`），
                 *    不是词典域那条！宿主页的 CSP 是 `default-src 'self'` —— 跨域那条
                 *    页面**取不到也放不了**（2026-09 诊断发现的）。
                 *    词条正文自己播的时候才走词典域，那条由词条正文的桥接脚本自己拼。
                 */
                var url = SoundUrlOf(owner, key);
                return Result(true, url, "", "dict", key, "", "", language, 0, 0, false, "", why,
                              LabelOf(engine, text, dictId), DictGainOf(engine, owner));
            }

            if (source == "online")
            {
                /*
                 * ── 豆包在线（三层里的第二层）──────────────────────────────────
                 *
                 * 这条路是"内核—平台—内核"三段式的第二个例子（第一个是机器翻译，
                 * 见 `ShellBridge` 里 `translate:text` 那一段）：
                 *   ① 内核 `dsh_speech_online_plan`：说该用哪个音色、模型版本配不配、
                 *      发到哪儿、请求体长什么样（**"中英混排必须走中文音色"那条约定在内核里**）；
                 *   ② 这一段：把那段字节 POST 出去，**整段缓冲**回包（SSE 的解析在内核）；
                 *   ③ 内核 `dsh_speech_online_accept`：把 HTTP 状态 + 整段回包 + 耗时交回去，
                 *      拿回**音频字节**或一句人话。
                 *
                 * ⚠️ 这一层**不认识**音色、也不认识 SSE / 错误码 —— 它只会"发"与"交回去"。
                 * ⚠️ 2026-09-24：② 那一步挪到了**线程池**上（见 `OnlinePending`），
                 *    ① 与 ③ 仍留在 UI 线程。
                 */
                string planJson;
                /*
                 * ⚠️ 把**同一串** overrides 交给内核那条接口：规划说"用这个音色/这个响度"，
                 *    真拼请求体时必须是同一份东西，否则会出现"规划说 A、请求体里是 B"。
                 */
                Dsh.SpeechOnlinePlan(engine, text, dictId, overridesJson, out planJson);
                if (!VirtualHost.JsonBool(planJson, "needsHttp", false))
                {
                    /* 没填 Key / 没配音色 / 文本空：内核给的是一句人话，原样交给界面 */
                    return Result(false, "", "", "online", "", "", "", language, 0, 0, false,
                                  VirtualHost.JsonString(planJson, "reason") ?? "在线发音用不了。", why,
                                  LabelOf(engine, text, dictId));
                }

                /*
                 * ★ 先看缓存 —— **然后再决定发不发请求**。
                 *
                 * 豆包的输出按内容缓存（键里含 resourceId/speaker/rate/文本，**不含 Key**）。
                 * 为什么这一步非有不可：TTS **按字符计费**，而"同一个词反复点发音"是最常见的用法；
                 * 少了这一步，每点一次就是一次真实请求（花钱、也慢几百毫秒）。
                 * 键算在**请求体**上（它就是"这次要什么"的完整描述）。
                 */
                var onlineKey = HashKey("online-v1|" + (VirtualHost.JsonString(planJson, "body") ?? ""));
                /*
                 * 音色：**id 进 `voiceId`、界面上的说法进 `voiceName`** —— 与参考实现那条在线路
                 * 逐字同约定（`VoiceId = speaker` / `VoiceName = speakerName = DescribeSpeaker(…)`）。
                 * 名字问内核要（`SpeakerLabel`），壳里不抄表。
                 */
                var speakerId = VirtualHost.JsonString(planJson, "speaker") ?? "";
                var speakerName = SpeakerLabel(speakerId);
                byte[] already;
                if (TryGet(onlineKey, out already) && already != null && already.Length > 0)
                {
                    var hitUrl = "https://" + VirtualHost.ShellDomain + VirtualHost.SpeechRoute + onlineKey;
                    return Result(true, hitUrl, MimeOf(onlineKey), "online", "", speakerId, speakerName,
                                  language, already.Length, 0, true, "", why,
                                  LabelOf(engine, text, dictId));
                }

                /* 需要联网：把这一段要用的东西打包给调用方（它发完 HTTP 再回来收尾） */
                pending = new OnlinePending
                {
                    PlanJson = planJson,
                    CacheKey = onlineKey,
                    SpeakerId = speakerId,
                    SpeakerName = speakerName,
                    Language = language,
                    Why = why,
                    Label = LabelOf(engine, text, dictId),
                };
                return null;
            }

            /* ── source == "system"：本机 SAPI 合成 ─────────────────────────── */
            var voiceId = VirtualHost.JsonString(plan, "voiceId") ?? "";
            var voiceName = VirtualHost.JsonString(plan, "voiceName") ?? "";
            var speechVoice = (voiceId.Length > 0) ? voiceId : voiceName;
            var rate = (int)VirtualHost.JsonLong(plan, "rate");
            var tag = CultureOf(speechVoice);
            var chunks = VirtualHost.JsonStringList(plan, "chunks", "text", "chunkCount");
            var declared = (int)VirtualHost.JsonLong(plan, "chunkCount");
            if (chunks.Count == 0 || chunks.Count != declared)
            {
                /*
                 * ⚠️ 这条**必须如实报**，不许拿整段文本顶上去：段数是内核给的，
                 *    这里对不上说明"内核说的"和"壳读到的"不是一回事 —— 悄悄念完整段
                 *    会把一个接口定义问题盖成"能出声"（那种 bug 只会越来越难查）。
                 */
                return Result(false, "", "", "system", "", voiceId, voiceName, language, 0, 0, false,
                              "内核给的朗读分段读不出来（说好 " +
                              declared.ToString(CultureInfo.InvariantCulture) + " 段，只认出 " +
                              chunks.Count.ToString(CultureInfo.InvariantCulture) + " 段）。", why);
            }

            /*
             * ★ 音量（系统离线这一条路的增益）：**在"产字节"这一侧施加** ——
             *   参考实现就是在合成完之后、把字节交给播放器之前缩一遍
             *   （`GainMath.ApplyToWav`），所以这条路回包里的 `gainDb` **恒 0**
             *   （前端再乘一次就是叠两遍）。
             *
             * ⚠️ 增益必须进**缓存键**：同一个词、同一个嗓子、两个不同的音量是**两份音频**。
             *    少了它，用户拖完滑块再点发音，拿到的还是上一次那份（"改了没反应"）——
             *    参考实现的 `SpeechCacheKey` 里就有 `GainMath.DbToken(gainDb)` 这一格。
             * ⚠️ 缩放的算术**全在内核**（`dsh_audio_apply_gain`）：削顶保护、"0 dB 一个字节都不动"、
             *    只动 16 位 PCM 这些都是约定，不许在这儿重写一份。
             *
             * ⚠️ **这一次的覆盖优先**（`overrides_json` 里的 `gainTenthsDb`）：界面的「平衡音量」
             *    就是拿 `gainDb: 0` 去量**中性电平**的 —— 照设置施加的话，量到的电平会随着
             *    用户拖那个滑块自己变，算出来的补偿值来回震荡
             *    （`web/src/manager/main.ts` 的 `systemLevelOf` 那段注释写的就是这件事）。
             */
            var systemGainDb = GainOverrideOf(overridesJson) ?? SystemGainOf(engine);
            var cacheKey = CacheKey(speechVoice, rate, tag, systemGainDb,
                                    string.Join("\u0001", chunks));
            byte[] wav;
            var cached = TryGet(cacheKey, out wav);
            if (!cached)
            {
                string fail;
                wav = SynthesizeAll(chunks, speechVoice, tag, rate, out fail);
                if (wav == null)
                {
                    return Result(false, "", "", "system", "", voiceId, voiceName, language, 0, 0, false,
                                  string.IsNullOrEmpty(fail) ? "系统语音合成失败。" : fail, why);
                }
                var scaled = ApplyGain(engine, wav, systemGainDb, out fail);
                if (scaled == null)
                {
                    return Result(false, "", "", "system", "", voiceId, voiceName, language, 0, 0, false,
                                  string.IsNullOrEmpty(fail) ? "系统语音的音量没施加成功。" : fail, why);
                }
                wav = scaled;
                Store(cacheKey, wav);
            }
            var speechUrl = "https://" + VirtualHost.ShellDomain + VirtualHost.SpeechRoute + cacheKey;
            return Result(true, speechUrl, "audio/wav", "system", "", voiceId, voiceName, language,
                          wav.Length, chunks.Count, cached, "", why, LabelOf(engine, text, dictId));
        }

        /// <summary>在线那一段要发的请求（后台线程上跑；纯 HTTP，不碰内核）</summary>
        internal static TranslateHttp.Reply PostOnline(OnlinePending pending)
        {
            return TranslateHttp.Post(
                VirtualHost.JsonString(pending.PlanJson, "url"),
                TranslateHttp.ParseHeaders(ShellBridge.Raw(pending.PlanJson, "headers")),
                VirtualHost.JsonString(pending.PlanJson, "body"));
        }

        /**
         * **第三段**（必须在 UI 线程上叫）：把 HTTP 结果交给内核收尾、存缓存、铺回包。
         *
         * ⚠️ 存缓存放在这一段是**刻意的**：`Store` 动的是壳侧那个普通字典（注释明说
         *    "只在 UI 线程上改，所以不需要锁"）—— 三段式之后它仍然只在 UI 线程上被碰，
         *    于是**不用给缓存加锁**（能不加锁就不加锁）。
         */
        internal static string FinishSpeak(IntPtr engine, OnlinePending pending,
                                           TranslateHttp.Reply reply)
        {
            IntPtr audio;
            UIntPtr audioLen;
            string meta;
            Dsh.SpeechOnlineAccept(engine, pending.PlanJson, reply.Status, reply.Body, reply.ElapsedMs,
                                   out audio, out audioLen, out meta);

            if (!VirtualHost.JsonBool(meta, "ok", false) || audio == IntPtr.Zero ||
                (ulong)audioLen == 0)
            {
                /* 失败也要还：这一路上"没拿到音频"是常态，不是例外 */
                if (audio != IntPtr.Zero) DshRaw.dsh_release(audio);
                var whyNot = VirtualHost.JsonString(meta, "message");
                if (string.IsNullOrEmpty(whyNot))
                {
                    whyNot = string.IsNullOrEmpty(reply.Why)
                        ? "在线合成没拿到音频。"
                        : reply.Why;
                }
                return Result(false, "", "", "online", "", "", "", pending.Language, 0, 0, false, whyNot,
                              pending.Why, pending.Label);
            }

            /*
             * 存进 `__speech__` 那个内存缓存里，回一条可播地址 ——
             * 与系统离线那条路**共用同一个出口**（这样播放层一行都不用改）。
             * ⚠️ MIME 要按这次真正拿到的格式存（豆包给的是 **mp3**，不是 wav）。
             */
            var bytes = new byte[(int)(ulong)audioLen];
            System.Runtime.InteropServices.Marshal.Copy(audio, bytes, 0, bytes.Length);
            /*
             * ★ 拷完就还 —— 这段字节是**内核分配的**（接口定义：内核给出去的内存
             *   只有 `dsh_release` 能还）。少了这一句，**每点一次在线发音漏一段 mp3**
             *   （一条路一个进程生命周期，看着不像问题，但它就是漏）。
             */
            DshRaw.dsh_release(audio);

            Store(pending.CacheKey, bytes, VirtualHost.JsonString(meta, "mime") ?? "audio/mpeg");
            var onlineUrl = "https://" + VirtualHost.ShellDomain + VirtualHost.SpeechRoute +
                            pending.CacheKey;
            return Result(true, onlineUrl, VirtualHost.JsonString(meta, "mime") ?? "audio/mpeg",
                          "online", "", pending.SpeakerId, pending.SpeakerName, pending.Language,
                          bytes.Length, 0, false, "", pending.Why, pending.Label);
        }

        /// <summary>
        /// 铺一份回包 JSON（形状与参考实现的 `SpeakResult` 对齐）。
        ///
        /// 各字段的约定：
        ///   · `bytes` / `chunks` 只在**合成**那条路上有值（`dict` 那条路是放一段现成的
        ///     录音，字节由 `/__sound__/` 那条路由从 `.mdd` 里取 —— 宿主这会儿不读它，
        ///     所以老实给 0 而不是编一个数）；`mime` 同理（那条路由自己会给 Content-Type）。
        ///   · `cached` 只对合成那一路有意义（同一段话再点一次不必重新合成）。
        ///   · `reason` 是**失败时那句人话**，一律来自内核或语音引擎，宿主不编。
        ///   · `why` 是**内核那句话**（"系统语音（离线） · Microsoft Zira（英语）"），
        ///     界面拿它当悬停提示 —— 界面一个字都不拼。
        ///
        /// ⚠️ 下面四个键是**界面那份 `SpeakResult` 要、而这一版原来没给**的
        ///    （`web/shared/types.ts` 的 `SpeakResult`）：
        ///   · `message` —— 失败时那句人话（界面读的是它，不是 `reason`）；成功时空串；
        ///   · `languageLabel` —— 语种的**中文名**（"英语"）。它是产品文案，
        ///     照硬规则只许有一处来源：内核的语种表（`dsh_language_detect` 给的 `label`），
        ///     所以由调用方算好传进来，这一层不拼；
        ///   · `templateIndex` —— 在线音源"第几条 URL 模板"的遗留字段：**恒 -1**
        ///     （豆包只有一个端点。参考实现的注释原话：字段留着是为了不动线上格式，
        ///     界面文案不要再依据它）；
        ///   · `gainDb` —— 播放侧要施加的增益。**只有词典自带录音那条非 0**（它跟着"这本词典
        ///     存的那个数"走，见 `DictGainOf`）；系统离线与豆包恒 0 —— 那两条的音量
        ///     已经在"产字节"那一侧施加过了，前端再乘一次就是叠两遍。
        /// </summary>
        private static string Result(bool ok, string url, string mime, string source, string file,
                                     string voiceId, string voiceName, string language, int bytes,
                                     int chunks, bool cached, string reason, string why,
                                     string languageLabel = null, double gainDb = 0)
        {
            var sb = new StringBuilder();
            sb.Append("{\"ok\":").Append(ok ? "true" : "false");
            sb.Append(",\"url\":").Append(Quote(url));
            sb.Append(",\"mime\":").Append(Quote(mime));
            sb.Append(",\"source\":").Append(Quote(source));
            sb.Append(",\"file\":").Append(Quote(file));
            sb.Append(",\"voiceId\":").Append(Quote(voiceId));
            sb.Append(",\"voiceName\":").Append(Quote(voiceName));
            sb.Append(",\"language\":").Append(Quote(language));
            sb.Append(",\"languageLabel\":").Append(Quote(languageLabel ?? ""));
            sb.Append(",\"bytes\":").Append(bytes.ToString(CultureInfo.InvariantCulture));
            sb.Append(",\"chunks\":").Append(chunks.ToString(CultureInfo.InvariantCulture));
            sb.Append(",\"cached\":").Append(cached ? "true" : "false");
            sb.Append(",\"templateIndex\":-1");
            sb.Append(",\"gainDb\":").Append(gainDb.ToString("0.0", CultureInfo.InvariantCulture));
            sb.Append(",\"message\":").Append(Quote(reason));
            sb.Append(",\"reason\":").Append(Quote(reason));
            /* `why` 是**内核那句话**（"系统离线语音 · Microsoft Huihui Desktop（中文）"），
             * 界面拿它当悬停提示 —— 界面一个字都不拼。 */
            sb.Append(",\"why\":").Append(Quote(why));
            sb.Append('}');
            return sb.ToString();
        }

        /// <summary>
        /// 语种的**中文名**（"英语" / "中文"）—— 界面那份 `SpeakResult.languageLabel` 要它。
        ///
        /// 为什么不在这儿拼：语种码 → 中文名那张表在内核里（`dsh_language_name`），
        /// 而它就是**判定语种用的同一张表**。壳里再存一份，两边迟早对不上
        /// （参考实现的注释专门点过这一条：`SpeechStatus.languages` 由后端给，
        /// 就是因为它与判定用的是同一张表）。所以这里只问内核要一个字符串。
        /// </summary>
        private static string LabelOf(IntPtr engine, string text, string dictId)
        {
            try
            {
                string json;
                Dsh.LanguageDetect(engine, text ?? "", dictId, out json);
                return VirtualHost.JsonString(json, "languageLabel") ?? "";
            }
            catch (Exception)
            {
                /* 问不到就空着 —— 界面显示"语种名缺失"也好过壳自己编一个 */
                return "";
            }
        }

        /// <summary>
        /// 词条正文里点了某个喇叭（`sound://` / `snd://`）—— 参考实现的 `speech:sound`。
        ///
        /// 与"按钮那条路"（<see cref="Speak"/>）的差别：这里点的是**具体的某一段**
        /// （可能是例句），所以**不挑口音、也不兜底换音源** —— 要的就是那一段，
        /// 放不了就如实说（参考实现的原话）。
        ///
        /// 这一步为什么会去读字节：`/__sound__/` 那条路由虽然自己也会 `audio_prepare`，
        /// 但那句话是**发回给 `&lt;audio&gt;` 的**（页面只在播放失败时才看得到）。
        /// 而这里先整备一次，就能在**点击那一刻**把"这段放不了、因为它是 Speex 而这一版解不了"
        /// 直接交给界面去显示 —— 用户不用等播放器不报错地失败。两处用的是同一个内核接口，
        /// 所以检查标准不会有两份。
        /// </summary>
        internal static string Sound(IntPtr engine, string dictId, string key)
        {
            if (string.IsNullOrEmpty(dictId) || string.IsNullOrEmpty(key))
            {
                return Result(false, "", "", "dict", "", "", "", "", 0, 0, false,
                              "这次没给词典或录音键名。", "");
            }

            byte[] raw;
            string why;
            if (!VirtualHost.TryResourceBytes(engine, dictId, key, out raw, out why))
            {
                return Result(false, "", "", "dict", key, "", "", "", 0, 0, false,
                              string.IsNullOrEmpty(why) ? "这段录音取不出来。" : why, "");
            }

            byte[] playable;
            string mime;
            string audioWhy;
            if (!VirtualHost.PrepareAudio(raw, out playable, out mime, out audioWhy))
            {
                // 内核那句话（"…是 Speex，这一版的 C 内核还没接…"）原样交给界面
                return Result(false, "", "", "dict", key, "", "", "", raw.Length, 0, false,
                              string.IsNullOrEmpty(audioWhy) ? "这段录音放不了。" : audioWhy, "");
            }

            /*
             * 地址拼在**外壳站点**上（`lookup.invalid/__sound__/<词典 id>/<键名>`）：
             * 这条路是**宿主页**在播（`playSound` 的调用方就是页面），而宿主页的 CSP 是
             * `default-src 'self'` —— 词典域那条跨域地址它取不到也放不了。
             * 两条路由都落在 `VirtualHost.ServeDictSound` 上，所以检查标准只有一份。
             */
            var url = SoundUrlOf(dictId, key);
            /*
             * 增益跟"发音按钮"那条路**一致**（参考实现的原话）：同一个词典的同一段录音，
             * 不管是从按钮还是从词条里的 🔊 点开的，听起来都该一样响。
             */
            return Result(true, url, mime, "dict", key, "", "", "", playable.Length, 0, false, "", "",
                          null, DictGainOf(engine, dictId));
        }

        /// <summary>
        /// 一段**词典自带录音**的可播地址（**只有这一处拼**）。
        ///
        /// ⚠️ 拼在**外壳站点**上（`lookup.invalid/__sound__/<词典 id>/<键名>`），不是词典域那条：
        ///    宿主页的 CSP 是 `default-src 'self'`，跨域那条页面取不到也放不了
        ///    （2026-09 诊断发现的）。词条正文自己播的时候才走词典域，
        ///    那条由词条正文的桥接脚本自己拼。
        ///
        /// 三处调用方：`Speak` 的 dict 那一层、`Sound`（词条里的 🔊）、`DictSamples` ——
        /// 前两处原来各拼一遍，第三处再抄一遍就成三份了，所以收到这里。
        /// </summary>
        private static string SoundUrlOf(string dictId, string key)
        {
            return "https://" + VirtualHost.ShellDomain + VirtualHost.SoundRoute +
                   Uri.EscapeDataString(dictId ?? "") + "/" + Uri.EscapeDataString(key ?? "");
        }

        /* ── 词典取样：挑几条有录音的词条（参考实现的 `speech:dictSamples`）────── */

        /// <summary>
        /// 从一本词典里挑几条**真的有录音**的词条，交回界面那份 `DictSamplesResult`。
        ///
        /// ── 这一层做什么、不做什么 ──────────────────────────────────────────────
        /// 「挑哪几条、每条真有没有录音、什么语种、扫了几条、哪句人话」**全在内核**
        /// （`dsh_speech_dict_samples`）。这一层只做一件**平台形状**的事：
        /// 给每条补上**可播地址**（内核给的是 `.mdd` 里的键名 `audioKey`，
        /// 地址怎么拼是外壳域的事 —— 与 `dsh_speech_plan` 的 dict 那一层同一条规矩）。
        ///
        /// ⚠️ 界面那份 `DictSample` 要的字段是 `{word,file,language,languageLabel,url}`：
        ///    `file` 就是内核的 `audioKey`（`.mdd` 里那条文件的实际键名），
        ///    **不是另一份数据** —— 所以这里只是改个名，不重新算任何东西。
        ///
        /// ⚠️ 今天**没有产品流程调它**（「平衡音量」按按需求删掉了，参考实现亦然）：
        ///    诊断（ --dict-samples`）在验它。**别因为没人调就删掉。**
        /// </summary>
        internal static string DictSamples(IntPtr engine)
        {
            string json;
            /* dict_id 传 null：内核按"当前词典（空着就兜底第一本）"取，壳不猜是哪本 */
            Dsh.SpeechDictSamples(engine, null, out json);

            var sb = new StringBuilder();
            sb.Append("{\"ok\":").Append(VirtualHost.JsonBool(json, "ok", false) ? "true" : "false");
            sb.Append(",\"message\":").Append(Quote(VirtualHost.JsonString(json, "message") ?? ""));
            sb.Append(",\"dictId\":").Append(Quote(VirtualHost.JsonString(json, "dictId") ?? ""));
            sb.Append(",\"dictTitle\":").Append(Quote(VirtualHost.JsonString(json, "dictTitle") ?? ""));
            sb.Append(",\"samples\":[");

            var dictId = VirtualHost.JsonString(json, "dictId") ?? "";
            var items = ShellBridge.SplitArray(ShellBridge.Raw(json, "samples") ?? "[]");
            for (var i = 0; i < items.Count; i++)
            {
                var item = items[i];
                var key = ShellBridge.Text(item, "audioKey") ?? "";
                if (i > 0) sb.Append(',');
                sb.Append("{\"word\":").Append(Quote(ShellBridge.Text(item, "word") ?? ""));
                sb.Append(",\"file\":").Append(Quote(key));
                sb.Append(",\"language\":").Append(Quote(ShellBridge.Text(item, "language") ?? ""));
                sb.Append(",\"languageLabel\":")
                  .Append(Quote(ShellBridge.Text(item, "languageLabel") ?? ""));
                sb.Append(",\"url\":")
                  .Append(Quote(key.Length > 0 ? SoundUrlOf(dictId, key) : ""));
                sb.Append('}');
            }
            sb.Append("]}");
            return sb.ToString();
        }

        /* ── 检测凭据：拿一个样本词真发一次请求（参考实现的 `speech:testDoubao`）── */
        /// <summary>
        /// 管理窗「检测凭据」的语音那一半：**逐项**真发一次合成请求，把每一项的结果报回去。
        ///
        /// ── 这一层做什么、不做什么 ──────────────────────────────────────────────
        /// 「测几项、每项用哪个音色、念哪个词」**全在内核**（`dsh_speech_online_test_plan`
        /// 回的 `plans[]`）：给一个音色就一项，没给就英文 + 中文各一项。这一层只做两件
        /// **平台能力**的事 —— 把每一项的字节发出去、把 HTTP 结果与内核那句话拼成界面那份
        /// `DoubaoTest`。**它不认识**"没填音色"与"没填 Key"的区别（那是内核 `reason` 里的话）。
        ///
        /// ⚠️ 与"真念一段话"（<see cref="Speak"/>）的差别只有三处：① 走的是
        ///    `test_plan`（音色由**界面正在填的那个**定，不是设置里存着的）；② 拿到的音频
        ///    **不播也不留**（检测只看通不通、几个字节），但**必须还掉**（内核分配的）；
        ///    ③ 每一项都要报回结果，所以"两项里有一项没配音色"也要如实说，不许整条抛。
        ///
        /// ⚠️ 它**不受"在线总开关"限制**：用户当然应该先测通了再打开它（参考实现同一约定）。
        ///    但这是**真联网、按字符计费**的一次请求，所以只由用户点按钮触发。
        /// </summary>
        internal static string TestDoubao(IntPtr engine, string speaker, string language)
        {
            var plans = TestPlans(engine, speaker, language);
            var sb = new StringBuilder();
            sb.Append('[');
            for (var i = 0; i < plans.Count; i++)
            {
                if (i > 0) sb.Append(',');
                sb.Append(OneTest(engine, plans[i]));
            }
            sb.Append(']');
            return sb.ToString();
        }

        /// <summary>
        /// 「检测凭据」那几项**要发什么**（内核给的计划数组；2026-09-24 拆出来给两段式用）。
        ///
        /// ⚠️ 这一段要调内核 → **只能在 UI 线程上叫**。
        /// </summary>
        internal static List<string> TestPlans(IntPtr engine, string speaker, string language)
        {
            string plansJson;
            Dsh.SpeechOnlineTestPlan(engine, speaker, language, out plansJson);
            return ShellBridge.SplitArray(ShellBridge.Raw(plansJson, "plans") ?? "[]");
        }

        /// <summary>这一项要发 HTTP 吗（不用发的那些：没填 Key / 音色——内核会给人话）</summary>
        internal static bool TestNeedsHttp(string plan)
        {
            return ShellBridge.Flag(plan, "needsHttp", false);
        }

        /// <summary>这一项要发的请求（**后台线程**上跑：纯 HTTP，不碰内核）</summary>
        internal static TranslateHttp.Reply PostTest(string plan)
        {
            return TranslateHttp.Post(
                ShellBridge.Text(plan, "url") ?? "",
                TranslateHttp.ParseHeaders(ShellBridge.Raw(plan, "headers")),
                ShellBridge.Text(plan, "body"));
        }

        /// <summary>
        /// 测一项并铺成 JSON（**UI 线程**：不发 HTTP 的那几项由它直接出结果，
        /// 发过的那些由它收尾）。
        ///
        /// `reply == null`（可空）表示"这一项还没有发过" —— 两种情形：这一项本来就不需要发
        /// （`needsHttp=false`，内核已给人话），或者调用方走的是**同步那条路**（诊断）。
        /// 可空是刻意的：`TranslateHttp.Reply` 是 struct，`null` 在 C# 里表达不了"没发过"。
        /// </summary>
        internal static string OneTest(IntPtr engine, string plan, TranslateHttp.Reply? reply = null)
        {
            var speaker = ShellBridge.Text(plan, "speaker") ?? "";
            var language = ShellBridge.Text(plan, "language") ?? "";
            var text = ShellBridge.Text(plan, "text") ?? "";
            /* 请求体里**刻意不写** `explicit_language`，所以这一项恒为空串（内核给的）*/
            var explicitLanguage = ShellBridge.Text(plan, "explicitLanguage") ?? "";
            var url = ShellBridge.Text(plan, "url") ?? "";

            if (!ShellBridge.Flag(plan, "needsHttp", false))
            {
                /*
                 * 这一项发不出去（音色没填 / 没填 Key）：把**内核那句话原文**报上去，
                 * 一个字都不改 —— 界面靠它区分"这一项没东西可测"与"真的不通"
                 * （`classifyDoubaoTest` 的检查标准）。
                 */
                return TestItem(speaker, language, text, explicitLanguage, url, false, 0, 0, "", 0, 0,
                                ShellBridge.Text(plan, "reason") ?? "");
            }

            if (reply == null)
            {
                /* 同步那条路（诊断）：就地发 */
                reply = PostTest(plan);
            }
            var sent = reply.Value;

            IntPtr audio;
            UIntPtr audioLen;
            string meta;
            Dsh.SpeechOnlineAccept(engine, plan, sent.Status, sent.Body, sent.ElapsedMs, out audio,
                                   out audioLen, out meta);
            /*
             * 检测这一路**不播、也不留**那段音频，但**必须还掉** —— 它是内核分配的
             * （接口定义：内核给出去的内存只有 `dsh_release` 能还）。
             */
            if (audio != IntPtr.Zero) DshRaw.dsh_release(audio);
            var gotBytes = (long)(ulong)audioLen;

            var ok = VirtualHost.JsonBool(meta, "ok", false);
            var message = VirtualHost.JsonString(meta, "message");
            if (!ok && string.IsNullOrEmpty(message))
            {
                /* 内核没给人话（不该发生）→ 用平台那句兜住形状，别让界面看到一个空的 error */
                message = string.IsNullOrEmpty(sent.Why) ? "在线合成没拿到音频。" : sent.Why;
            }
            return TestItem(speaker, language, text, explicitLanguage, url, ok, sent.Status,
                            ok ? gotBytes : 0, ok ? (VirtualHost.JsonString(meta, "mime") ?? "") : "",
                            sent.ElapsedMs, VirtualHost.JsonLong(meta, "textWords"), ok ? "" : message);
        }

        /// <summary>一份 `DoubaoTest`（界面那份形状，逐字段同名：`web/shared/types.ts`）</summary>
        private static string TestItem(string speaker, string language, string text,
                                       string explicitLanguage, string url, bool ok, int statusCode,
                                       long bytes, string mime, int elapsedMs, long billedWords,
                                       string error)
        {
            var sb = new StringBuilder();
            sb.Append("{\"speaker\":").Append(Quote(speaker));
            /*
             * `speakerName` 是**界面上的说法**（参考实现同约定：`SpeakerName = DescribeSpeaker(target)`）：
             * 默认那两个是 `Dacey` / `Vivi`，用户自己填的就是 id 本身。
             * ★ 问内核要（`SpeakerLabel`）—— 那张 id→名字的表在内核里，壳不抄第二份（那个坑）。
             */
            sb.Append(",\"speakerName\":").Append(Quote(SpeakerLabel(speaker)));
            sb.Append(",\"language\":").Append(Quote(language));
            sb.Append(",\"text\":").Append(Quote(text));
            sb.Append(",\"explicitLanguage\":").Append(Quote(explicitLanguage));
            sb.Append(",\"url\":").Append(Quote(url));
            sb.Append(",\"ok\":").Append(ok ? "true" : "false");
            sb.Append(",\"statusCode\":").Append(statusCode.ToString(CultureInfo.InvariantCulture));
            sb.Append(",\"bytes\":").Append(bytes.ToString(CultureInfo.InvariantCulture));
            sb.Append(",\"mime\":").Append(Quote(mime));
            sb.Append(",\"elapsedMs\":").Append(elapsedMs.ToString(CultureInfo.InvariantCulture));
            sb.Append(",\"billedWords\":").Append(billedWords.ToString(CultureInfo.InvariantCulture));
            sb.Append(",\"error\":").Append(Quote(error));
            sb.Append('}');
            return sb.ToString();
        }

        /* ── 给 `speech:status` 那一层用的几个只读出口 ───────────────────────── */

        /// <summary>
        /// 系统离线语音那条路的增益（dB）—— 内核那份 `VoiceGains` 视图里的 `systemGainDb`。
        ///
        /// 为什么不读设置：那个数**已经归一化**过了（夹到 [-24,+12]、取整到 0.1），
        /// 而"怎么归一"是内核的约定（见 `dsh_speech_gains_api.c`）。壳里再读一遍设置、
        /// 再夹一遍，就是同一个约定两个来源。
        /// </summary>
        internal static double SystemGainOf(IntPtr engine)
        {
            if (engine == IntPtr.Zero) return 0;
            try
            {
                string json;
                Dsh.SpeechGains(engine, null, VoiceCount(), out json);
                return ShellBridge.Num(json, "systemGainDb") ?? 0;
            }
            catch (Exception)
            {
                /* 问不到就按"不增不减"播（界面照样出声，只是不受那条滑块管）*/
                return 0;
            }
        }

        /// <summary>
        /// 「这一次的覆盖」里的增益（`overrides_json` 的 `gainTenthsDb`，dB×10）；
        /// 没给回 null = 照设置（<see cref="SystemGainOf"/>）。
        ///
        /// 为什么覆盖是**整数 dB×10**：接口定义没有浮点标量，而增益在设置那一层本来就取整到
        /// 0.1 dB（与 `dsh_audio_apply_gain` 的入参同一条约定）；界面传的 `gainDb` 是个小数，
        /// 由 <see cref="ShellBridge"/> 那边换算成 dB×10 再进来。
        /// </summary>
        private static double? GainOverrideOf(string overridesJson)
        {
            if (string.IsNullOrEmpty(overridesJson)) return null;
            var tenths = ShellBridge.Num(overridesJson, "gainTenthsDb");
            return tenths.HasValue ? tenths.Value / 10.0 : (double?)null;
        }

        /// <summary>
        /// 把音量**加到字节上**（系统离线语音那条路）：算全在内核
        /// （`dsh_audio_apply_gain`），这里只搬字节。
        ///
        /// ⚠️ 效果**不只是"数字记得住"**：内核会按峰值做削顶保护，并在被夹时说一句话。
        ///    0 dB 时内核**一个字节都不动**，所以没调过增益的路径与"没有这个功能"逐字节相同。
        /// </summary>
        private static byte[] ApplyGain(IntPtr engine, byte[] wav, double gainDb, out string why)
        {
            why = "";
            if (wav == null || wav.Length == 0) return wav;
            var tenths = (int)Math.Round(gainDb * 10.0, MidpointRounding.AwayFromZero);
            IntPtr input = System.Runtime.InteropServices.Marshal.AllocHGlobal(wav.Length);
            IntPtr outBytes = IntPtr.Zero;
            try
            {
                System.Runtime.InteropServices.Marshal.Copy(wav, 0, input, wav.Length);
                UIntPtr outLen = UIntPtr.Zero;
                string meta;
                Dsh.AudioApplyGain(input, (UIntPtr)(ulong)wav.Length, tenths, out outBytes, out outLen,
                                    out meta);
                if (outBytes == IntPtr.Zero || (ulong)outLen == 0)
                {
                    why = "音量这一步没拿到字节。";
                    return null;
                }
                var scaled = new byte[(ulong)outLen];
                System.Runtime.InteropServices.Marshal.Copy(outBytes, scaled, 0, scaled.Length);
                /* 被夹时那句人话（"峰值已经顶到满刻度…"）写在控制台里 —— 它不影响播放 */
                var note = VirtualHost.JsonString(meta, "note");
                if (!string.IsNullOrEmpty(note))
                {
                    Console.Error.WriteLine("[speech] 系统语音增益：" + note);
                }
                return scaled;
            }
            catch (Exception err)
            {
                why = "音量没施加成功：" + err.Message;
                return null;
            }
            finally
            {
                if (outBytes != IntPtr.Zero) DshRaw.dsh_release(outBytes);
                System.Runtime.InteropServices.Marshal.FreeHGlobal(input);
            }
        }

        /// <summary>探本机音色（**只探一次**，之后走缓存）；探不到时 `why` 是原因</summary>
        internal static List<Voice> ProbeVoices(out string why)
        {
            return Probe(out why);
        }

        /// <summary>本机探到的离线音色数（`voiceGains.systemAvailable` 与增益视图要用它）</summary>
        internal static int VoiceCount()
        {
            string why;
            return Probe(out why).Count;
        }

        /// <summary>
        /// **某本词典**自带录音的增益（dB）—— 参考实现的 `SpeechService.DictGainFor`。
        ///
        /// 为什么不在这儿自己读设置：增益那张表（`speech.dictGainDb` 是"一本一个数"的字典）
        /// 与"没设过 = 0"的约定都在内核那份 `VoiceGains` 视图里（见 `dsh_speech_gains_api.c`）。
        /// 壳里再读一遍设置拼一份，就是同一个约定有两个来源 —— 它们迟早会在某个分支上分叉。
        ///
        /// 用途只有一处：`speech:speak` / `speech:sound` 回包里那个 `gainDb` ——
        /// **只有词典原录音这条路非 0**，前端拿它自己乘（参考实现的原话：词典原录音是压缩格式，
        /// 服务端缩放要整段重编码，所以这一路是前端施加的）。
        /// </summary>
        internal static double DictGainOf(IntPtr engine, string dictId)
        {
            if (engine == IntPtr.Zero || string.IsNullOrEmpty(dictId)) return 0;
            try
            {
                string json;
                Dsh.SpeechGains(engine, dictId, VoiceCount(), out json);
                return ShellBridge.Num(json, "dictGainDb") ?? 0;
            }
            catch (Exception)
            {
                /* 问不到就按"不增不减"播 —— 界面照样出声，只是不受那条滑块管 */
                return 0;
            }
        }

        /// <summary>在线语音的端点（界面照实显示；排错时一眼看出被指到哪儿去了）</summary>
        internal const string OnlineEndpoint = "https://openspeech.bytedance.com/api/v3/tts/unidirectional/sse";

        /// <summary>
        /// 账号级的那把 Key（语音与翻译共用）—— 读设置，读不到回空串。
        ///
        /// ⚠️ 它在设置里是**两层**的：`{"volcengine":{"apiKey":"…"}}`（内核把
        ///    `speech.doubaoApiKey` 镜像到那儿，两个字段同值 —— 见 `dsh_settings.h`）。
        ///    第一版在这儿找的是一个**平的**键名 `volcengineApiKey`：那个键**从来不存在**，
        ///    于是这一格恒为空 → 界面显示"没填 Key"、输入框里也没有掩码，
        ///    而发音那两条路（都走内核）照样能用 —— 症状就是**"能出声，但设置页说没配"**
        ///    （用户 2026-09 报的第 4 条）。`translate:status` 的 `hasApiKey` 由内核给，
        ///    所以它当时是对的 —— 这就是"同一个东西两个来源"露出来的地方。
        /// </summary>
        internal static string ApiKeyOf(IntPtr engine)
        {
            try
            {
                string json;
                Dsh.EngineSettingsGet(engine, out json);
                var key = ShellBridge.Text(ShellBridge.Raw(json, "volcengine"), "apiKey");
                if (string.IsNullOrEmpty(key))
                {
                    /* 兜底读镜像那一份（同值时哪边先读到都一样；镜像断了也不至于两处都空）*/
                    key = ShellBridge.Text(ShellBridge.Raw(json, "speech"), "doubaoApiKey");
                }
                return key ?? "";
            }
            catch (Exception)
            {
                return "";
            }
        }

        /// <summary>缓存里现在有几段（界面显示实测结果用）</summary>
        internal static int CacheCount
        {
            get { lock (Gate) { return Cache.Count; } }
        }

        /// <summary>缓存一共占多少字节</summary>
        internal static int CacheBytes
        {
            get
            {
                lock (Gate)
                {
                    var total = 0;
                    foreach (var item in Cache.Values) if (item != null) total += item.Length;
                    return total;
                }
            }
        }
    }
}
