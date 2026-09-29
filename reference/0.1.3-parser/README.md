# reference/0.1.3-parser —— 0.1.3 参考实现的**冻结副本（只读）**

## 这是什么

`reference/0.1.3-parser/src/Dictionary/` 是 **0.1.3（0.1.x 的最后一版）C# 参考实现里
`src/Dictionary/` 那一目录的逐字节副本**，一共 **16 个 `.cs` 文件**、合计 **191,424 字节**。

它放在这里的**唯一目的**是：让本仓库**不再依赖兄弟目录**（原来那份 0.1.3 工作目录，
已归档、不随本仓库发布）。以前三个 `.csproj` 与几支脚本都是
穿到仓库外面去链那棵树的 —— 于是"这个仓库能不能自洽地跑一道对照测试"取决于
**别人的目录今天还在不在、有没有被改**。现在不取决于了。

## ⚠️ 只读：一个字节都不许改

**这些文件是标准答案文件（golden file）的产者** —— `tools/golden/baseline/` 里那份
`golden-baseline.json` 就是它们跑出来的。参考实现一旦漂移，基线会**继续通过**、
却已经不反映任何东西了：**对照测试最坏的失败模式不是红，而是悄悄地绿**。

所以：

- **不要**在这里改 bug、改风格、加注释、加 `using`、统一换行 —— 要改的是 `native/` 里的 C 实现；
- **不要**把这里的文件当成"可以顺手整理"的源码；它们与 0.1.3 的 `src/Dictionary/` 逐字节相同，
  这是这份目录**唯一的价值**；
- 0.1.3 那边**已经冻结**（只修 bug），且本仓库与它之间**不再有同步关系** ——
  这里就是那一份快照，谁都不许再往回写、也不许从那边再拉一次覆盖它。

逐文件哈希见同目录 [`SHA256.txt`](SHA256.txt)（`sha256sum -c` 可直接核）。

## 来源（精确出处）

| 项 | 值 |
| --- | --- |
| 源路径 | `<0.1.3 工作目录>/src/Dictionary/`（那棵目录**已归档、不随本仓库发布**；这里只记"它来自 0.1.3 的 `src/Dictionary/`"这件事，不记任何人的机器路径） |
| 版本 | **0.1.3**（0.1.x 的最后一版；冻结时的快照） |
| 拷贝方式 | 整目录 `Copy-Item *.cs`（**不加任何转换**：换行、BOM、编码一律原样） |
| 拷贝日期 | 2026-09-29 |
| 文件数 / 字节数 | 16 / 191,424 |
| 整份清单的总哈希 | `SHA256(reference/0.1.3-parser/SHA256.txt)` = `1ffcbf73adb1af4e995e2503bc6181b66fa85d9e68ff39eb23c738435840ade1` |

## 谁在链它（**基线再生工具**，不是日常运行的必要依赖）

| 工程 | 链进来的文件 | 产出 |
| --- | --- | --- |
| `tools/golden/GoldenDump.csproj` | `src/Dictionary/*.cs`（`Remove` 掉两个应用层文件 `DictionaryEngine.cs` / `DictionaryProbe.cs` —— 它们依赖主程序的 `Models`/`Newtonsoft`） | `tools/golden/baseline/golden-baseline.json`（标准答案文件基线） |
| `tools/golden/EntryAssets.csproj` | `src/Dictionary/EntryDocument.cs` | `native/src/dict/entry_assets.h`、`native/tests/entry_doc_vectors.h` |
| `tools/golden/HtmlVectors.csproj` | `src/Dictionary/HtmlUtils.cs` | `native/tests/html_vectors.h` |

三份 `.csproj` 里的相对路径都是 `../../reference/0.1.3-parser/...`（`tools/golden/` 在仓库根下两层）。

