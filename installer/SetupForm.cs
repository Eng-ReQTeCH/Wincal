using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Threading.Tasks;
using System.Windows.Forms;

internal static partial class Setup
{
    sealed class SetupForm : Form
    {
        readonly Color surface = SystemInformation.HighContrast ? SystemColors.Window : Color.FromArgb(245, 247, 251);
        readonly Color panel = SystemColors.Window;
        readonly Color ink = SystemInformation.HighContrast ? SystemColors.WindowText : Color.FromArgb(24, 37, 58);
        readonly Color muted = SystemInformation.HighContrast ? SystemColors.WindowText : Color.FromArgb(78, 93, 114);
        readonly Color accent = SystemInformation.HighContrast ? SystemColors.Highlight : Color.FromArgb(34, 80, 184);
        readonly Color border = SystemInformation.HighContrast ? SystemColors.WindowText : Color.FromArgb(206, 215, 228);
        readonly string root;
        readonly TextBox url = new TextBox(), user = new TextBox(), password = new TextBox();
        readonly CheckBox demo = new CheckBox { Text = "Try with a sample event", AutoSize = true };
        readonly Label feedback;
        readonly TableLayoutPanel content = Stack();
        bool checking;

        internal SetupForm(string directory)
        {
            root = directory;
            bool installed = File.Exists(Path.Combine(root, SettingsFile));
            Text = Setup.Name + " Setup";
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            AutoScaleDimensions = new SizeF(96, 96);
            AutoScaleMode = AutoScaleMode.Dpi;
            Font = new Font("Segoe UI", 10);
            BackColor = surface;
            ForeColor = ink;
            AutoScroll = true;
            ClientSize = new Size(600, 520);

            content.Padding = new Padding(28, 26, 28, 16);
            content.Dock = DockStyle.Top;
            Controls.Add(content);
            AddRow(content, Header(), 20);
            if (installed) AddRow(content, SavedConnection(), 12);
            AddRow(content, Connection(), 14);

            feedback = LabelText("Test the connection before saving, or install with a sample event.", muted);
            feedback.AccessibleName = "Connection status";
            AddRow(content, feedback, 14);

            var actions = InlineButtons();
            var install = MakeButton(installed ? "Save connection" : "Install", Save, true);
            actions.Controls.Add(install);
            actions.Controls.Add(MakeButton("Test connection", TestConnection));
            AddRow(content, actions, installed ? 12 : 0);
            AcceptButton = install;
            if (installed) AddRow(content, InstalledActions(), 0);

            PerformLayout();
            var workArea = Screen.FromControl(this).WorkingArea;
            int chrome = Height - ClientSize.Height;
            ClientSize = new Size(ClientSize.Width, Math.Min(content.PreferredSize.Height, workArea.Height - chrome));
            ActiveControl = url;
        }

        Control Header()
        {
            var header = Stack();
            header.ColumnCount = 2;
            header.ColumnStyles.Clear();
            header.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            header.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            header.Controls.Add(new CalendarGlyph(accent) { Margin = new Padding(0, 3, 16, 0) }, 0, 0);
            var copy = Stack();
            AddRow(copy, LabelText(Setup.Name, ink, 24, FontStyle.Bold), 2);
            AddRow(copy, LabelText("CalDAV events in your Windows taskbar calendar", muted), 0);
            header.Controls.Add(copy, 1, 0);
            return header;
        }

        Control SavedConnection()
        {
            var row = Stack();
            row.BackColor = panel;
            row.Padding = new Padding(16, 10, 12, 10);
            row.ColumnCount = 2;
            row.ColumnStyles.Clear();
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            row.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            row.Controls.Add(LabelText("Use your saved CalDAV connection", ink, 10, FontStyle.Bold), 0, 0);
            row.Controls.Add(MakeButton("Keep settings", Update), 1, 0);
            return row;
        }

