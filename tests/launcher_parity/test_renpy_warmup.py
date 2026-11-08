"""真实预热入口：Ren'Py 字面量与语句分类，C/C# 同时对照引擎语义。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


CASES = {
    'lexical_keys': (r'''label start:
    e "Open  the   door."
    e r"Keep \n literal."
    e "Use \[Door\] and \{Hint\} and \%."
    e "Backslash \\ then \u00e9."
    e "Keep\ \ spaces."
    e "Fold\   escaped spaces."
    e "Welcome back, [player_name]."
    e " Leading and trailing. "
''', ['Open the door.', r'Keep \n literal.', 'Use [[Door] and {{Hint} and %%.',
      'Backslash \\ then é.', 'Keep  spaces.', 'Fold escaped spaces.', 'Welcome back, [player_name].', ' Leading and trailing. ']),
    'monologue': ('''label start:
    e """First monologue paragraph.
       Continued on another line.

       Second monologue paragraph.

       {clear}

       Third monologue paragraph."""
    e "After the monologue."
''', ['First monologue paragraph. Continued on another line.', 'Second monologue paragraph.',
      'Third monologue paragraph.', 'After the monologue.']),
    'syntax_context': ('''define e = Character("Static Speaker")
label start:
    "Old Man" "Welcome to the village."
    menu route:
        "Choose the left path":
            jump left
screen preferences():
    text "Preferences"
    textbutton _("Save Game") action Return("technical-return-value")
    font "Some Fancy Font.otf"
    color "#abcdef"
    style_prefix "navigation panel"
label ask:
    $ name = renpy.input("Your name here")
image room:
    "A frame with words.webp" with Dissolve(0.2)
init python:
    x = "A technical python value."
    image = "Python variable named image."
    y = "Still a technical Python value."
    screen = "Python variable named screen."
    def f():
        return "Do not translate Python return."
label after:
    e "Actual dialogue after Python."
''', ['Old Man', 'Welcome to the village.', 'Choose the left path', 'Preferences', 'Save Game',
      'Your name here', 'Actual dialogue after Python.']),
    'comments_and_literals': ('''label start:
    e "Visible line." # "Comment is not dialogue."
    e "Another visible line." with Dissolve("not text")
    e "String with # literal."
    e "String with font literal."
''', ['Visible line.', 'Another visible line.', 'String with # literal.', 'String with font literal.']),
    'inline_font_and_crlf': ('label start:\r\n    e "{font=fonts/Original.ttf}Read this message.{/font}"\r\n'
                             '    e """First paragraph.\r\n\r\n    Second paragraph."""\r\n',
                            ['{font=fonts/Original.ttf}Read this message.{/font}', 'First paragraph.', 'Second paragraph.']),
    'screen_python_literals': (r'''screen literal_probe():
    text "Open  the   door."
    text "Keep \[literal\] and \% literal."
    text _("Hex \x21 and \u4e2d and \U0001f600.")
    text "Octal \101 and\t tab."
    text R"Raw \n and  spaces."
    text screen_variable style "technical style name"
    text ("Computed " + screen_variable) style "another technical style"
    text "Joined " "literal"
    text "Formatted %s" % screen_variable
label ask:
    $ name = renpy.input("Prompt \x21 and  spaces.")
    $ ignored = 'Code mentioning renpy.input("Not a prompt")'
    $ name = renpy.input ("Spaced call prompt")
''', ['Open  the   door.', r'Keep \[literal\] and \% literal.', 'Hex ! and 中 and 😀.',
      'Octal A and\t tab.', r'Raw \n and  spaces.', 'Prompt ! and  spaces.', 'Spaced call prompt']),
    'unsupported_python_escape': (r'''screen unicode_probe():
    text "Before unsupported literal."
    text "Named \N{GREEK SMALL LETTER PI} Unicode."
    text r"Ambiguous raw \u4e2d Unicode."
    text "After unsupported literal."
label after:
    "Adjacent ordinary dialogue."
''', ['Before unsupported literal.', 'After unsupported literal.', 'Adjacent ordinary dialogue.']),
}


def run(exe, game):
    result = subprocess.run([str(exe), str(game), '1', 'dump'], capture_output=True, timeout=20)
    assert result.returncode == 0, result.stderr.decode('utf-8', errors='replace')
    posts = [json.loads(line.split(' ', 1)[1]) for line in result.stdout.decode('utf-8-sig').splitlines()
             if line.startswith('post=/prefetch ')]
    assert all(post.get('engine') == 'renpy' for post in posts), posts
    return [text for post in posts for text in post['texts']], posts


def check(c, cs, root):
    failures = []
    for name, (source, expected) in CASES.items():
        game = root / name
        script = game / 'game' / 'script.rpy'
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_bytes(source.encode('utf-8'))
        original = hashlib.sha256(script.read_bytes()).hexdigest()
        snapshots = []
        for exe in (c, cs):
            texts, posts = run(exe, game)
            if texts != expected:
                failures.append(f'{name}/{exe.name}: expected={expected!r}, actual={texts!r}')
            assert hashlib.sha256(script.read_bytes()).hexdigest() == original, 'warmup modified script'
            snapshots.append(posts)
        if snapshots[0] != snapshots[1]: failures.append(name + ': C/C# request body mismatch')
    game = root / 'nested_only'
    script = game / 'game' / 'chapters' / 'script.rpy'
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text('label start:\n    "Nested story dialogue."\n', encoding='utf-8')
    for exe in (c, cs):
        texts, _ = run(exe, game)
        if texts != ['Nested story dialogue.']:
            failures.append(f'nested_only/{exe.name}: actual={texts!r}')
    if failures: raise AssertionError('\n'.join(failures))
    print('[PASS] RenPy warmup: exact lexical keys, monologue, screen/ATL/Python isolation, readonly C/C# parity')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--c', type=Path, required=True)
    parser.add_argument('--cs', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    check(args.c.resolve(), args.cs.resolve(), args.root.resolve())
