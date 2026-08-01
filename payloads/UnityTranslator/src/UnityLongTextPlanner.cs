using System;
using System.Collections.Generic;

public sealed class UnityLongTextPart
{
    public string LeadingWhitespace { get; private set; }

    public string Text { get; private set; }

    public string TrailingWhitespace { get; private set; }

    internal UnityLongTextPart(string leadingWhitespace, string text, string trailingWhitespace)
    {
        LeadingWhitespace = leadingWhitespace;
        Text = text;
        TrailingWhitespace = trailingWhitespace;
    }
}

/* Pure Unity payload helper. The input has already had tags, variables and
   numbers replaced with __DS_TOKEN_n__ placeholders, so boundaries cannot
   split renderer control data. Returning null keeps the existing atomic path. */
public static class UnityLongTextPlanner
{
    private const int MinimumSegmentChars = 64;

    public static IReadOnlyList<UnityLongTextPart> Plan(
        string protectedText,
        int threshold,
        int targetSegmentChars,
        int maxSegments)
    {
        if (string.IsNullOrWhiteSpace(protectedText) ||
            threshold < 1 ||
            protectedText.Length < threshold ||
            targetSegmentChars < MinimumSegmentChars ||
            maxSegments < 2)
        {
            return null;
        }

        int desiredSegments = (protectedText.Length + targetSegmentChars - 1) / targetSegmentChars;
        desiredSegments = Math.Max(2, Math.Min(maxSegments, desiredSegments));

        List<UnityLongTextPart> parts = new List<UnityLongTextPart>(desiredSegments);
        int start = 0;
        for (int segment = 0; segment < desiredSegments - 1; segment++)
        {
            int segmentsRemaining = desiredSegments - segment;
            int remainingChars = protectedText.Length - start;
            int ideal = start + (remainingChars + segmentsRemaining - 1) / segmentsRemaining;
            int minimum = start + MinimumSegmentChars;
            int maximum = protectedText.Length - MinimumSegmentChars * (segmentsRemaining - 1);
            if (maximum <= minimum)
            {
                return null;
            }

            int boundary = FindBoundary(protectedText, start, ideal, minimum, maximum);
            if (boundary <= start)
            {
                return null;
            }

            UnityLongTextPart part = CreatePart(protectedText.Substring(start, boundary - start));
            if (part == null || part.Text.Length < MinimumSegmentChars / 2)
            {
                return null;
            }
            parts.Add(part);
            start = boundary;
        }

        UnityLongTextPart finalPart = CreatePart(protectedText.Substring(start));
        if (finalPart == null || finalPart.Text.Length < MinimumSegmentChars / 2)
        {
            return null;
        }
        parts.Add(finalPart);
        return parts.Count >= 2 ? parts : null;
    }

    private static int FindBoundary(string text, int start, int ideal, int minimum, int maximum)
    {
        int after = FindForward(text, ideal, maximum, true);
        if (after >= minimum) return after;

        int before = FindBackward(text, ideal, minimum, true);
        if (before >= minimum) return before;

        after = FindForward(text, ideal, maximum, false);
        if (after >= minimum) return after;

        before = FindBackward(text, ideal, minimum, false);
        return before >= minimum && before > start ? before : -1;
    }

    private static int FindForward(string text, int from, int maximum, bool sentenceOnly)
    {
        int end = Math.Min(maximum, text.Length - 1);
        for (int boundary = Math.Max(1, from); boundary <= end; boundary++)
        {
            if (IsSafeBoundary(text, boundary, sentenceOnly)) return boundary;
        }
        return -1;
    }

    private static int FindBackward(string text, int from, int minimum, bool sentenceOnly)
    {
        for (int boundary = Math.Min(from, text.Length - 1); boundary >= minimum; boundary--)
        {
            if (IsSafeBoundary(text, boundary, sentenceOnly)) return boundary;
        }
        return -1;
    }

    private static bool IsSafeBoundary(string text, int boundary, bool sentenceOnly)
    {
        if (boundary <= 0 || boundary >= text.Length || IsInsideProtectedToken(text, boundary))
        {
            return false;
        }

        char previous = text[boundary - 1];
        char next = text[boundary];
        if (sentenceOnly)
        {
            if (previous == '\r' || previous == '\n') return true;
            if (!IsSentenceEnd(previous)) return false;
            if (IsFullWidthSentenceEnd(previous)) return true;
            return char.IsWhiteSpace(next);
        }
        return char.IsWhiteSpace(previous) || char.IsWhiteSpace(next);
    }

    private static bool IsSentenceEnd(char ch)
    {
        return ch == '.' || ch == '!' || ch == '?' || ch == ';' ||
               ch == '\u3002' || ch == '\uff01' || ch == '\uff1f' || ch == '\uff1b';
    }

    private static bool IsFullWidthSentenceEnd(char ch)
    {
        return ch == '\u3002' || ch == '\uff01' || ch == '\uff1f' || ch == '\uff1b';
    }

    private static bool IsInsideProtectedToken(string text, int boundary)
    {
        int tokenStart = text.LastIndexOf("__DS_TOKEN_", boundary - 1, StringComparison.Ordinal);
        if (tokenStart < 0) return false;
        int tokenEnd = text.IndexOf("__", tokenStart + "__DS_TOKEN_".Length, StringComparison.Ordinal);
        return tokenEnd >= 0 && boundary > tokenStart && boundary < tokenEnd + 2;
    }

    private static UnityLongTextPart CreatePart(string raw)
    {
        if (string.IsNullOrEmpty(raw)) return null;
        int first = 0;
        while (first < raw.Length && char.IsWhiteSpace(raw[first])) first++;
        int last = raw.Length;
        while (last > first && char.IsWhiteSpace(raw[last - 1])) last--;
        if (first >= last) return null;
        return new UnityLongTextPart(
            raw.Substring(0, first),
            raw.Substring(first, last - first),
            raw.Substring(last));
    }
}
