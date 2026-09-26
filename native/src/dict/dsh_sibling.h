/* 「不带 .mdd 的词典」的资源回落：`.mdx` 同目录散放着 css / ttf / png，**没有 `.mdd`**
 * （《新世纪汉英大词典》就是一个文件夹，词条正文用**裸文件名**引用旁边的 css）。
 * 四道检查（扩展名白名单 / 拒绝绝对路径 / 归一化后仍须在词典目录内 / 逐段拒重解析点）
 * 在 dsh_sibling.c —— 这是**唯一**一处「按界面给的路径去读本地文件」的地方。
 *
 * ⚠️ 这是一次**信任边界变更**，边界不在这个文件里、也不许顺手放宽：**只放行 `.js`**
 * （`.mjs` / `.html` / `.htm` 仍不放行），且 CSP 与词条 iframe 的 `sandbox` 一个字都不许动。 */

#ifndef DSH_LOOSE_H
#define DSH_LOOSE_H

/** 这个扩展名允不允许当同目录散放的文件读（`.js` 在里；`.mjs` / `.html` / `.htm` 不在 —— 见文件头） */
int dsh_sibling_is_allowed_extension(const char *path);

/**
 * 把界面给的资源路径解析成「词典目录下的那个真实文件」；不合法 / 不存在一律回 NULL。
 *
 * @param mdx_path       那本词典的 `.mdx` 路径（词典目录 = 它所在目录）
 * @param resource_path  URL 形式的相对路径（`a.css` / `images/a.png`，可能带 URL 编码、查询串）
 * @return 新分配的绝对路径（调用方 `dsh_release`）；不合法 / 不存在 → NULL，
 *         原因写进 last_error（**这条路上失败很常见，调用方按「没这个资源」处理**）
 */
char *dsh_sibling_locate(const char *mdx_path, const char *resource_path);

/** 词典目录下有没有**任何**同目录散放的文件可当资源用（决定词条正文的 `data-has-resources`） */
int dsh_sibling_has_any(const char *mdx_path);

/** 词典目录（= `.mdx` 所在目录）；取不到回 NULL（新分配，调用方 dsh_release） */
char *dsh_sibling_directory_of(const char *mdx_path);

#endif /* DSH_LOOSE_H */
