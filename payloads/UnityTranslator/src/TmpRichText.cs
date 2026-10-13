namespace DstUnity
{
    // Unity 显示层共享的标签边界；仅保护标签及属性，不改写任何共享译文或缓存键。
    public static class TmpRichText
    {
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
