namespace Lookup.App
{
    using System;
    using System.Collections.Generic;
    using System.Drawing;
    using System.Drawing.Drawing2D;
    using System.Drawing.Imaging;
    using System.Runtime.InteropServices;
    using System.Text.RegularExpressions;
    using System.Windows.Forms;

    /// <summary>
    /// CSS 颜色串 → `Color`：认 `rgb()` / `rgba()` / `#rrggbb` / `#rrggbbaa`，认不出来就回兜底色（**不猜**）。
    /// </summary>
    internal static class CssColor
    {
        private static readonly Regex Rgba = new Regex(
            @"rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*(?:,\s*([0-9.]+)\s*)?\)",
            RegexOptions.Compiled | RegexOptions.IgnoreCase);

        internal static Color Parse(string css, Color fallback)
        {
            if (string.IsNullOrWhiteSpace(css)) return fallback;
            var text = css.Trim();

            var match = Rgba.Match(text);
            if (match.Success)
            {
                var r = Clamp(byte.Parse(match.Groups[1].Value));
                var g = Clamp(byte.Parse(match.Groups[2].Value));
                var b = Clamp(byte.Parse(match.Groups[3].Value));
                var a = 255;
                if (match.Groups[4].Success)
                {
                    double value;
                    if (double.TryParse(match.Groups[4].Value,
                                        System.Globalization.NumberStyles.Float,
                                        System.Globalization.CultureInfo.InvariantCulture, out value))
                    {
                        a = Clamp((int)Math.Round(value * 255));
                    }
                }
                return Color.FromArgb(a, r, g, b);
            }

            if (text.StartsWith("#", StringComparison.Ordinal))
            {
                var hex = text.Substring(1);
                try
                {
                    if (hex.Length == 3)
                    {
                        return Color.FromArgb(255,
                            Convert.ToByte(new string(hex[0], 2), 16),
                            Convert.ToByte(new string(hex[1], 2), 16),
                            Convert.ToByte(new string(hex[2], 2), 16));
                    }
                    if (hex.Length == 6 || hex.Length == 8)
                    {
                        var r = Convert.ToByte(hex.Substring(0, 2), 16);
                        var g = Convert.ToByte(hex.Substring(2, 2), 16);
                        var b = Convert.ToByte(hex.Substring(4, 2), 16);
                        var a = (hex.Length == 8) ? Convert.ToByte(hex.Substring(6, 2), 16) : (byte)255;
                        return Color.FromArgb(a, r, g, b);
                    }
                }
                catch (Exception) { /* 认不出来就回兜底色 */ }
            }
            return fallback;
        }

        private static byte Clamp(int value)
        {
            if (value < 0) return 0;
            if (value > 255) return 255;
            return (byte)value;
        }
    }

    /// <summary>
    /// **外壳层**：独立分层窗口，压在承载网页的窗口**下面**，把页面报上来的卡片按真实形状重画一遍 ——
    /// 于是有抗锯齿的圆角与投影（承载窗口只能靠窗口 Region 裁，那道边硬，所以 Region 要往里缩 2px）。
    /// 位图只覆盖「卡片并集 + 投影余量」；要不要重画只看位图自己的内容，与窗口此刻在屏幕哪儿无关。
    /// </summary>
    internal sealed class ChromeWindow : IDisposable
    {
        /// <summary>投影余量（物理像素）—— 分层窗口要比卡片并集大出这一圈</summary>
        internal const int ShadowMargin = 26;
        private const int EmphasisMargin = 12;
        private const float ShadowBlurPhysical = 9f;
        private const float ShadowBlurEmphasized = 14f;
        private const float ShadowOffsetDip = 2.2f;

        /// <summary>一块要画出来的卡片（**窗口坐标**，物理像素）</summary>
        internal sealed class Card
        {
            internal RectangleF Rect;
            internal float Radius;
            internal Color Fill = Color.White;
            internal Color Border = Color.Transparent;
        }

        private readonly ChromeForm _form = new ChromeForm();
        private List<Card> _cards = new List<Card>();
        private Rectangle _bounds = Rectangle.Empty;
        private PointF _drawOrigin;
        private bool _dark;
        private bool _emphasized;
        private bool _hasContent;
        private int _dpi = 96;
        private bool _disposed;

