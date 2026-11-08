"""使用真实 Ren'Py 分段、FreeType 字形与离屏绘制检查字体来源。"""
import argparse
from pathlib import Path
import shutil

from test_renpy_engine_runtime import run


STORY = r'''
init -1 python:
    dst_group = FontGroup().add('fonts/Icons.ttf', 0xef00, 0xefff).add('fonts/Icons.ttf', 0x1f600, 0x1f600).add('fonts/Body.ttf', None, None).remap('@', 0xef0a)
    dst_no_default = FontGroup().add('fonts/Body.ttf', 0x20, 0x7e).add('fonts/Icons.ttf', 0xef00, 0xefff)
    dst_explicit = FontGroup().add('fonts/ExplicitCjk.otf', 0x4e2d, 0x4e2d).add('fonts/Body.ttf', None, None)
    config.font_name_map['dst_group_alias'] = dst_group
    config.font_replacement_map[('fonts/Icons.ttf', True, False)] = ('fonts/MappedIcons.ttf', False, False)
    dst_original_maps = dict(config.font_replacement_map)
    dst_original_group_map = dict(dst_group.map)
    dst_original_remap = dict(dst_group.char_map)
    dst_engine_subsegment = renpy.text.text.TextSegment.subsegment
    dst_subsegment_calls = []
    def dst_game_subsegment(segment, text):
        dst_subsegment_calls.append(text)
        if text == u'中 failure':
            raise ValueError('game font failure')
        for part in dst_engine_subsegment(segment, text):
            yield part
    renpy.text.text.TextSegment.subsegment = dst_game_subsegment
style default:
    font 'fonts/Body.ttf'
style dst_probe is default:
    size 48
    color '#ffffff'
style dst_group is dst_probe:
    font dst_group
style dst_no_default is dst_probe:
    font dst_no_default
style dst_explicit is dst_probe:
    font dst_explicit
translate dst_font_language style dst_group:
    font dst_no_default
'''


