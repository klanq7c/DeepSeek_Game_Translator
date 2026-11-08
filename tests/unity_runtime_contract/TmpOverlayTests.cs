using System;

internal static class TmpOverlayTests
{
    public static void Run(Action<string, bool> check)
    {
        TmpOverlaySeam.Reset();
        var source = TmpOverlaySeam.Source;
        string failure;
        check("IL2CPP fallback creates overlay", TmpOverlaySeam.TryUpdateTmpUguiOverlay(source, 1, "译文", out failure));
        check("IL2CPP fallback hides source without taking ownership of its color", !source.enabled && source.color.a == 1);
        source.color = new TmpOverlaySeam.Color(1, 0, 0, .2f);
        TmpOverlaySeam.TryUpdateTmpUguiOverlay(source, 1, "译文", out failure);
        check("IL2CPP fallback follows game color and alpha", TmpOverlaySeam.Overlay.color.g == 0 && TmpOverlaySeam.Overlay.color.a == .2f);
        source.color = new TmpOverlaySeam.Color(1, 0, 0, 0);
        TmpOverlaySeam.TryUpdateTmpUguiOverlay(source, 1, "译文", out failure);
        check("IL2CPP game fade to zero does not leave visible fallback", TmpOverlaySeam.Overlay.color.a == 0);
        TmpOverlaySeam.DestroyTmpUguiOverlay(1);
        check("IL2CPP removal keeps latest game color and restores source enabled state", source.enabled && source.color.g == 0 && source.color.a == 0 && TmpOverlaySeam.Overlay.gameObject.destroyed);

        TmpOverlaySeam.Reset();
        source = TmpOverlaySeam.Source;
        TmpOverlaySeam.RejectSourceDisable = true;
        bool accepted = TmpOverlaySeam.TryUpdateTmpUguiOverlay(source, 1, "译文", out failure);
        check("IL2CPP failed source hide reports failure and removes duplicate overlay", !accepted && failure == "hide-source"
            && source.enabled && source.color.a == 1 && TmpOverlaySeam.Overlay.gameObject.destroyed);

        TmpOverlaySeam.Reset();
        source = TmpOverlaySeam.Source;
        TmpOverlaySeam.TryUpdateTmpUguiOverlay(source, 1, "译文", out failure);
        source.gameObject.activeInHierarchy = false;
        accepted = TmpOverlaySeam.TryUpdateTmpUguiOverlay(source, 1, "译文", out failure);
        check("IL2CPP deactivated source still removes overlay", !accepted && source.enabled && TmpOverlaySeam.Overlay.gameObject.destroyed);
    }
}