        /// <summary>外壳窗口的句柄（自检那道 检查 要读它，确认它跟着宿主一起显示/隐藏）</summary>
        internal IntPtr Handle { get { return _form.Handle; } }
        internal bool Visible { get { return _form.Visible; } }

        /// <summary>
        /// 按页面报上来的形状重画并摆好外壳窗口。
        /// belowHwnd：压在谁的下面（承载网页的那个窗口）；windowBounds：那个窗口的屏幕矩形（物理像素）。
        /// cards：卡片（**那个窗口的客户区坐标**，物理像素）；dark：深色主题（投影浓度不一样）；
        /// emphasized：强调态（**只加浓投影，不画彩色描边**）。
        /// </summary>
        internal void Update(IntPtr belowHwnd, Rectangle windowBounds, List<Card> cards, bool dark,
                             bool emphasized)
        {
            if (_disposed) return;
            cards = cards ?? new List<Card>();
            var union = UnionOf(cards);
            if (union.IsEmpty)
            {
                _hasContent = false;
                Hide();
                return;
            }

            // 状态先落定再算尺寸；⚠️ dark 必须**先比后赋**（反过来那个比较恒为 false，主题变了也不重画）。
            var emphasisChanged = emphasized != _emphasized;
            var darkChanged = dark != _dark;
            _emphasized = emphasized;
            _dark = dark;

            var margin = emphasized ? ShadowMargin + EmphasisMargin : ShadowMargin;
            var outer = new Rectangle(
                windowBounds.X + (int)Math.Floor(union.Left) - margin,
                windowBounds.Y + (int)Math.Floor(union.Top) - margin,
                (int)Math.Ceiling(union.Width) + margin * 2,
                (int)Math.Ceiling(union.Height) + margin * 2);

            var contentWidth = (int)Math.Ceiling(union.Width) + margin * 2;
            var contentHeight = (int)Math.Ceiling(union.Height) + margin * 2;
            var originX = union.Left - margin;
            var originY = union.Top - margin;
            var dpi = _form.DeviceDpi;

            var contentChanged = !_hasContent ||
                                 contentWidth != _bounds.Width || contentHeight != _bounds.Height ||
                                 darkChanged || emphasisChanged || dpi != _dpi ||
                                 !SameBitmapCards(cards, originX, originY);

            _bounds = outer;
            _drawOrigin = new PointF(originX, originY);
            _cards = cards;
            _dpi = dpi;
            _hasContent = true;

            // 位置与尺寸**每一帧都要挪**（哪怕位图能复用）：分层位图跟着窗口走
            Native.SetWindowPos(_form.Handle, belowHwnd, outer.X, outer.Y, outer.Width, outer.Height,
                                Native.SWP_NOACTIVATE | Native.SWP_SHOWWINDOW);
            if (contentChanged) Render();
        }

        private static RectangleF UnionOf(List<Card> cards)
        {
            var union = RectangleF.Empty;
            foreach (var card in cards)
            {
                if (card.Rect.Width < 1 || card.Rect.Height < 1) continue;
                union = union.IsEmpty ? card.Rect : RectangleF.Union(union, card.Rect);
            }
            return union;
        }

        /// <summary>
        /// **整窗一块卡片**那种窗口（管理窗 / 托盘菜单）的快捷入口：坐标是客户区，拼错只表现为「投影偏了一点」，很难查。
        /// </summary>
        internal void UpdateWindowCard(IntPtr belowHwnd, Rectangle windowBounds, int clientWidth,
                                       int clientHeight, float radius, Color fill, Color border,
                                       bool dark, bool emphasized)
        {
            if (clientWidth <= 0 || clientHeight <= 0)
            {
                Hide();
                return;
            }
            var cards = new List<Card>
            {
                new Card
                {
                    Rect = new RectangleF(0, 0, clientWidth, clientHeight),
                    Radius = radius,
                    Fill = fill,
                    Border = border,
                },
            };
            Update(belowHwnd, windowBounds, cards, dark, emphasized);
        }

