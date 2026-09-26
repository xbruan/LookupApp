/* 资源扩展名 → MIME 类型（表与参考实现同一条约定）。
 *
 * 住在内核：宿主答 HTTP（206 / 416 / ETag）要 `Content-Type`，而「哪个扩展名是什么类型」
 * 是词典资源的常识、不是视图 —— 每个平台各写一份迟早在某个扩展名上分叉
 * （`.spx` 给 `audio/x-speex` 还是 `audio/ogg` 只许有一个答案）。 */

#ifndef DSH_MIME_H
#define DSH_MIME_H

/** 按路径的扩展名给 MIME（认不出的回 `application/octet-stream`；返回静态串） */
const char *dsh_mime_for(const char *path);

#endif /* DSH_MIME_H */
