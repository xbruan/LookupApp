/** 内联 SVG 图标：避免额外字体/图片依赖，颜色统一继承 currentColor */
export type IconName =
  | 'search'
  | 'library'
  | 'history'
  | 'close'
  | 'clearInput'
  | 'copy'
  | 'cut'
  | 'paste'
  | 'selectAll'
  | 'trash'
  | 'folder'
  | 'plus'
  | 'check'
  | 'arrowRight'
  | 'arrowLeft'
  | 'arrowUp'
  /* 往下：`arrowUp` 的镜像（词典排序那颗「下移」按钮用它）。成对的箭头就做两个方向 */
  | 'arrowDown'
  /*
   * 展开/收起：词库那一行"点开看详情"的箭头，只有朝下一个方向 —— 展开时由 CSS 把整个图标转 180°
   * （见 manager.css 的 `.dict-toggle .icon`），不需要第二个图标。
   */
  | 'chevronDown'
  | 'list'
  | 'pencil'
  | 'minimize'
  | 'power'
  | 'toTray'
  | 'eye'
  | 'eyeOff'
  | 'speaker'
  | 'gear'

const PATHS: Record<IconName, string> = {
  search: '<circle cx="7.2" cy="7.2" r="4.6"/><path d="M10.7 10.7 14 14"/>',
  library:
    '<path d="M3 3.8h3.2v8.4H3z"/><path d="M7.4 3.8h3.2v8.4H7.4z"/><path d="m12.1 4.4 3 .8-2.1 7.6-3-.8z"/>',
  history:
    '<path d="M2.6 8a5.4 5.4 0 1 0 1.7-3.9"/><path d="M2.4 2.6v2.6h2.6"/><path d="M8 5.2V8l2 1.4"/>',
  close: '<path d="M3.6 3.6 12.4 12.4"/><path d="M12.4 3.6 3.6 12.4"/>',
  /*
   * 清空输入：一个小一号的叉（7.2px @ 1.4，按 `close` 的 8.8px @ 1.7 等比缩），**不套任何容器**。
   * 本套图标统一 stroke-width 1.7，比 2.5×1.7≈4.2px 更细的细节会糊掉 —— "外框 + 内小标记"的两层结构
   * 在本笔宽下装不下（那把叉只有 2.8px，撑到几何极限照样糊），整条路已放弃，别再改回去。
   * 线宽写在 path 的 presentation attribute 上（压得过从 `.icon` 继承的 1.7），别改写成 `style=`：图标结构自检不允许。
   */
  clearInput:
    '<path stroke-width="1.4" d="M4.4 4.4 11.6 11.6"/><path stroke-width="1.4" d="M11.6 4.4 4.4 11.6"/>',
  copy: '<rect x="5.6" y="5.6" width="7.6" height="7.6" rx="1.8"/><path d="M10.6 5.6V4a1.6 1.6 0 0 0-1.6-1.6H4A1.6 1.6 0 0 0 2.4 4v5a1.6 1.6 0 0 0 1.6 1.6h1.6"/>',
  cut: '<circle cx="4.4" cy="11.6" r="1.9"/><circle cx="11.6" cy="11.6" r="1.9"/><path d="M5.7 10.2 12 2.6"/><path d="M10.3 10.2 4 2.6"/>',
  paste:
    '<path d="M6.4 2.8H9.6a1 1 0 0 1 1 1v.4a1 1 0 0 1-1 1H6.4a1 1 0 0 1-1-1v-.4a1 1 0 0 1 1-1z"/><path d="M11.6 4.6h.8A1.6 1.6 0 0 1 14 6.2v5.6a1.6 1.6 0 0 1-1.6 1.6H3.6A1.6 1.6 0 0 1 2 11.8V6.2a1.6 1.6 0 0 1 1.6-1.6h.8"/>',
  selectAll:
    '<rect x="2.6" y="2.6" width="10.8" height="10.8" rx="2"/><path d="M5.4 8.2 7.2 10l3.4-3.8"/>',
  trash:
    '<path d="M3 4.6h10"/><path d="M6.2 4.6V3.2A1 1 0 0 1 7.2 2.2h1.6a1 1 0 0 1 1 1v1.4"/><path d="M4.4 4.6l.6 8a1.2 1.2 0 0 0 1.2 1.1h3.6a1.2 1.2 0 0 0 1.2-1.1l.6-8"/>',
  folder:
    '<path d="M2.2 5.2A1.6 1.6 0 0 1 3.8 3.6h2.3l1.3 1.6h4.8a1.6 1.6 0 0 1 1.6 1.6v4.4a1.6 1.6 0 0 1-1.6 1.6H3.8a1.6 1.6 0 0 1-1.6-1.6z"/>',
  plus: '<path d="M8 3.4v9.2"/><path d="M3.4 8h9.2"/>',
  check: '<path d="M3.4 8.4 6.6 11.6l6-7.2"/>',
  arrowRight: '<path d="M3.4 8h9.2"/><path d="M9 4.4 12.6 8 9 11.6"/>',
  /* 返回：arrowRight 的镜像（链接跳转回上一个词条） */
  arrowLeft: '<path d="M12.6 8H3.4"/><path d="M7 4.4 3.4 8 7 11.6"/>',
  /* 回顶部：向左箭头的竖直版，箭头朝上 */
  arrowUp: '<path d="M8 13V3.2"/><path d="M4.2 7 8 3.2 11.8 7"/>',
  /* 往下：arrowUp 竖直镜像（词典排序「下移」用） */
  arrowDown: '<path d="M8 3v9.8"/><path d="M4.2 9 8 12.8 11.8 9"/>',
  /*
   * 展开箭头：一个"V"（两笔、没有竖杆）。不用 `arrowUp` 转 180° —— 那个带竖杆，转过来是"向下箭头"，
   * 而这个位置要表达的正是"可以展开"（列表里"这里能点开"的通用记号）。
   */
  chevronDown: '<path d="M3.8 6.2 8 10.4 12.2 6.2"/>',
  /*
   * 候选列表：三个项目符 + 三条项目文字（项目符是零长度的圆头线段，圆头笔帽把它们点成圆点）。
   * 不用左箭头 —— 左边那个左箭头已经是"返回上一个词条"，两个一样的箭头挨着，谁是谁全靠猜。
   */
  list:
    '<path d="M3.4 4h.01"/><path d="M3.4 8h.01"/><path d="M3.4 12h.01"/><path d="M6.6 4.2h6.2"/><path d="M6.6 8h6.2"/><path d="M6.6 11.8h6.2"/>',
  /* 铅笔：笔杆 + 笔尖，右下角一道斜切表示笔锋 */
  pencil:
    '<path d="M11.1 2.6 13.4 4.9 5.9 12.4l-3.3.9.9-3.3z"/><path d="M9.6 4.1 11.9 6.4"/>',
  minimize: '<path d="M3.4 8h9.2"/>',
  /*
   * 电源键：顶部开口的圆环 + 伸进开口的竖线（圆心 (8,8.665) 半径 6，缺口正好在正上方）。
   * "退出程序"用它，与"关闭窗口"的叉分开：关窗口只是把界面收起来，退出是结束进程，两件事分量不一样。
   */
  power: '<path d="M8 1.4v7.2"/><path d="M12.24 4.43a6 6 0 1 1-8.48 0"/>',
  /*
   * 收进托盘：向下的箭头落进底部一条横线（托盘的沿），用在关闭询问框的「最小化到托盘」那一项 ——
   * 它说的是"收起来，别真的关掉"，所以既不该用叉（那是关窗口），也不该用电源键（那是退出）。
   */
  toTray: '<path d="M8 2.4v7"/><path d="M4.8 6.2 8 9.4l3.2-3.2"/><path d="M3.4 13.4h9.2"/>',
  /*
   * 眼睛（托盘菜单的「显示悬浮窗」）：上下两条对称的弧拼成一个"眼形"轮廓 + 中间一个**实心**瞳孔。
   * ⚠️ 瞳孔必须是实心块（`fill="currentColor"` + `stroke="none"`）：少了 fill 会被 `.icon { fill: none }`
   * 继承成什么都没有；而 16px 下用描边画的 r1.6 小圆会糊成一个点（比最小可辨特征 4.2px 还小），
   * 实心块不受这条约束 —— 尺寸再小也是干净的形状。
   */
  eye:
    '<path d="M2.2 8c1.6-2.7 3.6-4 5.8-4s4.2 1.3 5.8 4c-1.6 2.7-3.6 4-5.8 4S3.8 10.7 2.2 8Z"/><path fill="currentColor" stroke="none" d="M6.4 8a1.6 1.6 0 1 0 3.2 0a1.6 1.6 0 1 0 -3.2 0Z"/>',
  /*
   * 划掉的眼睛（托盘菜单的「隐藏悬浮窗」）：上面那只眼睛 + 一条 45° 斜线（左下 → 右上）。
   * 两个状态的分工是"**图标画的是点下去会变成什么样**"：「隐藏悬浮窗」配划掉的眼睛、
   * 「显示悬浮窗」配睁着的眼睛（见 tray-menu/main.ts），别反过来。
   */
  eyeOff:
    '<path d="M2.2 8c1.6-2.7 3.6-4 5.8-4s4.2 1.3 5.8 4c-1.6 2.7-3.6 4-5.8 4S3.8 10.7 2.2 8Z"/><path fill="currentColor" stroke="none" d="M6.4 8a1.6 1.6 0 1 0 3.2 0a1.6 1.6 0 1 0 -3.2 0Z"/><path d="M3.6 12.8 12.4 3.2"/>',
  /*
   * 发音：**只有一个喇叭**，不画声波 —— 弧线紧贴着锥口，缩小看就是"喇叭旁边多了一堆噪点"；
   * 发声状态改用整枚图标的呼吸动画表达（见 floating.css 的 [data-playing]），图案本身越简单越清楚。
   */
  speaker: '<path d="M3.8 6.4h2.8L11.4 3.4v9.2L6.6 9.6H3.8z"/>',
  /*
   * 齿轮（「选项」）：6 齿 / 齿顶 7.1 / 齿根 4.9 / 孔 2.5（改参数按 docs/design/图标设计与改进指导.md 的公式重算），
   * 且必须是**实心**而不是描边 —— stroke-width 1.7 向两侧各扩 0.85px，齿高要 ≥4.2px 才看得见；
   * 描边齿轮实测只有两种下场：齿高≈笔画宽时被笔画填平成圆环、齿高拉大后退化成雪花。所以这个图标
   * （也只有这个）三个属性缺一不可（fill + stroke:none + fill-rule:evenodd 挖中心孔），
   * 尤其 `fill="currentColor"` —— 少了它会被 .icon 的 fill:none 继承成空形状。
   */
  gear:
    '<path fill="currentColor" stroke="none" fill-rule="evenodd" d="M6.23 1.12L9.77 1.12L9.42 3.31L11.35 4.43L13.07 3.03L14.84 6.09L12.77 6.88L12.77 9.12L14.84 9.91L13.07 12.97L11.35 11.57L9.42 12.69L9.77 14.88L6.23 14.88L6.58 12.69L4.65 11.57L2.93 12.97L1.16 9.91L3.23 9.12L3.23 6.88L1.16 6.09L2.93 3.03L4.65 4.43L6.58 3.31Z M5.5 8a2.5 2.5 0 1 0 5 0a2.5 2.5 0 1 0 -5 0Z"/>',
}

export function iconSvg(name: IconName, size = 16): string {
  return `<svg class="icon" viewBox="0 0 16 16" width="${size}" height="${size}" aria-hidden="true">${PATHS[name]}</svg>`
}

/** 应用标识：渐变圆底 + 放大镜，与托盘/安装包图标同一套视觉 */
export function appMarkSvg(size = 20): string {
  return `<svg class="app-mark" viewBox="0 0 32 32" width="${size}" height="${size}" aria-hidden="true">
  <defs>
    <linearGradient id="markGrad" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#5b8cff"/>
      <stop offset="1" stop-color="#9b5bff"/>
    </linearGradient>
  </defs>
  <rect x="0" y="0" width="32" height="32" rx="9" fill="url(#markGrad)"/>
  <circle cx="13.6" cy="13.3" r="6.9" fill="none" stroke="#fff" stroke-width="2.5"/>
  <path d="M18.7 18.4 24.2 23.8" fill="none" stroke="#fff" stroke-width="2.6" stroke-linecap="round"/>
</svg>`
}
