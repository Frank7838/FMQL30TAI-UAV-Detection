// 原生WinForms界面。HTTP只读画面/状态/导出，启停通过认证SSH组件完成。
// 用户配置位于LocalAppData，程序目录不携带私钥。

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Net;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using System.Reflection;
[assembly:AssemblyTitle("无人机检测系统")]
[assembly:AssemblyDescription("模型11 · FPGA板端检测与告警桌面客户端")]
[assembly:AssemblyCompany("")]
[assembly:AssemblyVersion("2.5.0.0")]
namespace UAVDetectionSystem {
// JSON字段类型转换集中在此。
// 显示层统一处理JSON的缺字段和数值转换；网络错误与板端故障状态仍由调用方分别显示。
static class J {
    public static Dictionary<string, object> D(object x) {
        return x as Dictionary<string, object> ?? new Dictionary<string, object>();
    }
    public static object G(object x, string k) {
        object v;
        return D(x).TryGetValue(k, out v) ? v : null;
    }
    public static string S(object x) { return x == null ? "" : Convert.ToString(x); }
    public static long N(object x) { return x == null ? 0 : Convert.ToInt64(x); }
    public static double F(object x) { return x == null ? 0 : Convert.ToDouble(x); }
    public static bool B(object x) { return x != null && Convert.ToBoolean(x); }
    public static string Encode(object x) { return new JavaScriptSerializer().Serialize(x); }
    // 限制单次JSON大小为1MiB，避免把无限响应作为状态对象加载。
    public static Dictionary<string, object> Decode(string x) {
        return new JavaScriptSerializer() { MaxJsonLength = 1024 * 1024 }
            .Deserialize<Dictionary<string, object>>(x);
    }
}
static class Theme {
    public static Color Bg = Color.FromArgb(243, 246, 251), Ink = Color.FromArgb(25, 41, 65),
                        Muted = Color.FromArgb(116, 132, 154), Blue = Color.FromArgb(49, 103, 235),
                        Green = Color.FromArgb(21, 151, 114), Red = Color.FromArgb(210, 65, 77),
                        Line = Color.FromArgb(228, 234, 243), Navy = Color.FromArgb(18, 32, 54);
    public static Font Font(float size = 10, FontStyle style = FontStyle.Regular) {
        return new Font("Microsoft YaHei UI", size, style, GraphicsUnit.Point);
    }
    public static GraphicsPath Round(Rectangle r, int n) {
        var p = new GraphicsPath();
        int d = n * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }
    public static Label Label(string text, float size, Color color,
                              FontStyle style = FontStyle.Regular) {
        return new Label() { Text = text, Font = Font(size, style), ForeColor = color,
                             AutoSize = false, BackColor = Color.Transparent };
    }
}
class Card : Panel {
    public Card() {
        DoubleBuffered = true;
        BackColor = Color.White;
        Padding = new Padding(18);
    }
    protected override void OnPaint(PaintEventArgs e) {
        base.OnPaint(e);
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        using (var path = Theme.Round(new Rectangle(0, 0, Width - 1, Height - 1), 12)) using (
            var pen = new Pen(Theme.Line)) e.Graphics.DrawPath(pen, path);
    }
}
// 按钮维护hover/pressed/键盘焦点外观，鼠标捕获丢失会清pressed，避免按钮看起来一直按下。
class ActionButton : Button {
    public Color Fill = Color.White, Ink = Theme.Ink, Border = Theme.Line;
    public bool ActiveNavigation;
    bool hover, pressed;
    public ActionButton() {
        FlatStyle = FlatStyle.Flat;
        FlatAppearance.BorderSize = 0;
        Font = Theme.Font(10, FontStyle.Bold);
        Cursor = Cursors.Hand;
        Height = 42;
        TabStop = true;
        UseVisualStyleBackColor = false;
        DoubleBuffered = true;
        ResizeRedraw = true;
    }
    protected override void OnMouseEnter(EventArgs e) {
        hover = true;
        Invalidate();
        base.OnMouseEnter(e);
    }
    protected override void OnMouseLeave(EventArgs e) {
        hover = false;
        pressed = false;
        Invalidate();
        base.OnMouseLeave(e);
    }
    protected override void OnMouseDown(MouseEventArgs e) {
        if (e.Button == MouseButtons.Left) { pressed = true; Invalidate(); }
        base.OnMouseDown(e);
    }
    protected override void OnMouseUp(MouseEventArgs e) {
        pressed = false; Invalidate(); base.OnMouseUp(e);
    }
    protected override void OnMouseCaptureChanged(EventArgs e) {
        if (!Capture) { pressed = false; Invalidate(); }
        base.OnMouseCaptureChanged(e);
    }
    protected override void OnPaint(PaintEventArgs e) {
        e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        bool dark = Fill.GetBrightness() < .38F;
        Color fill = !Enabled ? Color.FromArgb(232, 237, 244) :
            (pressed ? (dark ? ControlPaint.Light(Fill, .30F) : Color.FromArgb(218, 229, 252)) :
            (hover ? (dark ? ControlPaint.Light(Fill, .20F) : Color.FromArgb(237, 243, 255)) : Fill));
        if (Fill == Theme.Blue && Enabled) fill = pressed ? Color.FromArgb(32, 79, 200) :
            (hover ? Color.FromArgb(70, 124, 249) : Fill);
        Color outline = Enabled && hover && !dark && Fill != Theme.Blue ? Theme.Blue : Border;
        using (var p = Theme.Round(new Rectangle(1, 1, Width - 3, Height - 3), 8)) using (
            var b = new SolidBrush(fill)) using (var pen = new Pen(outline)) {
            e.Graphics.FillPath(b, p);
            e.Graphics.DrawPath(pen, p);
        }
        if (ActiveNavigation && Enabled) {
            using (var b = new SolidBrush(Color.FromArgb(116, 166, 255)))
                e.Graphics.FillRectangle(b, 2, 10, 3, Height - 20);
        }
        TextRenderer.DrawText(e.Graphics, Text, Font, ClientRectangle, Enabled ? Ink : Theme.Muted,
                              TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        if (Focused)
            ControlPaint.DrawFocusRectangle(e.Graphics,
                                            new Rectangle(5, 5, Width - 10, Height - 10));
    }
}
// 原生绘图按比例显示，换帧和释放时Dispose位图。
// 只在UI线程替换当前位图；等比例绘制，旧图和销毁控件时释放GDI对象。
class VideoSurface : Control {
    Image frame;
    public bool Live;
    public bool DisplayPaused, PreviewStale;
    public string Message = "正在连接板端…";
    public VideoSurface() {
        DoubleBuffered = true;
        BackColor = Color.FromArgb(9, 17, 29);
        AccessibleName = "相机实时画面";
    }
    // 从内存流解码后复制成独立Bitmap，离开using仍可显示；替换后Dispose旧帧。
    public void SetFrame(byte[] bytes) {
        using (var stream = new MemoryStream(bytes)) using (var original =
                                                                Image.FromStream(stream)) {
            var next = new Bitmap(original);
            var old = frame;
            frame = next;
            if (old != null)
                old.Dispose();
        }
        Invalidate();
    }
    protected override void OnPaint(PaintEventArgs e) {
        base.OnPaint(e);
        var g = e.Graphics;
        g.Clear(BackColor);
        if (frame != null) {
            double scale = Math.Min((double)Width / frame.Width, (double)Height / frame.Height);
            int w = (int)(frame.Width * scale), h = (int)(frame.Height * scale);
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.DrawImage(frame, (Width - w) / 2, (Height - h) / 2, w, h);
        } else {
            using (var pen = new Pen(Color.FromArgb(29, 45, 65))) for (int x = 0; x < Width;
                                                                       x += 40)
                g.DrawLine(pen, x, 0, x, Height);
        }
        if (DisplayPaused && frame != null) {
            using (var b = new SolidBrush(Color.FromArgb(215, 18, 32, 54)))
                g.FillRectangle(b, 0, 0, Width, 43);
            using (var font = Theme.Font(10, FontStyle.Bold))
                TextRenderer.DrawText(g, "画面已暂停 · 仅暂停本机显示", font,
                    new Rectangle(14, 0, Math.Max(10, Width - 28), 43), Color.White,
                    TextFormatFlags.Left | TextFormatFlags.VerticalCenter);
        } else if (!Live || PreviewStale) {
            using (var b = new SolidBrush(Color.FromArgb(155, 9, 17, 29)))
                g.FillRectangle(b, ClientRectangle);
            string message = PreviewStale && Live ? "画面暂未更新\n正在等待新画面" : Message;
            using (var font = Theme.Font(15, FontStyle.Bold))
            TextRenderer.DrawText(g, message, font,
                                  new Rectangle(20, Height / 2 - 40, Math.Max(10, Width - 40), 80),
                                  Color.White,
                                  TextFormatFlags.HorizontalCenter |
                                      TextFormatFlags.VerticalCenter | TextFormatFlags.WordBreak);
        }
    }
    protected override void Dispose(bool disposing) {
        if (disposing && frame != null)
            frame.Dispose();
        base.Dispose(disposing);
    }
}
// 网络边界：HTTP只读，控制走SSH子进程。
// HTTP只读状态/画面/导出，写操作调用独立SSH组件；客户端不加载模型、SDK或寄存器映射。
static class Api {
    public static string Root = AppDomain.CurrentDomain.BaseDirectory, Data, ConfigPath;
    public static Dictionary<string, object> Config;
    // 默认只绑定既有板卡地址/端口/指纹；私钥路径不随迁移包内置。
    public static Dictionary<string, object> Defaults() {
        return new Dictionary<string, object> {
            { "host", "169.254.74.247" },
            { "http_port", 8080 },
            { "ssh_port", 22 },
            { "username", "root" },
            { "bind_address", "" },
            { "key_file", "" },
            { "host_fingerprint", "" }
        };
    }
    // 连接配置保存于当前Windows用户LocalAppData，可移动程序目录而不把账号配置发给他人。
    public static void Initialize(string data) {
        Data = data;
        Directory.CreateDirectory(Data);
        ConfigPath = Path.Combine(Data, "connection.json");
        Config = Defaults();
        if (File.Exists(ConfigPath)) {
            foreach (var kv in J.Decode(File.ReadAllText(ConfigPath, Encoding.UTF8)))
                Config[kv.Key] = kv.Value;
        }
    }
    public static string Base(Dictionary<string, object> c) {
        return new UriBuilder("http", J.S(J.G(c, "host")), (int)J.N(J.G(c, "http_port")))
            .Uri.ToString()
            .TrimEnd('/');
    }
    // 后台线程发有超时和字节上限的HTTP GET；该函数本身不更新WinForms控件。
    public static byte[] Read(string path, int limit = 1024 * 1024) {
        var cfg = Config;
        var req = (HttpWebRequest)WebRequest.Create(Base(cfg) + path);
        req.Proxy = null;
        req.Timeout = 2000;
        req.ReadWriteTimeout = 2000;
        using (var response = req.GetResponse()) using (
            var stream = response.GetResponseStream()) using (var memory = new MemoryStream()) {
            byte[] b = new byte[32768];
            int n;
            while ((n = stream.Read(b, 0, b.Length)) > 0) {
                if (memory.Length + n > limit)
                    throw new Exception("响应超出显示上限");
                memory.Write(b, 0, n);
            }
            return memory.ToArray();
        }
    }
    public static Dictionary<string, object> State() {
        return J.Decode(Encoding.UTF8.GetString(Read("/api/state")));
    }
    // 只写本机连接配置，不修改网卡IP或板端网络；私钥只保存文件路径。
    public static void Save(Dictionary<string, object> c) {
        var host = J.S(J.G(c, "host"));
        IPAddress ip;
        if (!IPAddress.TryParse(host, out ip))
            throw new Exception("请输入板卡IPv4地址");
        foreach (var k in new[] { "http_port", "ssh_port" })
            if (J.N(J.G(c, k)) < 1 || J.N(J.G(c, k)) > 65535)
                throw new Exception("端口应在1到65535之间");
        string key = J.S(J.G(c, "key_file"));
        if (key.Length > 0 && !File.Exists(key))
            throw new Exception("所选密钥文件不存在");
        File.WriteAllText(ConfigPath, J.Encode(c), new UTF8Encoding(false));
        Config = c;
    }
    public static string Quote(string s) { return "\"" + s.Replace("\"", "\\\"") + "\""; }
    // 启动打包内的认证组件，并同时排空stdout/stderr；不依赖新电脑安装系统SSH。
    // 等待超时不表示板端已停止，也不强杀板端进程；上层继续以实际状态为准。
    public static Dictionary<string, object> Control(string action) {
        if (!File.Exists(ConfigPath))
            Save(Config);
        string exe = Path.Combine(Root, @"engine\UAVControlAgent.exe");
        if (!File.Exists(exe))
            throw new Exception("内置认证组件缺失，请完整解压软件文件夹，不要单独移动EXE。");
        var info = new ProcessStartInfo(exe, action + " --config " + Quote(ConfigPath)) {
            UseShellExecute = false,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden,
            WorkingDirectory = Root,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8
        };
        using (var process = Process.Start(info)) {
            var read = process.StandardOutput.ReadToEndAsync();
            var err = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(26000))
                throw new Exception("认证控制响应超时，未终止板端进程，请查看运行状态。");
            string text = read.Result.Trim();
            if (text.Length == 0)
                throw new Exception("认证组件未返回结果：" + err.Result);
            var result = J.Decode(text);
            if (!J.B(J.G(result, "ok")))
                throw new Exception(J.S(J.G(result, "error")));
            return result;
        }
    }
    // 先写本地.partial文件，完整接收成功后再替换目标ZIP，避免把半个导出误当完整记录。
    public static void Export(string path) {
        var req = (HttpWebRequest)WebRequest.Create(Base(Config) + "/export.zip");
        req.Proxy = null;
        req.Timeout = 15000;
        req.ReadWriteTimeout = 15000;
        string partial = path + ".partial";
        using (var response = req.GetResponse()) using (
            var stream =
                response.GetResponseStream()) using (var f = new FileStream(partial,
                                                                            FileMode.CreateNew)) {
            byte[] b = new byte[65536];
            int n;
            while ((n = stream.Read(b, 0, b.Length)) > 0)
                f.Write(b, 0, n);
        }
        if (File.Exists(path))
            File.Replace(partial, path, null);
        else
            File.Move(partial, path);
    }
}
// 本机连接设置；迁移后重新选择授权密钥和核实指纹。
class Settings : Form {
    Dictionary<string, TextBox> fields = new Dictionary<string, TextBox>();
    // 连接设置只收集地址、端口、已授权密钥位置和核实过的板卡指纹，不自动降低认证要求。
    public Settings() {
        Text = "连接设置";
        Size = new Size(740, 515);
        MinimumSize = Size;
        StartPosition = FormStartPosition.CenterParent;
        Font = Theme.Font();
        BackColor = Theme.Bg;
        var table = new TableLayoutPanel() { Dock = DockStyle.Fill, Padding = new Padding(24),
                                             ColumnCount = 3, RowCount = 10 };
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 135));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 80));
        Controls.Add(table);
        string[] keys = { "host",         "http_port", "ssh_port",        "username",
                          "bind_address", "key_file",  "host_fingerprint" };
        string[] names = {
            "板卡地址",      "查看端口",          "SSH端口", "SSH用户名", "本机网口地址（可空）",
            "已授权SSH私钥", "板卡主机SHA256指纹"
        };
        for (int i = 0; i < keys.Length; i++) {
            table.RowStyles.Add(new RowStyle(SizeType.Absolute, 39));
            table.Controls.Add(new Label() { Text = names[i], Dock = DockStyle.Fill,
                                             TextAlign = ContentAlignment.MiddleLeft,
                                             ForeColor = Theme.Ink },
                               0, i);
            var input = new TextBox() { Text = J.S(J.G(Api.Config, keys[i])), Dock = DockStyle.Fill,
                                        Margin = new Padding(4, 7, 4, 4),
                                        BorderStyle = BorderStyle.FixedSingle };
            fields[keys[i]] = input;
            table.Controls.Add(input, 1, i);
        }
        var choose =
            new ActionButton() { Text = "选择", Dock = DockStyle.Fill, Margin = new Padding(4) };
        table.Controls.Add(choose, 2, 5);
        choose.Click += delegate {
            using (var d =
                       new OpenFileDialog() {
                           Title = "选择本机已授权SSH私钥（不会复制进分发包）",
                           Filter = "密钥或任意文件|*.*"
                       }) if (d.ShowDialog(this) == DialogResult.OK) fields["key_file"]
                .Text = d.FileName;
        };
        var note = Theme.Label(
            "只查看画面无需密钥。启动/停止使用本机已授权密钥，并核对板卡指纹。\n软件不修改电脑网口、BOOT、位流或板卡电源。迁移到其他电脑后重新配置连接。",
            9, Theme.Muted);
        note.Dock = DockStyle.Fill;
        table.Controls.Add(note, 0, 7);
        table.SetColumnSpan(note, 3);
        table.RowStyles.Add(new RowStyle(SizeType.Absolute, 70));
        var save =
            new ActionButton() { Text = "保存连接",   Fill = Theme.Blue,      Ink = Color.White,
                                 Border = Theme.Blue, Dock = DockStyle.Right, Width = 130 };
        table.Controls.Add(save, 1, 8);
        save.Click += delegate {
            try {
                var config = Api.Defaults();
                foreach (var kv in fields)
                    config[kv.Key] = kv.Value.Text.Trim();
                Api.Save(config);
                DialogResult = DialogResult.OK;
                Close();
            } catch (Exception ex) {
                MessageBox.Show(this, ex.Message, "设置未保存", MessageBoxButtons.OK,
                                MessageBoxIcon.Information);
            }
        };
    }
}
class MainForm : Form {
    Panel workspace, monitor, records;
    VideoSurface video;
    Label connection, phase, reason, frames, fps, alarm, fault, duration, commits, age, notice,
        address, recordInfo;
    DataGridView eventGrid;
    ActionButton start, stop, export, pausePreview, savePreview;
    ActionButton navMonitor, navRecords;
    System.Windows.Forms.Timer timer, previewTimer;
    bool polling, commanding, closing, previewPolling, previewAvailable, previewRequested, viewPaused;
    byte[] lastPreview;
    string previewSession = "";
    Label displayRate;
    Stopwatch displayClock = Stopwatch.StartNew();
    Stopwatch previewFreshness = Stopwatch.StartNew();
    int displayed;
    Dictionary<string, object> snapshot;
    // 建立导航、状态和画面区；状态与预览使用独立定时器，各自限制一个在途请求。
    // 关闭窗口只退出本机查看，板端自启检测继续运行。
    public MainForm() {
        Text = "无人机检测系统";
        Size = new Size(1400, 900);
        MinimumSize = new Size(1160, 760);
        StartPosition = FormStartPosition.CenterScreen;
        Font = Theme.Font();
        BackColor = Theme.Bg;
        DoubleBuffered = true;
        KeyPreview = true;
        Build();
        KeyDown += async delegate(object sender, KeyEventArgs e) {
            if (e.KeyCode == Keys.Space && !e.Control && monitor.Visible && !(ActiveControl is ButtonBase)) {
                TogglePreview(); e.Handled = e.SuppressKeyPress = true;
            } else if (e.Control && e.KeyCode == Keys.S && monitor.Visible) {
                SavePreview(); e.Handled = e.SuppressKeyPress = true;
            } else if (e.KeyCode == Keys.F5) {
                await Poll(); if (records.Visible) await LoadEvents();
                e.Handled = e.SuppressKeyPress = true;
            }
        };
        timer = new System.Windows.Forms.Timer() { Interval = 650 };
        timer.Tick += async delegate { await Poll(); };
        // Two independent requests at most: state and latest image. Slow HTTP
        // responses never create a queue of preview tasks.
        previewTimer = new System.Windows.Forms.Timer() { Interval = 100 };
        previewTimer.Tick += async delegate { await PollPreview(); };
        Shown += async delegate {
            timer.Start();
            previewTimer.Start();
            await Poll();
        };
        FormClosing += delegate {
            closing = true;
            timer.Stop();
            previewTimer.Stop();
            lastPreview = null;
        };
    }
    Label L(Control parent, string text, float size, Color color, int x, int y, int w, int h,
            FontStyle style = FontStyle.Regular) {
        var v = Theme.Label(text, size, color, style);
        v.SetBounds(x, y, w, h);
        parent.Controls.Add(v);
        return v;
    }
    ActionButton Button(Control parent, string text, int x, int y, int w, Color fill, Color ink) {
        var b = new ActionButton() { Text = text, Fill = fill, Ink = ink,
                                     Border = fill == Theme.Blue ? Theme.Blue : Theme.Line };
        b.SetBounds(x, y, w, 42);
        parent.Controls.Add(b);
        return b;
    }
    void Build() {
        var sidebar = new Panel() { Dock = DockStyle.Left, Width = 170, BackColor = Theme.Navy };
        Controls.Add(sidebar);
        L(sidebar, "U A V", 22, Color.White, 25, 35, 130, 40, FontStyle.Bold);
        L(sidebar, "FPGA  /  EDGE VISION", 8, Color.FromArgb(134, 160, 197), 25, 78, 135, 30);
        navMonitor =
            Button(sidebar, "实时监测", 15, 150, 140, Color.FromArgb(42, 66, 103), Color.White);
        navRecords =
            Button(sidebar, "告警记录", 15, 205, 140, Theme.Navy, Color.FromArgb(179, 194, 216));
        var nav3 =
            Button(sidebar, "连接设置", 15, 260, 140, Theme.Navy, Color.FromArgb(179, 194, 216));
        var help =
            Button(sidebar, "操作说明", 15, 315, 140, Theme.Navy, Color.FromArgb(179, 194, 216));
        navMonitor.Border = navMonitor.Fill; navRecords.Border = navRecords.Fill;
        nav3.Border = nav3.Fill; help.Border = help.Fill; navMonitor.ActiveNavigation = true;
        var sideFoot = Theme.Label("CPU  /  NPU  /  FPGA\n板端独立运行", 8.5F, Color.FromArgb(164, 184, 213));
        sideFoot.SetBounds(24, Height - 150, 145, 58);
        sideFoot.Anchor = AnchorStyles.Left | AnchorStyles.Bottom;
        sidebar.Controls.Add(sideFoot);
        workspace = new Panel() { Dock = DockStyle.Fill, Padding = new Padding(26, 0, 26, 18),
                                  BackColor = Theme.Bg };
        Controls.Add(workspace);
        workspace.BringToFront();
        var header = new Panel() { Dock = DockStyle.Top, Height = 137 };
        workspace.Controls.Add(header);
        L(header, "无人机检测系统", 24, Theme.Ink, 0, 20, 480, 43, FontStyle.Bold);
        L(header, "异构协同的低空无人机检测系统", 10, Theme.Ink, 2, 68, 480, 24);
        L(header, "", 9, Theme.Muted, 2, 97, 480, 23);
        var context = new Panel() { BackColor = Color.Transparent };
        context.SetBounds(header.Width - 405, 18, 405, 107);
        context.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        header.Controls.Add(context);
        connection = L(context, "●  正在连接", 10, Theme.Muted, 135, 0, 270, 24,
                       FontStyle.Bold);
        connection.TextAlign = ContentAlignment.MiddleRight;
        var contest = L(context, "全国大学生嵌入式芯片与系统设计竞赛", 9.5F, Theme.Ink, 0, 32, 405, 22);
        contest.TextAlign = ContentAlignment.MiddleRight;
        var track = L(context, "2026  ·  FPGA 创新设计赛道", 9, Theme.Muted, 0, 56, 405, 21);
        track.TextAlign = ContentAlignment.MiddleRight;
        var board = L(context, "复旦微 FMQL30TAI 悟净开发板 · 完整版", 9.5F, Theme.Ink, 0, 79, 405, 22);
        board.TextAlign = ContentAlignment.MiddleRight;
        var toolbar = new Panel() { Dock = DockStyle.Top, Height = 67 };
        workspace.Controls.Add(toolbar);
        start = Button(toolbar, "▶  启动检测", 0, 6, 140, Theme.Blue, Color.White);
        stop = Button(toolbar, "■  正常停止", 152, 6, 140, Color.White, Theme.Ink);
        export = Button(toolbar, "↓  导出记录", 304, 6, 140, Color.White, Theme.Ink);
        var actionTips = new ToolTip();
        actionTips.SetToolTip(start, "通过已授权SSH连接请求板端启动检测。");
        actionTips.SetToolTip(stop, "通过已授权SSH请求正常停止；等待当前推理与SDK关闭。");
        actionTips.SetToolTip(export, "将当前会话的板端记录导出为ZIP。");
        actionTips.SetToolTip(navMonitor, "查看实时画面、运行状态和硬件告警。");
        actionTips.SetToolTip(navRecords, "查看当前会话最近的告警与故障事件。");
        actionTips.SetToolTip(nav3, "设置板卡地址及认证信息；只查看画面不需要私钥。");
        address = L(toolbar, "", 9, Theme.Muted, 470, 14, 450, 28);
        address.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        start.Click += async delegate { await Control("start"); };
        stop.Click += async delegate { await Control("stop"); };
        export.Click += async delegate { await Export(); };
        var footer = new Panel() { Dock = DockStyle.Bottom, Height = 43 };
        workspace.Controls.Add(footer);
        notice =
            L(footer, "检测由板端运行。关闭本程序不会停止检测。", 9, Theme.Muted, 0, 12, 1050, 26);
        notice.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        var host = new Panel() { Dock = DockStyle.Fill };
        workspace.Controls.Add(host);
        workspace.Controls.SetChildIndex(host, 0);
        workspace.Controls.SetChildIndex(toolbar, 1);
        workspace.Controls.SetChildIndex(header, 2);
        workspace.Controls.SetChildIndex(footer, 3);
        monitor = new Panel() { Dock = DockStyle.Fill };
        host.Controls.Add(monitor);
        var columns = new TableLayoutPanel() { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1,
                                               BackColor = Theme.Bg };
        columns.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        columns.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 320));
        monitor.Controls.Add(columns);
        var camera = new Card() { Dock = DockStyle.Fill, Padding = new Padding(0),
                                  Margin = new Padding(0, 0, 18, 0) };
        columns.Controls.Add(camera, 0, 0);
        var camTop = new Panel() { Dock = DockStyle.Top, Height = 57, BackColor = Color.White };
        L(camTop, "实时画面", 12, Theme.Ink, 19, 15, 160, 26, FontStyle.Bold);
        pausePreview = Button(camTop, "暂停画面", 155, 8, 95, Color.White, Theme.Ink);
        pausePreview.Height = 38;
        savePreview = Button(camTop, "保存画面", 258, 8, 95, Color.White, Theme.Ink);
        savePreview.Height = 38;
        pausePreview.Click += delegate { TogglePreview(); };
        savePreview.Click += delegate { SavePreview(); };
        var tips = new ToolTip();
        tips.SetToolTip(pausePreview, "暂停或恢复本机显示（空格）；不向板端发送停止请求。");
        tips.SetToolTip(savePreview, "保存当前预览画面（Ctrl+S），画面可能包含检测框和状态叠层。");
        displayRate = L(camTop, "画面更新 / 秒 —", 8, Theme.Muted, camTop.Width - 158, 18, 145, 22);
        displayRate.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        camera.Controls.Add(camTop);
        var camFoot = new Panel() { Dock = DockStyle.Bottom, Height = 52, BackColor = Color.White };
        L(camFoot, "绿色框：本帧检测   蓝色区域：告警区域   空格：暂停画面   Ctrl+S：保存", 9, Theme.Muted, 16,
          14, 700, 24);
        camera.Controls.Add(camFoot);
        video = new VideoSurface() { Dock = DockStyle.Fill };
        camera.Controls.Add(video);
        camera.Controls.SetChildIndex(video, 0);
        camera.Controls.SetChildIndex(camTop, 1);
        camera.Controls.SetChildIndex(camFoot, 2);
        var status = new Card() { Dock = DockStyle.Fill, Margin = new Padding(0), AutoScroll = true,
                                  Padding = new Padding(0) };
        columns.Controls.Add(status, 1, 0);
        L(status, "运行概览", 12, Theme.Ink, 22, 20, 240, 28, FontStyle.Bold);
        phase = L(status, "正在连接", 21, Theme.Muted, 20, 64, 260, 42, FontStyle.Bold);
        reason = L(status, "等待板端状态", 9, Theme.Muted, 22, 112, 260, 49);
        frames = L(status, "—", 22, Theme.Ink, 22, 181, 135, 40, FontStyle.Bold);
        fps = L(status, "—", 22, Theme.Ink, 160, 181, 125, 40, FontStyle.Bold);
        L(status, "已处理帧", 9, Theme.Muted, 24, 224, 128, 25);
        L(status, "检测 FPS", 9, Theme.Muted, 162, 224, 128, 25);
        Divider(status, 270);
        L(status, "PL 硬件告警", 9, Theme.Muted, 22, 287, 240, 24);
        alarm = L(status, "等待状态", 18, Theme.Muted, 22, 318, 260, 35, FontStyle.Bold);
        L(status, "输入链路", 9, Theme.Muted, 22, 372, 240, 24);
        fault = L(status, "等待状态", 12, Theme.Muted, 22, 402, 260, 30, FontStyle.Bold);
        Divider(status, 450);
        duration = StatusLine(status, "运行时长", 470);
        commits = StatusLine(status, "有效提交", 512);
        age = StatusLine(status, "状态帧龄", 554);
        records = new Card() { Dock = DockStyle.Fill, Visible = false, Padding = new Padding(20) };
        host.Controls.Add(records);
        L(records, "当前会话告警记录", 17, Theme.Ink, 22, 16, 700, 38, FontStyle.Bold);
        recordInfo = L(records, "板端滚动记录；以下最多显示100条，完整记录请导出ZIP。", 9,
                       Theme.Muted, 24, 62, 900, 28);
        var refresh = Button(records, "刷新记录", 22, 100, 120, Color.White, Theme.Ink);
        refresh.Click += async delegate { await LoadEvents(); };
        eventGrid = new DataGridView() { Anchor = AnchorStyles.Top | AnchorStyles.Bottom |
                                                  AnchorStyles.Left | AnchorStyles.Right,
                                         Location = new Point(22, 159),
                                         Size = new Size(records.Width - 44, records.Height - 182),
                                         BackgroundColor = Color.White,
                                         BorderStyle = BorderStyle.None,
                                         ReadOnly = true,
                                         AllowUserToAddRows = false,
                                         AllowUserToDeleteRows = false,
                                         RowHeadersVisible = false,
                                         AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill,
                                         SelectionMode = DataGridViewSelectionMode.FullRowSelect,
                                         Font = Theme.Font(10) };
        eventGrid.Columns.Add("frame", "结果帧");
        eventGrid.Columns.Add("time", "板端单调时间 / s");
        eventGrid.Columns.Add("event", "事件");
        eventGrid.Columns.Add("alarm", "告警状态");
        records.Controls.Add(eventGrid);
        navMonitor.Click += delegate {
            monitor.Visible = true;
            records.Visible = false;
            monitor.BringToFront();
            SetNavigation(true);
        };
        navRecords.Click += async delegate {
            monitor.Visible = false;
            records.Visible = true;
            records.BringToFront();
            SetNavigation(false);
            await LoadEvents();
        };
        nav3.Click += delegate {
            using (var d = new Settings()) if (d.ShowDialog(this) == DialogResult.OK) {
                address.Text = Api.Base(Api.Config);
                notice.Text = "连接已更新；读取状态不会启动板端推理。";
            }
        };
        help.Click += delegate {
            MessageBox.Show(
                this,
                "1. 接好摄像头并给板卡上电，检测自动运行。\n2. 本程序直接查看板端画面；首次手动控制需在连接设置中选择已授权SSH密钥。\n3. 正常停止后等待“已停止”，SDK正常关闭后再断电。\n4. 重要记录及时导出；录像完整封装需正常停止。\n\n复制整个软件文件夹到Windows10/11 x64即可运行，无需Python、浏览器或系统SSH。新电脑仍需连接到板卡网络并配置自己的认证。\n\n目前存在误检；此程序不会更改模型阈值、告警规则或位流。",
            "操作说明", MessageBoxButtons.OK, MessageBoxIcon.Information);
        };
        address.Text = Api.Base(Api.Config);
        Resize += delegate {
            eventGrid.Size =
                new Size(Math.Max(300, records.Width - 44), Math.Max(160, records.Height - 182));
        };
    }
    void SetNavigation(bool viewingMonitor) {
        navMonitor.ActiveNavigation = viewingMonitor;
        navRecords.ActiveNavigation = !viewingMonitor;
        navMonitor.Fill = viewingMonitor ? Color.FromArgb(42, 66, 103) : Theme.Navy;
        navRecords.Fill = viewingMonitor ? Theme.Navy : Color.FromArgb(42, 66, 103);
        navMonitor.Ink = viewingMonitor ? Color.White : Color.FromArgb(179, 194, 216);
        navRecords.Ink = viewingMonitor ? Color.FromArgb(179, 194, 216) : Color.White;
        navMonitor.Border = navMonitor.Fill; navRecords.Border = navRecords.Fill;
        navMonitor.Invalidate(); navRecords.Invalidate();
    }
    void TogglePreview() {
        if (lastPreview == null && !viewPaused) {
            notice.Text = "当前还没有可暂停的画面。";
            return;
        }
        viewPaused = !viewPaused;
        video.DisplayPaused = viewPaused;
        pausePreview.Text = viewPaused ? "恢复画面" : "暂停画面";
        if (viewPaused) {
            displayRate.Text = "画面已暂停";
            notice.Text = "仅暂停本机画面显示；没有向板端发送停止请求。";
        } else {
            previewRequested = true;
            previewFreshness.Restart();
            displayClock.Restart(); displayed = 0;
            notice.Text = "已恢复画面显示；板端运行状态不受影响。";
        }
        video.Invalidate();
    }
    void SavePreview() {
        if (lastPreview == null) {
            notice.Text = "当前没有可保存的预览画面。";
            return;
        }
        // Capture the frame at click time; polling may continue while the dialog is open.
        byte[] captured = lastPreview;
        using (var dialog = new SaveFileDialog() {
            Title = "保存当前预览画面", Filter = "JPEG图片|*.jpg",
            FileName = "无人机检测画面_" + DateTime.Now.ToString("yyyyMMdd_HHmmss") + ".jpg"
        }) {
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            try {
                WritePreview(dialog.FileName, captured);
                notice.Text = "画面已保存：" + dialog.FileName;
            } catch (Exception ex) {
                notice.Text = "保存画面失败：" + ex.Message;
                MessageBox.Show(this, ex.Message, "保存失败", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
        }
    }
    static void WritePreview(string path, byte[] jpeg) {
        if (jpeg == null || jpeg.Length == 0) throw new InvalidOperationException("当前没有可保存的预览画面。");
        File.WriteAllBytes(path, jpeg);
    }
    void Divider(Control p, int y) {
        var line = new Panel() { BackColor = Theme.Line };
        line.SetBounds(22, y, 260, 1);
        p.Controls.Add(line);
    }
    Label StatusLine(Control p, string text, int y) {
        L(p, text, 9, Theme.Muted, 22, y, 125, 29);
        return L(p, "—", 10, Theme.Ink, 151, y, 130, 29, FontStyle.Bold);
    }
    static string Phase(string v) {
        switch (v) {
        case "RUNNING":
            return "正在检测";
        case "STARTING":
        case "ACTIVE":
            return "正在启动";
        case "STOPPING":
            return "正在正常停止";
        case "STOPPED":
        case "COMPLETED":
            return "已停止";
        case "WAITING_CAMERA":
            return "等待摄像头";
        case "WAITING_FIRST_FRAME":
            return "等待首帧";
        case "UNKNOWN_RETAINED":
            return "设备状态未知";
        case "INHERITED_ALARM_HOLD":
            return "保留先前告警";
        case "STALE_INPUT":
        case "STALE_INPUT_FAULT":
            return "输入已过期";
        case "INPUT_FAULT":
            return "输入中断";
        case "FAILED":
            return "运行失败";
        default:
            return "等待状态";
        }
    }
    // 后台轮询有防重入，界面线程只更新控件。
    // 状态轮询防重入。boot或session变化时废弃旧预览身份，防止上一会话画面看似仍在实时更新。
    async Task Poll() {
        if (polling || closing)
            return;
        polling = true;
        try {
            var state = await Task.Run(() => Api.State());
            if (closing)
                return;
            snapshot = state;
            UpdateState(snapshot);
            string session = J.S(J.G(J.G(state, "active"), "session")) + ":" + J.S(J.G(state, "boot"));
            if (session != previewSession) {
                previewSession = session;
                lastPreview = null;
                displayed = 0;
                displayClock.Restart();
                previewFreshness.Restart();
                // A frame from the previous session must never look live in the new session.
                video.PreviewStale = true;
            }
            previewAvailable = J.B(J.G(state, "preview_available"));
            if (!previewAvailable && video.Live && !viewPaused && previewFreshness.Elapsed.TotalSeconds >= 2) {
                video.PreviewStale = true;
                video.Invalidate();
            }
            previewRequested = true;
        } catch (Exception) {
            if (!closing) {
                previewAvailable = false;
                connection.Text = "●  未连接";
                connection.ForeColor = Theme.Red;
                phase.Text = "连接中断";
                phase.ForeColor = Theme.Muted;
                reason.Text = "请检查板卡电源、网线与连接设置。";
                video.Live = false;
                video.PreviewStale = false;
                video.Message = "板卡未连接\n最后画面仅供回看";
                video.Invalidate();
                alarm.Text = "未知";
                fault.Text = "连接中断";
                fault.ForeColor = Theme.Red;
            }
        } finally {
            polling = false;
        }
    }
    // 仅用于判断JPEG是否换帧；静止场景也可能相同，不能据此判定NPU故障或无目标。
    static bool SameImage(byte[] a, byte[] b) {
        if (a == null || b == null || a.Length != b.Length) return false;
        for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return false;
        return true;
    }
    // 只获取最新JPEG，不积压显示任务；await返回后复核窗口、暂停和会话身份。
    // 预览刷新率独立于板端processing_fps，断图只提示画面待恢复。
    async Task PollPreview() {
        if (closing || viewPaused || previewPolling || !previewAvailable || (!video.Live && !previewRequested)) return;
        previewPolling = true;
        string session = previewSession;
        try {
            var jpeg = await Task.Run(() => Api.Read("/preview.jpg", 2 * 1024 * 1024));
            if (closing || viewPaused || session != previewSession) return;
            previewRequested = false;
            if (!SameImage(lastPreview, jpeg)) {
                video.SetFrame(jpeg);
                lastPreview = jpeg;
                displayed++;
                previewFreshness.Restart();
                video.PreviewStale = false;
            } else if (video.Live && previewFreshness.Elapsed.TotalSeconds >= 5) {
                // Identical bytes can also occur for a stationary view. Phrase this as
                // a display warning, never as a device fault or stopped detection.
                video.PreviewStale = true;
                video.Invalidate();
            }
            if (displayClock.Elapsed.TotalSeconds >= 2) {
                displayRate.Text = "画面更新 / 秒  " + (video.Live ? (displayed / displayClock.Elapsed.TotalSeconds).ToString("F1") : "回看");
                displayed = 0;
                displayClock.Restart();
            }
        } catch {
            // State polling owns connection/fault status. Retain the last image.
            if (!closing) {
                displayRate.Text = "画面待恢复";
                if (previewFreshness.Elapsed.TotalSeconds >= 2) {
                    video.PreviewStale = true;
                    video.Invalidate();
                }
            }
        } finally {
            previewPolling = false;
        }
    }
    // 根据live和phase区分实时状态与上次记录。
    // live由板端boot/时间戳共同判定；停止后仍可展示末帧，但告警必须标明“上次记录”。
    // 读取PL状态位0为alarm、位1为fault，不能把LED灭直接等同于输入正常。
    void UpdateState(Dictionary<string, object> x) {
        string p = J.S(J.G(x, "phase"));
        bool live = J.B(J.G(x, "live"));
        var s = J.D(J.G(x, "state"));
        connection.Text = "●  板端已连接";
        connection.ForeColor = Theme.Green;
        phase.Text = Phase(p);
        phase.ForeColor =
            live ? Theme.Green
                 : ((p == "FAILED" || p == "UNKNOWN_RETAINED") ? Theme.Red : Theme.Ink);
        string why = J.S(J.G(J.G(x, "app"), "reason"));
        if (why.Length == 0)
            why = J.S(J.G(s, "reason"));
        reason.Text = why.Length > 0 ? why
                                     : (live ? "板端独立检测与告警\n关闭窗口不影响设备运行。"
                                             : "最后画面保留；请根据状态处理。 ");
        long count = J.N(J.G(s, "frames"));
        double seconds = J.F(J.G(s, "elapsed_s"));
        frames.Text = count.ToString("N0");
        double processing = J.G(s, "processing_fps") == null ? (seconds > 0 ? count / seconds : 0) : J.F(J.G(s, "processing_fps"));
        fps.Text = processing > 0 ? processing.ToString("F2") : "—";
        var pl = J.D(J.G(s, "PL"));
        if (pl.Count == 0)
            pl = J.D(J.G(J.G(s, "result"), "final_PL"));
        bool has = J.G(pl, "32") != null;
        long bits = J.N(J.G(pl, "32"));
        alarm.Text =
            !has ? "未知" : ((bits & 1) != 0 ? "告警置位" : "未告警") + (live ? "" : " · 上次记录");
        alarm.ForeColor = !has ? Theme.Muted : ((bits & 1) != 0 ? Theme.Red : Theme.Green);
        fault.Text =
            !has ? "未知" : ((bits & 2) != 0 ? "输入超时" : "正常") + (live ? "" : " · 上次记录");
        fault.ForeColor = has && (bits & 2) == 0 ? Theme.Green : Theme.Red;
        duration.Text = String.Format("{0:00}:{1:00}:{2:00}", (int)seconds / 3600,
                                      ((int)seconds / 60) % 60, (int)seconds % 60);
        commits.Text = J.N(J.G(s, "commits")).ToString("N0");
        age.Text = J.G(x, "state_age_s") == null
                       ? "旧启动记录"
                       : J.F(J.G(x, "state_age_s")).ToString("F1") + " 秒";
        video.Live = live;
        video.Message = Phase(p) + "\n所见画面仅供回看";
        video.Invalidate();
    }
    // 仅发认证请求，超时不能当作板端已停止。
    // 同一时刻仅一个控制请求，等待时禁用启停按钮；响应成功后仍刷新实际板端阶段。
    async Task Control(string action) {
        if (commanding)
            return;
        if (!File.Exists(J.S(J.G(Api.Config, "key_file")))) {
            using (var d = new Settings()) d.ShowDialog(this);
            if (!File.Exists(J.S(J.G(Api.Config, "key_file")))) {
                notice.Text = "画面可只读查看；认证启停需要先配置本机已授权SSH密钥。";
                return;
            }
        }
        commanding = true;
        start.Enabled = stop.Enabled = false;
        notice.Text = action == "start" ? "正在认证发送启动请求…"
                                        : "正在请求正常停止；等待当前推理完成和SDK关闭…";
        try {
            await Task.Run(() => Api.Control(action));
            notice.Text = action == "start" ? "已发送认证启动请求，等待板端状态更新。"
                                            : "正常停止请求已发送；请等待“已停止”。";
            await Poll();
        } catch (Exception e) {
            notice.Text = e.Message;
            MessageBox.Show(this, e.Message, "控制请求未完成", MessageBoxButtons.OK,
                            MessageBoxIcon.Information);
        } finally {
            commanding = false;
            start.Enabled = stop.Enabled = true;
        }
    }
    // 导出近期记录到所选ZIP，不写板端状态。
    // 文件对话框在UI线程，网络写盘在Task.Run中；结束后恢复按钮并给出本地结果。
    async Task Export() {
        using (var dialog = new SaveFileDialog() {
            Title = "导出板端当前会话记录", Filter = "ZIP记录包|*.zip",
            FileName = "无人机检测记录_" + DateTime.Now.ToString("yyyyMMdd_HHmmss") + ".zip"
        }) {
            if (dialog.ShowDialog(this) != DialogResult.OK)
                return;
            export.Enabled = false;
            notice.Text = "正在导出当前记录…";
            try {
                await Task.Run(() => Api.Export(dialog.FileName));
                notice.Text = "记录已导出：" + dialog.FileName;
            } catch (Exception e) {
                notice.Text = "导出失败：" + e.Message;
            } finally {
                export.Enabled = true;
            }
        }
    }
    // 只显示当前滚动CSV的最近100行；它不是系统全部生命周期的告警总表。
    async Task LoadEvents() {
        try {
            string csv = await Task.Run(
                () => Encoding.UTF8.GetString(Api.Read("/events.csv", 5 * 1024 * 1024)));
            string[] lines = csv.Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries);
            eventGrid.Rows.Clear();
            for (int i = Math.Max(1, lines.Length - 100); i < lines.Length; i++) {
                var cells = lines[i].Trim().Split(',');
                if (cells.Length < 5)
                    continue;
                int bits;
                int.TryParse(cells[2], out bits);
                string evt =
                    (bits & 1) != 0
                        ? "告警置位"
                        : ((bits & 2) != 0
                               ? "告警解除"
                               : ((bits & 4) != 0 ? "输入故障"
                                                  : ((bits & 8) != 0 ? "输入恢复" : "状态事件")));
                eventGrid.Rows.Add(cells[0], cells[1], evt,
                                   cells[3].ToLower() == "true" ? "置位" : "未置位");
            }
            recordInfo.Text =
                "显示当前滚动CSV中的最新 " + eventGrid.Rows.Count + " 条；它不是完整生命周期记录。";
        } catch (Exception e) {
            recordInfo.Text = "记录暂不可用：" + e.Message;
        }
    }
}
static class Program {
    [STAThread]
    // 程序入口使用STA承载WinForms；--self-test只检查本机文件，不连接或启动板端。
    static void Main(string[] args) {
        try {
            string data = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "UAVDetectionSystem", "v2");
            for (int i = 0; i + 1 < args.Length; i++)
                if (args[i] == "--data-dir")
                    data = Path.GetFullPath(args[i + 1]);
            Api.Initialize(data);
            if (Array.IndexOf(args, "--self-test") >= 0) {
                string agent = Path.Combine(Api.Root, @"engine\UAVControlAgent.exe");
                File.WriteAllText(
                    Path.Combine(data, "SELF_TEST.json"),
                    J.Encode(new { ok = File.Exists(agent), app = "无人机检测系统", native = true,
                                   html = false, python_required = false,
                                   system_ssh_required = false, root = Api.Root }),
                    Encoding.UTF8);
                return;
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        } catch (Exception e) {
            MessageBox.Show("程序未能启动：" + e.Message, "无人机检测系统", MessageBoxButtons.OK,
                            MessageBoxIcon.Error);
        }
    }
}
}