        /// <summary>位图内容变了没有 —— 只看**相对绘制原点**的位置，不看屏幕坐标（见类顶上那一条）</summary>
        private bool SameBitmapCards(List<Card> fresh, float originX, float originY)
        {
            if (fresh.Count != _cards.Count) return false;
            for (var i = 0; i < fresh.Count; i++)
            {
                var a = fresh[i];
                var b = _cards[i];
                var ax = a.Rect.X - originX;
                var ay = a.Rect.Y - originY;
                var bx = b.Rect.X - _drawOrigin.X;
                var by = b.Rect.Y - _drawOrigin.Y;
                if (Math.Abs(ax - bx) > 0.01f || Math.Abs(ay - by) > 0.01f) return false;
                if (Math.Abs(a.Rect.Width - b.Rect.Width) > 0.01f) return false;
                if (Math.Abs(a.Rect.Height - b.Rect.Height) > 0.01f) return false;
                if (Math.Abs(a.Radius - b.Radius) > 0.01f) return false;
                if (!a.Fill.Equals(b.Fill) || !a.Border.Equals(b.Border)) return false;
            }
            return true;
        }

        internal void Hide()
        {
            if (_disposed) return;
            try { _form.Hide(); } catch (Exception) { }
        }

        /// <summary>
        /// 宿主窗口不可见时，外壳**绝不能自己亮出来**（页面那边排着的形状上报会 SWP_SHOWWINDOW，窗口没了投影还在）。
        /// </summary>
        internal void HideIfOwnerHidden(IntPtr ownerHwnd)
        {
            if (_disposed) return;
            try
            {
                if (!Native.IsWindowVisible(ownerHwnd)) Hide();
            }
            catch (Exception) { }
        }

        /// <summary>把外壳重新压回宿主窗口下面（重新显示之后要排一次 z 序）</summary>
        internal void RefreshZOrder(IntPtr belowHwnd)
        {
            if (_disposed || !_hasContent) return;
            try
            {
                Native.SetWindowPos(_form.Handle, belowHwnd, _bounds.X, _bounds.Y, _bounds.Width,
                                    _bounds.Height, Native.SWP_NOACTIVATE);
            }
            catch (Exception) { }
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            try { _form.Dispose(); } catch (Exception) { }
        }

        // ── 画：投影垫底 → 卡片压上来 ──

        private void Render()
        {
            var width = _bounds.Width;
            var height = _bounds.Height;
            if (width <= 0 || height <= 0) return;

            using (var bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb))
            {
                using (var g = Graphics.FromImage(bitmap))
                {
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                    g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                    g.CompositingMode = CompositingMode.SourceOver;

                    using (var union = BuildUnionPath())
                    {
                        DrawShadow(g, union);
                        foreach (var card in _cards)
                        {
                            using (var path = BuildCardPath(card))
                            {
                                using (var brush = new SolidBrush(card.Fill)) g.FillPath(brush, path);
                                if (card.Border.A > 0)
                                {
                                    using (var pen = new Pen(card.Border, 1.4f))
                                    {
                                        pen.Alignment = PenAlignment.Inset;
                                        g.DrawPath(pen, path);
                                    }
                                }
                            }
                        }
                    }
                }
                PushLayered(bitmap);
            }
        }

        private void DrawShadow(Graphics target, GraphicsPath unionPath)
        {
            const int scale = 3;
            var smallWidth = Math.Max(1, _bounds.Width / scale);
            var smallHeight = Math.Max(1, _bounds.Height / scale);

            // ⚠️ 模糊半径要**除以** scale（这一步在 1/3 尺度上做）：多乘一次 scale 半径变 3 倍，投影就看不见了。
            var blurRadius = Math.Max(1, (int)Math.Round(
                (_emphasized ? ShadowBlurEmphasized : ShadowBlurPhysical) / scale));
            var margin = blurRadius * 3;

            using (var small = new Bitmap(smallWidth + margin * 2, smallHeight + margin * 2,
                                          PixelFormat.Format32bppArgb))
            {
                using (var g = Graphics.FromImage(small))
                {
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    g.TranslateTransform(margin, margin);
                    g.ScaleTransform(1f / scale, 1f / scale);
                    using (var brush = new SolidBrush(Color.FromArgb(255, 0, 0, 0)))
                    {
                        g.FillPath(brush, unionPath);
                    }
                }

                BoxBlurAlpha(small, blurRadius);

                // 投影往下偏一点点，看起来是「浮起来」而不是「贴上去」
                var offsetY = (int)Math.Round(ShadowOffsetDip * _form.DeviceDpi / 96.0 / scale);
                var alpha = _dark ? 0.72f : 0.42f;
                if (_emphasized) alpha += _dark ? 0.18f : 0.16f;
                using (var attributes = new ImageAttributes())
                {
                    var matrix = new ColorMatrix { Matrix33 = alpha };
                    attributes.SetColorMatrix(matrix, ColorMatrixFlag.Default, ColorAdjustType.Bitmap);
                    var destination = new Rectangle(
                        -margin * scale,
                        -margin * scale + offsetY * scale,
                        small.Width * scale,
                        small.Height * scale);
                    target.DrawImage(small, destination, 0, 0, small.Width, small.Height,
                                     GraphicsUnit.Pixel, attributes);
                }
            }
        }