DRIVER = r'''
init 1000 python:
    def dst_font_guard():
        import hashlib
        import io
        import json
        import pygame_sdl2 as pygame
        failures = []
        checks = [0]
        def require(condition, message):
            checks[0] += 1
            if not condition:
                failures.append(message)
        pending = []
        store._ds_note_pending = lambda text, priority=False: pending.append(text)
        layout = renpy.text.text.Layout.__new__(renpy.text.text.Layout)
        layout.oversample = 1.0
        layout.start_segment = None
        layout.end_segment = None
        layout.cps = 0
        if hasattr(renpy.text.font, 'hbfont'):
            style.dst_probe.shaper = 'freetype'
        def segments(text, style_name='dst_probe'):
            displayable = renpy.text.text.Text(text, style=style_name, substitute=False)
            displayable.update()
            paragraphs = layout.segment(displayable.tokens, displayable.style, {}, displayable)
            return [(segment, value) for paragraph in paragraphs for segment, value in paragraph if isinstance(segment, renpy.text.text.TextSegment)]
        def raw_segments(font, text, bold=False, italic=False):
            segment = renpy.text.text.TextSegment()
            segment.cps = 0
            segment.take_style(style.dst_probe, layout)
            segment.font, segment.bold, segment.italic = font, bold, italic
            return [(segment, text)]
        def paint(parts, raw=False):
            surface = renpy.display.pgrender.surface((512, 112), True)
            surface.fill((0, 0, 0, 0))
            x = 8.0
            for segment, value in parts:
                if raw:
                    # 参考图直接加载指定文件，独立于全局字体替换表和被测分段方法。
                    if hasattr(renpy.text.font, 'hbfont'):
                        face = renpy.text.font.load_face(segment.font, 'freetype')
                    else:
                        face = renpy.text.font.load_face(segment.font)
                    font = renpy.text.ftfont.FTFont(face, float(segment.size), float(segment.bold), segment.italic, 0, segment.antialias, segment.vertical, segment.hinting)
                    glyphs = font.glyphs(value)
                else:
                    glyphs = segment.glyphs(value, layout)
                for glyph in glyphs:
                    glyph.x += int(round(x))
                    glyph.y += 72
                    x += glyph.advance
                if raw:
                    font.draw(surface, 0, 0, segment.color, glyphs, segment.underline, segment.strikethrough, segment.black_color)
                else:
                    class DrawInfo:
                        override_color = None
                        outline = 0
                    info = DrawInfo()
                    info.surface = surface
                    segment.draw(glyphs, info, 0, 0, layout)
            return surface
        def pixels(surface):
            data = bytearray()
            for y in range(surface.get_height()):
                for x in range(surface.get_width()):
                    data.extend(surface.get_at((x, y)))
            return bytes(data)
        def check_pixels(text, style_name, expected_font, expected_text=None, name=None):
            actual = paint(segments(text, style_name))
            expected = paint(raw_segments(expected_font, text if expected_text is None else expected_text), True)
            require(any(bytearray(pixels(expected))[3::4]), 'reference glyph was blank: ' + (name or text))
            require(pixels(actual) == pixels(expected), 'glyph pixels differ: ' + (name or text))
            if name:
                pygame.image.save(actual, renpy.config.basedir + '/font-' + name + '.png')
            return actual
        require(style.dst_group.font is dst_group, 'game FontGroup style was replaced')
        require(renpy.config.font_replacement_map == dst_original_maps, 'game font replacement map was changed')
        require(dst_group.map == dst_original_group_map and dst_group.char_map == dst_original_remap, 'original FontGroup data was mutated')
        check_pixels(u'\uef0a', 'dst_group', 'fonts/Icons.ttf', name='private-use')
        check_pixels(u'@', 'dst_group', 'fonts/Icons.ttf', u'\uef0a', name='remap')
        check_pixels(u'\U0001f600', 'dst_group', 'fonts/Icons.ttf', name='emoji')
        check_pixels(u'{b}\uef0a{/b}', 'dst_group', 'fonts/MappedIcons.ttf', u'\uef0a', name='bold-map')
        check_pixels(u'A', 'dst_probe', 'fonts/Body.ttf', name='latin')
        check_pixels(u'中', 'dst_probe', 'ds_font.otf', name='chinese')
        mixed = segments(u'中@\U0001f600', 'dst_group')
        require(any(segment.font == 'ds_font.otf' and value == u'中' for segment, value in mixed), 'FontGroup default did not gain Chinese coverage')
        pygame.image.save(paint(mixed), renpy.config.basedir + '/font-mixed.png')
        try:
            bare = segments(u'中\uef0a', 'dst_no_default')
            require(any(segment.font == 'ds_font.otf' and value == u'中' for segment, value in bare), 'FontGroup without default lost Chinese fallback')
        except Exception as exc:
            failures.append('FontGroup without default failed: ' + repr(exc))
        explicit = segments(u'中', 'dst_explicit')
        require(explicit[0][0].font == 'fonts/ExplicitCjk.otf', 'explicit CJK range was replaced')
        check_pixels(u'{font=dst_group_alias}@{/font}', 'dst_probe', 'fonts/Icons.ttf', u'\uef0a', name='alias')
        check_pixels(u'{font=fonts/Icons.ttf}\uef0a{/font}', 'dst_probe', 'fonts/Icons.ttf', u'\uef0a', name='inline')
        check_pixels(u'{font=fonts/InlineOriginal.ttf}中{/font}', 'dst_probe', 'ds_font.otf', u'中', name='inline-chinese')
        check_pixels(u'，。！？', 'dst_probe', 'ds_font.otf', name='punctuation')
        require(list(dst_group.segment(u'@')) == [('fonts/Icons.ttf', u'\uef0a')], 'standalone group remap changed')
        try:
            list(dst_no_default.segment(u'中'))
            require(False, 'standalone group unexpectedly accepted an unmapped character')
        except Exception as exc:
            require('FontGroup' in str(exc), 'standalone group failed for an unrelated reason')
        try:
            segments(u'\u03a9', 'dst_no_default')
            require(False, 'unmapped non-CJK error was concealed')
        except Exception as exc:
            require('FontGroup' in str(exc), 'unmapped non-CJK failed for an unrelated reason')
        require(dst_subsegment_calls, 'existing game subsegment hook was bypassed')
        try:
            list(raw_segments('fonts/Body.ttf', u'')[0][0].subsegment(u'中 failure'))
            require(False, 'game font exception was concealed')
        except ValueError as exc:
            require(str(exc) == 'game font failure', 'game font error changed')
        require(not store._ds_font_segment_context.stack, 'font failure left an active context')
        grouped = raw_segments(dst_explicit, u'')[0][0].subsegment(u'中A')
        require(next(grouped)[0].font == 'fonts/ExplicitCjk.otf', 'group generator lost explicit CJK')
        require(not store._ds_font_segment_context.stack, 'suspended group generator owns the context')
        plain = raw_segments('fonts/Body.ttf', u'')[0][0].subsegment(u'中A')
        require(next(plain)[0].font == 'ds_font.otf', 'interleaved plain generator inherited group ownership')
        require(next(grouped)[0].font == 'fonts/Body.ttf', 'interleaved group generator lost Latin font')
        grouped.close()
        plain.close()
        require(not store._ds_font_segment_context.stack, 'closed font generator left an active context')
        old_transform = renpy.game.preferences.font_transform
        transform_calls = []
        def transform(font):
            transform_calls.append(font)
            return dst_explicit
        renpy.config.font_transforms['dst_font_guard'] = transform
        try:
            renpy.game.preferences.font_transform = 'dst_font_guard'
            transformed = list(raw_segments('fonts/Body.ttf', u'')[0][0].subsegment(u'中A'))
            require(transform_calls == ['fonts/Body.ttf'], 'font transform was skipped, repeated, or passed a proxy')
            require([part.font for part, value in transformed] == ['fonts/ExplicitCjk.otf', 'fonts/Body.ttf'], 'font transform result lost group ownership')
        finally:
            renpy.game.preferences.font_transform = old_transform
            del renpy.config.font_transforms['dst_font_guard']
        remapped = FontGroup().add('fonts/Icons.ttf', 0xef00, 0xefff).add('fonts/Body.ttf', None, None).remap(u'中', 0xef0a).remap('@', 0x4e2d)
        remapped_parts = list(raw_segments(remapped, u'')[0][0].subsegment(u'中@'))
        require([(part.font, value) for part, value in remapped_parts] == [('fonts/Icons.ttf', u'\uef0a'), ('ds_font.otf', u'中')], 'CJK routing did not follow remap targets')
        for font in ('fonts/Body.ttf', dst_group):
            marks = list(raw_segments(font, u'')[0][0].subsegment(u'中\ufe00\U000e0100\u0301'))
            require([(part.font, value) for part, value in marks] == [('ds_font.otf', u'中\ufe00\U000e0100\u0301')], 'CJK variation/combining sequence was split across fonts: ' + repr([(part.font, value) for part, value in marks]))
            supplementary = list(raw_segments(font, u'')[0][0].subsegment(u'\U00020000\U0001f400'))
            require([(part.font, value) for part, value in supplementary] == [('ds_font.otf', u'\U00020000'), ('fonts/Body.ttf', u'\U0001f400')], 'supplementary CJK routing changed a neighboring emoji with the same low surrogate')
        installed_subsegment = renpy.text.text.TextSegment.subsegment
        installed_group_segment = renpy.text.font.FontGroup.segment
        store._ds_apply_font()
        require(renpy.text.text.TextSegment.subsegment == installed_subsegment and renpy.text.font.FontGroup.segment == installed_group_segment, 'font installation stacked wrappers')
        group_type = renpy.text.font.FontGroup
        class UnsupportedGroup:
            pass
        error_key = 'install-font-segmentation|AttributeError'
        old_errors = store._ds_error_counts.get(error_key, 0)
        try:
            renpy.text.font.FontGroup = UnsupportedGroup
            store._ds_apply_font()
            require(store._ds_error_counts.get(error_key, 0) == old_errors + 1, 'unsupported API lost its diagnostic')
            require(renpy.text.text.TextSegment.subsegment == installed_subsegment, 'unsupported API partially replaced font hooks')
        finally:
            renpy.text.font.FontGroup = group_type
        for frame in range(60):
            style.dst_probe.color = '#ff0000' if frame % 2 else '#00ff00'
            current = segments(u'中@', 'dst_group')
            require(current[0][0].color == current[1][0].color == style.dst_probe.color, 'font split lost changing style color')
        style.dst_probe.color = '#ffffff'
        renpy.change_language('dst_font_language', force=True)
        require(style.dst_group.font is dst_no_default, 'language style FontGroup was replaced')
        try:
            require(any(segment.font == 'ds_font.otf' for segment, value in segments(u'中', 'dst_group')), 'language change lost Chinese coverage')
        except Exception as exc:
            failures.append('language FontGroup failed: ' + repr(exc))
        renpy.change_language(None, force=True)
        require(style.dst_group.font is dst_group, 'return to original language lost FontGroup')
        require(renpy.config.font_replacement_map == dst_original_maps, 'language callback changed game replacement map')
        require(dst_group.map == dst_original_group_map and dst_group.char_map == dst_original_remap, 'language callback mutated original FontGroup')
        print('FONT_PROBE failures=' + repr(failures))
        if failures:
            raise AssertionError('; '.join(failures))
        version = renpy.version() if callable(renpy.version) else renpy.version
        with io.open(renpy.config.basedir + '/result.json', 'w', encoding='utf-8') as output:
            output.write(u'%s' % json.dumps({'kind': 'font', 'version': version, 'passed': checks[0], 'mixed_rgba_sha256': hashlib.sha256(pixels(paint(mixed))).hexdigest()}, ensure_ascii=True))
        return False
    renpy.arguments.register_command('dst-engine-guard', dst_font_guard)
'''


def prepare_game(game, runtime):
    source = runtime / 'renpy/common'
    for name in ('Body.ttf', 'Icons.ttf'):
        shutil.copyfile(source / 'DejaVuSans.ttf', game / 'fonts' / name)
    shutil.copyfile(source / 'DejaVuSans-Bold.ttf', game / 'fonts/MappedIcons.ttf')
    shutil.copyfile(game / 'ds_font.otf', game / 'fonts/ExplicitCjk.otf')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path)
    args = parser.parse_args()
    run(args.runtime, driver=DRIVER, story=STORY, prepare_game=prepare_game, artifacts=args.artifacts)
