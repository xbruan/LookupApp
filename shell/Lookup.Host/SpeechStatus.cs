namespace Lookup.Host
{
    using System;
    using System.Collections.Generic;
    using System.Globalization;
    using System.Text;
    using Lookup.Interop;

    /// <summary>
    /// `speech:status` —— **「语音」那一页要的全部现场**。这一层**一行判断都没有**：能不能念、走哪一层、挑哪个嗓子、
    /// 中英混排全在 `dsh_speech_plan` 里算好（连 `options[]` 的 label / detail / reason 都是内核给的人话），壳只做装配 —— 探本机离线音色
    /// + 拼成界面那份形状。⚠️ 字段名**必须与参考实现逐字相同**；⚠️ 语种中文名与 `voiceGains` 都**必须问内核**（`dsh_language_label` / `dsh_speech_gains`），壳里再存一份迟早会对不上。
    /// </summary>
    internal static class SpeechStatusBuilder
    {
        internal static string Build(IntPtr engine, string text, string dictId, string dictTitle,
                                     string keyText)
        {
            var sb = new StringBuilder(4096);

            /* ① 引擎（本机 SAPI）可用吗 —— 探不到就如实说，别让界面以为「能念」 */
            string voicesWhy;
            var voices = Speech.ProbeVoices(out voicesWhy);
            var engineAvailable = voices.Count > 0;

            /* ② 内核的规划：音源顺序、挑哪个嗓子、三条路各自能不能走（人话也在里面） */
            string plan;
            try
            {
                plan = Speech.Plan(engine, text ?? "", dictId);
            }
            catch (Exception err)
            {
                plan = "{\"source\":\"none\",\"why\":" + Dispatch.Quote("规划失败：" + err.Message) + "}";
            }

            /* ③ 设置里的 `speech` 那一节（界面照它摆表单）*/
            string settingsJson = "{}";
            try
            {
                string all;
                Dsh.EngineSettingsGet(engine, out all);
                settingsJson = ShellBridge.Raw(all, "speech") ?? "{}";
                /* 账号级那把 Key 在 `volcengine.apiKey` 里（语音与翻译共用）*/
            }
            catch (Exception) { /* 设置读不出来时给空对象，界面会按默认值渲染 */ }

            /* ④ 对当前这个词的语种判定（界面把 `reason` 原样当提示显示）*/
            string detectedJson = "{}";
            try
            {
                string detect;
                Dsh.LanguageDetect(engine, text ?? "", dictId, out detect);
                detectedJson = detect;
            }
            catch (Exception) { }

            sb.Append("{\"engineAvailable\":").Append(engineAvailable ? "true" : "false");
            sb.Append(",\"engineMessage\":")
              .Append(Dispatch.Quote(voicesWhy ?? ""));

            /* 音色清单：平台探到的 id/name/culture + **内核给的语种中文名** */
            sb.Append(",\"voices\":[");
            for (var i = 0; i < voices.Count; i++)
            {
                if (i > 0) sb.Append(',');
                sb.Append("{\"id\":").Append(Dispatch.Quote(voices[i].Id));
                sb.Append(",\"name\":").Append(Dispatch.Quote(voices[i].Name));
                sb.Append(",\"culture\":").Append(Dispatch.Quote(voices[i].Culture));
                sb.Append(",\"language\":").Append(Dispatch.Quote(voices[i].Language));
                sb.Append(",\"languageLabel\":").Append(Dispatch.Quote(LabelOf(engine, voices[i].Language)));
                sb.Append(",\"gender\":\"\"");
                sb.Append(",\"source\":\"system\"}");
            }
            sb.Append(']');

            sb.Append(",\"settings\":").Append(settingsJson);

            /* 对当前这个词的判定：界面要 text/language/label/tag/reason/mixed */
            sb.Append(",\"detected\":");
            sb.Append("{\"text\":").Append(Dispatch.Quote(text ?? ""));
            sb.Append(",\"language\":").Append(ShellBridge.Raw(detectedJson, "language") ?? "\"\"");
            sb.Append(",\"label\":").Append(ShellBridge.Raw(detectedJson, "languageLabel") ?? "\"\"");
            sb.Append(",\"tag\":").Append(Dispatch.Quote(ShellBridge.Text(detectedJson, "language") ?? ""));
            sb.Append(",\"reason\":").Append(ShellBridge.Raw(detectedJson, "explanation") ?? "\"\"");
            sb.Append(",\"mixed\":").Append(ShellBridge.Flag(detectedJson, "mixed", false) ? "true" : "false");
            sb.Append('}');

            /* 三条路各自能不能走 —— **内核给什么就照抄什么**（连人话都不许改写）*/
            sb.Append(",\"options\":").Append(ShellBridge.Raw(plan, "options") ?? "[]");

            /* 豆包那一块表单（只读视图；Key 只给掩码，完整值只有内核/设置里那一份）*/
            sb.Append(",\"doubao\":").Append(DoubaoView(engine, settingsJson));

            /*
             * `voiceGains`：设置页那两条音量滑块 —— **整份视图由内核给**（值、能不能调、不能调时那句话），壳一个字段都不拼。
             * ⚠️ 它认的是**当前词典**（内核按 `currentDictId` 取），**不是**这一条 `speech:status` 传下来的那个词条所属的词典 ——
             * 所以这里**故意不传 dictId**（那个入参只喂「本词条有没有自带录音」）。
             */
            sb.Append(",\"voiceGains\":").Append(VoiceGainsJson(engine, voices.Count));

            sb.Append(",\"languages\":[]"); /* 今天没有界面消费者（页面里写着这一条）*/

            /* 汇总那两句话：**内核给的 `why` / `disabledReason` 原样用** */
            sb.Append(",\"available\":").Append(engineAvailable ? "true" : "false");
            sb.Append(",\"message\":").Append(Dispatch.Quote(
                ShellBridge.Text(plan, "disabledReason") ?? ShellBridge.Text(plan, "why") ?? ""));
            sb.Append(",\"hint\":").Append(Dispatch.Quote(ShellBridge.Text(plan, "why") ?? ""));

            /* 壳里那块内存缓存的实测结果（界面显示"缓存里 N 段"）*/
            sb.Append(",\"cacheCount\":").Append(Speech.CacheCount.ToString(CultureInfo.InvariantCulture));
            sb.Append(",\"cacheBytes\":").Append(Speech.CacheBytes.ToString(CultureInfo.InvariantCulture));
            sb.Append('}');
            return sb.ToString();
        }

        /// <summary>语种码 → 中文名（**问内核要**，见类顶上那段）</summary>
        private static string LabelOf(IntPtr engine, string code)
        {
            try
            {
                string json;
                Dsh.LanguageLabel(code ?? "", out json);
                return ShellBridge.Text(json, "label") ?? "";
            }
            catch (Exception)
            {
                return "";
            }
        }

        /// <summary>
        /// 两条增益的视图（`voiceGains`）—— **整份由内核给**（见调用点那段注释）。
        /// ⚠️ 问不到时给 `null`（不是空对象）：界面把 `undefined` 当「读不到增益数据」（置灰 + 说明），
        /// 而空对象会被当成「增益都是 0、而且都能调」—— 那是把一次失败画成两个错的结论。
        /// </summary>
        private static string VoiceGainsJson(IntPtr engine, int voiceCount)
        {
            try
            {
                string json;
                Dsh.SpeechGains(engine, null, voiceCount, out json);
                return string.IsNullOrEmpty(json) ? "null" : json;
            }
            catch (Exception)
            {
                return "null";
            }
        }

        /// <summary>豆包那一块的表单视图（`DoubaoView`）</summary>
        private static string DoubaoView(IntPtr engine, string settingsJson)
        {
            var key = Speech.ApiKeyOf(engine);
            /*
             * 两个音色的**展示名**（`Dacey` / `Vivi`；用户自己填的音色就是 id 本身）：那张表在**内核**里
             * （`dsh_speech_speaker_label`），壳不许自己抄一份。
             */
            var speakerEnId = ShellBridge.Text(settingsJson, "doubaoSpeakerEn");
            var speakerZhId = ShellBridge.Text(settingsJson, "doubaoSpeakerZh");
            var sb = new StringBuilder();
            sb.Append("{\"endpoint\":").Append(Dispatch.Quote(Speech.OnlineEndpoint));
            sb.Append(",\"resourceId\":").Append(
                ShellBridge.Raw(settingsJson, "doubaoResourceId") ??
                Dispatch.Quote("seed-tts-2.0"));
            sb.Append(",\"format\":").Append(
                ShellBridge.Raw(settingsJson, "doubaoFormat") ?? Dispatch.Quote("mp3"));
            sb.Append(",\"hasApiKey\":").Append(string.IsNullOrEmpty(key) ? "false" : "true");
            sb.Append(",\"apiKeyMasked\":").Append(Dispatch.Quote(Mask(key)));
            sb.Append(",\"speakerEn\":").Append(
                ShellBridge.Raw(settingsJson, "doubaoSpeakerEn") ?? "null");
            sb.Append(",\"speakerEnName\":").Append(Dispatch.Quote(Speech.SpeakerLabel(speakerEnId)));
            sb.Append(",\"speakerZh\":").Append(
                ShellBridge.Raw(settingsJson, "doubaoSpeakerZh") ?? "null");
            sb.Append(",\"speakerZhName\":").Append(Dispatch.Quote(Speech.SpeakerLabel(speakerZhId)));
            sb.Append(",\"loudnessEn\":").Append(
                ShellBridge.Raw(settingsJson, "doubaoLoudnessEn") ?? "0");
            sb.Append(",\"loudnessZh\":").Append(
                ShellBridge.Raw(settingsJson, "doubaoLoudnessZh") ?? "0");
            sb.Append(",\"defaultLoudnessEn\":0,\"defaultLoudnessZh\":0");
            sb.Append('}');
            return sb.ToString();
        }

        /// <summary>
        /// Key 的掩码（`a30dba13…47fc`）。**只回前 8 后 4**：
        /// 界面拿它确认"填的是哪一把"，而完整值只有设置文件里那一份
        /// （参考实现同一条约定：掩码是为了认，不是为了还原）。
        /// </summary>
        private static string Mask(string key)
        {
            if (string.IsNullOrEmpty(key)) return "";
            if (key.Length <= 12) return "…";
            return key.Substring(0, 8) + "…" + key.Substring(key.Length - 4);
        }
    }
}
