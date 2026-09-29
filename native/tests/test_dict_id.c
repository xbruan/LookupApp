/* 内核单元测试 · 词典的内容哈希 id
 * 钉的是产品决定「id 认内容、不认路径」：①同内容换路径换名 id 不变；②差一个字节 id 就不同；
 * ③空文件算失败（否则 0 字节垃圾会被记成一本词库）；④恰好 64 位小写十六进制；⑤与 python3 hashlib 一致。
 */

#include "dict/dsh_dict_id.h"
#include "dsh_lookup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DSH_TESTDATA_DIR
#error "需要 -DDSH_TESTDATA_DIR=<0.2.0/testdata 的路径>（见 Makefile）"
#endif

static int g_checks = 0;
static int g_failed = 0;

static void ok(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n", what);
  }
}

static void ok_eq_str(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual == NULL || expected == NULL || strcmp(actual, expected) != 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%s\n      期望=%s\n", what, actual ? actual : "(null)",
            expected ? expected : "(null)");
  }
}

/** 真测试用词典的完整路径（用 malloc 拼，别用固定缓冲 —— 仓库路径里有空格与中文） */
static char *fixture(const char *name) {
  const size_t n = strlen(DSH_TESTDATA_DIR) + strlen(name) + 2;
  char *p = (char *)malloc(n);
  if (p == NULL) return NULL;
  snprintf(p, n, "%s/%s", DSH_TESTDATA_DIR, name);
  return p;
}

/** 把一段字节写进临时文件，返回路径（调用方释放） */
static char *write_temp(const char *name, const void *data, size_t len) {
  char *path = fixture(name);
  if (path == NULL) return NULL;
  FILE *fp = fopen(path, "wb");
  if (fp == NULL) {
    free(path);
    return NULL;
  }
  if (len > 0) fwrite(data, 1, len, fp);
  fclose(fp);
  return path;
}