        Control Connection()
        {
            var card = Stack();
            card.BackColor = panel;
            card.Padding = new Padding(18, 16, 18, 16);
            AddRow(card, LabelText("Calendar connection", ink, 14, FontStyle.Bold), 12);
            AddRow(card, Field("CalDAV calendar URL", url, 2047), 4);
            AddRow(card, LabelText("Enter the full calendar collection URL, including its path.", muted, 9), 12);
            password.UseSystemPasswordChar = true;
            var login = Stack();
            login.ColumnCount = 3;
            login.ColumnStyles.Clear();
            login.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
            login.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 14));
            login.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
            login.Controls.Add(Field("Username", user, 255), 0, 0);
            login.Controls.Add(Field("Password", password, 511), 2, 0);
            AddRow(card, login, 12);
            demo.CheckedChanged += delegate
            {
                url.Enabled = user.Enabled = password.Enabled = !demo.Checked;
                SetFeedback(demo.Checked ? "Setup will use a sample event and skip the CalDAV server." :
                    "Leave the login fields empty if your calendar allows anonymous access.", muted);
            };
            AddRow(card, demo, 0);
            return card;
        }

        Control InstalledActions()
        {
            var actions = InlineButtons();
            actions.FlowDirection = FlowDirection.LeftToRight;
            actions.Controls.Add(MakeButton("Flyout status", ShowStatus));
            actions.Controls.Add(MakeButton("Uninstall", Uninstall));
            return actions;
        }

        static TableLayoutPanel Stack()
        {
            var stack = new TableLayoutPanel
            {
                AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
                ColumnCount = 1, Dock = DockStyle.Fill, Margin = Padding.Empty
            };
            stack.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            return stack;
        }

        static void AddRow(TableLayoutPanel stack, Control control, int bottom)
        {
            int row = stack.RowCount++;
            stack.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            control.Dock = DockStyle.Fill;
            control.Margin = new Padding(0, 0, 0, bottom);
            stack.Controls.Add(control, 0, row);
        }

        static FlowLayoutPanel InlineButtons()
        {
            return new FlowLayoutPanel
            {
                AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
                FlowDirection = FlowDirection.RightToLeft, WrapContents = false, Margin = Padding.Empty
            };
        }

        Label LabelText(string value, Color color, float size = 10, FontStyle style = FontStyle.Regular)
        {
            return new Label { Text = value, ForeColor = color, Font = new Font(Font.FontFamily, size, style),
                TextAlign = ContentAlignment.MiddleLeft, AutoSize = true, Dock = DockStyle.Fill, Margin = Padding.Empty };
        }

        Control Field(string label, TextBox field, int length)
        {
            var stack = Stack();
            AddRow(stack, LabelText(label, ink, 9, FontStyle.Bold), 5);
            field.AccessibleName = label;
            field.MaxLength = length;
            AddRow(stack, field, 0);
            return stack;
        }

        Button MakeButton(string label, EventHandler action, bool primary = false)
        {
            var button = new Button
            {
                Text = label, AutoSize = true, MinimumSize = new Size(104, 36), Padding = new Padding(10, 0, 10, 0),
                Margin = new Padding(4, 0, 4, 0), ForeColor = primary
                    ? (SystemInformation.HighContrast ? SystemColors.HighlightText : Color.White) : ink,
                BackColor = primary ? accent : panel, FlatStyle = FlatStyle.Flat, UseVisualStyleBackColor = false
            };
            button.FlatAppearance.BorderColor = primary ? accent : border;
            if (!SystemInformation.HighContrast)
                button.FlatAppearance.MouseOverBackColor = primary ? Color.FromArgb(27, 64, 153) : surface;
            button.Click += action;
            return button;
        }

        void SetFeedback(string message, Color color)
        {
            feedback.ForeColor = color;
            feedback.Text = message;
        }

        async void TestConnection(object sender, EventArgs args)
        {
            if (checking) return;
            checking = true;
            var inputUrl = url.Text;
            var inputUser = user.Text;
            var inputPassword = password.Text;
            var useDemo = demo.Checked;
            Enabled = false;
            UseWaitCursor = true;
            SetFeedback("Testing the CalDAV connection…", muted);
            string temp = Path.Combine(Path.GetTempPath(), Setup.Name + "-" + Guid.NewGuid().ToString("N"));
            try
            {
                Directory.CreateDirectory(temp);
                string settings = Path.Combine(temp, SettingsFile);
                string exe = Path.Combine(temp, Launcher);
                WriteSettings(settings, inputUrl, inputUser, inputPassword, useDemo);
                Extract(Launcher, exe);
                var start = new ProcessStartInfo(exe, "--check " + Quote(settings))
                {
                    UseShellExecute = false, RedirectStandardError = true, CreateNoWindow = true
                };
                string error = await Task.Run(() =>
                {
                    using (Process process = Process.Start(start))
                    {
                        string message = process.StandardError.ReadToEnd().Trim();
                        process.WaitForExit();
                        if (process.ExitCode == 0) return null;
                        return message.Length > 0 ? message : "The calendar connection could not be verified.";
                    }
                });
                if (error == null)
                    SetFeedback(useDemo ? "The sample calendar is ready." : "Connected. The calendar is ready to sync.", accent);
                else SetFeedback(error, ink);
            }
            catch (Exception error) { SetFeedback(error.Message, ink); }
            finally
            {
                try
                {
                    if (RemoveKnownFiles(temp, new[] { SettingsFile, Launcher }) && Directory.Exists(temp))
                        Directory.Delete(temp);
                }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
                finally
                {
                    checking = false;
                    UseWaitCursor = false;
                    Enabled = true;
                }
            }
        }

        void Save(object sender, EventArgs args)
        {
            try
            {
                Install(root, WindowsRegistry, url.Text, user.Text, password.Text, demo.Checked);
                StartLauncher(root);
                Close();
            }
            catch (Exception error) { SetFeedback(error.Message, ink); }
        }

        void Update(object sender, EventArgs args)
        {
            try
            {
                StageUpdate(root, WindowsRegistry);
                StartLauncher(root);
                Close();
            }
            catch (Exception error) { SetFeedback(error.Message, ink); }
        }

        void Uninstall(object sender, EventArgs args)
        {
            try
            {
                string cmd = Path.Combine(Environment.SystemDirectory, "cmd.exe");
                using (Process process = Process.Start(new ProcessStartInfo(cmd, "/d /c \"" + Quote(Path.Combine(root, UninstallDriver)) + "\"")
                { UseShellExecute = false, CreateNoWindow = true })) { }
                Close();
            }
            catch (Exception error) { SetFeedback(error.Message, ink); }
        }

        void ShowStatus(object sender, EventArgs args)
        {
            try
            {
                string log = Path.Combine(root, "status.txt");
                using (var dialog = new Form { Text = "Flyout status", StartPosition = FormStartPosition.CenterParent,
                    ClientSize = new Size(560, 360), MinimizeBox = false, ShowInTaskbar = false, Padding = new Padding(12) })
                {
                    var status = new TextBox { Multiline = true, ReadOnly = true, Dock = DockStyle.Fill,
                        ScrollBars = ScrollBars.Vertical, Text = File.Exists(log) ? File.ReadAllText(log) :
                            "The calendar launcher has not reported its status yet." };
                    var close = new Button { Text = "Close", DialogResult = DialogResult.OK, Dock = DockStyle.Bottom, Height = 32 };
                    dialog.Controls.Add(status);
                    dialog.Controls.Add(close);
                    dialog.CancelButton = close;
                    dialog.ShowDialog(this);
                }
            }
            catch (IOException error) { SetFeedback(error.Message, ink); }
        }
    }

    sealed class CalendarGlyph : Control
    {
        readonly Color accent;

        internal CalendarGlyph(Color color)
        {
            accent = color;
            Size = new Size(54, 54);
            SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint, true);
            TabStop = false;
        }

        protected override void OnPaint(PaintEventArgs args)
        {
            var graphics = args.Graphics;
            graphics.SmoothingMode = SmoothingMode.AntiAlias;
            var state = graphics.Save();
            graphics.ScaleTransform(Width / 54f, Height / 54f);
            using (var background = new SolidBrush(accent))
            using (var shape = new GraphicsPath())
            using (var stroke = new Pen(SystemInformation.HighContrast ? SystemColors.HighlightText : Color.White, 1.8f))
            {
                shape.AddArc(0, 0, 24, 24, 180, 90);
                shape.AddArc(30, 0, 24, 24, 270, 90);
                shape.AddArc(30, 30, 24, 24, 0, 90);
                shape.AddArc(0, 30, 24, 24, 90, 90);
                shape.CloseFigure();
                graphics.FillPath(background, shape);
                graphics.DrawRectangle(stroke, 14, 16, 26, 24);
                graphics.DrawLine(stroke, 14, 23, 40, 23);
                graphics.DrawLine(stroke, 21, 13, 21, 19);
                graphics.DrawLine(stroke, 33, 13, 33, 19);
                using (var dot = new SolidBrush(stroke.Color))
                    for (int x = 20; x <= 32; x += 6) graphics.FillEllipse(dot, x, 30, 3, 3);
            }
            graphics.Restore(state);
        }
    }
}
