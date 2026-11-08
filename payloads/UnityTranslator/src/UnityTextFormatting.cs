using System;

// Unity Mono 的纯文本格式辅助器；不访问 Unity 对象、缓存、网络或线程状态。
internal static class UnityTextFormatting
{
    public static string RestoreLeadingTextDecoration(string translated, string originalText)
    {
        if (string.IsNullOrWhiteSpace(translated) || string.IsNullOrWhiteSpace(originalText))
        {
            return translated;
        }
        if (!TryGetLeadingTextDecoration(originalText, out var decoration, out _))
        {
            return translated;
        }
        int contentStart;
        if (!TryGetLeadingTextDecoration(translated, out _, out contentStart))
        {
            contentStart = 0;
            while (contentStart < translated.Length && IsHorizontalWhitespace(translated[contentStart]))
            {
                contentStart++;
            }
            int markerStart = contentStart;
            while (contentStart < translated.Length && IsLeadingTextDecorationMarker(translated[contentStart]))
            {
                contentStart++;
            }
            if (contentStart == markerStart)
            {
                contentStart = 0;
            }
            else
            {
                while (contentStart < translated.Length && IsHorizontalWhitespace(translated[contentStart]))
                {
                    contentStart++;
                }
            }
        }
        if (contentStart >= translated.Length || string.IsNullOrWhiteSpace(translated.Substring(contentStart)))
        {
            return translated;
        }
        return decoration + translated.Substring(contentStart);
    }

    private static bool TryGetLeadingTextDecoration(string text, out string decoration, out int contentStart)
    {
        decoration = null;
        contentStart = 0;
        if (string.IsNullOrEmpty(text))
        {
            return false;
        }
        int index = 0;
        while (index < text.Length && IsHorizontalWhitespace(text[index]))
        {
            index++;
        }
        int markerStart = index;
        while (index < text.Length && IsLeadingTextDecorationMarker(text[index]))
        {
            index++;
        }
        if (index == markerStart)
        {
            return false;
        }
        int separatorStart = index;
        while (index < text.Length && IsHorizontalWhitespace(text[index]))
        {
            index++;
        }
        if (index == separatorStart || index >= text.Length)
        {
            return false;
        }
        decoration = text.Substring(0, index);
        contentStart = index;
        return true;
    }

    private static bool IsHorizontalWhitespace(char c)
    {
        return c == ' ' || c == '\t';
    }

    private static bool IsLeadingTextDecorationMarker(char c)
    {
        return c == '-' || c == '\u2013' || c == '\u2014' || c == '\u2022' || c == '\u2023' || c == '\u25aa' || c == '\u25cf' || c == '\u25a0' || c == '\u25a1' || c == '*' || c == '>' || c == '+' || c == '\u00b7';
    }
}