**日常回归不碰这三个工程**：默认那道 gate（`tools/golden-gate.ps1` → `tools/check-golden.sh`）
只编、只跑 C 侧，然后与上面那份**冻结基线**比 —— 不需要 .NET、不需要编 C#。
只有**显式**再生基线时才编参考实现（见 `tools/run-golden.sh --regen`）。

## 逐文件 SHA256

| 文件 | SHA256 |
| --- | --- |
| `src/Dictionary/DictionaryEngine.cs` | `de1bd8333ecce0d30e1f0b7b3b30adcb27c91ea72cb72f29862161e6fb3e7d98` |
| `src/Dictionary/DictionaryProbe.cs` | `2d5cacee72704bc1ca062b0df90bd20a7f4af9a821a95b081bc7f9e1ef23f310` |
| `src/Dictionary/DictSampler.cs` | `c3290c4b04aec0d1a54dbe52f18ddfc3e4862191fa8c5e6865ec1c2288790279` |
| `src/Dictionary/EntryAudio.cs` | `b50614aa454c257327bd6d828b16296189e9863828eb61feb269c58a6ea64786` |
| `src/Dictionary/EntryDocument.cs` | `e40e18c2e1949c34a6aeed21cedb45d45b5ceef5d2b4eb220e9ec7fcf5be21fa` |
| `src/Dictionary/HtmlUtils.cs` | `b8df19ec18d3408845fdfc8865348c4aca043c05b4543b355bf4fe9e21910932` |
| `src/Dictionary/IndexSampling.cs` | `7cfcb94022add8ccb602bfa184b61a136e1bb84a85cbe32f17d5ae9530637d14` |
| `src/Dictionary/LruCache.cs` | `2691974706141e1a8ba5d458934bf32398890ec68a5bf4f31058d4c3d066fb2b` |
| `src/Dictionary/Lzo1x.cs` | `2362851af0a20b5308a612b7afa534dd90fab8080244d20d9f3d140a1c016ce3` |
| `src/Dictionary/MddReader.cs` | `bc5e4820f726bf79a264cf2f02ed705d7df544e03785a29e104ac464e292dd10` |
| `src/Dictionary/MdictCore.cs` | `5612a7f5c6f2166332eae80d6dba8248b4db4bf608f47870119c1a9482a54f23` |
| `src/Dictionary/MdictCrypto.cs` | `74620b26f5ce76ac74216daf768a8127b77da413077e0b7847731f882d0bbd13` |
| `src/Dictionary/MdxReader.cs` | `f69b50dff7b3b1624e398972f524a8f534ece462387e74dd043cbefab960dc45` |
| `src/Dictionary/MimeTypes.cs` | `c4013264c00374cee30c1215a5a826162b1496e5a43f32689aa0e0c26b177968` |
| `src/Dictionary/Ripemd128.cs` | `8e1a7822bf77129481599ee93c85af7e809f6d7ad306e971f3b5666a40660d5c` |
| `src/Dictionary/SiblingResource.cs` | `4c7fd943ca5e230fcc8f65be03cfb13be8b281189ae9306dc18d37fb54c8ff45` |

校验（任选其一）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\freeze-fixtures.ps1   # 参考源 + 测试用词典一起核
```

```bash
cd reference/0.1.3-parser && sha256sum -c SHA256.txt
```

## 加/删文件的规矩

1. **优先不加**。这三个工程之所以能编得过，靠的是"参考实现里应用层文件就那么两个" +
   `GoldenDump.csproj` 里两行显式 `Remove`。真要加，三处**必须同时**动：
   `SHA256.txt`、本文件的表、以及（如果是应用层文件）`GoldenDump.csproj` 的 `Remove`；
2. **删**一个文件等于**换掉参考实现** —— 那要**重新生成基线**（`--regen`）并把
   `tools/golden/baseline/provenance.md` 一起更新，不许只删不补；
3. 这里的文件**没有**"顺手同步到 0.1.3"这一说：0.1.3 已冻结，两边从此各走各的。