        /// <summary>对 32bppArgb 位图的 alpha 通道做三次盒式模糊（近似高斯）</summary>
        private static void BoxBlurAlpha(Bitmap bitmap, int radius)
        {
            if (radius < 1) return;
            var rect = new Rectangle(0, 0, bitmap.Width, bitmap.Height);
            var data = bitmap.LockBits(rect, ImageLockMode.ReadWrite, PixelFormat.Format32bppArgb);
            try
            {
                int width = data.Width, height = data.Height, stride = data.Stride;
                var pixels = new byte[stride * height];
                Marshal.Copy(data.Scan0, pixels, 0, pixels.Length);

                var alpha = new float[width * height];
                var scratch = new float[width * height];
                for (var y = 0; y < height; y++)
                {
                    for (var x = 0; x < width; x++) alpha[y * width + x] = pixels[y * stride + x * 4 + 3];
                }

                for (var pass = 0; pass < 3; pass++)
                {
                    BlurHorizontal(alpha, scratch, width, height, radius);
                    BlurVertical(scratch, alpha, width, height, radius);
                }

                for (var y = 0; y < height; y++)
                {
                    for (var x = 0; x < width; x++)
                    {
                        var offset = y * stride + x * 4;
                        var value = alpha[y * width + x];
                        if (value < 0) value = 0;
                        else if (value > 255) value = 255;
                        // 投影就是纯黑 + 模糊过的 alpha，颜色通道不必保留
                        pixels[offset] = 0;
                        pixels[offset + 1] = 0;
                        pixels[offset + 2] = 0;
                        pixels[offset + 3] = (byte)value;
                    }
                }

                Marshal.Copy(pixels, 0, data.Scan0, pixels.Length);
            }
            finally
            {
                bitmap.UnlockBits(data);
            }
        }

        private static void BlurHorizontal(float[] source, float[] target, int width, int height, int radius)
        {
            for (var y = 0; y < height; y++)
            {
                var row = y * width;
                for (var x = 0; x < width; x++)
                {
                    float sum = 0;
                    var count = 0;
                    var from = Math.Max(0, x - radius);
                    var to = Math.Min(width - 1, x + radius);
                    for (var k = from; k <= to; k++) { sum += source[row + k]; count++; }
                    target[row + x] = count == 0 ? 0 : sum / count;
                }
            }
        }

        private static void BlurVertical(float[] source, float[] target, int width, int height, int radius)
        {
            for (var x = 0; x < width; x++)
            {
                for (var y = 0; y < height; y++)
                {
                    float sum = 0;
                    var count = 0;
                    var from = Math.Max(0, y - radius);
                    var to = Math.Min(height - 1, y + radius);
                    for (var k = from; k <= to; k++) { sum += source[k * width + x]; count++; }
                    target[y * width + x] = count == 0 ? 0 : sum / count;
                }
            }
        }

        private GraphicsPath BuildUnionPath()
        {
            var path = new GraphicsPath { FillMode = FillMode.Winding };
            foreach (var card in _cards)
            {
                using (var single = BuildCardPath(card))
                {
                    path.AddPath(single, false);
                }
            }
            return path;
        }

        /// <summary>把**那个窗口的客户区坐标**的卡片矩形换算到外壳位图的坐标上</summary>
        private GraphicsPath BuildCardPath(Card card)
        {
            var rect = new RectangleF(
                card.Rect.X - _drawOrigin.X,
                card.Rect.Y - _drawOrigin.Y,
                card.Rect.Width,
                card.Rect.Height);
            var radius = Math.Max(0f, Math.Min(card.Radius, Math.Min(rect.Width, rect.Height) / 2f));
            var path = new GraphicsPath();
            if (radius <= 0.5f)
            {
                path.AddRectangle(rect);
                return path;
            }
            var diameter = radius * 2f;
            path.AddArc(rect.Left, rect.Top, diameter, diameter, 180, 90);
            path.AddArc(rect.Right - diameter, rect.Top, diameter, diameter, 270, 90);
            path.AddArc(rect.Right - diameter, rect.Bottom - diameter, diameter, diameter, 0, 90);
            path.AddArc(rect.Left, rect.Bottom - diameter, diameter, diameter, 90, 90);
            path.CloseFigure();
            return path;
        }

