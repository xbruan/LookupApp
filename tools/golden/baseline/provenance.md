# 标准答案文件基线的来源记录

> ⚠️ **本文件由 `tools/run-golden.sh --regen` 机械生成，别手改** —— 下次 --regen 会整份覆盖。
> 基线的规矩、白名单的含义、以及"为什么要与 C 侧分开"见同目录 `README.md`。

"标准答案"如果没有来源记录就等于没有标准：谁也说不清它出自哪一份参考实现、
哪一份清单、哪一天。所以下面这些字段是**强制**记下来的。

## 一、这一份是怎么生成的

| 项 | 值 |
| --- | --- |
| 生成时间（UTC） | 2026-09-29 05:04:41 |
| 生成的命令 | `sh tools/run-golden.sh <仓库的 WSL 路径> --regen`（特权操作，显式调用） |
| 产出它的程序 | `tools/golden/bin/Release/net48/GoldenDump.exe`（SHA256 `19c1300789c384c565b74004eebe2950409e7e7179c2f2bb1468cd1e3db7976e`） |
| 参考实现 | `reference/0.1.3-parser/src/Dictionary/`（0.1.3 的冻结副本，**只读**） |
| 参考实现清单 | `reference/0.1.3-parser/SHA256.txt`（SHA256 `2232dbe5fc8b20c31cf2f040624bf0140657f1d8ce70430584170012b1019a7d`） |
| 测试用词典清单 | `tools/golden/fixtures.txt`（SHA256 `607a7a153673535ef2f7d12f8c0c81b13e167b8cfd10380703b7d00cea10d6b3`，共 21 条） |
| 基线文件 | `golden-baseline.json`（399159 字节，SHA256 `73ddef58cdc9ed5c538a8b3c4025ee02340d75097e4b00e903fa6037d2441731`） |
| 与上一份基线 | 与上一份基线**逐字节相同**（参考实现与清单都没变过；这份 re-gen 是空跑） |
| dotnet SDK | 8.0.425 |
| git HEAD | `2ca9da831159a550e429ec7e237c32291c4797b9` |

## 二、文件构成

```text
golden-baseline.json   标准答案本体（一份 JSON 装全部测试用词典，按 fixtures.txt 的**顺序**）
provenance.md          本文件：这一份基线是怎么来的
README.md              基线的规矩（人工维护，不被 --regen 覆盖）
```

## 三、参考实现的逐文件 SHA256

基线是这些字节产出的 —— 它们一变，基线就不再代表任何东西（哪怕比对仍然是绿的）。

```text
  de1bd8333ecce0d30e1f0b7b3b30adcb27c91ea72cb72f29862161e6fb3e7d98  src/Dictionary/DictionaryEngine.cs
  2d5cacee72704bc1ca062b0df90bd20a7f4af9a821a95b081bc7f9e1ef23f310  src/Dictionary/DictionaryProbe.cs
  c3290c4b04aec0d1a54dbe52f18ddfc3e4862191fa8c5e6865ec1c2288790279  src/Dictionary/DictSampler.cs
  b50614aa454c257327bd6d828b16296189e9863828eb61feb269c58a6ea64786  src/Dictionary/EntryAudio.cs
  e40e18c2e1949c34a6aeed21cedb45d45b5ceef5d2b4eb220e9ec7fcf5be21fa  src/Dictionary/EntryDocument.cs
  b8df19ec18d3408845fdfc8865348c4aca043c05b4543b355bf4fe9e21910932  src/Dictionary/HtmlUtils.cs
  7cfcb94022add8ccb602bfa184b61a136e1bb84a85cbe32f17d5ae9530637d14  src/Dictionary/IndexSampling.cs
  2691974706141e1a8ba5d458934bf32398890ec68a5bf4f31058d4c3d066fb2b  src/Dictionary/LruCache.cs
  2362851af0a20b5308a612b7afa534dd90fab8080244d20d9f3d140a1c016ce3  src/Dictionary/Lzo1x.cs
  bc5e4820f726bf79a264cf2f02ed705d7df544e03785a29e104ac464e292dd10  src/Dictionary/MddReader.cs
  5612a7f5c6f2166332eae80d6dba8248b4db4bf608f47870119c1a9482a54f23  src/Dictionary/MdictCore.cs
  74620b26f5ce76ac74216daf768a8127b77da413077e0b7847731f882d0bbd13  src/Dictionary/MdictCrypto.cs
  f69b50dff7b3b1624e398972f524a8f534ece462387e74dd043cbefab960dc45  src/Dictionary/MdxReader.cs
  c4013264c00374cee30c1215a5a826162b1496e5a43f32689aa0e0c26b177968  src/Dictionary/MimeTypes.cs
  8e1a7822bf77129481599ee93c85af7e809f6d7ad306e971f3b5666a40660d5c  src/Dictionary/Ripemd128.cs
  4c7fd943ca5e230fcc8f65be03cfb13be8b281189ae9306dc18d37fb54c8ff45  src/Dictionary/SiblingResource.cs
```

## 四、用这份基线比什么

```sh
# 默认路径（不需要 .NET）：编 C 侧 → 跟这份基线比
sh tools/check-golden.sh <仓库的 WSL 路径>
```

判决标准是"逐字节 + 一张已确认差异的白名单"，白名单写在 `tools/golden/compare-golden.py` 顶部，
并附了现场实测证据（谁对、谁错、为什么）。**白名单外的任何差异都是失败**。
