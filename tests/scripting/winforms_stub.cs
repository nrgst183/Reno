// A stand-in for System.Windows.Forms for the Linux tests (where .NET has no Windows Forms): just what Reno's
// ScriptPortal.Vegas and the test scripts use. Message boxes print instead of showing.
using System;
namespace System.Windows.Forms
{
    public interface IWin32Window { IntPtr Handle { get; } }
    public class Control { }
    public class Form : Control { }
    public static class Application
    {
        public static void EnableVisualStyles() { }
        public static void DoEvents() { }
    }
    public enum DialogResult { None = 0, OK = 1, Cancel = 2, Abort = 3, Retry = 4, Ignore = 5, Yes = 6, No = 7 }
    public enum MessageBoxButtons { OK = 0, OKCancel = 1, AbortRetryIgnore = 2, YesNoCancel = 3, YesNo = 4, RetryCancel = 5 }
    public enum MessageBoxIcon { None = 0, Hand = 16, Error = 16, Stop = 16, Question = 32, Exclamation = 48, Warning = 48, Asterisk = 64, Information = 64 }
    public static class MessageBox
    {
        public static DialogResult Answer = DialogResult.OK;
        static DialogResult Say(string text, string caption) { Console.WriteLine("[MessageBox] " + (caption ?? "") + ": " + text); return Answer; }
        public static DialogResult Show(string text) { return Say(text, null); }
        public static DialogResult Show(string text, string caption) { return Say(text, caption); }
        public static DialogResult Show(string text, string caption, MessageBoxButtons b) { return Say(text, caption); }
        public static DialogResult Show(string text, string caption, MessageBoxButtons b, MessageBoxIcon i) { return Say(text, caption); }
        public static DialogResult Show(IWin32Window owner, string text) { return Say(text, null); }
        public static DialogResult Show(IWin32Window owner, string text, string caption) { return Say(text, caption); }
        public static DialogResult Show(IWin32Window owner, string text, string caption, MessageBoxButtons b) { return Say(text, caption); }
        public static DialogResult Show(IWin32Window owner, string text, string caption, MessageBoxButtons b, MessageBoxIcon i) { return Say(text, caption); }
    }
    // File dialogs answer with the file named by RENO_TEST_SAVE_PATH / RENO_TEST_OPEN_PATH (Cancel if not set).
    public class FileDialog
    {
        public string Filter { get; set; }
        public string Title { get; set; }
        public string FileName { get; set; }
        public string InitialDirectory { get; set; }
        public string DefaultExt { get; set; }
        public bool OverwritePrompt { get; set; }
        public bool CheckFileExists { get; set; }
        public int FilterIndex { get; set; }
        protected string answer;
        public DialogResult ShowDialog() { if (string.IsNullOrEmpty(answer)) return DialogResult.Cancel; FileName = answer; return DialogResult.OK; }
        public DialogResult ShowDialog(IWin32Window owner) { return ShowDialog(); }
    }
    public class SaveFileDialog : FileDialog { public SaveFileDialog() { answer = Environment.GetEnvironmentVariable("RENO_TEST_SAVE_PATH"); } }
    public class FolderBrowserDialog
    {
        public string Description { get; set; }
        public string SelectedPath { get; set; }
        public DialogResult ShowDialog(IWin32Window owner) { string a = Environment.GetEnvironmentVariable("RENO_TEST_FOLDER"); if (string.IsNullOrEmpty(a)) return DialogResult.Cancel; SelectedPath = a; return DialogResult.OK; }
    }
    public class OpenFileDialog : FileDialog { public OpenFileDialog() { answer = Environment.GetEnvironmentVariable("RENO_TEST_OPEN_PATH"); } }
}