        /// <summary>`UpdateLayeredWindow`：把那张位图（含逐像素 alpha）贴到屏幕上</summary>
        private void PushLayered(Bitmap bitmap)
        {
            var screenDc = Native.GetDC(IntPtr.Zero);
            var memoryDc = Native.CreateCompatibleDC(screenDc);
            var hBitmap = IntPtr.Zero;
            var previous = IntPtr.Zero;
            try
            {
                // GetHbitmap 传一个全透明底色，alpha 通道才会被保留下来
                hBitmap = bitmap.GetHbitmap(Color.FromArgb(0));
                previous = Native.SelectObject(memoryDc, hBitmap);

                var destination = new Native.POINT { X = _bounds.X, Y = _bounds.Y };
                var source = new Native.POINT { X = 0, Y = 0 };
                var size = new Native.SIZE(_bounds.Width, _bounds.Height);
                var blend = new Native.BLENDFUNCTION
                {
                    BlendOp = Native.AC_SRC_OVER,
                    BlendFlags = 0,
                    SourceConstantAlpha = 255,
                    AlphaFormat = Native.AC_SRC_ALPHA,
                };

                Native.UpdateLayeredWindow(_form.Handle, screenDc, ref destination, ref size,
                                           memoryDc, ref source, 0, ref blend, Native.ULW_ALPHA);
            }
            catch (Exception)
            {
                /* 画不出来就别画 —— 绝不让它把主流程弄崩 */
            }
            finally
            {
                if (previous != IntPtr.Zero) Native.SelectObject(memoryDc, previous);
                if (hBitmap != IntPtr.Zero) Native.DeleteObject(hBitmap);
                Native.DeleteDC(memoryDc);
                Native.ReleaseDC(IntPtr.Zero, screenDc);
            }
        }

        /// <summary>外壳窗口本身：分层 + 鼠标穿透 + 不抢焦点 —— 它只是画上去的一层皮，交互都该落到承载网页的窗口上。</summary>
        private sealed class ChromeForm : Form
        {
            internal ChromeForm()
            {
                FormBorderStyle = FormBorderStyle.None;
                ShowInTaskbar = false;
                StartPosition = FormStartPosition.Manual;
                AutoScaleMode = AutoScaleMode.None;
                SetStyle(ControlStyles.Opaque, true);
                SetStyle(ControlStyles.UserPaint, false);
                /*
                 * ⚠️ 刻意**不设** TopMost：WinForms 创建句柄时会用没带 SWP_NOACTIVATE 的 SetWindowPos 应用它，
                 *    正在显示的托盘菜单收到 WM_ACTIVATE(WA_INACTIVE) 会当场把菜单藏了。
                 *    层级靠 `Update` 里那个 `belowHwnd` 参数（`SetWindowPos` 的插入位置）保证。
                 */
            }

            protected override bool ShowWithoutActivation { get { return true; } }

            protected override CreateParams CreateParams
            {
                get
                {
                    var cp = base.CreateParams;
                    cp.ExStyle |= 0x00000080;  // WS_EX_TOOLWINDOW：不出现在 Alt-Tab
                    cp.ExStyle |= 0x00080000;  // WS_EX_LAYERED：逐像素 alpha
                    cp.ExStyle |= 0x00000020;  // WS_EX_TRANSPARENT：鼠标穿透（点在它上面要落到网页那层）
                    cp.ExStyle |= 0x08000000;  // WS_EX_NOACTIVATE：不抢焦点
                    cp.Style &= ~0x00C00000;   // 去掉 WS_CAPTION 残留
                    return cp;
                }
            }

            protected override void ScaleControl(SizeF factor, BoundsSpecified specified)
            {
                // 按物理像素摆放的窗口不能做自动缩放（与 ShellForm 同一条）
            }
        }
    }
}
