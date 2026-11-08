namespace DstUnity
{
    // Unity 显示层共享的标签边界；仅保护标签及属性，不改写任何共享译文或缓存键。
    public static class TmpRichText
    {
        // 显示层和只读审计共用已知标签入口；完整属性边界仍由下方解析器确定。
        public const string KnownTagPattern = @"<\s*/?\s*(?:b|i|u|s)\s*>|<\s*#[0-9A-Fa-f]{3,8}\s*>|<\s*(?:color|size|material)\s*=\s*[^>]+>|<\s*/\s*(?:color|size|material)\s*>|<\s*quad\b[^>]*>|<\s*br\s*/?\s*>|<\s*(?:mark|sprite|line-height|link|font-weight|font|align|alpha|cspace|indent|line-indent|margin|mspace|pos|rotate|space|style|voffset|width|gradient|page)\s*(?:=|\s+[A-Za-z-]+\s*=)[^>]*>|<\s*(?:nobr|noparse|allcaps|smallcaps|lowercase|uppercase|strikethrough)\s*>|<\s*/\s*(?:mark|sprite|line-height|link|font-weight|font|align|alpha|cspace|indent|line-indent|margin|mspace|pos|rotate|space|style|voffset|width|gradient|page|nobr|noparse|allcaps|smallcaps|lowercase|uppercase|strikethrough)\s*>";

        public static bool TryGetTagEnd(string text, int start, out int end)
        {
            end = start;
            if (text[start] != '<') return false;
            int i = start + 1;
            if (i < text.Length && text[i] == '/') i++;
            if (i >= text.Length) return false;
            if (text[i] == '#') i++;
            else
            {
                if (!IsLetter(text[i])) return false;
                while (i < text.Length && (IsLetter(text[i]) || (text[i] >= '0' && text[i] <= '9') || text[i] == '-')) i++;
                if (i < text.Length && text[i] != '>' && text[i] != '=' && text[i] != '/' && !char.IsWhiteSpace(text[i])) return false;
            }
            char quote = '\0';
            for (; i < text.Length; i++)
            {
                char ch = text[i];
                if (quote != '\0')
                {
                    if (ch == quote) quote = '\0';
                    continue;
                }
                if (ch == '"' || ch == '\'') { quote = ch; continue; }
                if (ch == '>') { end = i; return true; }
                if (ch == '<' || ch == '\r' || ch == '\n') return false;
            }
            return false;
        }

        private static bool IsLetter(char ch)
        {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        }
    }
}