int main(void) {
  char id[DSH_DICT_ID_BUF_LEN];

  /* ── ① 同一份内容、两个路径 → 同一个 id（换名不换身份）── */
  {
    static const char payload[] = "这不是一本真词典，只是内容哈希的样张。";
    char *p1 = write_temp("dsh-id-a.tmp", payload, sizeof(payload) - 1);
    char *p2 = write_temp("dsh-id-b.tmp", payload, sizeof(payload) - 1);
    char id1[DSH_DICT_ID_BUF_LEN] = "";
    char id2[DSH_DICT_ID_BUF_LEN] = "";
    ok(p1 != NULL && p2 != NULL, "写得出临时文件");
    if (p1 != NULL && p2 != NULL) {
      ok(dsh_dict_id_of_file(p1, id1) == 0, "算第一个文件成功");
      ok(dsh_dict_id_of_file(p2, id2) == 0, "算第二个文件成功");
      ok_eq_str(id2, id1, "① 内容相同、路径与文件名都不同 → id 必须相同");
    }
    if (p1 != NULL) { remove(p1); free(p1); }
    if (p2 != NULL) { remove(p2); free(p2); }
  }

  /* ── ② 内容差一个字节 → id 必须不同（哪怕长度一样）── */
  {
    static const char a[] = "abcdefghijklmnop";
    static const char b[] = "abcdefghijklmnoP"; /* 只差最后一位的大小写 */
    char *p1 = write_temp("dsh-id-c.tmp", a, sizeof(a) - 1);
    char *p2 = write_temp("dsh-id-d.tmp", b, sizeof(b) - 1);
    char id1[DSH_DICT_ID_BUF_LEN] = "";
    char id2[DSH_DICT_ID_BUF_LEN] = "";
    if (p1 != NULL && p2 != NULL) {
      ok(dsh_dict_id_of_file(p1, id1) == 0 && dsh_dict_id_of_file(p2, id2) == 0, "两个都能算");
      ok(strcmp(id1, id2) != 0, "② 内容差一个字节 → id 必须不同（长 16 字节，只差一个字符）");
    }
    if (p1 != NULL) { remove(p1); free(p1); }
    if (p2 != NULL) { remove(p2); free(p2); }
  }

  /* ── ③ 空文件算失败（不是「空内容的哈希」） */
  {
    char *p = write_temp("dsh-id-empty.tmp", "", 0);
    char got[DSH_DICT_ID_BUF_LEN] = "非空";
    if (p != NULL) {
      ok(dsh_dict_id_of_file(p, got) != 0, "③ 0 字节的文件必须算失败");
      ok(got[0] == '\0', "失败时出参要被清空（别留半截 id）");
      const char *msg = dsh_last_error_message();
      ok(msg != NULL && strstr(msg, "空") != NULL, "报错要说明是空文件");
      if (msg != NULL) dsh_release((void *)msg);
      remove(p);
      free(p);
    }
  }

  /* ── 不存在的路径要报错（不许安静地成功） */
  {
    char got[DSH_DICT_ID_BUF_LEN] = "非空";
    ok(dsh_dict_id_of_file("/definitely/not/here.mdx", got) != 0, "不存在的路径必须报错");
    ok(got[0] == '\0', "失败时出参要被清空");
    ok(dsh_dict_id_of_file(NULL, got) != 0, "路径为 NULL 必须报错");
    ok(dsh_dict_id_of_file("/etc/hostname", NULL) != 0, "出参为 NULL 必须报错");
  }

  /* ── ④ 形状：恰好 64 个小写十六进制字符 ── */
  {
    char *p = fixture("test.mdx");
    if (p == NULL || dsh_dict_id_of_file(p, id) != 0) {
      ok(0, "算真测试用词典 test.mdx 的 id");
      fprintf(stderr, "      %s\n", dsh_last_error_message());
    } else {
      ok(strlen(id) == DSH_DICT_ID_HEX_LEN, "id 恰好 64 个字符");
      int lower = 1;
      for (int i = 0; i < 64; i++) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) lower = 0;
      }
      ok(lower, "④ id 必须是小写十六进制（它要进设置文件、会被宿主直接比较）");
    }
    if (p != NULL) free(p);
  }

  /* ── ⑤ 与 python3 hashlib 的真实结果逐字节一致：期望值由 tools/golden/dict-id-vectors.py 现算、
   * 不许手抄。这条最值钱 —— 它挡住「少喂了最后一块」「按文本模式截断」这类只在真文件上才暴露的错。 */
  {
    struct { const char *file; const char *want; } cases[] = {
      /* 由 tools/golden/dict-id-vectors.py 生成 */
      {"test.mdx",
       "06f401db6bbc5d3fc053e916696c87fa190d470918b36bbc9e6c9c27560fc931"},
      {"link.mdx",
       "539a599f3d30819489d19024504647d0c6874a692c97b3539a294a37c195672b"},
      {"titled.mdx",
       "6129ab21babef58ea0b4a77b489e45e2137a8e808547dd6421be70dc42202e9f"},
      {"v2-multiblock.mdx",
       "50b866a37a939998b2b6700df9785ca2fdd685938e1ba1fe20bd656dfd88ef24"},
      /* 这一行的 id 必须与 testdata/SHA256.txt 里那一行一模一样：那份清单是校验测试用词典完整性用的，
       * 两边算的是同一件事，对不上就说明算词典 id 与校验清单用的不是同一份内容。 */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      char *p = fixture(cases[i].file);
      if (p == NULL) continue;
      char got[DSH_DICT_ID_BUF_LEN] = "";
      char label[192];
      snprintf(label, sizeof(label), "⑤ %s 的 id 必须与 python3 hashlib 的实测结果一致",
               cases[i].file);
      if (dsh_dict_id_of_file(p, got) == 0) {
        ok_eq_str(got, cases[i].want, label);
      } else {
        g_checks++;
        g_failed++;
        fprintf(stderr, "FAIL %s：算不出来：%s\n", cases[i].file, dsh_last_error_message());
      }
      free(p);
    }
  }

  /* ── 大文件：分块边界（64KB）两侧各一条，确保「少喂最后一块」会暴露出来 */
  {
    /* 内容 = 0..N-1 的字节循环；取 64KB-1 / 64KB / 64KB+1 三种长度 */
    const size_t sizes[] = {DSH_DICT_ID_CHUNK - 1, DSH_DICT_ID_CHUNK, DSH_DICT_ID_CHUNK + 1};
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
      uint8_t *buf = (uint8_t *)malloc(sizes[k]);
      if (buf == NULL) {
        ok(0, "分配测试缓冲");
        continue;
      }
      for (size_t i = 0; i < sizes[k]; i++) buf[i] = (uint8_t)(i * 31u + 7u);
      char *p = write_temp("dsh-id-big.tmp", buf, sizes[k]);
      if (p != NULL) {
        char file_id[DSH_DICT_ID_BUF_LEN] = "";
        char mem_id[DSH_DICT_ID_BUF_LEN] = "";
        dsh_dict_id_of_bytes(buf, sizes[k], mem_id);
        if (dsh_dict_id_of_file(p, file_id) == 0) {
          char label[128];
          snprintf(label, sizeof(label), "③ %zu 字节的文件 id 必须等于同样内容的字符串 id", sizes[k]);
          ok_eq_str(file_id, mem_id, label);
        } else {
          g_checks++;
          g_failed++;
          fprintf(stderr, "FAIL 算 %zu 字节文件的 id 失败\n", sizes[k]);
        }
        remove(p);
        free(p);
      }
      free(buf);
    }
  }

  /* ── 纯内存那条路：与写入器无关，直接给字节 ── */
  {
    char a[DSH_DICT_ID_BUF_LEN] = "";
    char b[DSH_DICT_ID_BUF_LEN] = "";
    dsh_dict_id_of_bytes("abc", 3, a);
    ok_eq_str(a, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "内存那条路与 SHA-256 向量一致（证明它没有自己再加工一遍）");
    dsh_dict_id_of_bytes("abc", 3, b);
    ok_eq_str(b, a, "同样的输入两次算出来必须一样");
    dsh_dict_id_of_bytes(NULL, 0, a);
    ok_eq_str(a, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "内存那条路允许空输入（与「文件为空算失败」是两回事，分工要清楚）");
    dsh_dict_id_of_bytes("abc", 3, NULL); /* 不许崩 */
    ok(1, "出参为 NULL 时不崩");
  }

  printf("dict_id：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
