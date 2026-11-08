using System;
using System.Threading;

namespace DstLauncher
{
    /* UI 线程拥有预留、线程与完成回调。关窗延后到真实线程退出；确认/完成
       弹窗的嵌套消息循环也不能重入生命周期操作。对应 ui_operation.c。 */
    internal static class UiOperation
    {
        private static bool _busy, _closing;
        private static Thread _thread;
        private static Action _complete;
        internal static Func<ThreadStart, Thread> CreateThread = work => new Thread(work);
        internal static bool Busy { get { return _busy; } }

        internal static bool Reserve()
        {
            if (_busy || _closing) {
                Log.Append("操作已忽略：启动器操作仍在进行或窗口正在关闭。");
                Log.Status("操作仍在进行，请稍候。");
                return false;
            }
            _busy = true;
            return true;
        }

        internal static void Cancel() { _busy = false; }

        internal static bool Start(Action work, Action complete = null)
        {
            if (_closing) { Cancel(); return false; }
            // 只处理线程创建这个外部资源边界；不捕获整条业务流程的未知异常。
            try {
                _thread = CreateThread(new ThreadStart(work));
                _thread.IsBackground = true;
                _thread.SetApartmentState(ApartmentState.STA);
                _complete = complete;
                _thread.Start();
                return true;
            } catch (OutOfMemoryException e) {
                CreationFailed(e);
            } catch (ThreadStateException e) {
                CreationFailed(e);
            } catch (System.Security.SecurityException e) {
                CreationFailed(e);
            }
            return false;
        }

        private static void CreationFailed(Exception error)
        {
            Log.Append("后台操作创建线程失败（" + error.GetType().Name + ": " + error.Message + "），未执行操作。");
            Log.Status("无法创建后台操作，请查看日志后重试。");
            _thread = null;
            _complete = null;
            Cancel();
        }

        internal static bool Poll()
        {
            if (_thread != null && _thread.Join(0)) {
                _thread = null;
                Action complete = _complete;
                _complete = null;
                try {
                    if (!_closing && complete != null) complete();
                } finally { _busy = false; }
            }
            return _closing && !_busy;
        }

        internal static bool RequestClose()
        {
            if (!_closing && _busy) {
                Log.Append("窗口将在当前操作完成后关闭。");
                Log.Status("正在等待当前操作完成后关闭...");
            }
            _closing = true;
            return !_busy;
        }
    }
}
